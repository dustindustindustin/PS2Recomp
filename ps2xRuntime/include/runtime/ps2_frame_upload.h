#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace PS2FrameUpload
{
    struct Rect
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
    };

    // Packs a rectangle from a tightly packed RGBA8 source frame into the
    // tightly packed layout expected by UpdateTextureRec. Returns false for
    // null inputs, empty rectangles, out-of-frame rectangles, or size overflow.
    inline bool PackRectRgba(const uint8_t *source,
                             uint32_t sourceWidth,
                             uint32_t sourceHeight,
                             const Rect &rect,
                             std::vector<uint8_t> &outPixels)
    {
        outPixels.clear();
        if (!source || sourceWidth == 0u || sourceHeight == 0u ||
            rect.width == 0u || rect.height == 0u ||
            rect.x > sourceWidth || rect.y > sourceHeight ||
            rect.width > sourceWidth - rect.x || rect.height > sourceHeight - rect.y)
            return false;

        if (sourceWidth > std::numeric_limits<size_t>::max() / 4u)
            return false;
        const size_t sourceRowBytes = static_cast<size_t>(sourceWidth) * 4u;
        if (sourceHeight > std::numeric_limits<size_t>::max() / sourceRowBytes)
            return false;

        const size_t rowBytes = static_cast<size_t>(rect.width) * 4u;
        if (rowBytes / 4u != rect.width ||
            rect.height > static_cast<size_t>(-1) / rowBytes)
            return false;
        outPixels.resize(rowBytes * rect.height);

        for (uint32_t row = 0u; row < rect.height; ++row)
        {
            const size_t sourceOffset =
                static_cast<size_t>(rect.y + row) * sourceRowBytes +
                static_cast<size_t>(rect.x) * 4u;
            std::memcpy(outPixels.data() + static_cast<size_t>(row) * rowBytes,
                        source + sourceOffset, rowBytes);
        }
        return true;
    }

    // Produces conservative, tile-aligned upload rectangles for RGBA8 frames.
    // Returns true when a full upload is cheaper or the inputs are incompatible.
    inline bool BuildDirtyTileRects(const uint8_t *current,
                                   const uint8_t *previous,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t tileSize,
                                   size_t maxRectCount,
                                   std::vector<Rect> &outRects)
    {
        outRects.clear();
        if (!current || !previous || width == 0u || height == 0u || tileSize == 0u)
            return true;

        const uint32_t tileColumns = (width + tileSize - 1u) / tileSize;
        const uint32_t tileRows = (height + tileSize - 1u) / tileSize;
        static thread_local std::vector<uint8_t> dirtyTiles;
        dirtyTiles.assign(static_cast<size_t>(tileColumns) * tileRows, 0u);
        size_t dirtyTileCount = 0u;
        const size_t rowBytes = static_cast<size_t>(width) * 4u;
        for (uint32_t tileY = 0u; tileY < tileRows; ++tileY)
        {
            const uint32_t y0 = tileY * tileSize;
            const uint32_t y1 = std::min(y0 + tileSize, height);
            for (uint32_t tileX = 0u; tileX < tileColumns; ++tileX)
            {
                const uint32_t x0 = tileX * tileSize;
                const uint32_t x1 = std::min(x0 + tileSize, width);
                bool changed = false;
                for (uint32_t y = y0; y < y1 && !changed; ++y)
                {
                    const size_t rowOffset = static_cast<size_t>(y) * rowBytes;
                    const size_t tileRowOffset = rowOffset + static_cast<size_t>(x0) * 4u;
                    const size_t tileRowBytes = static_cast<size_t>(x1 - x0) * 4u;
                    changed = std::memcmp(current + tileRowOffset,
                                          previous + tileRowOffset,
                                          tileRowBytes) != 0;
                }
                if (changed)
                {
                    dirtyTiles[static_cast<size_t>(tileY) * tileColumns + tileX] = 1u;
                    ++dirtyTileCount;
                }
            }
        }

        if (dirtyTileCount == 0u)
            return false;

        const size_t totalTiles = static_cast<size_t>(tileColumns) * tileRows;
        if (dirtyTileCount * 5u >= totalTiles * 2u)
            return true;

        static thread_local std::vector<Rect> activeRects;
        static thread_local std::vector<Rect> rowRuns;
        static thread_local std::vector<Rect> nextActiveRects;
        activeRects.clear();
        for (uint32_t tileY = 0u; tileY < tileRows; ++tileY)
        {
            rowRuns.clear();
            uint32_t tileX = 0u;
            while (tileX < tileColumns)
            {
                if (dirtyTiles[static_cast<size_t>(tileY) * tileColumns + tileX] == 0u)
                {
                    ++tileX;
                    continue;
                }
                const uint32_t firstTileX = tileX;
                while (tileX < tileColumns &&
                       dirtyTiles[static_cast<size_t>(tileY) * tileColumns + tileX] != 0u)
                    ++tileX;
                const uint32_t x = firstTileX * tileSize;
                const uint32_t y = tileY * tileSize;
                const uint32_t right = std::min(tileX * tileSize, width);
                const uint32_t bottom = std::min(y + tileSize, height);
                rowRuns.push_back({x, y, right - x, bottom - y});
            }

            nextActiveRects.clear();
            for (const Rect &active : activeRects)
            {
                const auto matchingRun = std::find_if(rowRuns.begin(), rowRuns.end(), [&](const Rect &run)
                {
                    return active.x == run.x && active.width == run.width &&
                           active.y + active.height == run.y;
                });
                if (matchingRun == rowRuns.end())
                {
                    outRects.push_back(active);
                    continue;
                }
                Rect extended = active;
                extended.height += matchingRun->height;
                nextActiveRects.push_back(extended);
                rowRuns.erase(matchingRun);
            }
            nextActiveRects.insert(nextActiveRects.end(), rowRuns.begin(), rowRuns.end());
            activeRects.swap(nextActiveRects);
            if (outRects.size() + activeRects.size() > maxRectCount)
            {
                outRects.clear();
                return true;
            }
        }
        outRects.insert(outRects.end(), activeRects.begin(), activeRects.end());
        if (outRects.size() > maxRectCount)
        {
            outRects.clear();
            return true;
        }
        return false;
    }
}
