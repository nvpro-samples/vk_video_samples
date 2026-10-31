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
 * Filters and the device context's locked submitters submitting to the same queues on several
 * threads at once, under the validation layer's thread-safety check.
 *
 * A submit is host access to the queue, which Vulkan requires to be externally synchronized. A
 * VulkanFilter retrieves its queue itself, by family and index, and submits through
 * SubmitCommandBuffer(); VulkanDeviceContext::MultiThreadedQueueSubmit() names its queue by role.
 * Every submit to one VkQueue must take that queue's one lock, whichever way it names the queue.
 * The threads, each with its own command pools:
 *   - two filters on the compute queue, and a submitter naming the compute role;
 *   - a filter on the graphics queue, and a submitter naming the graphics role.
 * Where the device's compute and graphics queues are one VkQueue, all five share it. The layer
 * reports a queue that two threads write at once; any error the layer reports fails the test.
 *
 * Exit codes: 0 pass, 1 fail, 77 skip: no validation layer (the test's oracle, which a machine with a
 * GPU may still lack), or no device with a graphics and a compute queue. VVS_REQUIRE_GPU=1 in the
 * environment, or the VVS_REQUIRE_GPU compile definition, makes the missing device a failure, not the
 * missing layer.
 */

#include <atomic>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "VkCodecUtils/VulkanCommandBufferPool.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VulkanFilter.h"

namespace {

constexpr int kExitSkip = 77;
constexpr uint32_t kSubmitsPerThread = 1000;
constexpr uint32_t kPoolNodes = 4;

bool RequireGpu() {
#ifdef VVS_REQUIRE_GPU
    return true;
#else
    const char* value = std::getenv("VVS_REQUIRE_GPU");
    return (value != nullptr) && (std::strcmp(value, "1") == 0);
#endif
}

// The machine has no validation layer to check with: a skip, GPU or not.
int NoLayer(const char* what, VkResult result) {
    std::printf("SKIP: %s (VkResult %d)\n", what, static_cast<int>(result));
    return kExitSkip;
}

// The machine has no device the test can use: a skip, or a failure where a GPU is required.
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

// A filter that records nothing of its own: the test records its command buffers itself and submits
// them through the filter's SubmitCommandBuffer().
class SubmitOnlyFilter : public VulkanFilter {
   public:
    SubmitOnlyFilter(const VulkanDeviceContext* vkDevCtx, uint32_t queueFamilyIndex, uint32_t queueIndex)
        : VulkanFilter(vkDevCtx, queueFamilyIndex, queueIndex) {}

    VkResult RecordCommandBuffer(VkCommandBuffer, uint32_t, const VkImageResourceView*, const VkVideoPictureResourceInfoKHR*,
                                 const VkImageResourceView*, const VkVideoPictureResourceInfoKHR*, const VkImageResourceView*,
                                 const VkVideoPictureResourceInfoKHR*) override {
        return VK_SUCCESS;
    }
};

// One thread: its own command pool nodes, and the way it submits.
struct Submitter {
    const char* name;
    VulkanCommandBufferPool* pool;
    SubmitOnlyFilter* filter;                         // submits through the filter, or
    VulkanDeviceContext::QueueFamilySubmitType role;  // through MultiThreadedQueueSubmit() for this role
};

// One thread's work: take a node, wait for its previous submit, record a command buffer with one
// barrier, submit it with the node's fence, kSubmitsPerThread times.
void Submit(const VulkanDeviceContext* vkDevCtx, const Submitter* submitter, uint32_t threads, std::atomic<uint32_t>* ready,
            std::atomic<uint32_t>* failures) {
    ready->fetch_add(1);
    while (ready->load() < threads) {
        std::this_thread::yield();
    }

    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    const VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                             VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};

    for (uint32_t submit = 0; submit < kSubmitsPerThread; submit++) {
        VkSharedBaseObj<VulkanCommandBufferPool::PoolNode> node;
        if (!submitter->pool->GetAvailablePoolNode(node) || !node) {
            failures->fetch_add(1);
            return;
        }
        node->ResetCommandBuffer(true, submitter->name);
        const VkCommandBuffer cmdBuf = node->BeginCommandBufferRecording(beginInfo);
        if (cmdBuf == VK_NULL_HANDLE) {
            failures->fetch_add(1);
            return;
        }
        vkDevCtx->CmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                     &barrier, 0, nullptr, 0, nullptr);
        if (node->EndCommandBufferRecording(cmdBuf) != VK_SUCCESS) {
            failures->fetch_add(1);
            return;
        }

