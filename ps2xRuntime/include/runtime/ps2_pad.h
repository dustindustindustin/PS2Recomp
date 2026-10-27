#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>
#include <array>
#include <atomic>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size,
                   uint64_t presentationGeneration = UINT64_MAX);
    // Host movie controls cross threads through these atomics only. Guest
    // polling owns the ordinary input latch and the release gate.
    void setStartupInputCapture(bool active);
    bool consumeStartupStartPress();

private:
    std::array<uint16_t, 2> m_latchedButtons{};
    std::array<uint8_t, 2> m_latchSamples{};
    std::array<uint64_t, 2> m_latchReleaseGeneration{};
    std::array<bool, 2> m_startupReleaseGate{};
    std::atomic<bool> m_startupInputCapture{false};
    std::atomic<bool> m_startupStartPress{false};
};

#endif
