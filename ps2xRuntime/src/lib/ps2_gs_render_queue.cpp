#include "runtime/ps2_gs_render_queue.h"
#include "runtime/ps2_gs_gpu.h"

#include <stdexcept>
#include <utility>

namespace
{
constexpr uint32_t kPointJobMagic = 0x314A5047u; // "GPJ1" in little-endian byte order.
constexpr uint32_t kPointJobVersion = 1u;
constexpr uint32_t kPointVramBytes = 4u * 1024u * 1024u;

void writeU32Le(uint8_t *dst, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i)
        dst[i] = static_cast<uint8_t>(value >> (i * 8u));
}

uint32_t readU32Le(const uint8_t *src)
{
    uint32_t value = 0u;
    for (unsigned i = 0u; i < 4u; ++i)
        value |= static_cast<uint32_t>(src[i]) << (i * 8u);
    return value;
}

void writeU64Le(uint8_t *dst, uint64_t value)
{
    for (unsigned i = 0u; i < 8u; ++i)
        dst[i] = static_cast<uint8_t>(value >> (i * 8u));
}

uint64_t readU64Le(const uint8_t *src)
{
    uint64_t value = 0u;
    for (unsigned i = 0u; i < 8u; ++i)
        value |= static_cast<uint64_t>(src[i]) << (i * 8u);
    return value;
}

bool validPointJob(const GsPointJobV1 &job)
{
    return job.operationSequence != 0u && (job.destinationByteOffset & 3u) == 0u &&
           job.destinationByteOffset <= kPointVramBytes - sizeof(uint32_t);
}
}

bool encodeGsPointJobV1(const GsPointJobV1 &job,
                        std::array<uint8_t, GS_POINT_JOB_V1_SIZE> &outBytes)
{
    if (!validPointJob(job))
        return false;
    writeU32Le(outBytes.data() + 0u, kPointJobMagic);
    writeU32Le(outBytes.data() + 4u, kPointJobVersion);
    writeU64Le(outBytes.data() + 8u, job.operationSequence);
    writeU32Le(outBytes.data() + 16u, job.destinationByteOffset);
    writeU32Le(outBytes.data() + 20u, job.rgba32);
    return true;
}

bool executeGsPointJobV1(const uint8_t *bytes, size_t sizeBytes, GsPointPatchV1 &outPatch)
{
    if (!bytes || sizeBytes != GS_POINT_JOB_V1_SIZE ||
        readU32Le(bytes + 0u) != kPointJobMagic ||
        readU32Le(bytes + 4u) != kPointJobVersion)
        return false;

    const GsPointJobV1 job{
        readU64Le(bytes + 8u),
        readU32Le(bytes + 16u),
        readU32Le(bytes + 20u),
    };
    if (!validPointJob(job))
        return false;
    outPatch = {job.operationSequence, job.destinationByteOffset, job.rgba32};
    return true;
}

GsRenderCommandQueue::GsRenderCommandQueue(size_t maxPendingCommands,
                                           size_t maxPendingBytes)
    : m_maxPendingCommands(maxPendingCommands),
      m_maxPendingBytes(maxPendingBytes)
{
    if (m_maxPendingCommands == 0u || m_maxPendingBytes == 0u)
        throw std::invalid_argument("GS render queue capacity must be nonzero");
}

GsRenderCommandQueue::~GsRenderCommandQueue() = default;

uint64_t GsRenderCommandQueue::submit(const uint8_t *payload, size_t sizeBytes)
{
    if (!payload || sizeBytes == 0u)
        throw std::invalid_argument("GS render command payload must not be empty");
    if (sizeBytes > m_maxPendingBytes)
        throw std::length_error("GS render command exceeds queue byte capacity");

    std::unique_lock<std::mutex> lock(m_mutex);
    m_capacityAvailable.wait(lock, [&]
    {
        return m_closed || !m_failure.empty() ||
               (m_outstanding.size() < m_maxPendingCommands &&
                m_pendingBytes <= m_maxPendingBytes - sizeBytes);
    });

    if (!m_failure.empty())
        throw std::runtime_error(m_failure);
    if (m_closed)
        throw std::runtime_error("GS render queue is closed");

    return submitLocked(payload, sizeBytes);
}

bool GsRenderCommandQueue::trySubmit(const uint8_t *payload,
                                     size_t sizeBytes,
                                     uint64_t *outSequence)
{
    if (!payload || sizeBytes == 0u)
        throw std::invalid_argument("GS render command payload must not be empty");
    if (sizeBytes > m_maxPendingBytes)
        throw std::length_error("GS render command exceeds queue byte capacity");

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed || !m_failure.empty() ||
        m_outstanding.size() >= m_maxPendingCommands ||
        m_pendingBytes > m_maxPendingBytes - sizeBytes)
        return false;
    const uint64_t sequence = submitLocked(payload, sizeBytes);
    if (outSequence)
        *outSequence = sequence;
    return true;
}

uint64_t GsRenderCommandQueue::submitLocked(const uint8_t *payload, size_t sizeBytes)
{
    if (m_lastSubmittedSequence == UINT64_MAX)
        throw std::overflow_error("GS render queue sequence exhausted");

    Command command;
    command.sequence = m_lastSubmittedSequence + 1u;
    command.payload.assign(payload, payload + sizeBytes);
    m_queue.push_back(std::move(command));
    try
    {
        m_outstanding.push_back({m_lastSubmittedSequence + 1u, sizeBytes});
    }
    catch (...)
    {
        m_queue.pop_back();
        throw;
    }
    ++m_lastSubmittedSequence;
    m_pendingBytes += sizeBytes;
    return m_lastSubmittedSequence;
}

