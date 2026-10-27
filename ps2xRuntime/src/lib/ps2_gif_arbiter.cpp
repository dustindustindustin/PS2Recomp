#include "runtime/ps2_gif_arbiter.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

GifArbiter::~GifArbiter()
{
    try
    {
        if (asyncEnabled())
            drain();
    }
    catch (...)
    {
        // Destruction must still stop and join the worker.
    }
    stopWorker();
}

void GifArbiter::setProcessPacketFn(ProcessPacketFn fn)
{
    if (asyncEnabled())
        drain();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_processFn = std::move(fn);
}

void GifArbiter::setAsyncEnabled(bool enabled)
{
    if (enabled)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_asyncEnabled)
            return;
        if (!m_processFn)
            throw std::logic_error("GIF async worker requires a packet processor");
        m_workerStopping = false;
        m_asyncEnabled = true;
        try
        {
            m_worker = std::thread(&GifArbiter::workerMain, this);
        }
        catch (...)
        {
            m_asyncEnabled = false;
            throw;
        }
        return;
    }

    std::exception_ptr failure;
    if (asyncEnabled())
    {
        try
        {
            drain();
        }
        catch (...)
        {
            failure = std::current_exception();
        }
    }
    stopWorker();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_asyncEnabled = false;
    }
    m_queueSpaceAvailable.notify_all();
    if (failure)
        std::rethrow_exception(failure);
}

bool GifArbiter::asyncEnabled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_asyncEnabled;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data,
                        uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16u)
        return;

    GifArbiterPacket packet;
    packet.pathId = pathId;
    packet.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    packet.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    packet.data.resize(sizeBytes);
    std::memcpy(packet.data.data(), data, sizeBytes);

    for (;;)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_processFn)
            return;
        if (!m_asyncEnabled)
        {
            m_queue.push_back(std::move(packet));
            return;
        }

        const size_t packetBytes = packet.data.size();
        if (packetBytes > kMaxAsyncQueueBytes)
            throw std::length_error("GIF packet exceeds the bounded async queue capacity");

        if (m_asyncBytes <= kMaxAsyncQueueBytes - packetBytes &&
            m_asyncPackets < kMaxAsyncQueuePackets)
        {
            m_queue.push_back(std::move(packet));
            m_asyncBytes += packetBytes;
            ++m_asyncPackets;
            m_highWaterBytes = std::max(m_highWaterBytes, m_asyncBytes);
            return;
        }

        ++m_backpressureCount;
        lock.unlock();
        flushAsync();
        lock.lock();
        m_queueSpaceAvailable.wait(lock, [&]
        {
            return !m_asyncEnabled ||
                   (m_asyncBytes <= kMaxAsyncQueueBytes - packetBytes &&
                    m_asyncPackets < kMaxAsyncQueuePackets);
        });
        if (!m_asyncEnabled)
        {
            lock.unlock();
            drain();
            std::lock_guard<std::mutex> syncLock(m_mutex);
            m_queue.push_back(std::move(packet));
            return;
        }
    }
}

void GifArbiter::flushAsync()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_asyncEnabled)
    {
        lock.unlock();
        drain();
        return;
    }
    if (m_queue.empty())
        return;

    sortByPriority(m_queue);
    for (auto &packet : m_queue)
    {
        const uint64_t sequence = ++m_nextSequence;
        m_workerQueue.emplace_back(sequence, std::move(packet));
    }
    m_queue.clear();
    lock.unlock();
    m_workAvailable.notify_one();
}

void GifArbiter::drain()
{
    if (asyncEnabled())
    {
        flushAsync();
        std::unique_lock<std::mutex> lock(m_mutex);
        const uint64_t fence = m_nextSequence;
        m_workCompleted.wait(lock, [&] { return m_completedSequence >= fence; });
        const std::exception_ptr failure = std::exchange(m_workerException, nullptr);
        lock.unlock();
        if (failure)
            std::rethrow_exception(failure);
        return;
    }

    std::vector<GifArbiterPacket> packets;
    ProcessPacketFn processFn;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_processFn)
            return;
        packets.swap(m_queue);
        processFn = m_processFn;
    }

    sortByPriority(packets);
    for (auto &packet : packets)
    {
        if (!packet.data.empty())
            processFn(packet.data.data(), static_cast<uint32_t>(packet.data.size()));
    }
}

bool GifArbiter::empty() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.empty() && m_workerQueue.empty() && !m_workerActive;
}

uint64_t GifArbiter::backpressureCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_backpressureCount;
}

size_t GifArbiter::highWaterBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_highWaterBytes;
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::sortByPriority(std::vector<GifArbiterPacket> &packets)
{
    std::stable_sort(packets.begin(), packets.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });
}

void GifArbiter::workerMain()
{
    for (;;)
    {
        std::deque<std::pair<uint64_t, GifArbiterPacket>> batch;
        ProcessPacketFn processFn;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workAvailable.wait(lock, [&]
            {
                return m_workerStopping || !m_workerQueue.empty();
            });
            if (m_workerStopping && m_workerQueue.empty())
                return;
            // Drain packets that are already queued as a batch. Processing stays
            // FIFO, while completion/accounting locks and wakeups are amortized
            // across a burst instead of being paid once per packet.
            batch.swap(m_workerQueue);
            m_workerActive = true;
            processFn = m_processFn;
        }

        std::exception_ptr batchException;
        size_t batchBytes = 0u;
        for (auto &item : batch)
        {
            batchBytes += item.second.data.size();
            try
            {
                if (processFn && !item.second.data.empty())
                    processFn(item.second.data.data(), static_cast<uint32_t>(item.second.data.size()));
            }
            catch (...)
            {
                if (!batchException)
                    batchException = std::current_exception();
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_asyncBytes -= batchBytes;
            m_asyncPackets -= batch.size();
            m_completedSequence = batch.back().first;
            if (!m_workerException && batchException)
                m_workerException = batchException;
            m_workerActive = false;
        }
        m_workCompleted.notify_all();
        m_queueSpaceAvailable.notify_all();
    }
}

void GifArbiter::stopWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_worker.joinable())
            return;
        m_workerStopping = true;
    }
    m_workAvailable.notify_all();
    m_worker.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_workerStopping = false;
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
