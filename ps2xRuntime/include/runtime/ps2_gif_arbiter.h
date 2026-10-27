#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <cstdint>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

enum class GifPathId : uint8_t
{
    Path1 = 1,
    Path2 = 2,
    Path3 = 3,
};

struct GifArbiterPacket
{
    GifPathId pathId;
    bool path2DirectHl = false;
    bool path3Image = false;
    std::vector<uint8_t> data;
};

class GifArbiter
{
public:
    using ProcessPacketFn = std::function<void(const uint8_t *, uint32_t)>;

    GifArbiter() = default;
    explicit GifArbiter(ProcessPacketFn processFn);
    ~GifArbiter();

    GifArbiter(const GifArbiter &) = delete;
    GifArbiter &operator=(const GifArbiter &) = delete;

    void setProcessPacketFn(ProcessPacketFn fn);
    void setAsyncEnabled(bool enabled);
    bool asyncEnabled() const;

    void submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl = false);

    // Sort and dispatch the staged batch without waiting for processing.
    // drain() remains a completion fence for guest-visible GS dependencies.
    void flushAsync();
    void drain();
    bool empty() const;
    uint64_t backpressureCount() const;
    size_t highWaterBytes() const;

private:
    static constexpr size_t kMaxAsyncQueueBytes = 8u * 1024u * 1024u;
    static constexpr size_t kMaxAsyncQueuePackets = 256u;

    ProcessPacketFn m_processFn;
    std::vector<GifArbiterPacket> m_queue;
    std::deque<std::pair<uint64_t, GifArbiterPacket>> m_workerQueue;
    mutable std::mutex m_mutex;
    std::condition_variable m_workAvailable;
    std::condition_variable m_workCompleted;
    std::condition_variable m_queueSpaceAvailable;
    std::thread m_worker;
    bool m_asyncEnabled = false;
    bool m_workerStopping = false;
    bool m_workerActive = false;
    size_t m_asyncBytes = 0u;
    size_t m_asyncPackets = 0u;
    size_t m_highWaterBytes = 0u;
    uint64_t m_nextSequence = 0u;
    uint64_t m_completedSequence = 0u;
    uint64_t m_backpressureCount = 0u;
    std::exception_ptr m_workerException;

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);
    static void sortByPriority(std::vector<GifArbiterPacket> &packets);
    void workerMain();
    void stopWorker();
};

#endif
