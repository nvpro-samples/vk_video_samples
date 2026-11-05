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

// A semaphore shared through a Win32 handle must carry signals in both directions between a queue
// of this device and a second device, on every queue family the samples submit to.
//
// Two VkDevices on the same GPU, each from its own VkInstance: the producer, with graphics,
// compute, transfer, video decode and video encode queues, and the peer, with one compute queue.
// For each producer queue, each Win32 handle type the driver can export and import
// (OPAQUE_WIN32, OPAQUE_WIN32_KMT), and each semaphore type (binary, timeline):
// - export: the producer creates the semaphore and exports it, the peer imports it. A producer
//   submission signals it, and a peer submission that waits on it signals a peer fence. A timeline
//   semaphore is also waited on from the host through the peer's handle.
// - import: the peer creates and exports the semaphore, the producer imports it. A producer
//   submission waits on it and signals a producer fence; then the peer signals it. For a timeline
//   semaphore the producer's wait is submitted before the peer's signal, so the producer queue
//   waits on the GPU. A second producer submission without a wait checks the queue still runs.
// Producer batches: no command buffer, an empty command buffer, on the encode queue a command buffer
// that encodes one H.264 IDR picture, and on the decode queue a command buffer that decodes an
// H.264 IDR picture the encode queue made at start-up. The last two run the encode or decode ASIC next to
// the semaphore operation; in the import direction the decode runs after the producer's GPU wait.
//
// Every wait has a timeout (default 2000 ms). A signal that never arrives means a consumer of the
// handle, such as another API or process, waits forever.
//
// --negative-control leaves out the producer's signals and runs only the timeline cases, so the first
// export check fails: it shows the test detects a signal that never arrives.
//
// Exit codes: 0 every signal arrived; 1 at least one did not; 77 could not run (no device, no
// Win32 semaphore export, no H.264 encode or decode queue). VVS_REQUIRE_GPU=1 in the environment, or the
// VVS_REQUIRE_GPU compile definition, makes "could not run" a failure. After a failure the process
// ends without destroying its objects, because a queue may still be waiting on a signal that will
// never come.

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "VkCodecUtils/VulkanDeviceContext.h"

namespace {

const int kExitPass = 0;
const int kExitFail = 1;
const int kExitCouldNotRun = 77;
const int kExitBadUsage = 64;

struct Options {
    int timeoutMs = 2000;
    int repeats = 3;
    int deviceId = -1;
    bool verbose = false;
    // The producer leaves out its signals, so every export check must fail: shows the test detects
    // a signal that never arrives.
    bool negativeControl = false;
};

// One producer queue under test.
struct QueueUnderTest {
    const char* name;
    VkQueue queue;
    int32_t family;
};

struct HandleTypeUnderTest {
    const char* name;
    VkExternalSemaphoreHandleTypeFlagBits type;
    bool binary;    // exportable and importable as a binary semaphore
    bool timeline;  // exportable and importable as a timeline semaphore
};

// A semaphore created on one device and imported into the other.
struct SharedSemaphore {
    VkSemaphore owner = VK_NULL_HANDLE;     // on the device that created and exported it
    VkSemaphore imported = VK_NULL_HANDLE;  // on the other device
    uint64_t value = 0;                     // the last value signalled (timeline)
};

class Win32SemaphoreInteropTest {
   public:
    explicit Win32SemaphoreInteropTest(const Options& options) : m_options(options) {}

    ~Win32SemaphoreInteropTest();

    // Creates both instances and devices; false means the test cannot run.
    bool Init(std::string& whyNot);

    // Runs every case and returns the exit code.
    int Run();

   private:
    // Creates a semaphore on 'from', exports it as handleType and imports it into 'to'.
    bool CreateShared(VulkanDeviceContext& from, VulkanDeviceContext& to, VkExternalSemaphoreHandleTypeFlagBits handleType,
                      VkSemaphoreType semaphoreType, SharedSemaphore& shared);
    void DestroyShared(VulkanDeviceContext& from, VulkanDeviceContext& to, SharedSemaphore& shared);

    // Submits one batch with at most one wait and one signal semaphore and an optional fence.
    VkResult Submit(VulkanDeviceContext& ctx, VkQueue queue, VkCommandBuffer commandBuffer, VkSemaphore waitSemaphore,
                    uint64_t waitValue, VkSemaphore signalSemaphore, uint64_t signalValue, VkFence fence);

    // Waits for a fence with the timeout; true if it signalled.
    bool WaitFence(VulkanDeviceContext& ctx, VkFence fence);

    // The two directions. Each returns false on a Vulkan error, which makes the test unable to run.
    bool CheckExport(const QueueUnderTest& q, const HandleTypeUnderTest& h, VkSemaphoreType semaphoreType,
                     VkCommandBuffer commandBuffer, const char* batchName, int repeat);
    bool CheckImport(const QueueUnderTest& q, const HandleTypeUnderTest& h, VkSemaphoreType semaphoreType,
                     VkCommandBuffer commandBuffer, const char* batchName, int repeat);

    void Report(bool ok, const char* direction, const QueueUnderTest& q, const HandleTypeUnderTest& h,
                VkSemaphoreType semaphoreType, const char* batchName, int repeat, const char* what);

    bool CreateEncodeResources(std::string& whyNot);
    void DestroyEncodeResources();
    // Records one IDR encode into dstBuffer. With hostReadsBitstream the command buffer ends with a
    // barrier that makes the bitstream available to the host and to later device reads.
    bool RecordEncode(VkCommandBuffer commandBuffer, bool firstUse, VkBuffer dstBuffer, VkDeviceSize dstRange,
                      bool hostReadsBitstream = false);
    int32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const;
    bool BindSessionMemory(VkVideoSessionKHR session, std::vector<VkDeviceMemory>& memory);

    // The decode case: an H.264 IDR picture the encode queue made, decoded on the decode queue.
    bool CreateDecodeResources(std::string& whyNot);
    void DestroyDecodeResources();
    // Encodes one IDR picture into m_decodeBitstream and finds its slice; false if the encoder wrote
    // no H.264 IDR slice there.
    bool EncodeDecodeInput(const QueueUnderTest& encodeQueue, VkCommandBuffer commandBuffer, std::string& whyNot);
    bool RecordDecode(VkCommandBuffer commandBuffer);
    bool CreateImage(const VkImageCreateInfo& info, VkImage& image, VkDeviceMemory& memory, VkImageView& view);

    Options m_options;
    VulkanDeviceContext m_producer;
    VulkanDeviceContext m_peer;
    VkQueue m_peerQueue = VK_NULL_HANDLE;
    VkFence m_producerFence = VK_NULL_HANDLE;
    VkFence m_peerFence = VK_NULL_HANDLE;
    std::vector<HandleTypeUnderTest> m_handleTypes;
    int m_checks = 0;
    int m_failures = 0;

