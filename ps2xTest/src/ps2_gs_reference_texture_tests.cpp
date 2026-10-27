#include "MiniTest.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_rasterizer.h"
#include "runtime/ps2_gs_psmct32.h"
#include "runtime/ps2_gs_psmt8.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    constexpr uint32_t kDestinationFbp = 4u;
    constexpr uint32_t kDestinationBlock = kDestinationFbp << 5u;

    void writeCt32(std::vector<uint8_t> &vram, uint32_t base, uint32_t x, uint32_t y, uint32_t color)
    {
        const uint32_t address = GSPSMCT32::addrPSMCT32(base, 1u, x, y);
        std::memcpy(vram.data() + address, &color, sizeof(color));
    }

    uint32_t readCt32(const std::vector<uint8_t> &vram, uint32_t base, uint32_t x, uint32_t y)
    {
        uint32_t color = 0u;
        const uint32_t address = GSPSMCT32::addrPSMCT32(base, 1u, x, y);
        std::memcpy(&color, vram.data() + address, sizeof(color));
        return color;
    }

    void configureSpriteDestination(GS &gs)
    {
        constexpr uint64_t frame =
            (static_cast<uint64_t>(kDestinationFbp) << 0u) |
            (1ull << 16u) |
            (static_cast<uint64_t>(GS_PSM_CT32) << 24u);
        constexpr uint64_t scissor = (3ull << 16u) | (3ull << 48u);
        constexpr uint64_t primitive =
            static_cast<uint64_t>(GS_PRIM_SPRITE) | (1ull << 4u) | (1ull << 8u);

        gs.writeRegister(GS_REG_FRAME_1, frame);
        gs.writeRegister(GS_REG_ZBUF_1, 1ull << 32u);
        gs.writeRegister(GS_REG_SCISSOR_1, scissor);
        gs.writeRegister(GS_REG_XYOFFSET_1, 0ull);
        gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
        gs.writeRegister(GS_REG_ALPHA_1, 0ull);
        gs.writeRegister(GS_REG_PRIM, primitive);
        gs.writeRegister(GS_REG_RGBAQ, 0x80808080ull);
    }

    void drawSprite(GS &gs, uint32_t width, uint32_t height)
    {
        gs.writeRegister(GS_REG_UV, 0ull);
        gs.writeRegister(GS_REG_XYZ2, 0ull);
        gs.writeRegister(GS_REG_UV, static_cast<uint64_t>(width * 16u) |
                                        (static_cast<uint64_t>(height * 16u) << 16u));
        gs.writeRegister(GS_REG_XYZ2, static_cast<uint64_t>(width * 16u) |
                                          (static_cast<uint64_t>(height * 16u) << 16u));
    }

    uint64_t directColorTex0(uint32_t tbp)
    {
        return (static_cast<uint64_t>(tbp) << 0u) |
               (1ull << 14u) |
               (static_cast<uint64_t>(GS_PSM_CT32) << 20u) |
               (1ull << 26u) | // TW=1: 2 texels.
               (1ull << 34u) | // TCC=RGBA.
               (1ull << 35u);  // TFX=DECAL.
    }
}