bool GsRenderCommandQueue::tryPop(Command &outCommand)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_failure.size() != 0u || m_queue.empty() || m_inFlightSequence != 0u)
        return false;
    outCommand = std::move(m_queue.front());
    m_queue.pop_front();
    m_inFlightSequence = outCommand.sequence;
    return true;
}

void GsRenderCommandQueue::complete(uint64_t sequence)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_failure.empty())
            throw std::runtime_error(m_failure);
        if (m_outstanding.empty() || m_inFlightSequence != sequence ||
            m_outstanding.front().sequence != sequence ||
            sequence != m_lastCompletedSequence + 1u)
            throw std::logic_error("GS render completion must acknowledge the in-flight command");

        m_pendingBytes -= m_outstanding.front().sizeBytes;
        m_outstanding.pop_front();
        m_inFlightSequence = 0u;
        m_lastCompletedSequence = sequence;
    }
    m_capacityAvailable.notify_all();
    m_completionChanged.notify_all();
}

void GsRenderCommandQueue::wait(uint64_t sequence)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (sequence > m_lastSubmittedSequence)
        throw std::invalid_argument("GS render fence refers to an unsubmitted sequence");
    m_completionChanged.wait(lock, [&]
    {
        return sequence <= m_lastCompletedSequence || !m_failure.empty();
    });
    if (!m_failure.empty())
        throw std::runtime_error(m_failure);
}

void GsRenderCommandQueue::waitUntilIdle()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const uint64_t target = m_lastSubmittedSequence;
    m_completionChanged.wait(lock, [&]
    {
        return target <= m_lastCompletedSequence || !m_failure.empty();
    });
    if (!m_failure.empty())
        throw std::runtime_error(m_failure);
}

void GsRenderCommandQueue::close()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }
    m_capacityAvailable.notify_all();
    m_completionChanged.notify_all();
}

void GsRenderCommandQueue::fail(const std::string &reason)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_failure.empty())
            m_failure = reason.empty() ? "GS render backend failed" : reason;
        m_closed = true;
        m_queue.clear();
        m_outstanding.clear();
        m_pendingBytes = 0u;
        m_inFlightSequence = 0u;
    }
    m_capacityAvailable.notify_all();
    m_completionChanged.notify_all();
}

bool GsRenderCommandQueue::isClosed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

size_t GsRenderCommandQueue::pendingCommands() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_outstanding.size();
}

size_t GsRenderCommandQueue::pendingBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pendingBytes;
}

uint64_t GsRenderCommandQueue::lastSubmittedSequence() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastSubmittedSequence;
}

uint64_t GsRenderCommandQueue::lastCompletedSequence() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastCompletedSequence;
}

GsRenderPumpStats pumpGsRenderCommands(GS &gs,
                                       GsRenderCommandQueue &queue,
                                       size_t maxCommands)
{
    GsRenderPumpStats stats{};
    GsRenderCommandQueue::Command command;
    while (stats.completed < maxCommands && queue.tryPop(command))
    {
        GsPointPatchV1 patch{};
        if (!executeGsPointJobV1(command.payload.data(), command.payload.size(), patch))
        {
            ++stats.invalid;
        }
        else if (gs.commitCT32PointPatch(patch.operationSequence,
                                         patch.destinationByteOffset,
                                         patch.rgba32))
        {
            ++stats.committed;
        }
        else
        {
            ++stats.stale;
        }

        queue.complete(command.sequence);
        ++stats.completed;
    }
    return stats;
}

bool GsGpuPointQueue::submitAndWait(uint32_t rgba32, uint32_t &outResult,
                                    std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_closed || !m_queue.empty() || m_inFlight || timeout.count() <= 0)
        return false;
    auto request = std::make_shared<Request>();
    request->rgba32 = rgba32;
    request->sequence = m_nextSequence++;
    m_queue.push_back(request);
    const bool signaled = m_changed.wait_for(lock, timeout, [&]
    {
        return request->completed || request->canceled || m_closed;
    });
    if (!signaled || request->canceled || m_closed || !request->succeeded)
    {
        request->canceled = true;
        return false;
    }
    outResult = request->result;
    return true;
}

bool GsGpuPointQueue::tryPop(Ticket &outRequest)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed || m_inFlight)
        return false;
    while (!m_queue.empty())
    {
        Ticket request = std::move(m_queue.front());
        m_queue.pop_front();
        if (request->canceled)
            continue;
        m_inFlight = request;
        outRequest = std::move(request);
        return true;
    }
    return false;
}

void GsGpuPointQueue::complete(const Ticket &request, bool succeeded, uint32_t rgba32)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!request || !m_inFlight || request != m_inFlight)
        throw std::logic_error("GPU point completion does not match the in-flight request");
    if (!request->canceled && !m_closed)
    {
        request->result = rgba32;
        request->succeeded = succeeded;
        request->completed = true;
    }
    m_inFlight.reset();
    m_changed.notify_all();
}

void GsGpuPointQueue::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_closed = true;
    for (const Ticket &request : m_queue)
        request->canceled = true;
    if (m_inFlight)
        m_inFlight->canceled = true;
    m_queue.clear();
    m_changed.notify_all();
}

size_t GsGpuPointQueue::pending() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size() + static_cast<size_t>(m_inFlight != nullptr);
}
