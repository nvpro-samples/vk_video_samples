#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <cstdio>
#include <chrono>

#include "VkCodecUtils/VulkanDeviceContext.h"

/**
 * @brief Frame descriptor queued for async writing by the dump thread pool.
 *
 * Contains everything a writer thread needs to:
 * 1. Wait for GPU completion (forward TL semaphore)
 * 2. Read frame pixels from mapped memory
 * 3. Signal consumer-done (backward TL semaphore)
 */
struct VkVideoDumpFrame {
    int32_t     pictureIndex{-1};
    int64_t     displayOrder{-1};
    uint64_t    decodeOrder{0};

    // Image data access
    uint32_t    displayWidth{0};
    uint32_t    displayHeight{0};

    // Forward sync: wait for decode+filter to complete before reading
    VkSemaphore forwardSemaphore{VK_NULL_HANDLE};
    uint64_t    forwardWaitValue{0};

    // Backward sync: signal when dump is done reading
    VkSemaphore releaseSemaphore{VK_NULL_HANDLE};
    uint64_t    releaseSignalValue{0};

    // Fence for legacy compatibility (signaled by filter, waited by slot reuse)
    VkFence     frameCompleteFence{VK_NULL_HANDLE};

    // Query pool for decode status
    VkQueryPool queryPool{VK_NULL_HANDLE};
    int32_t     startQueryId{0};

    // Callback to perform the actual frame write (set by VkVideoFrameToFile)
    // Returns bytes written, or negative on error.
    // The callback is called after GPU sync is confirmed.
    using WriteCallback = std::function<int64_t(const VkVideoDumpFrame& frame)>;
    WriteCallback writeCallback;

    // Callback to release the frame back to the decoder after write
    using ReleaseCallback = std::function<void(const VkVideoDumpFrame& frame)>;
    ReleaseCallback releaseCallback;
};

/**
 * @brief Thread pool for async frame dumping.
 *
 * Frames are queued from the decode/display loop (non-blocking) and written
 * to disk by a pool of worker threads. Each worker:
 * 1. Waits on the forward TL semaphore (GPU decode+filter done)
 * 2. Calls the write callback (reads mapped memory, writes to file)
 * 3. Signals the backward TL semaphore (frame released for slot reuse)
 * 4. Calls the release callback (returns frame to decoder)
 */
class VkVideoDumpPool {
public:
    VkVideoDumpPool() = default;
    ~VkVideoDumpPool() { shutdown(); }

    /**
     * @brief Initialize the dump pool with N writer threads and its own TL semaphore.
     * @param vkIf Vulkan function table
     * @param device Vulkan device
     * @param numThreads Number of writer threads (default 4)
     */
    VkResult init(const VulkanDeviceContext* vkDevCtx, VkDevice device, uint32_t numThreads = 4) {
        if (m_initialized) return VK_SUCCESS;

        m_vkDevCtx = vkDevCtx;
        m_device = device;
        m_shutdown = false;
        m_framesQueued = 0;
        m_framesWritten = 0;

        // Create a dedicated timeline semaphore for the dump consumer
        VkSemaphoreTypeCreateInfo timelineInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                                    nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
        VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timelineInfo };
        VkResult result = m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &semInfo, nullptr, &m_releaseSemaphore);
        if (result != VK_SUCCESS) {
            return result;
        }

        m_threads.reserve(numThreads);
        for (uint32_t i = 0; i < numThreads; i++) {
            m_threads.emplace_back(&VkVideoDumpPool::workerThread, this, i);
        }

        m_initialized = true;
        return VK_SUCCESS;
    }

    /**
     * @brief Get the dump consumer's release semaphore for external consumer registration.
     */
    VkSemaphore getReleaseSemaphore() const { return m_releaseSemaphore; }

    /**
     * @brief Queue a frame for async writing. Returns immediately.
     */
    void queueFrame(VkVideoDumpFrame&& frame) {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_frameQueue.push(std::move(frame));
            m_framesQueued++;
        }
        m_queueCV.notify_one();
    }

    /**
     * @brief Flush all pending writes and wait for completion.
     */
    void flush() {
        // Wait until all queued frames are written
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_flushCV.wait(lock, [this]() {
            return m_frameQueue.empty() && (m_framesWritten >= m_framesQueued);
        });
    }

    /**
     * @brief Shutdown the pool and join all threads.
     */
    void shutdown() {
        if (!m_initialized) return;

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_shutdown = true;
        }
        m_queueCV.notify_all();

        for (auto& t : m_threads) {
            if (t.joinable()) t.join();
        }
        m_threads.clear();

        if (m_releaseSemaphore != VK_NULL_HANDLE) {
            m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_releaseSemaphore, nullptr);
            m_releaseSemaphore = VK_NULL_HANDLE;
        }

        m_initialized = false;
    }

    uint64_t getFramesWritten() const { return m_framesWritten.load(); }
    uint64_t getFramesQueued() const { return m_framesQueued.load(); }
    bool isInitialized() const { return m_initialized; }

