#include "MiniTest.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_render_queue.h"

#include <array>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

void register_ps2_gs_render_queue_tests()
{
    MiniTest::Case("PS2GSRenderQueue", [](TestCase &tc)
    {
        tc.Run("GPU point handoff returns readback before guest continues and safely falls back", [](TestCase &t)
        {
            GsGpuPointQueue queue;
            auto guest = std::async(std::launch::async, [&]
            {
                uint32_t result = 0u;
                const bool completed = queue.submitAndWait(0xFF123456u, result, 1s);
                return std::pair<bool, uint32_t>{completed, result};
            });
            GsGpuPointQueue::Ticket request;
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            while (!queue.tryPop(request) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            t.IsTrue(request != nullptr, "host graphics owner should claim the guest point");
            if (request)
            {
                t.Equals(request->rgba32, 0xFF123456u,
                         "GPU input should be the immutable source color");
                t.Equals(guest.wait_for(10ms), std::future_status::timeout,
                         "guest must wait until graphics readback completes");
                queue.complete(request, true, 0xFF123456u);
            }
            t.Equals(guest.wait_for(1s), std::future_status::ready,
                     "completing the GPU request should wake the guest");
            const auto returned = guest.get();
            t.IsTrue(returned.first, "a successful graphics readback should be accepted");
            t.Equals(returned.second, 0xFF123456u,
                     "the GPU value should reach the guest without mutation");

            uint32_t ignored = 0u;
            t.IsFalse(queue.submitAndWait(0xFF010203u, ignored, 1ms),
                      "without a host pump, timeout must trigger software fallback");
            t.IsFalse(queue.tryPop(request),
                      "a timed-out request must never be applied later");
            queue.close();
            t.IsFalse(queue.submitAndWait(0xFF000000u, ignored, 1ms),
                      "shutdown must reject and fall back without waiting");

            GsGpuPointQueue shuttingDown;
            auto blockedGuest = std::async(std::launch::async, [&]
            {
                uint32_t readback = 0u;
                return shuttingDown.submitAndWait(0xFFFFFFFFu, readback, 1s);
            });
            const auto pendingDeadline = std::chrono::steady_clock::now() + 1s;
            while (shuttingDown.pending() == 0u &&
                   std::chrono::steady_clock::now() < pendingDeadline)
                std::this_thread::yield();
            t.Equals(shuttingDown.pending(), static_cast<size_t>(1u),
                     "shutdown fixture should have a guest awaiting GPU completion");
            shuttingDown.close();
            t.Equals(blockedGuest.wait_for(100ms), std::future_status::ready,
                     "closing graphics must release the blocked guest immediately");
            t.IsFalse(blockedGuest.get(),
                      "the released guest should choose software fallback");
        });

        tc.Run("opt-in GPU point result commits through GS before later operations", [](TestCase &t)
        {
            std::vector<uint8_t> vram(4u * 1024u * 1024u, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
            gs.writeRegister(GS_REG_FRAME_1, 1ull << 16u);
            gs.writeRegister(GS_REG_ZBUF_1, 1ull << 32u);
            gs.writeRegister(GS_REG_SCISSOR_1, (3ull << 16u) | (3ull << 48u));
            gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
            gs.writeRegister(GS_REG_PRMODECONT, 1ull);
            gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_POINT));
            int gpuRequests = 0;
            bool succeed = true;
            bool corruptReadback = false;
            gs.setGpuPointExecutor([&](uint32_t input, uint32_t &output)
            {
                ++gpuRequests;
                output = corruptReadback ? input ^ 1u : input;
                return succeed;
            });
            gs.writeRegister(GS_REG_RGBAQ, 0xFF123456ull);
            gs.writeRegister(GS_REG_XYZ2, 0ull);
            t.Equals(gpuRequests, 1, "eligible opaque point should use GPU readback");
            t.Equals(gs.readVramPixel(GS_PSM_CT32, 0u, 1u, 0u, 0u), 0xFF123456u,
                     "GPU result should be visible in CPU VRAM at draw completion");

            succeed = false;
            gs.writeRegister(GS_REG_RGBAQ, 0xFFABCDEFu);
            gs.writeRegister(GS_REG_XYZ2, 1ull << 4u);
            t.Equals(gpuRequests, 2, "failed GPU operation should be attempted once");
            t.Equals(gs.readVramPixel(GS_PSM_CT32, 0u, 1u, 1u, 0u), 0xFFABCDEFu,
                     "failed readback must rerun the point in software");

            succeed = true;
            corruptReadback = true;
            gs.writeRegister(GS_REG_RGBAQ, 0xFF765432u);
            gs.writeRegister(GS_REG_XYZ2, 3ull << 4u);
            t.Equals(gpuRequests, 3, "a corrupt readback should reach validation");
            t.Equals(gs.readVramPixel(GS_PSM_CT32, 0u, 1u, 3u, 0u), 0xFF765432u,
                     "mismatched GPU output must rerun the point in software");

            gs.writeRegister(GS_REG_RGBAQ, 0x80112233u);
            gs.writeRegister(GS_REG_XYZ2, 2ull << 4u);
            t.Equals(gpuRequests, 3, "unsupported source alpha must stay on software");
            t.Equals(gs.readVramPixel(GS_PSM_CT32, 0u, 1u, 2u, 0u), 0x80112233u,
                     "unsupported state must preserve the software result");
            gs.setGpuPointExecutor({});
        });

        tc.Run("queue copies payloads and preserves FIFO sequence fences", [](TestCase &t)
        {
            GsRenderCommandQueue queue(4u, 32u);
            std::array<uint8_t, 3> first{{1u, 2u, 3u}};
            std::array<uint8_t, 2> second{{4u, 5u}};
            const uint64_t firstSequence = queue.submit(first.data(), first.size());
            first.fill(0u);
            const uint64_t secondSequence = queue.submit(second.data(), second.size());

            GsRenderCommandQueue::Command command;
            t.IsTrue(queue.tryPop(command), "the first submitted command should be available to the host pump");
            t.Equals(command.sequence, firstSequence, "the host pump should receive the first sequence first");
            t.Equals(command.payload, std::vector<uint8_t>({1u, 2u, 3u}),
                     "submission should own an immutable copy of the caller payload");
            queue.complete(command.sequence);

            t.IsTrue(queue.tryPop(command), "the second submitted command should follow the first");
            t.Equals(command.sequence, secondSequence, "the second command should have the next sequence");
            t.Equals(command.payload, std::vector<uint8_t>({4u, 5u}),
                     "the second command payload should be preserved");
            queue.complete(command.sequence);
            queue.wait(secondSequence);
            t.Equals(queue.pendingCommands(), static_cast<size_t>(0u),
                     "completed commands should release queue capacity");
        });

        tc.Run("queue capacity includes host-popped commands until their fence completes", [](TestCase &t)
        {
            GsRenderCommandQueue queue(1u, 8u);
            const std::array<uint8_t, 4> payload{{9u, 8u, 7u, 6u}};
            const uint64_t first = queue.submit(payload.data(), payload.size());
            GsRenderCommandQueue::Command command;
            t.IsTrue(queue.tryPop(command), "the host should be able to pop the first command");

            auto producer = std::async(std::launch::async, [&]
            {
                return queue.submit(payload.data(), payload.size());
            });
            t.Equals(producer.wait_for(30ms), std::future_status::timeout,
                     "a popped but incomplete command should continue to apply backpressure");
            queue.complete(first);
            t.Equals(producer.wait_for(1s), std::future_status::ready,
                     "completing the fence should wake a producer waiting for capacity");
            const uint64_t second = producer.get();
            t.Equals(second, first + 1u, "the unblocked submission should receive the next sequence");

            t.IsTrue(queue.tryPop(command), "the producer's command should now be pumpable");
            queue.complete(command.sequence);
            queue.waitUntilIdle();
        });

        tc.Run("nonblocking submit leaves software fallback available when the queue is full", [](TestCase &t)
        {
            GsRenderCommandQueue queue(1u, GS_POINT_JOB_V1_SIZE);
            std::array<uint8_t, GS_POINT_JOB_V1_SIZE> payload{};
            t.IsTrue(encodeGsPointJobV1({1u, 0x100u, 0x11223344u}, payload),
                     "the seed command should be a valid bounded point job");
            const uint64_t accepted = queue.submit(payload.data(), payload.size());
            uint64_t rejectedSequence = 0u;
            t.IsFalse(queue.trySubmit(payload.data(), payload.size(), &rejectedSequence),
                      "a GS producer must not block when the host pump has exhausted capacity");
            t.Equals(rejectedSequence, 0ull,
                     "a rejected nonblocking submission must not allocate a sequence number");
            t.Equals(queue.pendingCommands(), static_cast<size_t>(1u),
                     "the accepted command must remain intact for the owner pump");

            GsRenderCommandQueue::Command command;
            t.IsTrue(queue.tryPop(command), "the host should still be able to pump accepted work");
            queue.complete(command.sequence);
            queue.wait(accepted);
        });

        tc.Run("closing wakes blocked producers and lets accepted work drain", [](TestCase &t)
        {
            GsRenderCommandQueue queue(1u, 8u);
            const std::array<uint8_t, 1> payload{{0x42u}};
            const uint64_t accepted = queue.submit(payload.data(), payload.size());
            GsRenderCommandQueue::Command command;
            t.IsTrue(queue.tryPop(command), "accepted command should be available before close");

            auto producer = std::async(std::launch::async, [&]
            {
                try
                {
                    (void)queue.submit(payload.data(), payload.size());
                    return false;
                }
                catch (const std::runtime_error &)
                {
                    return true;
                }
            });
            t.Equals(producer.wait_for(30ms), std::future_status::timeout,
                     "producer should wait while the sole pending slot is occupied");
            queue.close();
            t.Equals(producer.wait_for(1s), std::future_status::ready,
                     "close should wake a capacity-blocked producer");
            t.IsTrue(producer.get(), "submissions after close should fail explicitly");

            queue.complete(accepted);
            queue.waitUntilIdle();
            t.IsTrue(queue.isClosed(), "close should be observable by the owner loop");
        });

        tc.Run("queue allows only one in-flight command and rejects unclaimed completion", [](TestCase &t)
        {
            GsRenderCommandQueue queue(2u, 8u);
            const std::array<uint8_t, 1> payload{{0x10u}};
            const uint64_t first = queue.submit(payload.data(), payload.size());
            const uint64_t second = queue.submit(payload.data(), payload.size());
            GsRenderCommandQueue::Command command;
            t.IsTrue(queue.tryPop(command), "the first command should become in flight");
            t.Equals(command.sequence, first, "the first submitted command should be claimed first");
            t.IsFalse(queue.tryPop(command),
                      "a second consumer pop must wait until the current command has completed");

            bool rejected = false;
            try
            {
                queue.complete(second);
            }
            catch (const std::logic_error &)
            {
                rejected = true;
            }
            t.IsTrue(rejected, "an unclaimed command must not publish a completion fence");
            t.Equals(queue.lastCompletedSequence(), 0ull,
                     "a rejected completion must not advance guest-visible sequence state");

            queue.complete(first);
            t.IsTrue(queue.tryPop(command), "the second command should be claimable after first completion");
            t.Equals(command.sequence, second, "the next claimed command should preserve FIFO order");
            queue.complete(second);
            queue.waitUntilIdle();
        });

        tc.Run("software fallback can complete the original queued command after GPU failure", [](TestCase &t)
        {
            GsRenderCommandQueue queue(2u, 16u);
            const std::array<uint8_t, 4> payload{{0xA1u, 0xB2u, 0xC3u, 0xD4u}};
            const uint64_t sequence = queue.submit(payload.data(), payload.size());
            GsRenderCommandQueue::Command command;
            t.IsTrue(queue.tryPop(command), "the failed GPU operation should remain available to its executor");

            bool softwareFallbackRan = false;
            try
            {
                throw std::runtime_error("simulated graphics device loss");
            }
            catch (const std::runtime_error &)
            {
                // The executor retries this immutable submission using its
                // software backend before publishing guest-visible completion.
                softwareFallbackRan = command.payload ==
                                      std::vector<uint8_t>({0xA1u, 0xB2u, 0xC3u, 0xD4u});
                if (softwareFallbackRan)
                    queue.complete(command.sequence);
                else
                    queue.fail("software fallback could not recover command");
            }

            t.IsTrue(softwareFallbackRan, "fallback should receive the exact command that failed on GPU");
            queue.wait(sequence);
            t.Equals(queue.lastCompletedSequence(), sequence,
                     "guest fence should complete after successful software fallback");
        });

        tc.Run("point job v1 is pointer-free, versioned, and rejects invalid destinations", [](TestCase &t)
        {
            const GsPointJobV1 job{7u, 0x1234u, 0xA1B2C3D4u};
            std::array<uint8_t, GS_POINT_JOB_V1_SIZE> bytes{};
            t.IsTrue(encodeGsPointJobV1(job, bytes), "valid CT32 point jobs should encode");

            GsPointPatchV1 patch{};
            t.IsTrue(executeGsPointJobV1(bytes.data(), bytes.size(), patch),
                     "the versioned byte payload should decode into a patch");
            t.Equals(patch.operationSequence, job.operationSequence, "the operation token should survive encoding");
            t.Equals(patch.destinationByteOffset, job.destinationByteOffset, "the resolved byte offset should survive encoding");
            t.Equals(patch.rgba32, job.rgba32, "the CT32 color should survive encoding");

            auto badVersion = bytes;
            badVersion[4] = 2u;
            t.IsFalse(executeGsPointJobV1(badVersion.data(), badVersion.size(), patch),
                      "unsupported payload versions must be rejected");
            t.IsFalse(encodeGsPointJobV1({8u, 0x1235u, 0u}, bytes),
                      "unaligned CT32 offsets must be rejected");
            t.IsFalse(encodeGsPointJobV1({8u, 4u * 1024u * 1024u, 0u}, bytes),
                      "out-of-range CT32 writes must be rejected");
            t.IsFalse(encodeGsPointJobV1({0u, 0u, 0u}, bytes),
                      "the prototype operation token must be nonzero");
        });

        tc.Run("host owner pump applies current work and retires stale work in order", [](TestCase &t)
        {
            constexpr size_t kVramSize = 4u * 1024u * 1024u;
            constexpr uint32_t kCurrentOffset = 0x180u;
            constexpr uint32_t kStaleOffset = 0x1A0u;
            std::vector<uint8_t> vram(kVramSize, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
            GsRenderCommandQueue queue(4u, 4u * GS_POINT_JOB_V1_SIZE);
            std::array<uint8_t, GS_POINT_JOB_V1_SIZE> payload{};

            const uint64_t currentToken = gs.operationSequence();
            t.IsTrue(encodeGsPointJobV1({currentToken, kCurrentOffset, 0xAABBCCDDu}, payload),
                     "a current point patch should serialize");
            const uint64_t currentFence = queue.submit(payload.data(), payload.size());
            const GsRenderPumpStats current = pumpGsRenderCommands(gs, queue, 1u);
            t.Equals(current.completed, static_cast<size_t>(1u),
                     "the host pump should retire one accepted command within its budget");
            t.Equals(current.committed, static_cast<size_t>(1u),
                     "current work should commit on the GS owner thread");
            queue.wait(currentFence);

            const uint64_t staleToken = gs.operationSequence();
            t.IsTrue(encodeGsPointJobV1({staleToken, kStaleOffset, 0x10203040u}, payload),
                     "the later patch should serialize before another GS operation");
            const uint64_t staleFence = queue.submit(payload.data(), payload.size());
            gs.writeVramPixel(GS_PSM_CT32, 0u, 1u, 3u, 0u, 0x55667788u);
            const GsRenderPumpStats stale = pumpGsRenderCommands(gs, queue, 1u);
            t.Equals(stale.stale, static_cast<size_t>(1u),
                     "work whose GS sequence was overtaken must be dropped as stale");
            queue.wait(staleFence);

            uint32_t currentPixel = 0u;
            uint32_t stalePixel = 0xFFFFFFFFu;
            t.IsTrue(gs.copyVramBytes(kCurrentOffset, &currentPixel, sizeof(currentPixel)),
                     "the committed current pixel should remain readable");
            t.IsTrue(gs.copyVramBytes(kStaleOffset, &stalePixel, sizeof(stalePixel)),
                     "the stale destination should remain readable");
            t.Equals(currentPixel, 0xAABBCCDDu,
                     "the current owner-pump result should reach VRAM before its fence");
            t.Equals(stalePixel, 0u,
                     "stale work must not overwrite VRAM after a newer GS operation");
        });

        tc.Run("opt-in GS producer feeds host pump while CPU draw remains authoritative", [](TestCase &t)
        {
            constexpr size_t kVramSize = 4u * 1024u * 1024u;
            std::vector<uint8_t> vram(kVramSize, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
            GsRenderCommandQueue queue(1u, GS_POINT_JOB_V1_SIZE);
            gs.setOwnerPumpQueue(&queue, true);

            gs.writeRegister(GS_REG_FRAME_1,
                             (1ull << 16) | (static_cast<uint64_t>(GS_PSM_CT32) << 24));
            gs.writeRegister(GS_REG_ZBUF_1, 1ull << 32);
            gs.writeRegister(GS_REG_SCISSOR_1, 0ull);
            gs.writeRegister(GS_REG_XYOFFSET_1, 0ull);
            gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
            gs.writeRegister(GS_REG_PRMODECONT, 1ull);
            gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_POINT));
            gs.writeRegister(GS_REG_RGBAQ, 0xFF123456ull);
            gs.writeRegister(GS_REG_XYZ2, 0ull);
            t.Equals(gs.readVramPixel(GS_PSM_CT32, 0u, 1u, 0u, 0u), 0xFF123456u,
                     "the CPU reference result must already be visible before the host pump runs");
            t.Equals(queue.pendingCommands(), static_cast<size_t>(1u),
                     "an enabled safe point should publish an immutable shadow command");

            const GsRenderPumpStats pump = pumpGsRenderCommands(gs, queue, 8u);
            t.Equals(pump.committed, static_cast<size_t>(1u),
                     "the host pump should accept the still-current CPU result");
            t.Equals(gs.readVramPixel(GS_PSM_CT32, 0u, 1u, 0u, 0u), 0xFF123456u,
                     "the owner-pumped duplicate result must preserve the exact CPU output");
            queue.waitUntilIdle();

            std::array<uint8_t, GS_POINT_JOB_V1_SIZE> filler{};
            t.IsTrue(encodeGsPointJobV1({gs.operationSequence(), 0x300u, 0xDEADBEEFu}, filler),
                     "a second owner command should be valid for the saturation check");
            t.IsTrue(queue.trySubmit(filler.data(), filler.size()),
                     "the test should occupy the sole host-pump slot first");
            gs.writeRegister(GS_REG_RGBAQ, 0xFFABCDEFu);
            gs.writeRegister(GS_REG_SCISSOR_1, 1ull << 16);
            gs.writeRegister(GS_REG_XYZ2, 1ull << 4);
            t.Equals(gs.readVramPixel(GS_PSM_CT32, 0u, 1u, 1u, 0u), 0xFFABCDEFu,
                     "queue saturation must leave the just-rendered CPU point visible");
            const GsRenderPumpStats afterSaturation = pumpGsRenderCommands(gs, queue, 8u);
            t.Equals(afterSaturation.stale, static_cast<size_t>(1u),
                     "the host should retire the older queued result after newer GS work");
            queue.waitUntilIdle();

            queue.fail("simulated host-pump failure");
            gs.writeRegister(GS_REG_RGBAQ, 0xFFFEEDCBu);
            gs.writeRegister(GS_REG_SCISSOR_1, 2ull << 16);
            gs.writeRegister(GS_REG_XYZ2, 2ull << 4);
            t.Equals(gs.readVramPixel(GS_PSM_CT32, 0u, 1u, 2u, 0u), 0xFFFEEDCBu,
                     "a failed shadow queue must leave subsequent CPU rendering functional");
            gs.setOwnerPumpQueue(nullptr, false);
        });

        tc.Run("queued point patch commits through GS owner before its fence completes", [](TestCase &t)
        {
            constexpr size_t kVramSize = 4u * 1024u * 1024u;
            constexpr uint32_t kDestination = 0x248u;
            constexpr uint32_t kNeighbor = 0xDEADBEEFu;
            constexpr uint32_t kColor = 0x80402010u;
            std::vector<uint8_t> vram(kVramSize, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
            t.IsTrue(gs.writeVramBytes(kDestination + 4u, &kNeighbor, sizeof(kNeighbor)),
                     "the sentinel neighbor should seed through synchronized GS access");
            const uint64_t gsOperationSequence = gs.operationSequence();

            GsRenderCommandQueue queue(1u, GS_POINT_JOB_V1_SIZE);
            std::array<uint8_t, GS_POINT_JOB_V1_SIZE> payload{};
            t.IsTrue(encodeGsPointJobV1({gsOperationSequence, kDestination, kColor}, payload),
                     "the exact one-pixel CT32 job should be representable");
            const uint64_t queueSequence = queue.submit(payload.data(), payload.size());

            GsRenderCommandQueue::Command command;
            t.IsTrue(queue.tryPop(command), "the owner pump should claim the immutable point job");
            GsPointPatchV1 patch{};
            t.IsTrue(executeGsPointJobV1(command.payload.data(), command.payload.size(), patch),
                     "the isolated executor should produce a patch from the copied payload");
            t.Equals(patch.operationSequence, gsOperationSequence,
                     "the prototype token should be carried to the owner-side commit");
            t.IsTrue(gs.commitCT32PointPatch(patch.operationSequence,
                                             patch.destinationByteOffset,
                                             patch.rgba32),
                     "only a current GS operation token should apply the returned patch");
            queue.complete(command.sequence);
            queue.wait(queueSequence);

            uint32_t actual = 0u;
            uint32_t neighbor = 0u;
            t.IsTrue(gs.copyVramBytes(kDestination, &actual, sizeof(actual)),
                     "the committed CT32 bytes should be readable through GS ownership");
            t.IsTrue(gs.copyVramBytes(kDestination + 4u, &neighbor, sizeof(neighbor)),
                     "the adjacent sentinel should be readable through GS ownership");
            t.Equals(actual, kColor, "the fence should follow the exact committed pixel result");
            t.Equals(neighbor, kNeighbor, "the one-pixel patch must leave neighboring bytes untouched");

            const uint64_t staleSequence = gs.operationSequence();
            gs.writeVramPixel(GS_PSM_CT32, 0u, 1u, 2u, 0u, 0xABCDEF01u);
            t.IsFalse(gs.commitCT32PointPatch(staleSequence, kDestination, 0x01020304u),
                      "an intervening GS write must invalidate an earlier asynchronous result");
            uint32_t staleTarget = 0u;
            t.IsTrue(gs.copyVramBytes(kDestination, &staleTarget, sizeof(staleTarget)),
                     "the original point should remain readable after stale work is rejected");
            t.Equals(staleTarget, kColor,
                     "rejecting a stale patch must preserve the framebuffer pixel");
            t.IsFalse(gs.writeVramBytes(kVramSize - 2u, &patch.rgba32, sizeof(patch.rgba32)),
                      "GS-owned byte writes must reject a range that crosses the VRAM boundary");
            t.IsFalse(gs.copyVramBytes(kVramSize, &actual, sizeof(actual)),
                      "GS-owned byte reads must reject an out-of-range interval");
        });

        tc.Run("terminal backend failure wakes fences and rejects new submissions", [](TestCase &t)
        {
            GsRenderCommandQueue queue(2u, 8u);
            const std::array<uint8_t, 1> payload{{0x7Fu}};
            const uint64_t sequence = queue.submit(payload.data(), payload.size());
            std::promise<void> waiting;
            auto fence = std::async(std::launch::async, [&]
            {
                waiting.set_value();
                try
                {
                    queue.wait(sequence);
                    return std::string();
                }
                catch (const std::runtime_error &error)
                {
                    return std::string(error.what());
                }
            });
            waiting.get_future().wait();
            queue.fail("graphics context lost");
            t.Equals(fence.wait_for(1s), std::future_status::ready,
                     "backend failure should release a guest thread waiting at a fence");
            t.Equals(fence.get(), std::string("graphics context lost"),
                     "the fence should report the backend failure reason");

            bool rejected = false;
            try
            {
                (void)queue.submit(payload.data(), payload.size());
            }
            catch (const std::runtime_error &error)
            {
                rejected = std::string(error.what()) == "graphics context lost";
            }
            t.IsTrue(rejected, "new work after terminal backend failure should fail with its cause");
            t.Equals(queue.pendingBytes(), static_cast<size_t>(0u),
                     "terminal failure should release bounded queue storage");
        });
    });
}
