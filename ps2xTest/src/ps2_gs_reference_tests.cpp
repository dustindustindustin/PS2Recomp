#include "MiniTest.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_memory.h"
#include "runtime/ps2_memory.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    uint32_t referenceCT32ByteOffset(uint32_t baseBlock, uint32_t bufferWidth,
                                     uint32_t x, uint32_t y)
    {
        static constexpr uint8_t kBlockTable[4][8] = {
            {0, 1, 4, 5, 16, 17, 20, 21},
            {2, 3, 6, 7, 18, 19, 22, 23},
            {8, 9, 12, 13, 24, 25, 28, 29},
            {10, 11, 14, 15, 26, 27, 30, 31},
        };
        static constexpr uint8_t kColumnTable[8][8] = {
            {0, 1, 4, 5, 8, 9, 12, 13},
            {2, 3, 6, 7, 10, 11, 14, 15},
            {16, 17, 20, 21, 24, 25, 28, 29},
            {18, 19, 22, 23, 26, 27, 30, 31},
            {32, 33, 36, 37, 40, 41, 44, 45},
            {34, 35, 38, 39, 42, 43, 46, 47},
            {48, 49, 52, 53, 56, 57, 60, 61},
            {50, 51, 54, 55, 58, 59, 62, 63},
        };
        const uint32_t pagesPerRow = (bufferWidth != 0u) ? bufferWidth : 1u;
        const uint32_t page = (baseBlock >> 5u) + (y >> 5u) * pagesPerRow + (x >> 6u);
        const uint32_t blockId = (baseBlock & 0x1Fu) + kBlockTable[(y >> 3u) & 3u][(x >> 3u) & 7u];
        const uint32_t pageOffset = (blockId >> 5u) << 13u;
        const uint32_t localBlock = blockId & 0x1Fu;
        return (page << 13u) + pageOffset + localBlock * 256u +
               static_cast<uint32_t>(kColumnTable[y & 7u][x & 7u]) * 4u;
    }

    uint32_t renderSinglePoint(uint8_t framePsm, uint32_t initialPixel)
    {
        std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
        GS gs;
        gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

        constexpr uint32_t kX = 2u;
        constexpr uint32_t kY = 3u;
        // Seed the hidden fourth storage byte through the CT32 alias; CT24
        // writes correctly ignore that byte by definition.
        const uint8_t seedPsm = (framePsm == GS_PSM_CT24) ? GS_PSM_CT32 : framePsm;
        gs.writeVramPixel(seedPsm, 0u, 1u, kX, kY, initialPixel);
        gs.writeVramPixel(seedPsm, 0u, 1u, kX + 1u, kY, initialPixel);

        gs.writeRegister(GS_REG_FRAME_1,
                         (1ull << 16) | (static_cast<uint64_t>(framePsm) << 24));
        gs.writeRegister(GS_REG_ZBUF_1, 1ull << 32); // ZMSK
        gs.writeRegister(GS_REG_SCISSOR_1, (7ull << 16) | (7ull << 48));
        gs.writeRegister(GS_REG_XYOFFSET_1, 0ull);
        gs.writeRegister(GS_REG_TEST_1, 0x30000ull); // ZTE, ZTST=ALWAYS
        gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_POINT));
        // RGBA = (E7, A3, 5D, FF), packed in the GS register's low word.
        gs.writeRegister(GS_REG_RGBAQ, 0xFF5DA3E7ull);
        gs.writeRegister(GS_REG_XYZ2,
                         static_cast<uint64_t>(kX * 16u) |
                             (static_cast<uint64_t>(kY * 16u) << 16u));

        // Read CT24 through the 32-bit alias so the oracle can also observe
        // the storage byte that the RGB24 format itself masks on readback.
        const uint8_t readPsm = (framePsm == GS_PSM_CT24) ? GS_PSM_CT32 : framePsm;
        return gs.readVramPixel(readPsm, 0u, 1u, kX, kY);
    }
}

