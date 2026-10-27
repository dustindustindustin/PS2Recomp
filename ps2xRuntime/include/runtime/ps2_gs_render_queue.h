#ifndef PS2_GS_RENDER_QUEUE_H
#define PS2_GS_RENDER_QUEUE_H

#include <condition_variable>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// First renderer-boundary wire format. It deliberately represents only one
// already-resolved CT32 point write; it carries no pointers or live GS state.
// operationSequence is the per-GS operation sequence captured by the producer.
struct GsPointJobV1
{
    uint64_t operationSequence = 0u;
    uint32_t destinationByteOffset = 0u;
    uint32_t rgba32 = 0u;
};

struct GsPointPatchV1
{
    uint64_t operationSequence = 0u;
    uint32_t destinationByteOffset = 0u;
    uint32_t rgba32 = 0u;
};

constexpr size_t GS_POINT_JOB_V1_SIZE = 24u;
bool encodeGsPointJobV1(const GsPointJobV1 &job, std::array<uint8_t, GS_POINT_JOB_V1_SIZE> &outBytes);
bool executeGsPointJobV1(const uint8_t *bytes, size_t sizeBytes, GsPointPatchV1 &outPatch);

// A bounded, host-pumped queue for immutable GS renderer submissions.
// Producers may run on the guest/GIF threads; only the host graphics owner
// should call tryPop() and complete(). Payloads are opaque to keep this queue
// independent of a specific graphics API or GS command representation.
class GsRenderCommandQueue
{
public:
    struct Command
    {
        uint64_t sequence = 0u;
        std::vector<uint8_t> payload;
    };

    explicit GsRenderCommandQueue(size_t maxPendingCommands = 256u,
                                  size_t maxPendingBytes = 8u * 1024u * 1024u);
    ~GsRenderCommandQueue();

    GsRenderCommandQueue(const GsRenderCommandQueue &) = delete;
    GsRenderCommandQueue &operator=(const GsRenderCommandQueue &) = delete;

    // Copies payload before returning. Blocks when pending work reaches either
    // capacity limit. Throws if closed, failed, empty, or larger than capacity.
    uint64_t submit(const uint8_t *payload, size_t sizeBytes);

    // Nonblocking submission for producers that must not wait while holding
    // GS state. Returns false when capacity is full or the queue is closed or
    // failed; the caller must keep its already-completed software result.
    bool trySubmit(const uint8_t *payload, size_t sizeBytes, uint64_t *outSequence = nullptr);

    // Nonblocking single-consumer host-loop pump. At most one command may be
    // in flight; it remains pending and consumes capacity until complete().
    bool tryPop(Command &outCommand);

    // Completions must be acknowledged in submission order.
    void complete(uint64_t sequence);

    // Wait for a guest-visible completion fence. Re-throws queue failure.
    void wait(uint64_t sequence);
    void waitUntilIdle();

    // Stop accepting new commands while allowing queued work to drain.
    void close();

    // Report a terminal backend/context failure and wake all producers/waiters.
    // The caller may software-render a command before acknowledging it instead
    // of calling fail(); fail() is for work that cannot be recovered.
    void fail(const std::string &reason);

    bool isClosed() const;
    size_t pendingCommands() const;
    size_t pendingBytes() const;
    uint64_t lastSubmittedSequence() const;
    uint64_t lastCompletedSequence() const;

private:
    struct OutstandingCommand
    {
        uint64_t sequence;
        size_t sizeBytes;
    };

    const size_t m_maxPendingCommands;
    const size_t m_maxPendingBytes;
    mutable std::mutex m_mutex;
    std::condition_variable m_capacityAvailable;
    std::condition_variable m_completionChanged;
    std::deque<Command> m_queue;
    std::deque<OutstandingCommand> m_outstanding;
    size_t m_pendingBytes = 0u;
    uint64_t m_inFlightSequence = 0u;
    uint64_t m_lastSubmittedSequence = 0u;
    uint64_t m_lastCompletedSequence = 0u;
    bool m_closed = false;
    std::string m_failure;

    uint64_t submitLocked(const uint8_t *payload, size_t sizeBytes);
};

class GS;

struct GsRenderPumpStats
{
    size_t completed = 0u;
    size_t committed = 0u;
    size_t stale = 0u;
    size_t invalid = 0u;
};

// Run from the host/render owner only. This prototype applies a result only
// while its per-GS operation sequence is current. Invalid or stale jobs are
// retired because the producer has already completed software rendering.
GsRenderPumpStats pumpGsRenderCommands(GS &gs,
                                      GsRenderCommandQueue &queue,
                                      size_t maxCommands);

// One synchronous GPU point request at a time. The GS producer waits while
// retaining its state lock, so no later GS operation can observe an uncommitted
// point. A timeout, closed queue, or failed graphics operation returns false
// and the caller immediately uses the software rasterizer. The host never
// touches GS or VRAM while servicing this request.
class GsGpuPointQueue
{
public:
    struct Request
    {
        uint32_t rgba32 = 0u;
        uint64_t sequence = 0u;

    private:
        friend class GsGpuPointQueue;
        bool canceled = false;
        bool completed = false;
        bool succeeded = false;
        uint32_t result = 0u;
    };
    using Ticket = std::shared_ptr<Request>;

    bool submitAndWait(uint32_t rgba32, uint32_t &outResult,
                       std::chrono::milliseconds timeout);
    bool tryPop(Ticket &outRequest);
    void complete(const Ticket &request, bool succeeded, uint32_t rgba32);
    void close();
    size_t pending() const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<Ticket> m_queue;
    Ticket m_inFlight;
    uint64_t m_nextSequence = 1u;
    bool m_closed = false;
};

#endif