    // The H.264 encode case.
    VkVideoEncodeH264ProfileInfoKHR m_h264Profile{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR m_profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoProfileListInfoKHR m_profileList{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    VkExtent2D m_extent{};
    VkFormat m_pictureFormat = VK_FORMAT_UNDEFINED;
    VkVideoSessionKHR m_session = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> m_sessionMemory;
    VkVideoSessionParametersKHR m_sessionParameters = VK_NULL_HANDLE;
    VkImage m_srcImage = VK_NULL_HANDLE;
    VkDeviceMemory m_srcImageMemory = VK_NULL_HANDLE;
    VkImageView m_srcImageView = VK_NULL_HANDLE;
    VkBuffer m_bitstream = VK_NULL_HANDLE;
    VkDeviceMemory m_bitstreamMemory = VK_NULL_HANDLE;
    VkDeviceSize m_bitstreamSize = 0;
    StdVideoH264SequenceParameterSet m_sps{};
    StdVideoH264PictureParameterSet m_pps{};
    uint32_t m_encodeBitstreamAlignment = 1;

    // The H.264 decode cases. The bitstream buffer is shared by the encode queue, which writes the
    // IDR picture, and the decode queue, which reads it.
    VkVideoDecodeH264ProfileInfoKHR m_decH264Profile{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR m_decProfile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoProfileListInfoKHR m_decProfileList{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    VkVideoProfileInfoKHR m_bothProfiles[2]{};
    VkVideoProfileListInfoKHR m_bothProfileList{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    bool m_dpbIsOutput = true;
    VkVideoSessionKHR m_decSession = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> m_decSessionMemory;
    VkVideoSessionParametersKHR m_decSessionParameters = VK_NULL_HANDLE;
    VkImage m_dpbImage = VK_NULL_HANDLE;
    VkDeviceMemory m_dpbImageMemory = VK_NULL_HANDLE;
    VkImageView m_dpbImageView = VK_NULL_HANDLE;
    VkImage m_dstImage = VK_NULL_HANDLE;
    VkDeviceMemory m_dstImageMemory = VK_NULL_HANDLE;
    VkImageView m_dstImageView = VK_NULL_HANDLE;
    VkBuffer m_decodeBitstream = VK_NULL_HANDLE;
    VkDeviceMemory m_decodeBitstreamMemory = VK_NULL_HANDLE;
    VkDeviceSize m_decodeBitstreamSize = 0;
    VkDeviceSize m_decodeSizeAlignment = 1;
    VkDeviceSize m_decodeRange = 0;
    uint32_t m_sliceOffset = 0;
};

Win32SemaphoreInteropTest::~Win32SemaphoreInteropTest() {
    if (m_producer.getDevice() != VK_NULL_HANDLE) {
        m_producer.DeviceWaitIdle();
        DestroyDecodeResources();
        DestroyEncodeResources();
        if (m_producerFence != VK_NULL_HANDLE) {
            m_producer.DestroyFence(m_producer.getDevice(), m_producerFence, nullptr);
        }
    }
    if (m_peer.getDevice() != VK_NULL_HANDLE) {
        m_peer.DeviceWaitIdle();
        if (m_peerFence != VK_NULL_HANDLE) {
            m_peer.DestroyFence(m_peer.getDevice(), m_peerFence, nullptr);
        }
    }
}

bool Win32SemaphoreInteropTest::Init(std::string& whyNot) {
    static const char* const instanceExtensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                                                     VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, nullptr};
    static const char* const producerExtensions[] = {
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,  VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,  nullptr};
    static const char* const peerExtensions[] = {
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, nullptr};

    m_producer.AddReqInstanceExtensions(instanceExtensions);
    m_producer.AddReqDeviceExtensions(producerExtensions, m_options.verbose);
    if (m_producer.InitVulkanDevice("Win32SemaphoreInteropTest", VK_NULL_HANDLE, m_options.verbose) != VK_SUCCESS) {
        whyNot = "InitVulkanDevice failed (no Vulkan loader or driver?)";
        return false;
    }
    vk::DeviceUuidUtils anyDevice;
    const VkQueueFlags queueTypes = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT |
                                    VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_VIDEO_ENCODE_BIT_KHR;
    if (m_producer.InitPhysicalDevice(m_options.deviceId, anyDevice, queueTypes, nullptr, VK_QUEUE_VIDEO_DECODE_BIT_KHR,
                                      VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, VK_QUEUE_VIDEO_ENCODE_BIT_KHR,
                                      VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) != VK_SUCCESS) {
        whyNot =
            "no physical device with graphics, compute, transfer, video decode and H.264 video encode queues "
            "and Win32 external semaphores";
        return false;
    }

    VkPhysicalDeviceIDProperties idProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idProps};
    m_producer.GetPhysicalDeviceProperties2(m_producer.getPhysicalDevice(), &props2);
    const VkPhysicalDeviceProperties& props = props2.properties;
    std::printf("device: %s, driver version %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.driverVersion),
                VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));

    // The handle types this device can both export and import, per semaphore type.
    HandleTypeUnderTest candidates[] = {
        {"opaque-win32", VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT, false, false},
        {"opaque-kmt", VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, false, false},
    };
    const VkExternalSemaphoreFeatureFlags needed =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    for (HandleTypeUnderTest& h : candidates) {
        for (VkSemaphoreType semaphoreType : {VK_SEMAPHORE_TYPE_BINARY, VK_SEMAPHORE_TYPE_TIMELINE}) {
            VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            typeInfo.semaphoreType = semaphoreType;
            VkPhysicalDeviceExternalSemaphoreInfo semInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &typeInfo};
            semInfo.handleType = h.type;
            VkExternalSemaphoreProperties semProps{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
            m_producer.GetPhysicalDeviceExternalSemaphoreProperties(m_producer.getPhysicalDevice(), &semInfo, &semProps);
            const bool supported = (semProps.externalSemaphoreFeatures & needed) == needed;
            (semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE ? h.timeline : h.binary) = supported;
        }
        std::printf("handle type %s: binary %s, timeline %s\n", h.name, h.binary ? "tested" : "not supported, not tested",
                    h.timeline ? "tested" : "not supported, not tested");
        if (h.binary || h.timeline) {
            m_handleTypes.push_back(h);
        }
    }
    if (m_handleTypes.empty()) {
        whyNot = "the device can neither export nor import a semaphore as a Win32 handle";
        return false;
    }

    if (m_producer.CreateVulkanDevice(
            1, 1, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR | VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, true /* transfer */,
            true /* graphics */, false /* present */, true /* compute */) != VK_SUCCESS) {
        whyNot = "CreateVulkanDevice failed for the producer";
        return false;
    }

    // The peer: its own instance, the same GPU, one compute queue.
    m_peer.AddReqInstanceExtensions(instanceExtensions);
    m_peer.AddReqDeviceExtensions(peerExtensions, m_options.verbose);
    if (m_peer.InitVulkanDevice("Win32SemaphoreInteropPeer", VK_NULL_HANDLE, m_options.verbose) != VK_SUCCESS) {
        whyNot = "InitVulkanDevice failed for the peer";
        return false;
    }
    vk::DeviceUuidUtils sameDevice(idProps.deviceUUID);
    if ((m_peer.InitPhysicalDevice(-1, sameDevice, VK_QUEUE_COMPUTE_BIT, nullptr, 0, VK_VIDEO_CODEC_OPERATION_NONE_KHR, 0,
                                   VK_VIDEO_CODEC_OPERATION_NONE_KHR) != VK_SUCCESS) ||
        (m_peer.CreateVulkanDevice(0, 0, VK_VIDEO_CODEC_OPERATION_NONE_KHR, false, false, false, true /* compute */) !=
         VK_SUCCESS)) {
        whyNot = "cannot create the peer device on the same GPU";
        return false;
    }
    m_peerQueue = m_peer.GetComputeQueue();
    if (m_peerQueue == VK_NULL_HANDLE) {
        whyNot = "the peer device has no compute queue";
        return false;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((m_producer.CreateFence(m_producer.getDevice(), &fenceInfo, nullptr, &m_producerFence) != VK_SUCCESS) ||
        (m_peer.CreateFence(m_peer.getDevice(), &fenceInfo, nullptr, &m_peerFence) != VK_SUCCESS)) {
        whyNot = "vkCreateFence failed";
        return false;
    }
    return true;
}

int32_t Win32SemaphoreInteropTest::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const {
    VkPhysicalDeviceMemoryProperties memProps{};
    m_producer.GetPhysicalDeviceMemoryProperties(m_producer.getPhysicalDevice(), &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (((typeBits & (1u << i)) != 0) && ((memProps.memoryTypes[i].propertyFlags & required) == required)) {
            return (int32_t)i;
        }
    }
    return -1;
}

bool Win32SemaphoreInteropTest::BindSessionMemory(VkVideoSessionKHR session, std::vector<VkDeviceMemory>& memory) {
    VkDevice device = m_producer.getDevice();
    uint32_t reqCount = 0;
    m_producer.GetVideoSessionMemoryRequirementsKHR(device, session, &reqCount, nullptr);
    std::vector<VkVideoSessionMemoryRequirementsKHR> reqs(reqCount, {VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR});
    m_producer.GetVideoSessionMemoryRequirementsKHR(device, session, &reqCount, reqs.data());
    std::vector<VkBindVideoSessionMemoryInfoKHR> binds;
    for (const VkVideoSessionMemoryRequirementsKHR& req : reqs) {
        int32_t type = FindMemoryType(req.memoryRequirements.memoryTypeBits, 0);
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = req.memoryRequirements.size;
        allocInfo.memoryTypeIndex = (uint32_t)type;
        VkDeviceMemory deviceMemory = VK_NULL_HANDLE;
        if ((type < 0) || (m_producer.AllocateMemory(device, &allocInfo, nullptr, &deviceMemory) != VK_SUCCESS)) {
            return false;
        }
        memory.push_back(deviceMemory);
        VkBindVideoSessionMemoryInfoKHR bind{VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR};
        bind.memoryBindIndex = req.memoryBindIndex;
        bind.memory = deviceMemory;
        bind.memorySize = req.memoryRequirements.size;
        binds.push_back(bind);
    }
    return m_producer.BindVideoSessionMemoryKHR(device, session, (uint32_t)binds.size(), binds.data()) == VK_SUCCESS;
}

bool Win32SemaphoreInteropTest::CreateShared(VulkanDeviceContext& from, VulkanDeviceContext& to,
                                             VkExternalSemaphoreHandleTypeFlagBits handleType, VkSemaphoreType semaphoreType,
                                             SharedSemaphore& shared) {
    VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = semaphoreType;
    typeInfo.initialValue = 0;
    VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, &typeInfo};
    exportInfo.handleTypes = handleType;
    VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exportInfo};
    if (from.CreateSemaphore(from.getDevice(), &createInfo, nullptr, &shared.owner) != VK_SUCCESS) {
        return false;
    }

    VkSemaphoreGetWin32HandleInfoKHR getInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    getInfo.semaphore = shared.owner;
    getInfo.handleType = handleType;
    HANDLE handle = nullptr;
    if (from.GetSemaphoreWin32HandleKHR(from.getDevice(), &getInfo, &handle) != VK_SUCCESS) {
        return false;
    }

    // The importing semaphore must have the same type as the exported payload.
    VkSemaphoreTypeCreateInfo importTypeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    importTypeInfo.semaphoreType = semaphoreType;
    VkSemaphoreCreateInfo importCreateInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &importTypeInfo};
    if (to.CreateSemaphore(to.getDevice(), &importCreateInfo, nullptr, &shared.imported) != VK_SUCCESS) {
        if (handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT) {
            CloseHandle(handle);
        }
        return false;
    }
    VkImportSemaphoreWin32HandleInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    importInfo.semaphore = shared.imported;
    importInfo.handleType = handleType;
    importInfo.handle = handle;
    const VkResult result = to.ImportSemaphoreWin32HandleKHR(to.getDevice(), &importInfo);
    // An NT handle (OPAQUE_WIN32) stays owned by the application after the import; a KMT handle is
    // not a reference and is never closed.
    if (handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT) {
        CloseHandle(handle);
    }
    return result == VK_SUCCESS;
}

void Win32SemaphoreInteropTest::DestroyShared(VulkanDeviceContext& from, VulkanDeviceContext& to, SharedSemaphore& shared) {
    if (shared.imported != VK_NULL_HANDLE) {
        to.DestroySemaphore(to.getDevice(), shared.imported, nullptr);
    }
    if (shared.owner != VK_NULL_HANDLE) {
        from.DestroySemaphore(from.getDevice(), shared.owner, nullptr);
    }
    shared = SharedSemaphore();
}

VkResult Win32SemaphoreInteropTest::Submit(VulkanDeviceContext& ctx, VkQueue queue, VkCommandBuffer commandBuffer,
                                           VkSemaphore waitSemaphore, uint64_t waitValue, VkSemaphore signalSemaphore,
                                           uint64_t signalValue, VkFence fence) {
    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = waitSemaphore;
    waitInfo.value = waitValue;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = signalSemaphore;
    signalInfo.value = signalValue;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = commandBuffer;
    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.waitSemaphoreInfoCount = (waitSemaphore != VK_NULL_HANDLE) ? 1 : 0;
    submitInfo.pWaitSemaphoreInfos = &waitInfo;
    submitInfo.commandBufferInfoCount = (commandBuffer != VK_NULL_HANDLE) ? 1 : 0;
    submitInfo.pCommandBufferInfos = &cbInfo;
    submitInfo.signalSemaphoreInfoCount = (signalSemaphore != VK_NULL_HANDLE) ? 1 : 0;
    submitInfo.pSignalSemaphoreInfos = &signalInfo;
    return ctx.QueueSubmit2KHR(queue, 1, &submitInfo, fence);
}

bool Win32SemaphoreInteropTest::WaitFence(VulkanDeviceContext& ctx, VkFence fence) {
    const uint64_t timeoutNs = (uint64_t)m_options.timeoutMs * 1000000ull;
    const bool signalled = ctx.WaitForFences(ctx.getDevice(), 1, &fence, VK_TRUE, timeoutNs) == VK_SUCCESS;
    if (signalled) {
        ctx.ResetFences(ctx.getDevice(), 1, &fence);
    }
    return signalled;
}

void Win32SemaphoreInteropTest::Report(bool ok, const char* direction, const QueueUnderTest& q, const HandleTypeUnderTest& h,
                                       VkSemaphoreType semaphoreType, const char* batchName, int repeat, const char* what) {
    m_checks++;
    const char* type = (semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) ? "timeline" : "binary";
    if (!ok) {
        m_failures++;
        std::printf("  FAIL %-6s %-8s %-12s %-8s %-15s #%d: %s within %d ms\n", direction, q.name, h.name, type, batchName, repeat,
                    what, m_options.timeoutMs);
    } else if (m_options.verbose) {
        std::printf("  ok   %-6s %-8s %-12s %-8s %-15s #%d: %s\n", direction, q.name, h.name, type, batchName, repeat, what);
    }
}

// The producer signals, the peer waits.
bool Win32SemaphoreInteropTest::CheckExport(const QueueUnderTest& q, const HandleTypeUnderTest& h, VkSemaphoreType semaphoreType,
                                            VkCommandBuffer commandBuffer, const char* batchName, int repeat) {
    const bool timeline = (semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE);
    SharedSemaphore shared;
    if (!CreateShared(m_producer, m_peer, h.type, semaphoreType, shared)) {
        std::printf("  export %s %s: cannot create, export or import the semaphore\n", q.name, h.name);
        DestroyShared(m_producer, m_peer, shared);
        return false;
    }
    const uint64_t value = timeline ? 1 : 0;
    const VkSemaphore producerSignal = m_options.negativeControl ? VK_NULL_HANDLE : shared.owner;
    if (Submit(m_producer, q.queue, commandBuffer, VK_NULL_HANDLE, 0, producerSignal, value, VK_NULL_HANDLE) != VK_SUCCESS ||
        Submit(m_peer, m_peerQueue, VK_NULL_HANDLE, shared.imported, value, VK_NULL_HANDLE, 0, m_peerFence) != VK_SUCCESS) {
        std::printf("  export %s %s: vkQueueSubmit2 failed\n", q.name, h.name);
        return false;
    }
    const bool peerSawSignal = WaitFence(m_peer, m_peerFence);
    Report(peerSawSignal, "export", q, h, semaphoreType, batchName, repeat, "the peer's wait did not complete");
    if (!peerSawSignal) {
        return true;
    }
    if (timeline) {
        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &shared.imported;
        waitInfo.pValues = &value;
        const bool hostSawSignal =
            m_peer.WaitSemaphores(m_peer.getDevice(), &waitInfo, (uint64_t)m_options.timeoutMs * 1000000ull) == VK_SUCCESS;
        Report(hostSawSignal, "export", q, h, semaphoreType, batchName, repeat, "the peer's host wait did not complete");
    }
    m_producer.QueueWaitIdle(q.queue);
    DestroyShared(m_producer, m_peer, shared);
    return true;
}

// The peer signals, the producer waits.
bool Win32SemaphoreInteropTest::CheckImport(const QueueUnderTest& q, const HandleTypeUnderTest& h, VkSemaphoreType semaphoreType,
                                            VkCommandBuffer commandBuffer, const char* batchName, int repeat) {
    const bool timeline = (semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE);
    SharedSemaphore shared;
    if (!CreateShared(m_peer, m_producer, h.type, semaphoreType, shared)) {
        std::printf("  import %s %s: cannot create, export or import the semaphore\n", q.name, h.name);
        DestroyShared(m_peer, m_producer, shared);
        return false;
    }
    const uint64_t value = timeline ? 1 : 0;
    VkResult result = VK_SUCCESS;
    if (timeline) {
        // Wait before signal: the producer queue waits on the GPU for the peer.
        result = Submit(m_producer, q.queue, commandBuffer, shared.imported, value, VK_NULL_HANDLE, 0, m_producerFence);
        if (result == VK_SUCCESS) {
            Sleep(10);
            result = Submit(m_peer, m_peerQueue, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, shared.owner, value, VK_NULL_HANDLE);
        }
    } else {
        // A binary wait must be submitted after its signal.
        result = Submit(m_peer, m_peerQueue, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, shared.owner, value, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) {
            result = Submit(m_producer, q.queue, commandBuffer, shared.imported, value, VK_NULL_HANDLE, 0, m_producerFence);
        }
    }
    if (result != VK_SUCCESS) {
        std::printf("  import %s %s: vkQueueSubmit2 failed (%d)\n", q.name, h.name, result);
        return false;
    }
    const bool producerSawSignal = WaitFence(m_producer, m_producerFence);
    Report(producerSawSignal, "import", q, h, semaphoreType, batchName, repeat, "the producer's wait did not complete");
    if (!producerSawSignal) {
        return true;
    }

    // The queue still runs work submitted after the wait.
    if (Submit(m_producer, q.queue, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0, m_producerFence) != VK_SUCCESS) {
        std::printf("  import %s %s: vkQueueSubmit2 failed after the wait\n", q.name, h.name);
        return false;
    }
    Report(WaitFence(m_producer, m_producerFence), "import", q, h, semaphoreType, batchName, repeat,
           "the producer's next submission did not complete");
    m_peer.QueueWaitIdle(m_peerQueue);
    DestroyShared(m_peer, m_producer, shared);
    return true;
}

bool Win32SemaphoreInteropTest::CreateEncodeResources(std::string& whyNot) {
    VkDevice device = m_producer.getDevice();
    VkPhysicalDevice physDevice = m_producer.getPhysicalDevice();

    m_h264Profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    m_profile.pNext = &m_h264Profile;
    m_profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    m_profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    m_profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_profileList.profileCount = 1;
    m_profileList.pProfiles = &m_profile;

    VkVideoEncodeH264CapabilitiesKHR h264Caps{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR};
    VkVideoEncodeCapabilitiesKHR encodeCaps{VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR, &h264Caps};
    VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, &encodeCaps};
    if (m_producer.GetPhysicalDeviceVideoCapabilitiesKHR(physDevice, &m_profile, &caps) != VK_SUCCESS) {
        whyNot = "H.264 High 4:2:0 8-bit encode is not supported";
        return false;
    }

