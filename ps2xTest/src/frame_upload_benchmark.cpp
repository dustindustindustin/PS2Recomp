#include "runtime/ps2_frame_upload.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
constexpr uint32_t kWidth = 640u;
constexpr uint32_t kHeight = 512u;
constexpr uint32_t kTileSize = 32u;
constexpr size_t kFrameBytes = static_cast<size_t>(kWidth) * kHeight * 4u;

struct Scenario
{
    std::string_view name;
    uint32_t changedTiles;
    bool spread;
    bool vertical = false;
};

void runScenario(const Scenario &scenario)
{
    std::vector<uint8_t> previous(kFrameBytes, 0u);
    std::vector<uint8_t> current(kFrameBytes, 0u);
    const uint32_t columns = kWidth / kTileSize;
    const uint32_t totalTiles = columns * (kHeight / kTileSize);
    const uint32_t dirtyTiles = std::min(scenario.changedTiles, totalTiles);
    for (uint32_t tile = 0u; tile < dirtyTiles; ++tile)
    {
        const uint32_t tileIndex = scenario.vertical
            ? tile * columns + columns / 2u
            : scenario.spread ? tile * totalTiles / std::max(dirtyTiles, 1u) : tile;
        const uint32_t x = (tileIndex % columns) * kTileSize;
        const uint32_t y = (tileIndex / columns) * kTileSize;
        current[(static_cast<size_t>(y) * kWidth + x) * 4u] = 0xFFu;
    }

    std::vector<PS2FrameUpload::Rect> rects;
    constexpr uint32_t kWarmupIterations = 40u;
    constexpr uint32_t kMeasureIterations = 500u;
    bool fullUpload = false;
    for (uint32_t i = 0u; i < kWarmupIterations; ++i)
        fullUpload = PS2FrameUpload::BuildDirtyTileRects(current.data(), previous.data(),
                                                         kWidth, kHeight, kTileSize, 64u, rects);

    const auto start = std::chrono::steady_clock::now();
    uint64_t countedRects = 0u;
    for (uint32_t i = 0u; i < kMeasureIterations; ++i)
    {
        fullUpload = PS2FrameUpload::BuildDirtyTileRects(current.data(), previous.data(),
                                                         kWidth, kHeight, kTileSize, 64u, rects);
        countedRects += fullUpload ? 1u : rects.size();
    }
    const double elapsedUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();
    uint64_t uploadBytes = fullUpload ? kFrameBytes : 0u;
    if (!fullUpload)
    {
        for (const auto &rect : rects)
            uploadBytes += static_cast<uint64_t>(rect.width) * rect.height * 4u;
    }
    std::vector<uint8_t> packed;
    uint64_t packedBytes = 0u;
    uint64_t checksum = 0u;
    const auto packStart = std::chrono::steady_clock::now();
    if (!fullUpload)
    {
        for (uint32_t i = 0u; i < kMeasureIterations; ++i)
        {
            for (const auto &rect : rects)
            {
                const bool packedOk = PS2FrameUpload::PackRectRgba(
                    current.data(), kWidth, kHeight, rect, packed);
                if (!packedOk)
                {
                    std::cerr << "rectangle packing failed for benchmark scenario "
                              << scenario.name << '\n';
                    return;
                }
                packedBytes += packed.size();
                if (!packed.empty()) checksum += packed.front();
            }
        }
    }
    const double packElapsedUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - packStart).count();
    const uint64_t fullBytes = kFrameBytes;
    const double reduction = fullBytes == 0u ? 0.0 :
        100.0 * (1.0 - static_cast<double>(uploadBytes) / static_cast<double>(fullBytes));
    std::cout << scenario.name
              << " avg_us=" << (elapsedUs / kMeasureIterations)
              << " upload_bytes=" << uploadBytes
              << " full_bytes=" << fullBytes
              << " byte_reduction_pct=" << reduction
              << " rects=" << (fullUpload ? 1u : rects.size())
              << " fallback=" << (fullUpload ? 1u : 0u)
              << " pack_avg_us=" << (packElapsedUs / kMeasureIterations)
              << " pack_bytes=" << (packedBytes / kMeasureIterations)
              << " checksum=" << checksum
              << " rect_samples=" << countedRects << '\n';
}
}

int main()
{
    runScenario({"unchanged", 0u, false});
    runScenario({"one_tile", 1u, false});
    runScenario({"vertical_column", 16u, false, true});
    runScenario({"sparse_10pct", 32u, true});
    runScenario({"scattered_25pct", 80u, true});
    runScenario({"full_frame", 320u, false});
    return 0;
}
