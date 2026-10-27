#include "runtime/ps2_audio.h"
#include "runtime/ps2_autodma.h"
#include <array>
#include <atomic>
#include <utility>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include "runtime/ps2_memory.h"
#include "ps2_host_backend.h"
#include <cstring>
#include <vector>

namespace
{
    std::vector<uint8_t> buildWavFromPcm(const int16_t *pcm, size_t sampleCount, uint32_t sampleRate)
    {
        const uint32_t dataSize = static_cast<uint32_t>(sampleCount * 2);
        const uint32_t fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(8 + fileSize);

        uint8_t *p = wav.data();
        p[0] = 'R';
        p[1] = 'I';
        p[2] = 'F';
        p[3] = 'F';
        p[4] = static_cast<uint8_t>(fileSize);
        p[5] = static_cast<uint8_t>(fileSize >> 8);
        p[6] = static_cast<uint8_t>(fileSize >> 16);
        p[7] = static_cast<uint8_t>(fileSize >> 24);
        p[8] = 'W';
        p[9] = 'A';
        p[10] = 'V';
        p[11] = 'E';
        p[12] = 'f';
        p[13] = 'm';
        p[14] = 't';
        p[15] = ' ';
        p[16] = 16;
        p[17] = 0;
        p[18] = 0;
        p[19] = 0;
        p[20] = 1;
        p[21] = 0;
        p[22] = 1;
        p[23] = 0;
        p[24] = static_cast<uint8_t>(sampleRate);
        p[25] = static_cast<uint8_t>(sampleRate >> 8);
        p[26] = static_cast<uint8_t>(sampleRate >> 16);
        p[27] = static_cast<uint8_t>(sampleRate >> 24);
        const uint32_t byteRate = sampleRate * 2;
        p[28] = static_cast<uint8_t>(byteRate);
        p[29] = static_cast<uint8_t>(byteRate >> 8);
        p[30] = static_cast<uint8_t>(byteRate >> 16);
        p[31] = static_cast<uint8_t>(byteRate >> 24);
        p[32] = 2;
        p[33] = 0;
        p[34] = 16;
        p[35] = 0;
        p[36] = 'd';
        p[37] = 'a';
        p[38] = 't';
        p[39] = 'a';
        p[40] = static_cast<uint8_t>(dataSize);
        p[41] = static_cast<uint8_t>(dataSize >> 8);
        p[42] = static_cast<uint8_t>(dataSize >> 16);
        p[43] = static_cast<uint8_t>(dataSize >> 24);
        std::memcpy(p + 44, pcm, dataSize);
        return wav;
    }
}

namespace ps2_vag
{
    bool decode(const uint8_t *data, uint32_t sizeBytes,
                std::vector<int16_t> &outPcm, uint32_t &outSampleRate);
}

namespace {
    struct AutoDmaDeviceState {
        std::mutex mutex;
        Ps2AutoDmaStream ring;
        uint64_t renderedFrames = 0u, nonzeroSamples = 0u;
    };
    // Raylib callbacks carry no user data. Each typed thunk owns a shared state
    // slot, never a backend pointer; unload clears it before device teardown.
    constexpr size_t autoDmaSlotCount = 16u;
    std::array<std::atomic<std::shared_ptr<AutoDmaDeviceState>>, autoDmaSlotCount> autoDmaSlots;
    std::array<std::atomic_bool, autoDmaSlotCount> autoDmaSlotReserved{};
    template<size_t Index> void autoDmaCallback(void *buffer, unsigned frames) {
        const auto state = autoDmaSlots[Index].load();
        auto output = std::span<int16_t>(static_cast<int16_t *>(buffer), static_cast<size_t>(frames) * 2u);
        if (!state) { std::fill(output.begin(), output.end(), 0); return; }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->ring.renderInto(output);
        state->ring.retire(frames);
        state->renderedFrames += frames;
        state->nonzeroSamples += std::count_if(output.begin(), output.end(), [](int16_t value) { return value != 0; });
    }
    template<size_t... Index> auto autoDmaCallbacks(std::index_sequence<Index...>) {
        return std::array<AudioCallback, sizeof...(Index)>{autoDmaCallback<Index>...};
    }
    const auto autoDmaCallbackTable = autoDmaCallbacks(std::make_index_sequence<autoDmaSlotCount>{});
}

