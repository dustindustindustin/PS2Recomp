#include "MiniTest.h"

#include "runtime/ps2_frame_upload.h"

#include <cstdint>
#include <vector>

void register_ps2_frame_upload_tests()
{
    MiniTest::Case("PS2FrameUpload", [](TestCase &tc)
    {
        tc.Run("dirty tile planner skips identical frames and bounds sparse updates", [](TestCase &t)
        {
            constexpr uint32_t kWidth = 128u;
            constexpr uint32_t kHeight = 128u;
            std::vector<uint8_t> previous(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
            std::vector<uint8_t> current = previous;
            std::vector<PS2FrameUpload::Rect> rects;

            t.IsTrue(!PS2FrameUpload::BuildDirtyTileRects(current.data(), previous.data(),
                                                           kWidth, kHeight, 32u, 64u, rects),
                     "identical frames do not request a full upload");
            t.Equals(rects.size(), 0u, "identical frames produce no rectangles");

            current[(5u * kWidth + 5u) * 4u] = 0x7Fu;
            t.IsTrue(!PS2FrameUpload::BuildDirtyTileRects(current.data(), previous.data(),
                                                           kWidth, kHeight, 32u, 64u, rects),
                     "a sparse pixel change stays on the partial upload path");
            t.Equals(rects.size(), 1u, "one changed tile produces one upload rectangle");
            if (!rects.empty())
            {
                t.Equals(rects[0].x, 0u, "dirty rectangles align left to the tile boundary");
                t.Equals(rects[0].y, 0u, "dirty rectangles align top to the tile boundary");
                t.Equals(rects[0].width, 32u, "one tile spans its full width");
                t.Equals(rects[0].height, 32u, "one tile spans its full height");
            }

            current[(5u * kWidth + 70u) * 4u] = 0x55u;
            t.IsTrue(!PS2FrameUpload::BuildDirtyTileRects(current.data(), previous.data(),
                                                           kWidth, kHeight, 32u, 64u, rects),
                     "separate sparse tiles remain eligible for partial uploads");
            t.Equals(rects.size(), 2u, "two separate tile runs produce two rectangles");

            current[(40u * kWidth + 5u) * 4u] = 0x22u;
            t.IsTrue(!PS2FrameUpload::BuildDirtyTileRects(current.data(), previous.data(),
                                                           kWidth, kHeight, 32u, 64u, rects),
                     "a vertically coherent sparse update stays on the partial path");
            t.Equals(rects.size(), 2u, "matching dirty runs across tile rows merge into one taller rectangle");
            bool foundVerticalMerge = false;
            for (const auto &rect : rects)
            {
                if (rect.x == 0u && rect.y == 0u && rect.width == 32u && rect.height == 64u)
                    foundVerticalMerge = true;
            }
            t.IsTrue(foundVerticalMerge,
                     "the merged vertical run remains tile-aligned and spans both tile rows");

            for (size_t i = 0u; i < current.size(); i += 4u)
                current[i] = 0xFFu;
            t.IsTrue(PS2FrameUpload::BuildDirtyTileRects(current.data(), previous.data(),
                                                          kWidth, kHeight, 32u, 64u, rects),
                     "broad frame changes request one full upload");
            t.Equals(rects.size(), 0u, "full upload fallback discards partial rectangles");
        });

        tc.Run("rectangle packing preserves frame row stride and validates bounds", [](TestCase &t)
        {
            constexpr uint32_t kWidth = 5u;
            constexpr uint32_t kHeight = 4u;
            std::vector<uint8_t> source(static_cast<size_t>(kWidth) * kHeight * 4u);
            for (uint32_t y = 0u; y < kHeight; ++y)
            {
                for (uint32_t x = 0u; x < kWidth; ++x)
                {
                    const size_t pixel = (static_cast<size_t>(y) * kWidth + x) * 4u;
                    source[pixel + 0u] = static_cast<uint8_t>(y * 10u + x);
                    source[pixel + 1u] = static_cast<uint8_t>(y + 40u);
                    source[pixel + 2u] = 0xA5u;
                    source[pixel + 3u] = 0xFFu;
                }
            }

            std::vector<uint8_t> packed;
            const PS2FrameUpload::Rect rect{2u, 1u, 2u, 2u};
            t.IsTrue(PS2FrameUpload::PackRectRgba(source.data(), kWidth, kHeight, rect, packed),
                     "valid partial rectangle packs successfully");
            t.Equals(packed.size(), static_cast<size_t>(16u), "packed size uses rectangle dimensions");
            if (packed.size() == 16u)
            {
                t.Equals(packed[0u], static_cast<uint8_t>(12u), "first pixel starts at rectangle x/y");
                t.Equals(packed[4u], static_cast<uint8_t>(13u), "first packed row is contiguous");
                t.Equals(packed[8u], static_cast<uint8_t>(22u), "second packed row honors source stride");
                t.Equals(packed[12u], static_cast<uint8_t>(23u), "second packed row ends at rectangle edge");
            }

            t.IsTrue(!PS2FrameUpload::PackRectRgba(source.data(), kWidth, kHeight,
                                                   PS2FrameUpload::Rect{4u, 0u, 2u, 1u}, packed),
                     "rectangle extending past right edge is rejected");
            t.IsTrue(packed.empty(), "failed pack clears stale output pixels");
            t.IsTrue(!PS2FrameUpload::PackRectRgba(source.data(), kWidth, kHeight,
                                                   PS2FrameUpload::Rect{0u, 4u, 1u, 1u}, packed),
                     "rectangle extending past bottom edge is rejected");
            t.IsTrue(!PS2FrameUpload::PackRectRgba(nullptr, kWidth, kHeight, rect, packed),
                     "null source is rejected");
        });
    });
}