void register_ps2_gs_reference_tests()
{
    MiniTest::Case("PS2GSReference", [](TestCase &tc)
    {
        tc.Run("CT16 and CT24 point writes match exact framebuffer-format output", [](TestCase &t)
        {
            // CT16 stores the upper 5 bits of each color channel and the
            // source alpha MSB: 28 | (20 << 5) | (11 << 10) | (1 << 15).
            const uint32_t ct16 = renderSinglePoint(GS_PSM_CT16, 0x1234u);
            t.Equals(ct16, 0xAE9Cu,
                     "CT16 output should quantize RGB to 5:5:5 and retain the alpha bit");

            // RGB24 updates exactly the lower three bytes. Its fourth byte is
            // outside the framebuffer format and must retain the sentinel.
            const uint32_t ct24 = renderSinglePoint(GS_PSM_CT24, 0xAB030201u);
            t.Equals(ct24, 0xAB5DA3E7u,
                     "CT24 output should write the exact RGBAQ low-word payload while preserving the unrelated high byte");
        });

        tc.Run("2D CT32 local-to-host readback preserves swizzled row order across partial chunks", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t kSourceX = 7u;
            constexpr uint32_t kSourceY = 31u;
            constexpr uint32_t kWidth = 3u;
            constexpr uint32_t kHeight = 2u;
            constexpr uint32_t kSourceBufferWidth = 2u;
            constexpr std::array<uint32_t, kWidth * kHeight> kPixels{{
                0x10203040u, 0x21314151u, 0x32425262u,
                0x43536373u, 0x54647484u, 0x65758595u,
            }};

            std::vector<uint8_t> expected;
            expected.reserve(kPixels.size() * sizeof(uint32_t));
            for (uint32_t row = 0u; row < kHeight; ++row)
            {
                for (uint32_t column = 0u; column < kWidth; ++column)
                {
                    const uint32_t pixel = kPixels[row * kWidth + column];
                    const uint32_t byteOffset = referenceCT32ByteOffset(
                        0u, kSourceBufferWidth, kSourceX + column, kSourceY + row);
                    std::memcpy(vram.data() + byteOffset, &pixel, sizeof(pixel));
                    expected.push_back(static_cast<uint8_t>(pixel));
                    expected.push_back(static_cast<uint8_t>(pixel >> 8u));
                    expected.push_back(static_cast<uint8_t>(pixel >> 16u));
                    expected.push_back(static_cast<uint8_t>(pixel >> 24u));
                }
            }

            const uint64_t bitbltbuf =
                (static_cast<uint64_t>(kSourceBufferWidth) << 16u) |
                (static_cast<uint64_t>(GS_PSM_CT32) << 24u);
            const uint64_t trxpos =
                (static_cast<uint64_t>(kSourceX) << 0u) |
                (static_cast<uint64_t>(kSourceY) << 16u);
            const uint64_t trxreg =
                (static_cast<uint64_t>(kWidth) << 0u) |
                (static_cast<uint64_t>(kHeight) << 32u);
            gs.writeRegister(GS_REG_BITBLTBUF, bitbltbuf);
            gs.writeRegister(GS_REG_TRXPOS, trxpos);
            gs.writeRegister(GS_REG_TRXREG, trxreg);
            gs.writeRegister(GS_REG_TRXDIR, 1ull);

            std::vector<uint8_t> actual(expected.size(), 0u);
            const uint32_t first = gs.consumeLocalToHostBytes(actual.data(), 5u);
            const uint32_t second = gs.consumeLocalToHostBytes(actual.data() + first, 11u);
            const uint32_t third = gs.consumeLocalToHostBytes(actual.data() + first + second,
                                                              static_cast<uint32_t>(actual.size()) - first - second);

            t.Equals(first, 5u, "the first partial read should return exactly five bytes");
            t.Equals(second, 11u, "the second partial read should continue at the exact byte offset");
            t.Equals(third, 8u, "the final partial read should drain the remaining bytes");
            t.Equals(actual, expected,
                     "2D CT32 readback should match logical row-major pixels across page and chunk boundaries");
        });
    });
}