private:
    void workerThread(int threadId) {
        while (true) {
            VkVideoDumpFrame frame;

            // Wait for a frame to process
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCV.wait(lock, [this]() {
                    return m_shutdown || !m_frameQueue.empty();
                });

                if (m_shutdown && m_frameQueue.empty()) {
                    return; // Exit thread
                }

                if (m_frameQueue.empty()) continue;

                frame = std::move(m_frameQueue.front());
                m_frameQueue.pop();
            }

            // 1. Wait for GPU completion (forward TL semaphore)
            if (frame.forwardSemaphore != VK_NULL_HANDLE && frame.forwardWaitValue > 0) {
                auto t0 = std::chrono::high_resolution_clock::now();

                VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                                  nullptr, 0, 1,
                                                  &frame.forwardSemaphore,
                                                  &frame.forwardWaitValue };
                VkResult semResult = m_vkDevCtx->WaitSemaphores(*m_vkDevCtx, &waitInfo,
                                                             10ULL * 1000ULL * 1000ULL * 1000ULL /* 10s */);

                auto t1 = std::chrono::high_resolution_clock::now();
                double waitMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

                if (semResult != VK_SUCCESS) {
                    uint64_t currentValue = 0;
                    m_vkDevCtx->GetSemaphoreCounterValue(*m_vkDevCtx, frame.forwardSemaphore, &currentValue);
                    fprintf(stderr, "[DumpPool] T%d: TL semaphore wait FAILED for pic=%d "
                            "(displayOrder=%lld, expected=%llu, current=%llu, result=%d, %.1fms)\n",
                            threadId, frame.pictureIndex,
                            (long long)frame.displayOrder,
                            (unsigned long long)frame.forwardWaitValue,
                            (unsigned long long)currentValue,
                            semResult, waitMs);
                    // Signal release even on failure to prevent deadlock
                    signalRelease(frame);
                    if (frame.releaseCallback) frame.releaseCallback(frame);
                    m_framesWritten++;
                    m_flushCV.notify_all();
                    continue;
                }
            }

            // 2. Wait for our turn to write (enforce display order)
            {
                std::unique_lock<std::mutex> orderLock(m_orderMutex);
                m_orderCV.wait(orderLock, [this, &frame]() {
                    return m_nextWriteOrder == frame.displayOrder;
                });
            }

            // 3. Call the write callback (reads mapped memory, writes to file)
            if (frame.writeCallback) {
                int64_t bytesWritten = frame.writeCallback(frame);
                if (bytesWritten <= 0LL) {
                    printf("frame.writeCallback return invalid value %lld\n", (long long int)bytesWritten);
                }
            }

            // 4. Advance the write order and notify other waiting threads
            {
                std::lock_guard<std::mutex> orderLock(m_orderMutex);
                m_nextWriteOrder = frame.displayOrder + 1;
            }
            m_orderCV.notify_all();

            // 3. Signal consumer-done (backward TL semaphore)
            signalRelease(frame);

            // 4. Release frame back to decoder
            if (frame.releaseCallback) {
                frame.releaseCallback(frame);
            }

            m_framesWritten++;
            m_flushCV.notify_all();
        }
    }

    void signalRelease(const VkVideoDumpFrame& frame) {
        if (frame.releaseSemaphore != VK_NULL_HANDLE && frame.releaseSignalValue > 0) {
            VkSemaphoreSignalInfo signalInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                                  nullptr,
                                                  frame.releaseSemaphore,
                                                  frame.releaseSignalValue };
            m_vkDevCtx->SignalSemaphore(*m_vkDevCtx, &signalInfo);
        }
    }

    const VulkanDeviceContext* m_vkDevCtx{nullptr};
    VkDevice                    m_device{VK_NULL_HANDLE};
    VkSemaphore                 m_releaseSemaphore{VK_NULL_HANDLE};
    bool                        m_initialized{false};
    std::atomic<bool>           m_shutdown{false};
    std::atomic<uint64_t>       m_framesQueued{0};
    std::atomic<uint64_t>       m_framesWritten{0};

    std::vector<std::thread>    m_threads;
    std::queue<VkVideoDumpFrame> m_frameQueue;
    std::mutex                  m_queueMutex;
    std::condition_variable     m_queueCV;
    std::condition_variable     m_flushCV;

    // Ordering: workers wait until their displayOrder matches m_nextWriteOrder
    std::mutex                  m_orderMutex;
    std::condition_variable     m_orderCV;
    int64_t                     m_nextWriteOrder{0};
};
