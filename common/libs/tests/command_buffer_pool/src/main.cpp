/*
 * Copyright 2026 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * VulkanCommandBufferPool's nodes recorded on two threads at once, under the validation layer's
 * thread-safety check.
 *
 * Recording into a command buffer is host access to the command pool it was allocated from, and
 * Vulkan requires that access to be externally synchronized. Two threads that each hold a node of
 * one VulkanCommandBufferPool must be able to record at the same time, so no two nodes may share a
 * VkCommandPool. The layer reports a command pool that two threads write at once; any error the
 * layer reports fails the test.
 *
 * Exit codes: 0 pass, 1 fail, 77 skip (no validation layer, or no device with a compute queue;
 * with VVS_REQUIRE_GPU=1 in the environment, or the VVS_REQUIRE_GPU compile definition, a failure).
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "VkCodecUtils/VulkanCommandBufferPool.h"
#include "VkCodecUtils/VulkanDeviceContext.h"

namespace {

constexpr int kExitSkip = 77;
constexpr uint32_t kThreads = 2;
constexpr uint32_t kRecordingsPerThread = 2000;
constexpr uint32_t kCommandsPerRecording = 64;
constexpr uint32_t kPoolNodes = 4;

bool RequireGpu() {
#ifdef VVS_REQUIRE_GPU
    return true;
#else
    const char* value = std::getenv("VVS_REQUIRE_GPU");
    return (value != nullptr) && (std::strcmp(value, "1") == 0);
#endif
}

// The machine lacks what the test needs: a skip, or a failure where a GPU is required.
int NoDevice(const char* what, VkResult result) {
    const bool requireGpu = RequireGpu();
    std::printf("%s: %s (VkResult %d)%s\n", requireGpu ? "FAIL" : "SKIP", what, static_cast<int>(result),
                requireGpu ? ", and VVS_REQUIRE_GPU is set" : "");
    return requireGpu ? 1 : kExitSkip;
}

int Fail(const char* what, VkResult result) {
    std::printf("FAIL: %s (VkResult %d)\n", what, static_cast<int>(result));
    return 1;
}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// One thread's work: take a node, record a command buffer with kCommandsPerRecording barriers,
// give the node back, kRecordingsPerThread times. Nothing is submitted.
void RecordOnNodes(const VulkanDeviceContext* vkDevCtx, VulkanCommandBufferPool* pool, std::atomic<uint32_t>* ready,
                   std::atomic<uint32_t>* failures) {
    ready->fetch_add(1);
    while (ready->load() < kThreads) {
        std::this_thread::yield();
    }

    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    const VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                             VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};

    for (uint32_t recording = 0; recording < kRecordingsPerThread; recording++) {
        VkSharedBaseObj<VulkanCommandBufferPool::PoolNode> node;
        if (!pool->GetAvailablePoolNode(node) || !node) {
            failures->fetch_add(1);
            return;
        }
        node->ResetCommandBuffer(false);
        const VkCommandBuffer cmdBuf = node->BeginCommandBufferRecording(beginInfo);
        if (cmdBuf == VK_NULL_HANDLE) {
            failures->fetch_add(1);
            return;
        }
        for (uint32_t command = 0; command < kCommandsPerRecording; command++) {
            vkDevCtx->CmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                         &barrier, 0, nullptr, 0, nullptr);
        }
        if (node->EndCommandBufferRecording(cmdBuf) != VK_SUCCESS) {
            failures->fetch_add(1);
            return;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string deviceUuidText;
    for (int i = 1; i < argc; i++) {
        if ((std::strcmp(argv[i], "--deviceUuid") == 0) && (i + 1 < argc)) {
            deviceUuidText = argv[++i];
        } else {
            std::printf("usage: %s [--deviceUuid <uuid>]\n", argv[0]);
            return 1;
        }
    }

    // The oracle. The layer's thread-safety check is on by default; the test does not depend on
    // the default.
    SetEnvironment("VK_KHRONOS_VALIDATION_THREAD_SAFETY", "true");

    static const char* const instanceLayers[] = {"VK_LAYER_KHRONOS_validation", nullptr};
    static const char* const instanceExtensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, nullptr};
    static const char* const deviceExtensions[] = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                                   VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, nullptr};

    VulkanDeviceContext vkDevCtx;
    vkDevCtx.AddReqInstanceLayers(instanceLayers);
    vkDevCtx.AddReqInstanceExtensions(instanceExtensions);
    vkDevCtx.AddReqDeviceExtensions(deviceExtensions);

    VkResult result = vkDevCtx.InitVulkanDevice("CommandBufferPoolTwoThreads", VK_NULL_HANDLE, false);
    if (result != VK_SUCCESS) {
        return NoDevice("no Vulkan instance with VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils", result);
    }
    result = vkDevCtx.InitDebugReport(true, false);
    if (result != VK_SUCCESS) {
        return Fail("the validation messenger could not be created, so nothing would be checked", result);
    }

    vk::DeviceUuidUtils deviceUuid;
    if (!deviceUuidText.empty() && (deviceUuid.StringToUUID(deviceUuidText.c_str()) == 0)) {
        std::printf("FAIL: cannot parse --deviceUuid '%s'\n", deviceUuidText.c_str());
        return 1;
    }
    result = vkDevCtx.InitPhysicalDevice(-1, deviceUuid, VK_QUEUE_COMPUTE_BIT, nullptr, 0, VK_VIDEO_CODEC_OPERATION_NONE_KHR, 0,
                                         VK_VIDEO_CODEC_OPERATION_NONE_KHR);
    if (result != VK_SUCCESS) {
        return NoDevice("no device with a compute queue", result);
    }
    result = vkDevCtx.CreateVulkanDevice(0, 0, VK_VIDEO_CODEC_OPERATION_NONE_KHR, false, false, false, true);
    if (result != VK_SUCCESS) {
        return Fail("CreateVulkanDevice", result);
    }

    VkSharedBaseObj<VulkanCommandBufferPool> pool;
    result = VulkanCommandBufferPool::Create(&vkDevCtx, pool);
    if (result != VK_SUCCESS) {
        return Fail("VulkanCommandBufferPool::Create", result);
    }
    result = pool->Configure(&vkDevCtx, kPoolNodes, static_cast<uint32_t>(vkDevCtx.GetComputeQueueFamilyIdx()));
    if (result != VK_SUCCESS) {
        return Fail("VulkanCommandBufferPool::Configure", result);
    }

    VulkanDeviceContext::ResetValidationErrorCount();
    std::atomic<uint32_t> ready{0};
    std::atomic<uint32_t> failures{0};
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < kThreads; t++) {
        threads.emplace_back(RecordOnNodes, &vkDevCtx, pool.get(), &ready, &failures);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    const uint32_t errors = VulkanDeviceContext::GetValidationErrorCount();
    std::printf("%u threads, %u recordings each of %u commands, %u pool nodes: %u recording failure(s), %u validation error(s)\n",
                kThreads, kRecordingsPerThread, kCommandsPerRecording, kPoolNodes, failures.load(), errors);
    pool = nullptr;

    if ((failures.load() != 0) || (errors != 0)) {
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