        VkResult result;
        if (submitter->filter != nullptr) {
            result = submitter->filter->SubmitCommandBuffer(1, &cmdBuf, 0, nullptr, nullptr, 0, nullptr, nullptr, node->GetFence());
        } else {
            VkCommandBufferSubmitInfoKHR cmdBufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR};
            cmdBufferInfo.commandBuffer = cmdBuf;
            VkSubmitInfo2KHR submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR};
            submitInfo.commandBufferInfoCount = 1;
            submitInfo.pCommandBufferInfos = &cmdBufferInfo;
            result = vkDevCtx->MultiThreadedQueueSubmit(submitter->role, 0, 1, &submitInfo, node->GetFence());
        }
        if (result != VK_SUCCESS) {
            std::printf("%s: submit %u: VkResult %d\n", submitter->name, submit, static_cast<int>(result));
            failures->fetch_add(1);
            return;
        }
        node->SetCommandBufferSubmitted();
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

    VkResult result = vkDevCtx.InitVulkanDevice("FilterSubmitThreads", VK_NULL_HANDLE, false);
    if ((result == VK_ERROR_LAYER_NOT_PRESENT) || (result == VK_ERROR_EXTENSION_NOT_PRESENT)) {
        return NoLayer("no VK_LAYER_KHRONOS_validation with VK_EXT_debug_utils", result);
    }
    if (result != VK_SUCCESS) {
        return NoDevice("no Vulkan instance", result);
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
    result = vkDevCtx.InitPhysicalDevice(-1, deviceUuid, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, nullptr, 0,
                                         VK_VIDEO_CODEC_OPERATION_NONE_KHR, 0, VK_VIDEO_CODEC_OPERATION_NONE_KHR);
    if (result != VK_SUCCESS) {
        return NoDevice("no device with a graphics and a compute queue", result);
    }
    result = vkDevCtx.CreateVulkanDevice(0, 0, VK_VIDEO_CODEC_OPERATION_NONE_KHR, false, true, false, true);
    if (result != VK_SUCCESS) {
        return Fail("CreateVulkanDevice", result);
    }
    const uint32_t computeFamily = static_cast<uint32_t>(vkDevCtx.GetComputeQueueFamilyIdx());
    const uint32_t gfxFamily = static_cast<uint32_t>(vkDevCtx.GetGfxQueueFamilyIdx());

    // Each filter is a command buffer pool of its own; the role submitters get one each.
    VkSharedBaseObj<SubmitOnlyFilter> computeFilters[2] = {std::make_shared<SubmitOnlyFilter>(&vkDevCtx, computeFamily, 0),
                                                           std::make_shared<SubmitOnlyFilter>(&vkDevCtx, computeFamily, 0)};
    VkSharedBaseObj<SubmitOnlyFilter> gfxFilter = std::make_shared<SubmitOnlyFilter>(&vkDevCtx, gfxFamily, 0);
    VkSharedBaseObj<VulkanCommandBufferPool> computePool;
    VkSharedBaseObj<VulkanCommandBufferPool> gfxPool;
    if ((VulkanCommandBufferPool::Create(&vkDevCtx, computePool) != VK_SUCCESS) ||
        (VulkanCommandBufferPool::Create(&vkDevCtx, gfxPool) != VK_SUCCESS)) {
        return Fail("VulkanCommandBufferPool::Create", VK_ERROR_OUT_OF_HOST_MEMORY);
    }
    struct PoolFamily {
        VulkanCommandBufferPool* pool;
        uint32_t family;
    };
    for (const PoolFamily& poolFamily : {PoolFamily{computeFilters[0].get(), computeFamily},
                                         PoolFamily{computeFilters[1].get(), computeFamily}, PoolFamily{gfxFilter.get(), gfxFamily},
                                         PoolFamily{computePool.get(), computeFamily}, PoolFamily{gfxPool.get(), gfxFamily}}) {
        result = poolFamily.pool->Configure(&vkDevCtx, kPoolNodes, poolFamily.family);
        if (result != VK_SUCCESS) {
            return Fail("VulkanCommandBufferPool::Configure", result);
        }
    }

    const Submitter submitters[] = {
        {"compute filter 1", computeFilters[0].get(), computeFilters[0].get(), VulkanDeviceContext::COMPUTE},
        {"compute filter 2", computeFilters[1].get(), computeFilters[1].get(), VulkanDeviceContext::COMPUTE},
        {"compute role", computePool.get(), nullptr, VulkanDeviceContext::COMPUTE},
        {"graphics filter", gfxFilter.get(), gfxFilter.get(), VulkanDeviceContext::GRAPHICS},
        {"graphics role", gfxPool.get(), nullptr, VulkanDeviceContext::GRAPHICS},
    };
    const uint32_t threadCount = static_cast<uint32_t>(sizeof(submitters) / sizeof(submitters[0]));

    VulkanDeviceContext::ResetValidationErrorCount();
    std::atomic<uint32_t> ready{0};
    std::atomic<uint32_t> failures{0};
    std::vector<std::thread> threads;
    for (const Submitter& submitter : submitters) {
        threads.emplace_back(Submit, &vkDevCtx, &submitter, threadCount, &ready, &failures);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    vkDevCtx.MultiThreadedQueueWaitIdle(VulkanDeviceContext::COMPUTE, 0);
    vkDevCtx.MultiThreadedQueueWaitIdle(VulkanDeviceContext::GRAPHICS, 0);

    const uint32_t errors = VulkanDeviceContext::GetValidationErrorCount();
    std::printf(
        "%u threads, %u submits each; compute family %u, graphics family %u, %s: %u submit failure(s), %u validation error(s)\n",
        threadCount, kSubmitsPerThread, computeFamily, gfxFamily,
        (vkDevCtx.GetComputeQueue() == vkDevCtx.GetGfxQueue()) ? "one queue" : "two queues", failures.load(), errors);
    computeFilters[0] = nullptr;
    computeFilters[1] = nullptr;
    gfxFilter = nullptr;
    computePool = nullptr;
    gfxPool = nullptr;

    if ((failures.load() != 0) || (errors != 0)) {
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