    // A small picture, aligned to the macroblock size and inside the supported range.
    m_extent.width = std::max(caps.minCodedExtent.width, 128u);
    m_extent.height = std::max(caps.minCodedExtent.height, 128u);
    m_extent.width = (m_extent.width + 15u) & ~15u;
    m_extent.height = (m_extent.height + 15u) & ~15u;

    VkPhysicalDeviceVideoFormatInfoKHR formatInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR, &m_profileList};
    formatInfo.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
    uint32_t formatCount = 0;
    m_producer.GetPhysicalDeviceVideoFormatPropertiesKHR(physDevice, &formatInfo, &formatCount, nullptr);
    std::vector<VkVideoFormatPropertiesKHR> formats(formatCount, {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
    m_producer.GetPhysicalDeviceVideoFormatPropertiesKHR(physDevice, &formatInfo, &formatCount, formats.data());
    if (formatCount == 0) {
        whyNot = "no encode source format for H.264";
        return false;
    }
    m_pictureFormat = formats[0].format;
    const VkImageTiling srcTiling = formats[0].imageTiling;

    VkVideoSessionCreateInfoKHR sessionInfo{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    sessionInfo.queueFamilyIndex = (uint32_t)m_producer.GetVideoEncodeQueueFamilyIdx();
    sessionInfo.pVideoProfile = &m_profile;
    sessionInfo.pictureFormat = m_pictureFormat;
    sessionInfo.maxCodedExtent = m_extent;
    sessionInfo.referencePictureFormat = m_pictureFormat;
    sessionInfo.maxDpbSlots = 0;
    sessionInfo.maxActiveReferencePictures = 0;
    sessionInfo.pStdHeaderVersion = &caps.stdHeaderVersion;
    if (m_producer.CreateVideoSessionKHR(device, &sessionInfo, nullptr, &m_session) != VK_SUCCESS) {
        whyNot = "vkCreateVideoSessionKHR failed";
        return false;
    }

    if (!BindSessionMemory(m_session, m_sessionMemory)) {
        whyNot = "cannot allocate or bind the video session memory";
        return false;
    }

    // One SPS and one PPS for progressive 4:2:0 pictures of m_extent. The decode case gives the
    // decoder the same two.
    StdVideoH264SequenceParameterSet& sps = m_sps;
    sps.flags.frame_mbs_only_flag = 1;
    sps.flags.direct_8x8_inference_flag = 1;
    sps.profile_idc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_4_1;
    sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
    sps.seq_parameter_set_id = 0;
    sps.log2_max_frame_num_minus4 = 0;
    sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
    sps.log2_max_pic_order_cnt_lsb_minus4 = 0;
    sps.max_num_ref_frames = 1;
    sps.pic_width_in_mbs_minus1 = m_extent.width / 16 - 1;
    sps.pic_height_in_map_units_minus1 = m_extent.height / 16 - 1;
    StdVideoH264PictureParameterSet& pps = m_pps;
    pps.seq_parameter_set_id = 0;
    pps.pic_parameter_set_id = 0;
    pps.weighted_bipred_idc = STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_DEFAULT;
    m_encodeBitstreamAlignment = (uint32_t)caps.minBitstreamBufferSizeAlignment;

    VkVideoEncodeH264SessionParametersAddInfoKHR addInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
    addInfo.stdSPSCount = 1;
    addInfo.pStdSPSs = &sps;
    addInfo.stdPPSCount = 1;
    addInfo.pStdPPSs = &pps;
    VkVideoEncodeH264SessionParametersCreateInfoKHR h264ParamsInfo{
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
    h264ParamsInfo.maxStdSPSCount = 1;
    h264ParamsInfo.maxStdPPSCount = 1;
    h264ParamsInfo.pParametersAddInfo = &addInfo;
    VkVideoSessionParametersCreateInfoKHR paramsInfo{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR, &h264ParamsInfo};
    paramsInfo.videoSession = m_session;
    if (m_producer.CreateVideoSessionParametersKHR(device, &paramsInfo, nullptr, &m_sessionParameters) != VK_SUCCESS) {
        whyNot = "vkCreateVideoSessionParametersKHR failed";
        return false;
    }

    // The source picture. Its content is never written: the test is about the semaphore, not the
    // bitstream.
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &m_profileList};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = m_pictureFormat;
    imageInfo.extent = {m_extent.width, m_extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = srcTiling;
    imageInfo.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_producer.CreateImage(device, &imageInfo, nullptr, &m_srcImage) != VK_SUCCESS) {
        whyNot = "cannot create the encode source image";
        return false;
    }
    VkMemoryRequirements imageReqs{};
    m_producer.GetImageMemoryRequirements(device, m_srcImage, &imageReqs);
    VkMemoryAllocateInfo imageAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    imageAlloc.allocationSize = imageReqs.size;
    int32_t imageType = FindMemoryType(imageReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    imageAlloc.memoryTypeIndex = (uint32_t)imageType;
    if ((imageType < 0) || (m_producer.AllocateMemory(device, &imageAlloc, nullptr, &m_srcImageMemory) != VK_SUCCESS) ||
        (m_producer.BindImageMemory(device, m_srcImage, m_srcImageMemory, 0) != VK_SUCCESS)) {
        whyNot = "cannot allocate or bind the encode source image";
        return false;
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = m_srcImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_pictureFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (m_producer.CreateImageView(device, &viewInfo, nullptr, &m_srcImageView) != VK_SUCCESS) {
        whyNot = "cannot create the encode source image view";
        return false;
    }

    // The bitstream buffer.
    m_bitstreamSize = 1024 * 1024;
    m_bitstreamSize = (m_bitstreamSize + caps.minBitstreamBufferSizeAlignment - 1) & ~(caps.minBitstreamBufferSizeAlignment - 1);
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &m_profileList};
    bufferInfo.size = m_bitstreamSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m_producer.CreateBuffer(device, &bufferInfo, nullptr, &m_bitstream) != VK_SUCCESS) {
        whyNot = "cannot create the bitstream buffer";
        return false;
    }
    VkMemoryRequirements bufferReqs{};
    m_producer.GetBufferMemoryRequirements(device, m_bitstream, &bufferReqs);
    VkMemoryAllocateInfo bufferAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bufferAlloc.allocationSize = bufferReqs.size;
    int32_t bufferType = FindMemoryType(bufferReqs.memoryTypeBits, 0);
    bufferAlloc.memoryTypeIndex = (uint32_t)bufferType;
    if ((bufferType < 0) || (m_producer.AllocateMemory(device, &bufferAlloc, nullptr, &m_bitstreamMemory) != VK_SUCCESS) ||
        (m_producer.BindBufferMemory(device, m_bitstream, m_bitstreamMemory, 0) != VK_SUCCESS)) {
        whyNot = "cannot allocate or bind the bitstream buffer";
        return false;
    }
    return true;
}

void Win32SemaphoreInteropTest::DestroyEncodeResources() {
    VkDevice device = m_producer.getDevice();
    if (m_bitstream != VK_NULL_HANDLE) {
        m_producer.DestroyBuffer(device, m_bitstream, nullptr);
    }
    if (m_bitstreamMemory != VK_NULL_HANDLE) {
        m_producer.FreeMemory(device, m_bitstreamMemory, nullptr);
    }
    if (m_srcImageView != VK_NULL_HANDLE) {
        m_producer.DestroyImageView(device, m_srcImageView, nullptr);
    }
    if (m_srcImage != VK_NULL_HANDLE) {
        m_producer.DestroyImage(device, m_srcImage, nullptr);
    }
    if (m_srcImageMemory != VK_NULL_HANDLE) {
        m_producer.FreeMemory(device, m_srcImageMemory, nullptr);
    }
    if (m_sessionParameters != VK_NULL_HANDLE) {
        m_producer.DestroyVideoSessionParametersKHR(device, m_sessionParameters, nullptr);
    }
    if (m_session != VK_NULL_HANDLE) {
        m_producer.DestroyVideoSessionKHR(device, m_session, nullptr);
    }
    for (VkDeviceMemory memory : m_sessionMemory) {
        m_producer.FreeMemory(device, memory, nullptr);
    }
    m_bitstream = VK_NULL_HANDLE;
    m_bitstreamMemory = VK_NULL_HANDLE;
    m_srcImageView = VK_NULL_HANDLE;
    m_srcImage = VK_NULL_HANDLE;
    m_srcImageMemory = VK_NULL_HANDLE;
    m_sessionParameters = VK_NULL_HANDLE;
    m_session = VK_NULL_HANDLE;
    m_sessionMemory.clear();
}

// Records one H.264 IDR picture with no reference pictures and the implementation's default rate
// control. The session is reset in every command buffer, so each one stands alone.
bool Win32SemaphoreInteropTest::RecordEncode(VkCommandBuffer commandBuffer, bool firstUse, VkBuffer dstBuffer,
                                             VkDeviceSize dstRange, bool hostReadsBitstream) {
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (m_producer.BeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        return false;
    }

    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
    barrier.oldLayout = firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_srcImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    m_producer.CmdPipelineBarrier2KHR(commandBuffer, &dependency);

    VkVideoBeginCodingInfoKHR beginCoding{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    beginCoding.videoSession = m_session;
    beginCoding.videoSessionParameters = m_sessionParameters;
    m_producer.CmdBeginVideoCodingKHR(commandBuffer, &beginCoding);

    VkVideoCodingControlInfoKHR control{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
    control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    m_producer.CmdControlVideoCodingKHR(commandBuffer, &control);

    StdVideoEncodeH264SliceHeader sliceHeader{};
    sliceHeader.slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
    sliceHeader.cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0;
    sliceHeader.disable_deblocking_filter_idc = STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED;
    VkVideoEncodeH264NaluSliceInfoKHR slice{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR};
    slice.constantQp = 0;
    slice.pStdSliceHeader = &sliceHeader;
    StdVideoEncodeH264ReferenceListsInfo refLists{};
    std::memset(refLists.RefPicList0, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(refLists.RefPicList0));
    std::memset(refLists.RefPicList1, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(refLists.RefPicList1));
    StdVideoEncodeH264PictureInfo stdPicture{};
    stdPicture.flags.IdrPicFlag = 1;
    stdPicture.seq_parameter_set_id = 0;
    stdPicture.pic_parameter_set_id = 0;
    stdPicture.idr_pic_id = 0;
    stdPicture.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_IDR;
    stdPicture.frame_num = 0;
    stdPicture.PicOrderCnt = 0;
    stdPicture.pRefLists = &refLists;
    VkVideoEncodeH264PictureInfoKHR h264Picture{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR};
    h264Picture.naluSliceEntryCount = 1;
    h264Picture.pNaluSliceEntries = &slice;
    h264Picture.pStdPictureInfo = &stdPicture;

    VkVideoEncodeInfoKHR encodeInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR, &h264Picture};
    encodeInfo.dstBuffer = dstBuffer;
    encodeInfo.dstBufferOffset = 0;
    encodeInfo.dstBufferRange = dstRange;
    encodeInfo.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    encodeInfo.srcPictureResource.codedExtent = m_extent;
    encodeInfo.srcPictureResource.imageViewBinding = m_srcImageView;
    m_producer.CmdEncodeVideoKHR(commandBuffer, &encodeInfo);

    VkVideoEndCodingInfoKHR endCoding{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    m_producer.CmdEndVideoCodingKHR(commandBuffer, &endCoding);

    if (hostReadsBitstream) {
        VkBufferMemoryBarrier2 bufferBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        bufferBarrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        bufferBarrier.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
        bufferBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        bufferBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferBarrier.buffer = dstBuffer;
        bufferBarrier.offset = 0;
        bufferBarrier.size = VK_WHOLE_SIZE;
        VkDependencyInfo bufferDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        bufferDependency.bufferMemoryBarrierCount = 1;
        bufferDependency.pBufferMemoryBarriers = &bufferBarrier;
        m_producer.CmdPipelineBarrier2KHR(commandBuffer, &bufferDependency);
    }

    return m_producer.EndCommandBuffer(commandBuffer) == VK_SUCCESS;
}

bool Win32SemaphoreInteropTest::CreateImage(const VkImageCreateInfo& info, VkImage& image, VkDeviceMemory& memory,
                                            VkImageView& view) {
    VkDevice device = m_producer.getDevice();
    if (m_producer.CreateImage(device, &info, nullptr, &image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements reqs{};
    m_producer.GetImageMemoryRequirements(device, image, &reqs);
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = reqs.size;
    int32_t type = FindMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    allocInfo.memoryTypeIndex = (uint32_t)type;
    if ((type < 0) || (m_producer.AllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) ||
        (m_producer.BindImageMemory(device, image, memory, 0) != VK_SUCCESS)) {
        return false;
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = info.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return m_producer.CreateImageView(device, &viewInfo, nullptr, &view) == VK_SUCCESS;
}

bool Win32SemaphoreInteropTest::CreateDecodeResources(std::string& whyNot) {
    VkDevice device = m_producer.getDevice();
    VkPhysicalDevice physDevice = m_producer.getPhysicalDevice();

    m_decH264Profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    m_decH264Profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    m_decProfile.pNext = &m_decH264Profile;
    m_decProfile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    m_decProfile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    m_decProfile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_decProfile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_decProfileList.profileCount = 1;
    m_decProfileList.pProfiles = &m_decProfile;
    m_bothProfiles[0] = m_profile;
    m_bothProfiles[1] = m_decProfile;
    m_bothProfileList.profileCount = 2;
    m_bothProfileList.pProfiles = m_bothProfiles;

    VkVideoDecodeH264CapabilitiesKHR h264Caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
    VkVideoDecodeCapabilitiesKHR decodeCaps{VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR, &h264Caps};
    VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, &decodeCaps};
    if (m_producer.GetPhysicalDeviceVideoCapabilitiesKHR(physDevice, &m_decProfile, &caps) != VK_SUCCESS) {
        whyNot = "H.264 High 4:2:0 8-bit decode is not supported";
        return false;
    }
    if ((m_extent.width < caps.minCodedExtent.width) || (m_extent.height < caps.minCodedExtent.height) ||
        (m_extent.width > caps.maxCodedExtent.width) || (m_extent.height > caps.maxCodedExtent.height) || (caps.maxDpbSlots < 1)) {
        whyNot = "the encoded picture size is outside the H.264 decode limits";
        return false;
    }
    m_dpbIsOutput = (decodeCaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) != 0;
    m_decodeSizeAlignment = caps.minBitstreamBufferSizeAlignment;

    // The reconstructed picture goes to a DPB slot; the output is that picture where the device
    // allows it, else a separate image.
    auto findFormat = [&](VkImageUsageFlags usage, VkFormat& format, VkImageTiling& tiling) {
        VkPhysicalDeviceVideoFormatInfoKHR formatInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR, &m_decProfileList};
        formatInfo.imageUsage = usage;
        uint32_t count = 0;
        m_producer.GetPhysicalDeviceVideoFormatPropertiesKHR(physDevice, &formatInfo, &count, nullptr);
        std::vector<VkVideoFormatPropertiesKHR> formats(count, {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
        m_producer.GetPhysicalDeviceVideoFormatPropertiesKHR(physDevice, &formatInfo, &count, formats.data());
        if (count == 0) {
            return false;
        }
        format = formats[0].format;
        tiling = formats[0].imageTiling;
        return true;
    };
    const VkImageUsageFlags dpbUsage =
        VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | (m_dpbIsOutput ? VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR : 0);
    VkFormat dpbFormat = VK_FORMAT_UNDEFINED, dstFormat = VK_FORMAT_UNDEFINED;
    VkImageTiling dpbTiling = VK_IMAGE_TILING_OPTIMAL, dstTiling = VK_IMAGE_TILING_OPTIMAL;
    if (!findFormat(dpbUsage, dpbFormat, dpbTiling) ||
        (!m_dpbIsOutput && !findFormat(VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR, dstFormat, dstTiling))) {
        whyNot = "no H.264 decode picture format";
        return false;
    }
    if (m_dpbIsOutput) {
        dstFormat = dpbFormat;
    }

    VkVideoSessionCreateInfoKHR sessionInfo{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    sessionInfo.queueFamilyIndex = (uint32_t)m_producer.GetVideoDecodeQueueFamilyIdx();
    sessionInfo.pVideoProfile = &m_decProfile;
    sessionInfo.pictureFormat = dstFormat;
    sessionInfo.maxCodedExtent = m_extent;
    sessionInfo.referencePictureFormat = dpbFormat;
    sessionInfo.maxDpbSlots = 1;
    sessionInfo.maxActiveReferencePictures = std::min(caps.maxActiveReferencePictures, 1u);
    sessionInfo.pStdHeaderVersion = &caps.stdHeaderVersion;
    if (m_producer.CreateVideoSessionKHR(device, &sessionInfo, nullptr, &m_decSession) != VK_SUCCESS) {
        whyNot = "vkCreateVideoSessionKHR failed for H.264 decode";
        return false;
    }
    if (!BindSessionMemory(m_decSession, m_decSessionMemory)) {
        whyNot = "cannot allocate or bind the decode session memory";
        return false;
    }

    VkVideoDecodeH264SessionParametersAddInfoKHR addInfo{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
    addInfo.stdSPSCount = 1;
    addInfo.pStdSPSs = &m_sps;
    addInfo.stdPPSCount = 1;
    addInfo.pStdPPSs = &m_pps;
    VkVideoDecodeH264SessionParametersCreateInfoKHR h264ParamsInfo{
        VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
    h264ParamsInfo.maxStdSPSCount = 1;
    h264ParamsInfo.maxStdPPSCount = 1;
    h264ParamsInfo.pParametersAddInfo = &addInfo;
    VkVideoSessionParametersCreateInfoKHR paramsInfo{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR, &h264ParamsInfo};
    paramsInfo.videoSession = m_decSession;
    if (m_producer.CreateVideoSessionParametersKHR(device, &paramsInfo, nullptr, &m_decSessionParameters) != VK_SUCCESS) {
        whyNot = "vkCreateVideoSessionParametersKHR failed for H.264 decode";
        return false;
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &m_decProfileList};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = dpbFormat;
    imageInfo.extent = {m_extent.width, m_extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = dpbTiling;
    imageInfo.usage = dpbUsage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!CreateImage(imageInfo, m_dpbImage, m_dpbImageMemory, m_dpbImageView)) {
        whyNot = "cannot create the decode DPB picture";
        return false;
    }
    if (!m_dpbIsOutput) {
        imageInfo.format = dstFormat;
        imageInfo.tiling = dstTiling;
        imageInfo.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
        if (!CreateImage(imageInfo, m_dstImage, m_dstImageMemory, m_dstImageView)) {
            whyNot = "cannot create the decode output picture";
            return false;
        }
    }

    // The bitstream: written by the encode queue, read by the decode queue and by the host, which
    // finds the IDR slice in it.
    VkDeviceSize alignment = std::max<VkDeviceSize>(m_decodeSizeAlignment, m_encodeBitstreamAlignment);
    m_decodeBitstreamSize = (1024 * 1024 + alignment - 1) / alignment * alignment;
    const uint32_t families[2] = {(uint32_t)m_producer.GetVideoEncodeQueueFamilyIdx(),
                                  (uint32_t)m_producer.GetVideoDecodeQueueFamilyIdx()};
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &m_bothProfileList};
    bufferInfo.size = m_decodeBitstreamSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    bufferInfo.sharingMode = (families[0] != families[1]) ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    bufferInfo.queueFamilyIndexCount = (families[0] != families[1]) ? 2 : 0;
    bufferInfo.pQueueFamilyIndices = families;
    if (m_producer.CreateBuffer(device, &bufferInfo, nullptr, &m_decodeBitstream) != VK_SUCCESS) {
        whyNot = "cannot create a bitstream buffer for both encode and decode";
        return false;
    }
    VkMemoryRequirements bufferReqs{};
    m_producer.GetBufferMemoryRequirements(device, m_decodeBitstream, &bufferReqs);
    VkMemoryAllocateInfo bufferAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bufferAlloc.allocationSize = bufferReqs.size;
    int32_t bufferType =
        FindMemoryType(bufferReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    bufferAlloc.memoryTypeIndex = (uint32_t)bufferType;
    if ((bufferType < 0) || (m_producer.AllocateMemory(device, &bufferAlloc, nullptr, &m_decodeBitstreamMemory) != VK_SUCCESS) ||
        (m_producer.BindBufferMemory(device, m_decodeBitstream, m_decodeBitstreamMemory, 0) != VK_SUCCESS)) {
        whyNot = "cannot allocate host-visible memory for the shared bitstream buffer";
        return false;
    }
    return true;
}

void Win32SemaphoreInteropTest::DestroyDecodeResources() {
    VkDevice device = m_producer.getDevice();
    for (VkImageView view : {m_dstImageView, m_dpbImageView}) {
        if (view != VK_NULL_HANDLE) {
            m_producer.DestroyImageView(device, view, nullptr);
        }
    }
    for (VkImage image : {m_dstImage, m_dpbImage}) {
        if (image != VK_NULL_HANDLE) {
            m_producer.DestroyImage(device, image, nullptr);
        }
    }
    if (m_decodeBitstream != VK_NULL_HANDLE) {
        m_producer.DestroyBuffer(device, m_decodeBitstream, nullptr);
    }
    for (VkDeviceMemory memory : {m_dstImageMemory, m_dpbImageMemory, m_decodeBitstreamMemory}) {
        if (memory != VK_NULL_HANDLE) {
            m_producer.FreeMemory(device, memory, nullptr);
        }
    }
    if (m_decSessionParameters != VK_NULL_HANDLE) {
        m_producer.DestroyVideoSessionParametersKHR(device, m_decSessionParameters, nullptr);
    }
    if (m_decSession != VK_NULL_HANDLE) {
        m_producer.DestroyVideoSessionKHR(device, m_decSession, nullptr);
    }
    for (VkDeviceMemory memory : m_decSessionMemory) {
        m_producer.FreeMemory(device, memory, nullptr);
    }
    m_dstImageView = m_dpbImageView = VK_NULL_HANDLE;
    m_dstImage = m_dpbImage = VK_NULL_HANDLE;
    m_dstImageMemory = m_dpbImageMemory = m_decodeBitstreamMemory = VK_NULL_HANDLE;
    m_decodeBitstream = VK_NULL_HANDLE;
    m_decSessionParameters = VK_NULL_HANDLE;
    m_decSession = VK_NULL_HANDLE;
    m_decSessionMemory.clear();
}

bool Win32SemaphoreInteropTest::EncodeDecodeInput(const QueueUnderTest& encodeQueue, VkCommandBuffer commandBuffer,
                                                  std::string& whyNot) {
    VkDevice device = m_producer.getDevice();
    void* mapped = nullptr;
    if (m_producer.MapMemory(device, m_decodeBitstreamMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        whyNot = "cannot map the shared bitstream buffer";
        return false;
    }
    // Zeroed first, so the end of the encoded data is the last nonzero byte: an H.264 slice NAL unit
    // ends with its stop bit, and cabac_zero_words end in 0x03.
    std::memset(mapped, 0, (size_t)m_decodeBitstreamSize);

    // The first encode of the run: the source picture comes from an undefined layout.
    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = commandBuffer;
    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cbInfo;
    if (!RecordEncode(commandBuffer, true, m_decodeBitstream, m_decodeBitstreamSize, true) ||
        (m_producer.QueueSubmit2KHR(encodeQueue.queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) ||
        (m_producer.QueueWaitIdle(encodeQueue.queue) != VK_SUCCESS)) {
        m_producer.UnmapMemory(device, m_decodeBitstreamMemory);
        whyNot = "the encode of the decode input failed";
        return false;
    }

    // The first NAL unit with nal_unit_type 5 (an IDR slice), at its start code.
    const uint8_t* data = static_cast<const uint8_t*>(mapped);
    const size_t size = (size_t)m_decodeBitstreamSize;
    size_t end = size;
    while ((end > 0) && (data[end - 1] == 0)) {
        end--;
    }
    bool found = false;
    for (size_t i = 0; (i + 3 < end) && !found; i++) {
        if ((data[i] == 0) && (data[i + 1] == 0) && (data[i + 2] == 1) && ((data[i + 3] & 0x1f) == 5)) {
            m_sliceOffset = (uint32_t)((i > 0) && (data[i - 1] == 0) ? i - 1 : i);
            found = true;
        }
    }
    m_producer.UnmapMemory(device, m_decodeBitstreamMemory);
    if (!found) {
        whyNot = "the encoder wrote no H.264 IDR slice";
        return false;
    }
    m_decodeRange = (end + m_decodeSizeAlignment - 1) / m_decodeSizeAlignment * m_decodeSizeAlignment;
    if (m_options.verbose) {
        std::printf("  decode input: IDR slice at byte %u, %zu bytes encoded\n", m_sliceOffset, end);
    }
    return m_decodeRange <= m_decodeBitstreamSize;
}

// Records the decode of the IDR picture into the DPB slot (and the output picture when the two are
// separate). The session is reset in every command buffer, so each one stands alone.
bool Win32SemaphoreInteropTest::RecordDecode(VkCommandBuffer commandBuffer) {
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (m_producer.BeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        return false;
    }

    // Every decode writes the whole picture, so the previous contents are discarded.
    VkImageMemoryBarrier2 barriers[2]{};
    uint32_t barrierCount = 0;
    for (VkImage image : {m_dpbImage, m_dstImage}) {
        if (image == VK_NULL_HANDLE) {
            continue;
        }
        VkImageMemoryBarrier2& barrier = barriers[barrierCount++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = (image == m_dpbImage) ? VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR : VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = barrierCount;
    dependency.pImageMemoryBarriers = barriers;
    m_producer.CmdPipelineBarrier2KHR(commandBuffer, &dependency);

    VkVideoPictureResourceInfoKHR dpbResource{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    dpbResource.codedExtent = m_extent;
    dpbResource.imageViewBinding = m_dpbImageView;
    VkVideoPictureResourceInfoKHR dstResource = dpbResource;
    if (!m_dpbIsOutput) {
        dstResource.imageViewBinding = m_dstImageView;
    }

    // The slot the decode sets up is bound, still inactive (slot index -1), when coding begins.
    VkVideoReferenceSlotInfoKHR beginSlot{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    beginSlot.slotIndex = -1;
    beginSlot.pPictureResource = &dpbResource;
    VkVideoBeginCodingInfoKHR beginCoding{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    beginCoding.videoSession = m_decSession;
    beginCoding.videoSessionParameters = m_decSessionParameters;
    beginCoding.referenceSlotCount = 1;
    beginCoding.pReferenceSlots = &beginSlot;
    m_producer.CmdBeginVideoCodingKHR(commandBuffer, &beginCoding);

    VkVideoCodingControlInfoKHR control{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
    control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    m_producer.CmdControlVideoCodingKHR(commandBuffer, &control);

    StdVideoDecodeH264ReferenceInfo stdReference{};
    VkVideoDecodeH264DpbSlotInfoKHR h264Slot{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
    h264Slot.pStdReferenceInfo = &stdReference;
    VkVideoReferenceSlotInfoKHR setupSlot{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, &h264Slot};
    setupSlot.slotIndex = 0;
    setupSlot.pPictureResource = &dpbResource;

    StdVideoDecodeH264PictureInfo stdPicture{};
    stdPicture.flags.is_intra = 1;
    stdPicture.flags.IdrPicFlag = 1;
    stdPicture.flags.is_reference = 1;
    stdPicture.seq_parameter_set_id = 0;
    stdPicture.pic_parameter_set_id = 0;
    stdPicture.frame_num = 0;
    stdPicture.idr_pic_id = 0;
    VkVideoDecodeH264PictureInfoKHR h264Picture{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR};
    h264Picture.pStdPictureInfo = &stdPicture;
    h264Picture.sliceCount = 1;
    h264Picture.pSliceOffsets = &m_sliceOffset;

    VkVideoDecodeInfoKHR decodeInfo{VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR, &h264Picture};
    decodeInfo.srcBuffer = m_decodeBitstream;
    decodeInfo.srcBufferOffset = 0;
    decodeInfo.srcBufferRange = m_decodeRange;
    decodeInfo.dstPictureResource = dstResource;
    decodeInfo.pSetupReferenceSlot = &setupSlot;
    m_producer.CmdDecodeVideoKHR(commandBuffer, &decodeInfo);

    VkVideoEndCodingInfoKHR endCoding{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    m_producer.CmdEndVideoCodingKHR(commandBuffer, &endCoding);
    return m_producer.EndCommandBuffer(commandBuffer) == VK_SUCCESS;
}

int Win32SemaphoreInteropTest::Run() {
    VkDevice device = m_producer.getDevice();
    const QueueUnderTest queues[] = {
        {"graphics", m_producer.GetGfxQueue(), m_producer.GetGfxQueueFamilyIdx()},
        {"compute", m_producer.GetComputeQueue(), m_producer.GetComputeQueueFamilyIdx()},
        {"transfer", m_producer.GetTransferQueue(), m_producer.GetTransferQueueFamilyIdx()},
        {"decode", m_producer.GetVideoDecodeQueue(), m_producer.GetVideoDecodeQueueFamilyIdx()},
        {"encode", m_producer.GetVideoEncodeQueue(), m_producer.GetVideoEncodeQueueFamilyIdx()},
    };

    for (const QueueUnderTest& q : queues) {
        if ((q.queue == VK_NULL_HANDLE) || (q.family < 0)) {
            std::printf("COULD NOT RUN: the device has no %s queue\n", q.name);
            return kExitCouldNotRun;
        }
    }

    // The encode and decode resources, and the IDR picture the decode batches decode, made by the
    // encode queue before any check.
    {
        std::string whyNot;
        if (!CreateEncodeResources(whyNot) || !CreateDecodeResources(whyNot)) {
            std::printf("COULD NOT RUN: the video cases: %s\n", whyNot.c_str());
            return kExitCouldNotRun;
        }
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = (uint32_t)queues[4].family;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        bool ok = (m_producer.CreateCommandPool(device, &poolInfo, nullptr, &pool) == VK_SUCCESS);
        allocInfo.commandPool = pool;
        ok = ok && (m_producer.AllocateCommandBuffers(device, &allocInfo, &commandBuffer) == VK_SUCCESS) &&
             EncodeDecodeInput(queues[4], commandBuffer, whyNot);
        if (pool != VK_NULL_HANDLE) {
            m_producer.DestroyCommandPool(device, pool, nullptr);
        }
        if (!ok) {
            std::printf("COULD NOT RUN: the decode input: %s\n", whyNot.empty() ? "a Vulkan call failed" : whyNot.c_str());
            return kExitCouldNotRun;
        }
    }

    for (const QueueUnderTest& q : queues) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = (uint32_t)q.family;
        VkCommandPool pool = VK_NULL_HANDLE;
        if (m_producer.CreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            std::printf("COULD NOT RUN: vkCreateCommandPool failed on the %s queue family\n", q.name);
            return kExitCouldNotRun;
        }
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        m_producer.AllocateCommandBuffers(device, &allocInfo, &commandBuffer);

        const bool encodeQueue = (std::strcmp(q.name, "encode") == 0);
        const bool decodeQueue = (std::strcmp(q.name, "decode") == 0);

        // A queue that missed a signal may still be waiting on it and hold up every later check, so
        // the run stops at the first failure.
        bool ok = true;
        bool firstEncode = true;
        for (const HandleTypeUnderTest& h : m_handleTypes) {
            for (VkSemaphoreType semaphoreType : {VK_SEMAPHORE_TYPE_BINARY, VK_SEMAPHORE_TYPE_TIMELINE}) {
                if (!((semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) ? h.timeline : h.binary)) {
                    continue;
                }
                // A binary wait with no signal pending is invalid usage; a timeline wait for a value
                // nobody signals is valid, and only times out.
                if (m_options.negativeControl && (semaphoreType == VK_SEMAPHORE_TYPE_BINARY)) {
                    continue;
                }
                for (int r = 0; ok && (r < m_options.repeats); r++) {
                    ok = CheckExport(q, h, semaphoreType, VK_NULL_HANDLE, "no-cmdbuf", r) && (m_failures == 0) &&
                         CheckImport(q, h, semaphoreType, VK_NULL_HANDLE, "no-cmdbuf", r);
                }
                for (int r = 0; ok && (r < m_options.repeats); r++) {
                    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    ok = (m_producer.BeginCommandBuffer(commandBuffer, &beginInfo) == VK_SUCCESS) &&
                         (m_producer.EndCommandBuffer(commandBuffer) == VK_SUCCESS) &&
                         CheckExport(q, h, semaphoreType, commandBuffer, "empty-cmdbuf", r) && (m_failures == 0) &&
                         CheckImport(q, h, semaphoreType, commandBuffer, "empty-cmdbuf", r);
                }
                for (int r = 0; ok && decodeQueue && (r < m_options.repeats); r++) {
                    ok = RecordDecode(commandBuffer) && CheckExport(q, h, semaphoreType, commandBuffer, "h264-idr-decode", r) &&
                         (m_failures == 0) && CheckImport(q, h, semaphoreType, commandBuffer, "h264-idr-decode", r);
                }
                for (int r = 0; ok && encodeQueue && (r < m_options.repeats); r++) {
                    ok = RecordEncode(commandBuffer, firstEncode, m_bitstream, m_bitstreamSize) &&
                         CheckExport(q, h, semaphoreType, commandBuffer, "h264-idr-encode", r) && (m_failures == 0) &&
                         CheckImport(q, h, semaphoreType, commandBuffer, "h264-idr-encode", r);
                    firstEncode = false;
                }
                if (m_failures != 0) {
                    break;
                }
            }
            if (m_failures != 0) {
                break;
            }
        }
        if (m_failures != 0) {
            break;
        }
        m_producer.DestroyCommandPool(device, pool, nullptr);
        if (!ok) {
            std::printf("COULD NOT RUN: a Vulkan call failed on the %s queue\n", q.name);
            return kExitCouldNotRun;
        }
    }

    std::printf("%d checks, %d failed\n", m_checks, m_failures);
    std::printf("RESULT: %s\n", (m_failures == 0) ? "PASS" : "FAIL");
    return (m_failures == 0) ? kExitPass : kExitFail;
}

bool RequireGpu() {
#ifdef VVS_REQUIRE_GPU
    return true;
#else
    const char* value = std::getenv("VVS_REQUIRE_GPU");
    return (value != nullptr) && (std::strcmp(value, "1") == 0);
#endif
}

void PrintHelp(const char* prog) {
    std::printf(
        "Usage: %s [--timeout-ms N] [--repeats N] [--device-id N] [--verbose] [--negative-control]\n"
        "Shares semaphores through Win32 handles between the graphics, compute, transfer, decode and\n"
        "encode queues and a second device on the same GPU, in both directions, and checks that every\n"
        "signal arrives. The encode and decode queues also run an H.264 IDR encode and decode.\n"
        "--negative-control leaves out the producer's signals: the run must fail.\n"
        "Exit codes: 0 pass, 1 a signal did not arrive, 77 could not run, 64 bad usage.\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if ((arg == "--help") || (arg == "-h")) {
            PrintHelp(argv[0]);
            return kExitPass;
        } else if ((arg == "--verbose") || (arg == "-v")) {
            options.verbose = true;
        } else if (arg == "--negative-control") {
            options.negativeControl = true;
        } else if ((arg == "--timeout-ms") && (i + 1 < argc)) {
            options.timeoutMs = std::atoi(argv[++i]);
        } else if ((arg == "--repeats") && (i + 1 < argc)) {
            options.repeats = std::atoi(argv[++i]);
        } else if ((arg == "--device-id") && (i + 1 < argc)) {
            options.deviceId = (int)std::strtol(argv[++i], nullptr, 0);
        } else {
            PrintHelp(argv[0]);
            return kExitBadUsage;
        }
    }
    if ((options.timeoutMs <= 0) || (options.repeats <= 0)) {
        PrintHelp(argv[0]);
        return kExitBadUsage;
    }

    // Heap-allocated so a failed run can end without destroying it (see the file comment).
    Win32SemaphoreInteropTest* test = new Win32SemaphoreInteropTest(options);
    std::string whyNot;
    int exitCode = kExitCouldNotRun;
    if (!test->Init(whyNot)) {
        std::printf("COULD NOT RUN: %s\n", whyNot.c_str());
    } else {
        exitCode = test->Run();
    }
    if ((exitCode == kExitCouldNotRun) && RequireGpu()) {
        std::printf("FAIL: the test could not run, and VVS_REQUIRE_GPU is set\n");
        exitCode = kExitFail;
    }
    std::fflush(stdout);
    if (exitCode == kExitFail) {
        TerminateProcess(GetCurrentProcess(), (UINT)exitCode);
    }
    delete test;
    return exitCode;
}

#else  // !defined(_WIN32)

#include <cstdio>

int main() {
    std::printf("COULD NOT RUN: Win32 handles are a Windows handle type\n");
    return 77;
}

#endif  // defined(_WIN32)
