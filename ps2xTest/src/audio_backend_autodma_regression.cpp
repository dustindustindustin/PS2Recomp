#include "runtime/ps2_audio.h"
#include "raylib.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <iostream>
#include <functional>
#include <stdexcept>
#include <vector>

namespace {
unsigned updates = 0, unloads = 0, loads = 0;
AudioCallback callback = nullptr;
std::function<void()> onUnload;
std::vector<int16_t> lastPcm;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
}
AudioStream LoadAudioStream(unsigned rate, unsigned bits, unsigned channels) {
    require(rate == 48000 && bits == 16 && channels == 2, "hardware stream format");
    ++loads;
    AudioStream stream{};
    stream.buffer = reinterpret_cast<rAudioBuffer *>(uintptr_t{1});
    return stream;
}
bool IsAudioStreamValid(AudioStream stream) { return stream.buffer != nullptr; }
void UnloadAudioStream(AudioStream) {
    ++unloads;
    if (onUnload) { const auto hook = std::move(onUnload); onUnload = {}; hook(); }
    callback = nullptr;
}
void SetAudioStreamCallback(AudioStream, AudioCallback next) { callback = next; }
void consume(unsigned frames) {
    require(callback != nullptr, "stream callback should be attached by owner");
    lastPcm.assign(static_cast<size_t>(frames) * 2u, 0);
    callback(lastPcm.data(), frames);
    ++updates;
}
void PlayAudioStream(AudioStream) {}
void StopAudioStream(AudioStream) {}
void SetAudioStreamVolume(AudioStream, float) {}
Wave LoadWaveFromMemory(const char *, const unsigned char *, int) { return {}; }
Sound LoadSoundFromWave(Wave) { return {}; }
void UnloadWave(Wave) {}
void UnloadSound(Sound) {}
void SetSoundPitch(Sound, float) {}
void SetSoundVolume(Sound, float) {}
bool IsSoundPlaying(Sound) { return false; }
void StopSound(Sound) {}
void PlaySound(Sound) {}

int main() {
    try {
        PS2AudioBackend backend;
        std::vector<uint8_t> ram(0x2000000u, 0u);
        constexpr uint32_t base = 0xc1800u;
        for (uint32_t block = 0; block < 256; ++block) {
            for (uint32_t i = 0; i < 256; ++i) {
                const int16_t l = 1234, r = -2345;
                std::memcpy(ram.data() + base + block*1024 + i*2, &l, 2);
                std::memcpy(ram.data() + base + block*1024 + 512 + i*2, &r, 2);
            }
        }
        std::array<uint32_t, 16> words{0x400080u, base, 0x40000u, 1u};
        std::array<uint32_t, 4> reply{};
        auto command = [&](uint32_t function) {
            backend.onDuelistsAutoDma(ram.data(), function,
                reinterpret_cast<uint8_t *>(words.data()), sizeof(words),
                reinterpret_cast<uint8_t *>(reply.data()), sizeof(reply));
        };
        command(0x1020);
        require(updates == 0 && loads == 0, "guest start must not call device APIs");
        backend.setAudioReady(true);
        backend.pump();
        require(updates == 0, "device creation must not pretend samples were consumed");
        command(0x5002);
        require(reply[2] == base, "creation must not publish consumed samples");
        consume(4096u); // Device period deliberately exceeds the former 2048 assumption.
        require(lastPcm.front() == 1234 && lastPcm[1] == -2345 &&
                lastPcm[lastPcm.size()-2] == 1234 && lastPcm.back() == -2345,
                "entire oversized device request must receive nonzero stereo PCM");
        command(0x5002);
        require(reply[2] == base + 16384u, "actual callback size advances DMA address");
        consume(0x8000u - 4096u);
        command(0x5002);
        require(reply[2] == (0x01000000u | (base + 0x20000u)), "consumed half updates refill bit");
        words[1] = words[2] = words[3] = 0;
        command(0x1021);
        require(reply[2] == 0, "no-argument stop clears DMA status");
        backend.pump();
        require(unloads == 1, "owner pump must unload stopped stream");
        words[1] = base; words[2] = 0x40000; words[3] = 1;
        command(0x1020);
        backend.pump();
        command(0x5002);
        require(reply[2] == base, "restart resets cursor");
        words[1] = 1u;
        command(0xf006);
        words[1] = 0x7fffu;
        command(0x5003);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        backend.pump();
        command(0x5002);
        require(reply[0] == 0x7fff && reply[2] != 0u, "F006 must suspend fade updates");
        words[1] = 0u;
        command(0xf006);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        backend.pump();
        command(0x5002);
        require(reply[0] == 0u && reply[2] == 0u, "fade-out to zero must stop DMA");
        backend.setAudioReady(false);
        require(unloads == 2, "device shutdown unloads stream before device closes");
        const unsigned beforeRaii = unloads;
        {
            PS2AudioBackend closing;
            words[1] = base; words[2] = 0x40000; words[3] = 1;
            closing.onDuelistsAutoDma(ram.data(), 0x1020,
                reinterpret_cast<uint8_t *>(words.data()), sizeof(words),
                reinterpret_cast<uint8_t *>(reply.data()), sizeof(reply));
            closing.setAudioReady(true);
            closing.pump();
            const auto oldCallback = callback;
            onUnload = [&] {
                PS2AudioBackend other;
                other.onDuelistsAutoDma(ram.data(), 0x1020,
                    reinterpret_cast<uint8_t *>(words.data()), sizeof(words),
                    reinterpret_cast<uint8_t *>(reply.data()), sizeof(reply));
                other.setAudioReady(true);
                other.pump();
                require(callback != oldCallback, "a thunk must remain reserved until old unload completes");
            };
            // Destructor must release active stream without explicit shutdown.
        }
        require(unloads == beforeRaii + 2u, "both active backends must unload on destruction");
        std::cout << "AutoDMA backend output/status/stop/restart passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
