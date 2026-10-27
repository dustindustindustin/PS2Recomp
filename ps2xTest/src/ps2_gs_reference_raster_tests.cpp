#include "MiniTest.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_memory.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <vector>

namespace
{
    constexpr uint32_t kFrameBlock = 0u;
    constexpr uint32_t kDepthPage = 1u;
    constexpr uint32_t kDepthBlock = kDepthPage << 5u;
    constexpr uint32_t kPixelX = 2u;
    constexpr uint32_t kPixelY = 3u;

    void configurePointTarget(GS &gs, uint8_t *vram, uint32_t vramSize)
    {
        gs.init(vram, vramSize, nullptr);
        gs.writeRegister(GS_REG_FRAME_1,
                         (1ull << 16u) | (static_cast<uint64_t>(GS_PSM_CT32) << 24u));
        gs.writeRegister(GS_REG_ZBUF_1, kDepthPage);
        gs.writeRegister(GS_REG_SCISSOR_1, (7ull << 16u) | (7ull << 48u));
        gs.writeRegister(GS_REG_XYOFFSET_1, 0u);
        gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_POINT));
    }

    uint64_t pointXyz(uint32_t x, uint32_t y, uint32_t z)
    {
        return static_cast<uint64_t>(x * 16u) |
               (static_cast<uint64_t>(y * 16u) << 16u) |
               (static_cast<uint64_t>(z) << 32u);
    }

    void drawPoint(GS &gs, uint32_t x, uint32_t y, uint32_t z, uint32_t rgba)
    {
        gs.writeRegister(GS_REG_RGBAQ, rgba);
        gs.writeRegister(GS_REG_XYZ2, pointXyz(x, y, z));
    }
}

