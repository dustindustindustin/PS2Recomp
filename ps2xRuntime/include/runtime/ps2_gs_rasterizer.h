#ifndef PS2_GS_RASTERIZER_H
#define PS2_GS_RASTERIZER_H

#include <cstdint>
#include <array>

class GS;
struct GSPrimitiveSubmission;
struct GSPrimitiveEffects;

struct GsSamplerStats
{
    uint64_t texturedTriangles = 0u;
    uint64_t eligibleTriangles = 0u;
    uint64_t texturedSamples = 0u;
    uint64_t fastSamples = 0u;
};

// Cumulative, opt-in triangle coverage and pixel-pipeline counters. Each
// histogram is a marginal count of the same covered pixels, not an additional
// pixel total. PSM indices are the six-bit GS format values.
struct GsPixelStateStats
{
    uint64_t coveredPixels = 0u;
    std::array<uint64_t, 64> framePsm{};
    std::array<uint64_t, 64> depthPsm{};
    std::array<uint64_t, 64> texturePsm{};
    std::array<uint64_t, 4> ztst{};
    std::array<uint64_t, 2> zmask{};
    std::array<uint64_t, 2> abe{};
    std::array<uint64_t, 2> alphaTest{};
    std::array<uint64_t, 2> date{};
    std::array<uint64_t, 2> fbmsk{};
    uint64_t untextured = 0u;
    uint64_t alphaRejected = 0u;
    uint64_t alphaDiscarded = 0u;
    uint64_t dateRejected = 0u;
    uint64_t depthRejected = 0u;
    uint64_t framebufferReads = 0u;
    uint64_t depthReads = 0u;
    uint64_t framebufferWrites = 0u;
    uint64_t depthWrites = 0u;
};

class GSRasterizer
{
public:
    void setFastFstPointSamplingEnabled(bool enabled) { m_fastFstPointSamplingEnabled = enabled; }
    void setSamplerStatsEnabled(bool enabled) { m_samplerStatsEnabled = enabled; m_samplerStats = {}; }
    GsSamplerStats samplerStats() const { return m_samplerStats; }
    void setPixelStateStatsEnabled(bool enabled) { m_pixelStateStatsEnabled = enabled; m_pixelStateStats = {}; }
    GsPixelStateStats pixelStateStats() const { return m_pixelStateStats; }
    GSPrimitiveEffects drawPrimitive(GS *gs, const GSPrimitiveSubmission &submission);
    void writePixel(GS *gs, const GSPrimitiveSubmission &submission,
                    int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t sampleTexture(GS *gs, const GSPrimitiveSubmission &submission,
                           float s, float t, float q, uint16_t u, uint16_t v,
                           int linearFilterOverride = -1);
    uint32_t lookupCLUT(GS *gs, const GSPrimitiveSubmission &submission,
                        uint8_t index, uint32_t cbp, uint8_t cpsm, uint8_t csm,
                        uint8_t csa, uint8_t sourcePsm);

private:
    bool m_fastFstPointSamplingEnabled = true;
    bool m_samplerStatsEnabled = false;
    GsSamplerStats m_samplerStats{};
    bool m_pixelStateStatsEnabled = false;
    GsPixelStateStats m_pixelStateStats{};
    void drawSprite(GS *gs, const GSPrimitiveSubmission &submission, GSPrimitiveEffects &effects);
    void drawTriangle(GS *gs, const GSPrimitiveSubmission &submission);
    void drawLine(GS *gs, const GSPrimitiveSubmission &submission);
};

#endif
