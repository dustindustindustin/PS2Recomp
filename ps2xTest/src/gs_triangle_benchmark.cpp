#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_rasterizer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <tuple>
#include <vector>

namespace
{
constexpr size_t kVramBytes = 4u * 1024u * 1024u;
constexpr uint32_t kTextureBase = 64u;
constexpr int kIterations = 1000;

GSPrimitiveSubmission makeTriangle(uint8_t psm, bool textured)
{
    GSPrimitiveSubmission submission{};
    submission.prim.type = GS_PRIM_TRIANGLE;
    submission.prim.tme = textured;
    submission.prim.fst = true;
    submission.context.frame = {16u, 1u, GS_PSM_CT32, 0u};
    submission.context.scissor = {0u, 63u, 0u, 63u};
    submission.context.zbuf.zmask = true;
    submission.context.test = 0x30000ull;
    submission.context.tex0.tbp0 = kTextureBase;
    submission.context.tex0.tbw = 1u;
    submission.context.tex0.psm = psm;
    submission.context.tex0.tw = 6u;
    submission.context.tex0.th = 6u;
    submission.context.tex0.tcc = 1u;
    submission.context.tex0.tfx = 1u;
    for (uint32_t i = 0; i < 256u; ++i)
    {
        const uint32_t color = 0x80000000u | (i * 0x00010203u);
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
    submission.vertices[0] = vertex(0.0f, 0.0f, 0u, 0u);
    submission.vertices[1] = vertex(64.0f, 0.0f, 1024u, 0u);
    submission.vertices[2] = vertex(0.0f, 64.0f, 0u, 1024u);
    return submission;
}

void seedTexture(GS &gs, uint8_t psm)
{
    for (uint32_t y = 0u; y < 64u; ++y)
        for (uint32_t x = 0u; x < 64u; ++x)
        {
            const uint32_t index = (x * 17u + y * 31u) & 255u;
            gs.writeVramPixel(psm, kTextureBase, 1u, x, y,
                psm == GS_PSM_CT32
                    ? 0x80000000u | (index * 0x00010203u)
                    : index);
        }
}

double run(GS &gs, GSRasterizer &rasterizer,
           const GSPrimitiveSubmission &submission)
{
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i)
        (void)rasterizer.drawPrimitive(&gs, submission);
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

double median(std::vector<double> times)
{
    std::sort(times.begin(), times.end());
    return times[times.size() / 2u];
}
}

int main()
{
    for (const auto [label, psm, textured] : {
             std::tuple{"CT32", static_cast<uint8_t>(GS_PSM_CT32), true},
             std::tuple{"T8", static_cast<uint8_t>(GS_PSM_T8), true},
             std::tuple{"untextured", static_cast<uint8_t>(GS_PSM_CT32), false}})
    {
        std::vector<uint8_t> seed(kVramBytes, 0u);
        GS seedGs;
        seedGs.init(seed.data(), static_cast<uint32_t>(seed.size()), nullptr);
        if (textured)
            seedTexture(seedGs, psm);
        const GSPrimitiveSubmission submission = makeTriangle(psm, textured);
        std::vector<double> referenceTimes, fastTimes;
        for (int trial = 0; trial < 7; ++trial)
        {
            std::vector<uint8_t> referenceVram = seed;
            std::vector<uint8_t> fastVram = seed;
            GS referenceGs, fastGs;
            referenceGs.init(referenceVram.data(), static_cast<uint32_t>(referenceVram.size()), nullptr);
            fastGs.init(fastVram.data(), static_cast<uint32_t>(fastVram.size()), nullptr);
            GSRasterizer reference, fast;
            reference.setFastFstPointSamplingEnabled(false);
            const bool fastFirst = (trial & 1) != 0;
            double referenceMs = 0.0, fastMs = 0.0;
            if (fastFirst)
            {
                fastMs = run(fastGs, fast, submission);
                referenceMs = run(referenceGs, reference, submission);
            }
            else
            {
                referenceMs = run(referenceGs, reference, submission);
                fastMs = run(fastGs, fast, submission);
            }
            if (referenceVram != fastVram)
            {
                std::cerr << label << " output mismatch" << std::endl;
                return 1;
            }
            if (trial != 0)
            {
                referenceTimes.push_back(referenceMs);
                fastTimes.push_back(fastMs);
            }
        }
        const double referenceMs = median(referenceTimes);
        const double fastMs = median(fastTimes);
        std::cout << label << " " << kIterations << " triangles: reference="
                  << referenceMs << " ms fast=" << fastMs
                  << " ms ratio=" << fastMs / referenceMs << std::endl;
    }
}
