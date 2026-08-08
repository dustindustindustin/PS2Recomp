#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>
#include <array>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);

private:
    std::array<uint16_t, 2> m_latchedButtons{};
    std::array<uint8_t, 2> m_latchSamples{};
};

#endif