struct PS2AudioBackend::Impl
{
    struct TrackedSound
    {
        Sound snd;
        uint32_t sampleKey;
    };
    std::vector<TrackedSound> activeSounds;
    std::shared_ptr<AutoDmaDeviceState> device = std::make_shared<AutoDmaDeviceState>();
    size_t callbackSlot = autoDmaSlotCount;
    bool traceAudio = false;
    uint32_t traceEvents = 0u;
    AudioStream stream{};
    bool streamLoaded = false;
    uint64_t generation = 0u, deviceGeneration = 0u;
    uint32_t ringAddress = 0u;
    uint32_t volume = 0u;
    int64_t fadeStep = 0;
    bool fadeSuspended = false;
    std::chrono::steady_clock::time_point fadeClock{};
};

PS2AudioBackend::PS2AudioBackend() : m_impl(std::make_unique<Impl>())
{
    const char *trace = std::getenv("PS2_DUELISTS_AUDIO_TRACE");
    m_impl->traceAudio = trace && trace[0] != '\0' && trace[0] != '0';
}

PS2AudioBackend::~PS2AudioBackend()
{
    if (m_impl) {
        setAudioReady(false);
        stopAll();
    }
}

void PS2AudioBackend::onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes)
{
    if (!rdram || sizeBytes < 48)
        return;

    const uint32_t physAddr = srcAddr & PS2_RAM_MASK;
    if (physAddr + sizeBytes > PS2_RAM_SIZE)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(rdram + physAddr, sizeBytes, pcm, sampleRate))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = std::move(sample);
    m_mostRecentSampleKey = physAddr;
}

void PS2AudioBackend::onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr)
{
    if (!data || sizeBytes < 48)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(data, sizeBytes, pcm, sampleRate))
        return;

    const uint32_t physAddr = keyAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = sample;
    m_mostRecentSampleKey = physAddr;
    m_loadOrderSamples.push_back(std::move(sample));
    m_loadOrderSampleKeys.push_back(physAddr);
    constexpr size_t kMaxLoadOrderSamples = 32;
    if (m_loadOrderSamples.size() > kMaxLoadOrderSamples)
    {
        m_loadOrderSamples.erase(m_loadOrderSamples.begin());
        m_loadOrderSampleKeys.erase(m_loadOrderSampleKeys.begin());
    }
}

namespace
{
    constexpr uint32_t LIBSD_CMD_BLOCK_TRANS_STATUS = 0x8100u;
}

void PS2AudioBackend::onSoundCommand(uint32_t sid, uint32_t rpcNum,
                                     const uint8_t *sendBuf, uint32_t sendSize,
                                     uint8_t *recvBuf, uint32_t recvSize)
{
    if (sid != 0x80000701u)
        return;

    if ((rpcNum & 0xFF00u) == LIBSD_CMD_BLOCK_TRANS_STATUS &&
        sendBuf && sendSize >= 20)
    {
        uint32_t sampleAddr = 0;
        uint32_t voiceIndex = 0xFFFFFFFFu;
        for (int vo = 4; vo >= 0 && voiceIndex == 0xFFFFFFFFu; vo -= 4)
        {
            if (vo < static_cast<int>(sendSize))
            {
                uint32_t v = 0;
                std::memcpy(&v, sendBuf + vo, sizeof(v));
                if (v < 24u)
                    voiceIndex = v;
            }
        }

        constexpr uint32_t kMinPlausibleAddr = 0x1000u;
        for (int off = 12; off <= 24 && sampleAddr == 0; off += 4)
        {
            if (sendSize >= static_cast<uint32_t>(off + 4))
            {
                uint32_t cand = 0;
                std::memcpy(&cand, sendBuf + off, sizeof(cand));
                if (cand >= kMinPlausibleAddr && (cand <= PS2_RAM_MASK || (cand & ~PS2_RAM_MASK) == 0))
                    sampleAddr = cand;
            }
        }
        if (sampleAddr == 0)
            sampleAddr = m_mostRecentSampleKey;

        float pitch = 1.0f;
        if (sendSize >= 12)
        {
            uint16_t pitchHalf = 0;
            std::memcpy(&pitchHalf, sendBuf + 8, sizeof(pitchHalf));
            if (pitchHalf != 0)
                pitch = 4096.0f / static_cast<float>(pitchHalf);
        }
        play(sampleAddr, pitch, 1.0f, voiceIndex);
    }
}