void register_ps2_gs_reference_raster_tests()
{
    MiniTest::Case("PS2GSReferenceRaster", [](TestCase &tc)
    {
        tc.Run("strict depth comparison preserves rejected pixels and adjacent sentinels", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            configurePointTarget(gs, vram.data(), static_cast<uint32_t>(vram.size()));

            constexpr uint32_t kFramebufferSentinel = 0xA1B2C3D4u;
            constexpr uint32_t kSourceDepth = 0x22222222u;
            constexpr uint32_t kDepthEqualToSource = kSourceDepth;
            constexpr uint32_t kAdjacentFramebufferSentinel = 0x5A6B7C8Du;
            constexpr uint32_t kAdjacentDepthSentinel = 0x10203040u;
            gs.writeVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX, kPixelY, kFramebufferSentinel);
            gs.writeVramPixel(GS_PSM_Z32, kDepthBlock, 1u, kPixelX, kPixelY, kDepthEqualToSource);
            gs.writeVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX + 1u, kPixelY,
                         kAdjacentFramebufferSentinel);
            gs.writeVramPixel(GS_PSM_Z32, kDepthBlock, 1u, kPixelX + 1u, kPixelY,
                         kAdjacentDepthSentinel);

            // ZTE=1, ZTST=GREATER. Equality must fail; the comparison is strict.
            gs.writeRegister(GS_REG_TEST_1, (1ull << 16u) | (3ull << 17u));
            drawPoint(gs, kPixelX, kPixelY, kSourceDepth, 0x7F563412u);
            t.Equals(gs.readVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX, kPixelY),
                     kFramebufferSentinel,
                     "equal source and stored depths must fail the strict GREATER comparison");
            t.Equals(gs.readVramPixel(GS_PSM_Z32, kDepthBlock, 1u, kPixelX, kPixelY),
                     kDepthEqualToSource,
                     "a failed depth comparison must not update the depth buffer");

            // Lowering the stored value makes the same source strictly greater.
            gs.writeVramPixel(GS_PSM_Z32, kDepthBlock, 1u, kPixelX, kPixelY,
                         kSourceDepth - 1u);
            drawPoint(gs, kPixelX, kPixelY, kSourceDepth, 0x7F563412u);
            t.Equals(gs.readVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX, kPixelY), 0x7F563412u,
                     "a strictly greater source depth must write the exact RGBA value");
            t.Equals(gs.readVramPixel(GS_PSM_Z32, kDepthBlock, 1u, kPixelX, kPixelY), kSourceDepth,
                     "a passing depth test must store the source depth");

            t.Equals(gs.readVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX + 1u, kPixelY),
                     kAdjacentFramebufferSentinel,
                     "a point draw must not touch the neighboring framebuffer sentinel");
            t.Equals(gs.readVramPixel(GS_PSM_Z32, kDepthBlock, 1u, kPixelX + 1u, kPixelY),
                     kAdjacentDepthSentinel,
                     "a point draw must not touch the neighboring depth sentinel");
        });

        tc.Run("alpha threshold uses source alpha and KEEP leaves color and depth sentinels intact", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            configurePointTarget(gs, vram.data(), static_cast<uint32_t>(vram.size()));

            constexpr uint32_t kFramebufferSentinel = 0xA1B2C3D4u;
            constexpr uint32_t kDepthSentinel = 0x10203040u;
            constexpr uint32_t kSourceDepth = 0x55667788u;
            gs.writeVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX, kPixelY, kFramebufferSentinel);
            gs.writeVramPixel(GS_PSM_Z32, kDepthBlock, 1u, kPixelX, kPixelY, kDepthSentinel);

            // ATE=1, ATST=GEQUAL, AREF=0x80, AFAIL=KEEP; ZTST=ALWAYS.
            constexpr uint64_t kAlphaGate = 1ull | (5ull << 1u) | (0x80ull << 4u) |
                                            (1ull << 16u) | (1ull << 17u);
            gs.writeRegister(GS_REG_TEST_1, kAlphaGate);
            drawPoint(gs, kPixelX, kPixelY, kSourceDepth, 0x7F563412u);
            t.Equals(gs.readVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX, kPixelY),
                     kFramebufferSentinel,
                     "alpha 0x7F must fail GEQUAL 0x80 and preserve the framebuffer with AFAIL=KEEP");
            t.Equals(gs.readVramPixel(GS_PSM_Z32, kDepthBlock, 1u, kPixelX, kPixelY), kDepthSentinel,
                     "alpha rejection with AFAIL=KEEP must preserve depth");

            drawPoint(gs, kPixelX, kPixelY, kSourceDepth, 0x80563412u);
            t.Equals(gs.readVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX, kPixelY), 0x80563412u,
                     "alpha exactly equal to AREF must pass GEQUAL and write the framebuffer");
            t.Equals(gs.readVramPixel(GS_PSM_Z32, kDepthBlock, 1u, kPixelX, kPixelY), kSourceDepth,
                     "a source at the alpha threshold must also reach the ALWAYS depth write");
        });

        tc.Run("fixed-factor alpha blend matches channel arithmetic and preserves untouched pixels", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            configurePointTarget(gs, vram.data(), static_cast<uint32_t>(vram.size()));

            constexpr uint32_t kDestination = 0xAA785028u; // A=AA, B=120, G=80, R=40.
            constexpr uint32_t kSource = (0x55u << 24u) | // alpha
                                         20u |            // red
                                         (100u << 8u) |   // green
                                         (200u << 16u);   // blue
            constexpr uint32_t kNeighbor = 0xDEADBEEFu;
            gs.writeVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX, kPixelY, kDestination);
            gs.writeVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX + 1u, kPixelY, kNeighbor);
            gs.writeRegister(GS_REG_TEST_1, 0x30000ull); // ZTE, ZTST=ALWAYS.
            // (A-B)*C/128 + D = (source-destination)*64/128 + destination.
            // Channels therefore resolve to R=30, G=90, B=160. Alpha comes
            // from the source because CT32 blending affects RGB only.
            gs.writeRegister(GS_REG_ALPHA_1, (1ull << 2u) | (2ull << 4u) |
                                                  (1ull << 6u) | (64ull << 32u));
            gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_POINT) | (1ull << 6u));
            drawPoint(gs, kPixelX, kPixelY, 0x12345678u, kSource);

            t.Equals(gs.readVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX, kPixelY), 0x55A05A1Eu,
                     "fixed 0x40 blend factor should produce exact per-channel midpoint values");
            t.Equals(gs.readVramPixel(GS_PSM_CT32, kFrameBlock, 1u, kPixelX + 1u, kPixelY), kNeighbor,
                     "blending one point must preserve the neighboring framebuffer sentinel");
        });
    });
}
