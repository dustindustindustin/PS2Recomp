#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>

int main()
{
    auto memory = std::make_unique<PS2Memory>();
    auto gs = std::make_unique<GS>();
    auto vu = std::make_unique<VU1Interpreter>();
    if (!memory->initialize()) return 1;
    memory->bindGS(*gs);
    // Repeated independent ADD.xyzw instructions with immediate lower words.
    // Every block ends with E + its delay slot so all pending writes drain.
    for (unsigned pair = 0; pair < 2048; ++pair)
    {
        const uint32_t lower = 0;
        uint32_t upper = 0x80000000u | (15u << 21) | (2u << 16) |
                         (1u << 11) | ((3u + pair % 24u) << 6) | 0x28u;
        if (pair == 2046) upper |= 0x40000000u;
        std::memcpy(memory->getVU1Code() + pair * 8, &lower, 4);
        std::memcpy(memory->getVU1Code() + pair * 8 + 4, &upper, 4);
    }
    memory->markVU1CodeModified();
    for (unsigned lane = 0; lane < 4; ++lane)
    {
        vu->state().vf[1][lane] = float(lane + 1);
        vu->state().vf[2][lane] = float((lane + 1) * 10);
    }
    auto execute = [&] {
        vu->execute(memory->getVU1Code(), PS2_VU1_CODE_SIZE,
                    memory->getVU1Data(), PS2_VU1_DATA_SIZE, *gs, memory.get());
    };
    for (unsigned i = 0; i < 20; ++i) execute();
    const auto start = std::chrono::steady_clock::now();
    const auto pairsBefore = vu->instructionPairCount();
    for (unsigned i = 0; i < 2000; ++i) execute();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    if (vu->unsupportedUpperCount() || vu->unsupportedLowerCount()) return 2;
    for (unsigned reg = 3; reg < 27; ++reg)
        for (unsigned lane = 0; lane < 4; ++lane)
            if (vu->state().vf[reg][lane] != float((lane + 1) * 11)) return 3;
    const auto pairs = vu->instructionPairCount() - pairsBefore;
    if (pairs != 2000ull * 2048ull) return 4;
    std::cout << "seconds=" << seconds << " pairs=" << pairs
              << " pairs_per_second=" << pairs / seconds << '\n';

    // Deterministic output fingerprint for opt-in versus fallback builds.
    // Sweep the regular and special upper opcode families with ordinary and
    // exponent-boundary sources. This is separate from the timed ADD loop.
    uint64_t fingerprint = 1469598103934665603ull;
    auto mix = [&](uint32_t value)
    {
        fingerprint ^= value;
        fingerprint *= 1099511628211ull;
    };
    const auto writePair = [&](unsigned pair, uint32_t upper)
    {
        const uint32_t lower = 0u;
        std::memcpy(memory->getVU1Code() + pair * 8u, &lower, 4);
        std::memcpy(memory->getVU1Code() + pair * 8u + 4u, &upper, 4);
    };
    constexpr std::array<uint32_t, 4> normalVs{0x3F800000u, 0xC0000000u, 0x40400000u, 0x40800000u};
    constexpr std::array<uint32_t, 4> normalVt{0x40000000u, 0xC0400000u, 0x3F000000u, 0x3FC00000u};
    constexpr std::array<uint32_t, 4> normalAcc{0x3E800000u, 0x3F800000u, 0xC0800000u, 0x40000000u};
    constexpr std::array<uint32_t, 4> edgeVs{0x00000001u, 0x80000001u, 0x7F800000u, 0xFF800000u};
    constexpr std::array<uint32_t, 4> edgeVt{0x7FC00000u, 0x00800000u, 0x7F7FFFFFu, 0xFF7FFFFFu};
    constexpr std::array<uint32_t, 4> edgeAcc{0x807FFFFFu, 0x00000000u, 0xFF800000u, 0x7F800001u};
    uint32_t randomState = 0x83517A4Du;
    auto nextRandom = [&]
    {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        return randomState;
    };
    auto sweep = [&](uint32_t upper)
    {
        writePair(0u, upper);
        writePair(1u, 0x400002FFu);
        writePair(2u, 0x000002FFu);
        memory->markVU1CodeModified();
        for (unsigned pattern = 0; pattern < 3u; ++pattern)
        {
            vu->reset();
            for (unsigned lane = 0; lane < 4u; ++lane)
            {
                const uint32_t vsBits = pattern == 0u ? normalVs[lane]
                    : pattern == 1u ? edgeVs[lane] : nextRandom();
                const uint32_t vtBits = pattern == 0u ? normalVt[lane]
                    : pattern == 1u ? edgeVt[lane] : nextRandom();
                const uint32_t accBits = pattern == 0u ? normalAcc[lane]
                    : pattern == 1u ? edgeAcc[lane] : nextRandom();
                vu->state().vf[1][lane] = std::bit_cast<float>(vsBits);
                vu->state().vf[2][lane] = std::bit_cast<float>(vtBits);
                vu->state().acc[lane] = std::bit_cast<float>(accBits);
            }
            vu->state().q = std::bit_cast<float>(pattern == 1u ? 0x7F800000u : 0x40000000u);
            vu->state().i = std::bit_cast<float>(pattern == 1u ? 0x80000001u : 0xBF800000u);
            vu->execute(memory->getVU1Code(), PS2_VU1_CODE_SIZE,
                        memory->getVU1Data(), PS2_VU1_DATA_SIZE, *gs, memory.get(),
                        0u, 0u, 0u, 8u);
            for (unsigned lane = 0; lane < 4u; ++lane)
            {
                mix(std::bit_cast<uint32_t>(vu->state().vf[2][lane]));
                mix(std::bit_cast<uint32_t>(vu->state().vf[3][lane]));
                mix(std::bit_cast<uint32_t>(vu->state().acc[lane]));
            }
            mix(vu->state().mac);
            mix(vu->state().status);
            mix(vu->state().clip);
        }
    };
    for (uint32_t opcode = 0u; opcode <= 0x2Fu; ++opcode)
        sweep((15u << 21) | (2u << 16) | (1u << 11) | (3u << 6) | opcode);
    for (uint32_t special = 0u; special <= 0x30u; ++special)
    {
        if (special == 0x2Bu)
            continue;
        sweep((15u << 21) | (2u << 16) | (1u << 11) |
              ((special & 0x7Cu) << 4) | (special & 3u) | 0x3Cu);
    }
    if (vu->unsupportedUpperCount() || vu->unsupportedLowerCount()) return 5;
    std::cout << "upper_opcode_fingerprint=" << std::hex << fingerprint << std::dec << '\n';
}