void PS2AudioBackend::play(uint32_t sampleAddr, float pitch, float volume, uint32_t voiceIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample *sampleToPlay = nullptr;
    uint32_t sampleKey = 0;

    auto it = m_sampleBank.find(sampleAddr & PS2_RAM_MASK);
    if (it != m_sampleBank.end())
    {
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    else if (voiceIndex != 0xFFFFFFFFu &&
             voiceIndex < m_loadOrderSamples.size() &&
             voiceIndex < m_loadOrderSampleKeys.size())
    {
        sampleToPlay = &m_loadOrderSamples[voiceIndex];
        sampleKey = m_loadOrderSampleKeys[voiceIndex];
    }
    else
    {
        it = m_sampleBank.find(m_mostRecentSampleKey);
        if (it == m_sampleBank.end())
            return;
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    if (!sampleToPlay || sampleToPlay->pcm.empty())
        return;

    const bool isBgm = (sampleToPlay->pcm.size() > static_cast<size_t>(sampleToPlay->sampleRate * 5));
    playDecodedSample(sampleKey, *sampleToPlay, pitch, volume, isBgm);
}

void PS2AudioBackend::pruneFinishedSounds()
{
#if defined(PLATFORM_VITA)
    return;
#else
    auto &sounds = m_impl->activeSounds;
    auto it = sounds.begin();
    while (it != sounds.end())
    {
        if (!IsSoundPlaying(it->snd))
        {
            UnloadSound(it->snd);
            it = sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                                        bool isBgm)
{
#if defined(PLATFORM_VITA)
    (void)sampleKey;
    (void)sample;
    (void)pitch;
    (void)volume;
    (void)isBgm;
    return;
#else
    if (!m_audioReady || sample.pcm.empty())
        return;

    pruneFinishedSounds();

    for (const auto &t : m_impl->activeSounds)
    {
        if (t.sampleKey == sampleKey && IsSoundPlaying(t.snd))
            return;
    }

    auto &sounds = m_impl->activeSounds;
    if (isBgm)
    {
        for (auto it = sounds.begin(); it != sounds.end();)
        {
            if (IsSoundPlaying(it->snd))
            {
                StopSound(it->snd);
                UnloadSound(it->snd);
                it = sounds.erase(it);
            }
            else
                ++it;
        }
    }

    constexpr int kMaxConcurrentSounds = 4;
    while (static_cast<int>(sounds.size()) >= kMaxConcurrentSounds)
    {
        StopSound(sounds.front().snd);
        UnloadSound(sounds.front().snd);
        sounds.erase(sounds.begin());
    }

    std::vector<uint8_t> wav = buildWavFromPcm(sample.pcm.data(), sample.pcm.size(), sample.sampleRate);
    Wave wave = LoadWaveFromMemory(".wav", wav.data(), static_cast<int>(wav.size()));
    if (wave.frameCount <= 0)
        return;
    Sound snd = LoadSoundFromWave(wave);
    UnloadWave(wave);
    SetSoundPitch(snd, pitch);
    SetSoundVolume(snd, volume);
    m_impl->activeSounds.push_back({snd, sampleKey});
    PlaySound(snd);
#endif
}

void PS2AudioBackend::stop(uint32_t voiceId)
{
    (void)voiceId;
}

void PS2AudioBackend::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    for (auto &t : m_impl->activeSounds)
    {
        StopSound(t.snd);
        UnloadSound(t.snd);
    }
    m_impl->activeSounds.clear();
#endif
}

void PS2AudioBackend::setAudioReady(bool ready)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!ready && m_impl->streamLoaded) {
        autoDmaSlots[m_impl->callbackSlot].store({});
        StopAudioStream(m_impl->stream);
        UnloadAudioStream(m_impl->stream);
        autoDmaSlotReserved[m_impl->callbackSlot].store(false);
        m_impl->streamLoaded = false;
        m_impl->callbackSlot = autoDmaSlotCount;
    }
    m_audioReady = ready;
}

void PS2AudioBackend::onDuelistsAutoDma(const uint8_t *rdram, uint32_t function,
                                       const uint8_t *send, uint32_t sendSize,
                                       uint8_t *receive, uint32_t receiveSize)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto &state = *m_impl;
    if (function == 0xffffu) {
        state.device = std::make_shared<AutoDmaDeviceState>();
        state.volume = 0u;
        state.fadeStep = 0;
        state.fadeSuspended = false;
        ++state.generation;
        return;
    }
    if (!send || sendSize != 0x40u || !receive || receiveSize != 0x10u) return;
    std::array<uint32_t, 16> words{};
    std::memcpy(words.data(), send, sizeof(words));
    if (function == 0x1020u) {
        const uint32_t address = words[1] & PS2_RAM_MASK;
        const auto next = std::make_shared<AutoDmaDeviceState>();
        if (!rdram || words[2] != 0x40000u || address > PS2_RAM_SIZE - words[2] ||
            !next->ring.start(words[1], {rdram + address, words[2]}, words[3])) return;
        state.device = next;
        if (state.traceAudio && state.traceEvents++ < 32u)
            std::cerr << "[duelists-audio] start ring=0x" << std::hex << words[1]
                      << " bytes=0x" << words[2] << " mode=" << words[3] << std::dec << '\n';
        state.ringAddress = address;
        state.volume = 0x7fffu;
        ++state.generation;
    } else if (function == 0x5003u) {
        const int64_t step = static_cast<int32_t>(words[1]);
        state.fadeStep = step > 0 ? -step : step;
    } else if (function == 0x5004u) {
        const int64_t step = static_cast<int32_t>(words[1]);
        state.fadeStep = step < 0 ? -step : step;
    } else if (function == 0xf006u) {
        state.fadeSuspended = words[1] != 0u;
    }
    std::lock_guard<std::mutex> deviceLock(state.device->mutex);
    if (function == 0x1021u) {
        state.device->ring.stop();
        state.volume = 0u;
        ++state.generation;
    }
    // Mirror on the guest thread. Callback code only reads the owned mirror.
    if (function == 0x5002u && rdram && state.device->ring.active())
        state.device->ring.mirror({rdram + state.ringAddress, 0x40000u});
    const std::array<uint32_t, 4> status{state.volume, 0u, state.device->ring.status(), state.volume};
    std::memcpy(receive, status.data(), sizeof(status));
}

