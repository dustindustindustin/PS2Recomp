#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

// SPU2 AutoDMA: 256 signed PCM16 left samples followed by 256 right.
// This state owns a mirror: the audio owner never reads mutable guest RAM.
class Ps2AutoDmaStream {
public:
    static constexpr uint32_t sampleRate = 48000u;
    bool start(uint32_t address, std::span<const uint8_t> ring, uint32_t mode) {
        if (mode != 1u || ring.size() != 0x40000u || address > 0x00ffffffu - ring.size()) return false;
        m_ring.assign(ring.begin(), ring.end());
        m_address = address; m_queued = m_consumed = 0u; m_active = true;
        return true;
    }
    void mirror(std::span<const uint8_t> ring) {
        if (m_active && ring.size() == m_ring.size()) std::copy(ring.begin(), ring.end(), m_ring.begin());
    }
    std::vector<int16_t> render(uint32_t frames) {
        if (!m_active || frames > 0x10000u) return {};
        std::vector<int16_t> pcm(frames * 2u);
        renderInto(pcm);
        return pcm;
    }
    void renderInto(std::span<int16_t> pcm) {
        const uint32_t frames = static_cast<uint32_t>(pcm.size() / 2u);
        if (!m_active) { std::fill(pcm.begin(), pcm.end(), 0); return; }
        for (uint32_t i = 0u; i < frames; ++i) {
            const uint32_t frame = static_cast<uint32_t>((m_queued + i) % 0x10000u);
            const uint32_t offset = (frame / 256u) * 1024u + (frame % 256u) * 2u;
            std::memcpy(&pcm[i * 2u], m_ring.data() + offset, 2u);
            std::memcpy(&pcm[i * 2u + 1u], m_ring.data() + offset + 512u, 2u);
        }
        m_queued += frames;
    }
    void retire(uint32_t frames) { m_consumed = std::min(m_consumed + frames, m_queued); }
    void stop() { m_active = false; m_queued = m_consumed = 0u; }
    uint32_t status() const {
        if (!m_active) return 0u;
        const uint32_t byteOffset = static_cast<uint32_t>((m_consumed / 256u * 1024u) % 0x40000u);
        return ((byteOffset / 0x20000u) << 24u) | ((m_address + byteOffset) & 0x00ffffffu);
    }
    bool active() const { return m_active; }
private:
    std::vector<uint8_t> m_ring;
    uint32_t m_address = 0u;
    uint64_t m_queued = 0u, m_consumed = 0u;
    bool m_active = false;
};