void register_ps2_gs_reference_texture_tests()
{
    MiniTest::Case("PS2GSReferenceTexture", [](TestCase &tc)
    {
        tc.Run("nearest FST CT32 and T8 triangle fast path matches reference VRAM", [](TestCase &t)
        {
            for (const uint8_t psm : {static_cast<uint8_t>(GS_PSM_CT32),
                                      static_cast<uint8_t>(GS_PSM_T8)})
            {
                for (uint64_t wrap = 0u; wrap < 4u; ++wrap)
                {
                    std::vector<uint8_t> fastVram(PS2_GS_VRAM_SIZE, 0x5Au);
                    GS fastGs;
                    fastGs.init(fastVram.data(), static_cast<uint32_t>(fastVram.size()), nullptr);
                    constexpr uint32_t textureBase = 64u;
                    for (uint32_t y = 0u; y < 4u; ++y)
                        for (uint32_t x = 0u; x < 4u; ++x)
                        {
                            const uint32_t index = 1u + x + 4u * y;
                            fastGs.writeVramPixel(psm, textureBase, 1u, x, y,
                                psm == GS_PSM_CT32
                                    ? 0x80000000u | (index * 0x00110803u)
                                    : index);
                        }
                    std::vector<uint8_t> referenceVram = fastVram;
                    GS referenceGs;
                    referenceGs.init(referenceVram.data(),
                                     static_cast<uint32_t>(referenceVram.size()), nullptr);

                    GSPrimitiveSubmission submission{};
                    submission.prim.type = GS_PRIM_TRIANGLE;
                    submission.prim.tme = true;
                    submission.prim.fst = true;
                    submission.context.frame = {kDestinationFbp, 1u, GS_PSM_CT32, 0u};
                    submission.context.scissor = {0u, 7u, 0u, 7u};
                    submission.context.zbuf.zmask = true;
                    submission.context.test = 0x30000ull;
                    submission.context.tex0.tbp0 = textureBase;
                    submission.context.tex0.tbw = 1u;
                    submission.context.tex0.psm = psm;
                    submission.context.tex0.tw = 2u;
                    submission.context.tex0.th = 2u;
                    submission.context.tex0.tcc = 1u;
                    submission.context.tex0.tfx = 1u;
                    submission.context.tex0.cpsm = GS_PSM_CT32;
                    submission.context.clamp = wrap | (wrap << 2u) |
                        (1ull << 4u) | (2ull << 14u) |
                        (1ull << 24u) | (2ull << 34u);
                    for (uint32_t i = 0u; i < 16u; ++i)
                    {
                        const uint32_t color = 0x80000000u | (i * 0x000A0D11u);
                        submission.clut[i] = static_cast<uint16_t>(color);
                        submission.clut[i + 256u] = static_cast<uint16_t>(color >> 16u);
                    }
                    const auto vertex = [](float x, float y, uint16_t u, uint16_t v)
                    {
                        GSVertex result{};
                        result.x = x;
                        result.y = y;
                        result.z = 1.0;
                        result.r = result.g = result.b = result.a = 0x80u;
                        result.q = 1.0f;
                        result.u = u;
                        result.v = v;
                        return result;
                    };
                    submission.vertices[0] = vertex(0.0f, 0.0f, 5u, 11u);
                    submission.vertices[1] = vertex(8.0f, 0.0f, 133u, 11u);
                    submission.vertices[2] = vertex(0.0f, 8.0f, 5u, 139u);

                    GSRasterizer fast;
                    GSRasterizer reference;
                    fast.setSamplerStatsEnabled(true);
                    reference.setFastFstPointSamplingEnabled(false);
                    (void)fast.drawPrimitive(&fastGs, submission);
                    (void)reference.drawPrimitive(&referenceGs, submission);
                    t.IsTrue(fastVram == referenceVram,
                             "all VRAM bytes, including neighboring sentinels, must match the reference");
                    t.IsTrue(fast.samplerStats().fastSamples > 0u,
                             "the fixture must actually exercise the specialized sampler");
                }
            }
        });

        tc.Run("nearest FST fast path matches feedback and CLUT boundary cases", [](TestCase &t)
        {
            // The duel board samples textures while drawing into GS VRAM. Keep
            // the source and destination on the same page so each pixel can
            // observe earlier writes from this primitive in raster order.
            for (const uint8_t texturePsm : {static_cast<uint8_t>(GS_PSM_CT32),
                                             static_cast<uint8_t>(GS_PSM_T8)})
            {
                for (const uint8_t clutPsm : {static_cast<uint8_t>(GS_PSM_CT32),
                                              static_cast<uint8_t>(GS_PSM_CT16)})
                {
                    for (const uint16_t uvBase : {static_cast<uint16_t>(8u),
                                                  static_cast<uint16_t>(0xFF08u)})
                    {
                        std::vector<uint8_t> fastVram(PS2_GS_VRAM_SIZE, 0x5Au);
                        GS fastGs;
                        fastGs.init(fastVram.data(), static_cast<uint32_t>(fastVram.size()), nullptr);
                        for (uint32_t y = 0u; y < 8u; ++y)
                            for (uint32_t x = 0u; x < 8u; ++x)
                                writeCt32(fastVram, kDestinationBlock, x, y,
                                          0x40000000u | (x * 0x00110803u) | (y * 0x00010211u));
                        std::vector<uint8_t> referenceVram = fastVram;
                        GS referenceGs;
                        referenceGs.init(referenceVram.data(),
                                         static_cast<uint32_t>(referenceVram.size()), nullptr);

                        GSPrimitiveSubmission submission{};
                        submission.prim.type = GS_PRIM_TRIANGLE;
                        submission.prim.tme = true;
                        submission.prim.fst = true;
                        submission.context.frame = {kDestinationFbp, 1u, GS_PSM_CT32, 0u};
                        submission.context.scissor = {0u, 7u, 0u, 7u};
                        submission.context.zbuf.zmask = true;
                        submission.context.test = 0x30000ull;
                        submission.context.tex0.tbp0 = kDestinationBlock;
                        submission.context.tex0.tbw = 1u;
                        submission.context.tex0.psm = texturePsm;
                        submission.context.tex0.tw = 3u;
                        submission.context.tex0.th = 3u;
                        submission.context.tex0.tcc = 1u;
                        submission.context.tex0.tfx = 1u;
                        submission.context.tex0.cpsm = clutPsm;
                        submission.context.tex0.csa = 3u;
                        submission.context.clamp = 3ull | (2ull << 2u) |
                            (3ull << 4u) | (5ull << 14u) |
                            (1ull << 24u) | (6ull << 34u);
                        submission.texa = {0x20u, true, 0xA0u};
                        for (uint32_t i = 0u; i < submission.clut.size(); ++i)
                            submission.clut[i] = static_cast<uint16_t>(i * 193u + 0x1515u);
                        const auto vertex = [](float x, float y, uint16_t u, uint16_t v)
                        {
                            GSVertex result{};
                            result.x = x;
                            result.y = y;
                            result.z = 1.0;
                            result.r = result.g = result.b = result.a = 0x80u;
                            result.q = 1.0f;
                            result.u = u;
                            result.v = v;
                            return result;
                        };
                        submission.vertices[0] = vertex(0.5f, 0.5f, uvBase, uvBase);
                        submission.vertices[1] = vertex(7.5f, 0.5f,
                            static_cast<uint16_t>(uvBase + 112u), uvBase);
                        submission.vertices[2] = vertex(0.5f, 7.5f,
                            uvBase, static_cast<uint16_t>(uvBase + 112u));

                        GSRasterizer fast;
                        GSRasterizer reference;
                        fast.setSamplerStatsEnabled(true);
                        reference.setFastFstPointSamplingEnabled(false);
                        (void)fast.drawPrimitive(&fastGs, submission);
                        (void)reference.drawPrimitive(&referenceGs, submission);
                        t.IsTrue(fast.samplerStats().fastSamples > 0u,
                                 "the feedback fixture must use the fast sampler");
                        t.IsTrue(fastVram == referenceVram,
                                 "fast and reference samplers must produce identical VRAM with feedback");
                    }
                }
            }
        });

        tc.Run("FST nearest sampler floors fractional UV and repeats at texture edge", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
            constexpr uint32_t textureBase = 64u;
            constexpr uint32_t first = 0x80402010u;
            constexpr uint32_t second = 0xFFABCDEFu;
            writeCt32(vram, textureBase, 0u, 0u, first);
            writeCt32(vram, textureBase, 1u, 0u, second);

            GSPrimitiveSubmission submission{};
            submission.prim.fst = true;
            submission.context.tex0.tbp0 = textureBase;
            submission.context.tex0.tbw = 1u;
            submission.context.tex0.psm = GS_PSM_CT32;
            submission.context.tex0.tw = 1u;
            submission.context.tex0.th = 0u;
            GSRasterizer rasterizer;
            const auto sample = [&](uint16_t u) {
                return rasterizer.sampleTexture(&gs, submission, 0.0f, 0.0f, 1.0f,
                                                u, 0u, 0);
            };
            t.Equals(sample(0u), first, "integer UV zero selects the first texel");
            t.Equals(sample(15u), first, "fractional UV below one texel stays on the first texel");
            t.Equals(sample(16u), second, "integer UV one selects the second texel");
            t.Equals(sample(31u), second, "fractional UV below two texels stays on the second texel");
            t.Equals(sample(32u), first, "repeat wraps two texels to the first texel");
            t.Equals(sample(0xFFFFu), second, "largest unsigned UV repeats to the second texel");
        });
        tc.Run("FST CT32 texture sprite reproduces exact source pixels and leaves neighbors untouched", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t textureBase = 64u;
            constexpr uint32_t first = 0x80402010u;
            constexpr uint32_t second = 0xFFABCDEFu;
            writeCt32(vram, textureBase, 0u, 0u, first);
            writeCt32(vram, textureBase, 1u, 0u, second);

            configureSpriteDestination(gs);
            gs.writeRegister(GS_REG_TEX0_1, directColorTex0(textureBase));
            drawSprite(gs, 2u, 1u);

            t.Equals(readCt32(vram, kDestinationBlock, 0u, 0u), first,
                     "the first destination texel should equal the exact CT32 source value");
            t.Equals(readCt32(vram, kDestinationBlock, 1u, 0u), second,
                     "the second destination texel should equal the exact CT32 source value");
            t.Equals(readCt32(vram, kDestinationBlock, 2u, 0u), 0u,
                     "the sprite must not write the first pixel beyond its right edge");
            t.Equals(readCt32(vram, kDestinationBlock, 0u, 1u), 0u,
                     "the sprite must not write the first pixel beyond its bottom edge");
        });

        tc.Run("FST PSMT8 sprite resolves the loaded CT32 CLUT entry exactly", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t textureBase = 64u;
            constexpr uint32_t clutBase = 128u;
            constexpr uint32_t paletteColor = 0x7F31A5E2u;
            constexpr uint8_t paletteIndex = 3u;
            const uint32_t indexAddress = GSPSMT8::addrPSMT8(textureBase, 1u, 0u, 0u);
            vram[indexAddress] = paletteIndex;
            writeCt32(vram, clutBase, paletteIndex, 0u, paletteColor);

            configureSpriteDestination(gs);
            const uint64_t indexedTex0 =
                (static_cast<uint64_t>(textureBase) << 0u) |
                (1ull << 14u) |
                (static_cast<uint64_t>(GS_PSM_T8) << 20u) |
                (0ull << 26u) |
                (0ull << 30u) |
                (1ull << 34u) |
                (1ull << 35u) |
                (static_cast<uint64_t>(clutBase) << 37u) |
                (static_cast<uint64_t>(GS_PSM_CT32) << 51u) |
                (1ull << 61u); // CLD=1: load the CSM1 palette.
            gs.writeRegister(GS_REG_TEX0_1, indexedTex0);
            drawSprite(gs, 1u, 1u);

            t.Equals(readCt32(vram, kDestinationBlock, 0u, 0u), paletteColor,
                     "the PSMT8 source index should resolve to its loaded CT32 CLUT color");
            t.Equals(readCt32(vram, kDestinationBlock, 1u, 0u), 0u,
                     "the indexed sprite must not write outside its one-pixel texture extent");
            t.Equals(readCt32(vram, kDestinationBlock, 0u, 1u), 0u,
                     "the indexed sprite must leave the row below its destination untouched");
        });
    });
}