void PS2AudioBackend::pump()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto &state = *m_impl;
    if (!m_audioReady) return;
    bool active;
    {
        std::lock_guard<std::mutex> deviceLock(state.device->mutex);
        active = state.device->ring.active();
    }
    if (state.streamLoaded && (state.deviceGeneration != state.generation || !active)) {
        autoDmaSlots[state.callbackSlot].store({});
        StopAudioStream(state.stream);
        UnloadAudioStream(state.stream);
        autoDmaSlotReserved[state.callbackSlot].store(false);
        state.streamLoaded = false;
        state.callbackSlot = autoDmaSlotCount;
    }
    if (!active) return;
    if (!state.streamLoaded) {
        size_t slot = 0u;
        for (; slot < autoDmaSlotCount; ++slot) {
            bool empty = false;
            if (autoDmaSlotReserved[slot].compare_exchange_strong(empty, true)) {
                autoDmaSlots[slot].store(state.device);
                break;
            }
        }
        if (slot == autoDmaSlotCount) return;
        state.stream = LoadAudioStream(Ps2AutoDmaStream::sampleRate, 16u, 2u);
        if (!IsAudioStreamValid(state.stream)) {
            autoDmaSlots[slot].store({});
            autoDmaSlotReserved[slot].store(false);
            return;
        }
        state.callbackSlot = slot;
        state.streamLoaded = true;
        state.deviceGeneration = state.generation;
        state.fadeClock = std::chrono::steady_clock::now();
        SetAudioStreamCallback(state.stream, autoDmaCallbackTable[slot]);
        PlayAudioStream(state.stream);
    }
    const auto now = std::chrono::steady_clock::now();
    constexpr auto vsync = std::chrono::nanoseconds(16683333); // NTSC 59.94 Hz.
    const int64_t ticks = (now - state.fadeClock) / vsync;
    state.fadeClock += vsync * ticks;
    if (ticks > 0 && !state.fadeSuspended && state.fadeStep != 0) {
        const int64_t next = static_cast<int64_t>(state.volume) + state.fadeStep * ticks;
        state.volume = static_cast<uint32_t>(std::clamp<int64_t>(next, 0, 0x7fff));
        if (next <= 0 || next >= 0x7fff) state.fadeStep = 0;
        if (!state.volume) {
            std::lock_guard<std::mutex> deviceLock(state.device->mutex);
            state.device->ring.stop();
        }
    }
    SetAudioStreamVolume(state.stream, static_cast<float>(state.volume) / 32767.0f);
    if (state.traceAudio && state.traceEvents < 32u) {
        std::lock_guard<std::mutex> deviceLock(state.device->mutex);
        if (state.device->renderedFrames != 0u) {
            ++state.traceEvents;
            std::cerr << "[duelists-audio] consumed frames=" << state.device->renderedFrames
                      << " nonzero=" << state.device->nonzeroSamples << " status=0x"
                      << std::hex << state.device->ring.status() << std::dec << '\n';
        }
    }
}
