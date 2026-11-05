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

// A SYNC_FD exported from a binary semaphore that a queue submission signals must become readable
// once the submission completes, on every queue family the samples submit to.
//
// For each of the graphics, compute, transfer, video decode and video encode queues, the test
// submits a batch that signals a semaphore created exportable as SYNC_FD, exports the fd with
// vkGetSemaphoreFdKHR and poll()s it. The batches:
// - no command buffer;
// - an empty command buffer;
// - on the encode queue, a command buffer that encodes one H.264 IDR picture. This is the batch the
//   encoder library's release fence is exported from;
// - on the decode queue, a command buffer that decodes an H.264 IDR picture the encode queue made
//   before, so the Vulkan decode queue decodes before the signal;
// - on the decode queue, the same decode waiting on a SYNC_FD imported from a pending encode batch's
//   export, the way a decoder consumes an encoder's release fence. The decode's own export can
//   signal only once the imported fence has signalled and the decode has run.
//
// The fd must become readable within the timeout (default 2000 ms). An fd that never does means a
// consumer waiting on it, such as a compositor or a second device, waits forever.
//
// Exit codes: 0 every exported fd signalled; 1 at least one did not; 77 could not run (no device,
// no SYNC_FD export, no H.264 encode or decode queue). VVS_REQUIRE_GPU=1 in the environment, or the
// VVS_REQUIRE_GPU compile definition, makes "could not run" a failure.

#if defined(__linux__)

#include <poll.h>
#include <unistd.h>

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
};

struct Results {
    int checks = 0;
    int failures = 0;
};

// One queue under test.
struct QueueUnderTest {
    const char* name;
    VkQueue queue;
    int32_t family;
};

class SyncFdExportTest {
   public:
    explicit SyncFdExportTest(const Options& options) : m_options(options) {}

    ~SyncFdExportTest();

    // Creates the instance and the device; false means the test cannot run.
    bool Init(std::string& whyNot);

    // Runs every case and returns the exit code.
    int Run();

   private:
    // Submits one batch that signals a new exportable semaphore, exports the SYNC_FD and polls it.
    // commandBuffer may be VK_NULL_HANDLE; the batch waits on waitSemaphore when it is not
    // VK_NULL_HANDLE. Returns false on a Vulkan error.
    bool SubmitExportAndPoll(const QueueUnderTest& q, VkCommandBuffer commandBuffer, const char* batchName, int repeat,
                             VkSemaphore waitSemaphore = VK_NULL_HANDLE);

    bool CreateEncodeResources(std::string& whyNot);
    void DestroyEncodeResources();
    // Records one IDR encode into dstBuffer. With hostReadsBitstream the command buffer ends with a
    // barrier that makes the bitstream available to the host and to later device reads.
    bool RecordEncode(VkCommandBuffer commandBuffer, bool firstUse, VkBuffer dstBuffer, VkDeviceSize dstRange,
                      bool hostReadsBitstream = false);
    int32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const;
    bool BindSessionMemory(VkVideoSessionKHR session, std::vector<VkDeviceMemory>& memory);

    // The decode batches, run once the encode queue has been tested.
    bool RunDecodeCases(const QueueUnderTest& decodeQueue, const QueueUnderTest& encodeQueue, std::string& whyNot);
    bool CreateDecodeResources(std::string& whyNot);
    void DestroyDecodeResources();
    // Encodes one IDR picture into m_decodeBitstream and finds its size; false if the encoder wrote
    // no H.264 IDR slice there.
    bool EncodeDecodeInput(const QueueUnderTest& encodeQueue, VkCommandBuffer commandBuffer, std::string& whyNot);
    bool RecordDecode(VkCommandBuffer commandBuffer);
    bool CreateImage(const VkImageCreateInfo& info, VkImage& image, VkDeviceMemory& memory, VkImageView& view);

    Options m_options;
    VulkanDeviceContext m_vkDevCtx;
    Results m_results;

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

SyncFdExportTest::~SyncFdExportTest() {
    if (m_vkDevCtx.getDevice() != VK_NULL_HANDLE) {
        m_vkDevCtx.DeviceWaitIdle();
        DestroyDecodeResources();
        DestroyEncodeResources();
    }
}

bool SyncFdExportTest::Init(std::string& whyNot) {
    static const char* const instanceExtensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                                                     VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, nullptr};
    m_vkDevCtx.AddReqInstanceExtensions(instanceExtensions);

    static const char* const deviceExtensions[] = {
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,  VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,  nullptr};
    m_vkDevCtx.AddReqDeviceExtensions(deviceExtensions, m_options.verbose);

    if (m_vkDevCtx.InitVulkanDevice("SyncFdExportTest", VK_NULL_HANDLE, m_options.verbose) != VK_SUCCESS) {
        whyNot = "InitVulkanDevice failed (no Vulkan loader or driver?)";
        return false;
    }

    vk::DeviceUuidUtils deviceUuid;
    const VkQueueFlags queueTypes = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT |
                                    VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_VIDEO_ENCODE_BIT_KHR;
    if (m_vkDevCtx.InitPhysicalDevice(m_options.deviceId, deviceUuid, queueTypes, nullptr, VK_QUEUE_VIDEO_DECODE_BIT_KHR,
                                      VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, VK_QUEUE_VIDEO_ENCODE_BIT_KHR,
                                      VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) != VK_SUCCESS) {
        whyNot =
            "no physical device with graphics, compute, transfer, video decode and H.264 video encode "
            "queues and external semaphore fds";
        return false;
    }

    VkPhysicalDeviceProperties props{};
    m_vkDevCtx.GetPhysicalDeviceProperties(m_vkDevCtx.getPhysicalDevice(), &props);
    std::printf("device: %s, driver version %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.driverVersion),
                VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));

    // The SYNC_FD export of a binary semaphore must be supported, or the case cannot run.
    VkPhysicalDeviceExternalSemaphoreInfo semInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    semInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkExternalSemaphoreProperties semProps{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    m_vkDevCtx.GetPhysicalDeviceExternalSemaphoreProperties(m_vkDevCtx.getPhysicalDevice(), &semInfo, &semProps);
    if ((semProps.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) == 0) {
        whyNot = "the device cannot export a binary semaphore as a SYNC_FD";
        return false;
    }

    if (m_vkDevCtx.CreateVulkanDevice(
            1, 1, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR | VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, true /* transfer */,
            true /* graphics */, false /* present */, true /* compute */) != VK_SUCCESS) {
        whyNot = "CreateVulkanDevice failed";
        return false;
    }
    return true;
}

int32_t SyncFdExportTest::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const {
    VkPhysicalDeviceMemoryProperties memProps{};
    m_vkDevCtx.GetPhysicalDeviceMemoryProperties(m_vkDevCtx.getPhysicalDevice(), &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (((typeBits & (1u << i)) != 0) && ((memProps.memoryTypes[i].propertyFlags & required) == required)) {
            return (int32_t)i;
        }
    }
    return -1;
}

bool SyncFdExportTest::BindSessionMemory(VkVideoSessionKHR session, std::vector<VkDeviceMemory>& memory) {
    VkDevice device = m_vkDevCtx.getDevice();
    uint32_t reqCount = 0;
    m_vkDevCtx.GetVideoSessionMemoryRequirementsKHR(device, session, &reqCount, nullptr);
    std::vector<VkVideoSessionMemoryRequirementsKHR> reqs(reqCount, {VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR});
    m_vkDevCtx.GetVideoSessionMemoryRequirementsKHR(device, session, &reqCount, reqs.data());
    std::vector<VkBindVideoSessionMemoryInfoKHR> binds;
    for (const VkVideoSessionMemoryRequirementsKHR& req : reqs) {
        int32_t type = FindMemoryType(req.memoryRequirements.memoryTypeBits, 0);
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = req.memoryRequirements.size;
        allocInfo.memoryTypeIndex = (uint32_t)type;
        VkDeviceMemory deviceMemory = VK_NULL_HANDLE;
        if ((type < 0) || (m_vkDevCtx.AllocateMemory(device, &allocInfo, nullptr, &deviceMemory) != VK_SUCCESS)) {
            return false;
        }
        memory.push_back(deviceMemory);
        VkBindVideoSessionMemoryInfoKHR bind{VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR};
        bind.memoryBindIndex = req.memoryBindIndex;
        bind.memory = deviceMemory;
        bind.memorySize = req.memoryRequirements.size;
        binds.push_back(bind);
    }
    return m_vkDevCtx.BindVideoSessionMemoryKHR(device, session, (uint32_t)binds.size(), binds.data()) == VK_SUCCESS;
}

bool SyncFdExportTest::SubmitExportAndPoll(const QueueUnderTest& q, VkCommandBuffer commandBuffer, const char* batchName,
                                           int repeat, VkSemaphore waitSemaphore) {
    VkDevice device = m_vkDevCtx.getDevice();

    VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo semCreateInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exportInfo};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (m_vkDevCtx.CreateSemaphore(device, &semCreateInfo, nullptr, &semaphore) != VK_SUCCESS) {
        std::printf("  %s %s: vkCreateSemaphore failed\n", q.name, batchName);
        return false;
    }

    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = semaphore;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = commandBuffer;
    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = waitSemaphore;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.waitSemaphoreInfoCount = (waitSemaphore != VK_NULL_HANDLE) ? 1 : 0;
    submitInfo.pWaitSemaphoreInfos = (waitSemaphore != VK_NULL_HANDLE) ? &waitInfo : nullptr;
    submitInfo.commandBufferInfoCount = (commandBuffer != VK_NULL_HANDLE) ? 1 : 0;
    submitInfo.pCommandBufferInfos = (commandBuffer != VK_NULL_HANDLE) ? &cbInfo : nullptr;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalInfo;

    VkResult result = m_vkDevCtx.QueueSubmit2KHR(q.queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        std::printf("  %s %s: vkQueueSubmit2 failed (%d)\n", q.name, batchName, result);
        m_vkDevCtx.DestroySemaphore(device, semaphore, nullptr);
        return false;
    }

    // Export right after the submission, while it may still be pending: the fd then carries the
    // submission's completion, and the poll below waits for it.
    VkSemaphoreGetFdInfoKHR getFdInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    getFdInfo.semaphore = semaphore;
    getFdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int fd = -1;
    result = m_vkDevCtx.GetSemaphoreFdKHR(device, &getFdInfo, &fd);

    bool signalled = false;
    int pollResult = 0;
    if (result == VK_SUCCESS) {
        if (fd == -1) {
            // -1 is a valid export of a payload that is already signalled.
            signalled = true;
        } else {
            struct pollfd pfd = {fd, POLLIN, 0};
            pollResult = poll(&pfd, 1, m_options.timeoutMs);
            signalled = (pollResult == 1);
            close(fd);
        }
    }

    m_results.checks++;
    if ((result != VK_SUCCESS) || !signalled) {
        m_results.failures++;
        std::printf("  FAIL %-9s %-24s #%d: vkGetSemaphoreFdKHR %d, fd %d, poll returned %d after %d ms\n", q.name, batchName,
                    repeat, result, fd, pollResult, m_options.timeoutMs);
    } else if (m_options.verbose) {
        std::printf("  ok   %-9s %-24s #%d: fd %d signalled\n", q.name, batchName, repeat, fd);
    }

    // A batch that waits on an imported fence which never signalled never completes, so its objects
    // cannot be released: report and end the process without teardown.
    if (!signalled && (waitSemaphore != VK_NULL_HANDLE)) {
        std::printf("  the %s batch waits on an imported SYNC_FD that never signalled: ending without teardown\n", batchName);
        std::printf("RESULT: FAIL\n");
        std::fflush(stdout);
        _exit(kExitFail);
    }

    // The submission must complete whether or not the fd signalled, before its objects go away.
    m_vkDevCtx.QueueWaitIdle(q.queue);
    m_vkDevCtx.DestroySemaphore(device, semaphore, nullptr);
    return true;
}

bool SyncFdExportTest::CreateEncodeResources(std::string& whyNot) {
    VkDevice device = m_vkDevCtx.getDevice();
    VkPhysicalDevice physDevice = m_vkDevCtx.getPhysicalDevice();

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
    if (m_vkDevCtx.GetPhysicalDeviceVideoCapabilitiesKHR(physDevice, &m_profile, &caps) != VK_SUCCESS) {
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
    m_vkDevCtx.GetPhysicalDeviceVideoFormatPropertiesKHR(physDevice, &formatInfo, &formatCount, nullptr);
    std::vector<VkVideoFormatPropertiesKHR> formats(formatCount, {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
    m_vkDevCtx.GetPhysicalDeviceVideoFormatPropertiesKHR(physDevice, &formatInfo, &formatCount, formats.data());
    if (formatCount == 0) {
        whyNot = "no encode source format for H.264";
        return false;
    }
    m_pictureFormat = formats[0].format;
    const VkImageTiling srcTiling = formats[0].imageTiling;

    VkVideoSessionCreateInfoKHR sessionInfo{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    sessionInfo.queueFamilyIndex = (uint32_t)m_vkDevCtx.GetVideoEncodeQueueFamilyIdx();
    sessionInfo.pVideoProfile = &m_profile;
    sessionInfo.pictureFormat = m_pictureFormat;
    sessionInfo.maxCodedExtent = m_extent;
    sessionInfo.referencePictureFormat = m_pictureFormat;
    sessionInfo.maxDpbSlots = 0;
    sessionInfo.maxActiveReferencePictures = 0;
    sessionInfo.pStdHeaderVersion = &caps.stdHeaderVersion;
    if (m_vkDevCtx.CreateVideoSessionKHR(device, &sessionInfo, nullptr, &m_session) != VK_SUCCESS) {
        whyNot = "vkCreateVideoSessionKHR failed";
        return false;
    }

    if (!BindSessionMemory(m_session, m_sessionMemory)) {
        whyNot = "cannot allocate or bind the video session memory";
        return false;
    }

    // One SPS and one PPS for progressive 4:2:0 pictures of m_extent. The decode cases give the
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
    if (m_vkDevCtx.CreateVideoSessionParametersKHR(device, &paramsInfo, nullptr, &m_sessionParameters) != VK_SUCCESS) {
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
    if (m_vkDevCtx.CreateImage(device, &imageInfo, nullptr, &m_srcImage) != VK_SUCCESS) {
        whyNot = "cannot create the encode source image";
        return false;
    }
    VkMemoryRequirements imageReqs{};
    m_vkDevCtx.GetImageMemoryRequirements(device, m_srcImage, &imageReqs);
    VkMemoryAllocateInfo imageAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    imageAlloc.allocationSize = imageReqs.size;
    int32_t imageType = FindMemoryType(imageReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    imageAlloc.memoryTypeIndex = (uint32_t)imageType;
    if ((imageType < 0) || (m_vkDevCtx.AllocateMemory(device, &imageAlloc, nullptr, &m_srcImageMemory) != VK_SUCCESS) ||
        (m_vkDevCtx.BindImageMemory(device, m_srcImage, m_srcImageMemory, 0) != VK_SUCCESS)) {
        whyNot = "cannot allocate or bind the encode source image";
        return false;
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = m_srcImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_pictureFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (m_vkDevCtx.CreateImageView(device, &viewInfo, nullptr, &m_srcImageView) != VK_SUCCESS) {
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
    if (m_vkDevCtx.CreateBuffer(device, &bufferInfo, nullptr, &m_bitstream) != VK_SUCCESS) {
        whyNot = "cannot create the bitstream buffer";
        return false;
    }
    VkMemoryRequirements bufferReqs{};
    m_vkDevCtx.GetBufferMemoryRequirements(device, m_bitstream, &bufferReqs);
    VkMemoryAllocateInfo bufferAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bufferAlloc.allocationSize = bufferReqs.size;
    int32_t bufferType = FindMemoryType(bufferReqs.memoryTypeBits, 0);
    bufferAlloc.memoryTypeIndex = (uint32_t)bufferType;
    if ((bufferType < 0) || (m_vkDevCtx.AllocateMemory(device, &bufferAlloc, nullptr, &m_bitstreamMemory) != VK_SUCCESS) ||
        (m_vkDevCtx.BindBufferMemory(device, m_bitstream, m_bitstreamMemory, 0) != VK_SUCCESS)) {
        whyNot = "cannot allocate or bind the bitstream buffer";
        return false;
    }
    return true;
}

void SyncFdExportTest::DestroyEncodeResources() {
    VkDevice device = m_vkDevCtx.getDevice();
    if (m_bitstream != VK_NULL_HANDLE) {
        m_vkDevCtx.DestroyBuffer(device, m_bitstream, nullptr);
    }
    if (m_bitstreamMemory != VK_NULL_HANDLE) {
        m_vkDevCtx.FreeMemory(device, m_bitstreamMemory, nullptr);
    }
    if (m_srcImageView != VK_NULL_HANDLE) {
        m_vkDevCtx.DestroyImageView(device, m_srcImageView, nullptr);
    }
    if (m_srcImage != VK_NULL_HANDLE) {
        m_vkDevCtx.DestroyImage(device, m_srcImage, nullptr);
    }
    if (m_srcImageMemory != VK_NULL_HANDLE) {
        m_vkDevCtx.FreeMemory(device, m_srcImageMemory, nullptr);
    }
    if (m_sessionParameters != VK_NULL_HANDLE) {
        m_vkDevCtx.DestroyVideoSessionParametersKHR(device, m_sessionParameters, nullptr);
    }
    if (m_session != VK_NULL_HANDLE) {
        m_vkDevCtx.DestroyVideoSessionKHR(device, m_session, nullptr);
    }
    for (VkDeviceMemory memory : m_sessionMemory) {
        m_vkDevCtx.FreeMemory(device, memory, nullptr);
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
bool SyncFdExportTest::RecordEncode(VkCommandBuffer commandBuffer, bool firstUse, VkBuffer dstBuffer, VkDeviceSize dstRange,
                                    bool hostReadsBitstream) {
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (m_vkDevCtx.BeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
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
    m_vkDevCtx.CmdPipelineBarrier2KHR(commandBuffer, &dependency);

    VkVideoBeginCodingInfoKHR beginCoding{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    beginCoding.videoSession = m_session;
    beginCoding.videoSessionParameters = m_sessionParameters;
    m_vkDevCtx.CmdBeginVideoCodingKHR(commandBuffer, &beginCoding);

    VkVideoCodingControlInfoKHR control{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
    control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    m_vkDevCtx.CmdControlVideoCodingKHR(commandBuffer, &control);

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
    m_vkDevCtx.CmdEncodeVideoKHR(commandBuffer, &encodeInfo);

    VkVideoEndCodingInfoKHR endCoding{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    m_vkDevCtx.CmdEndVideoCodingKHR(commandBuffer, &endCoding);

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
        m_vkDevCtx.CmdPipelineBarrier2KHR(commandBuffer, &bufferDependency);
    }

    return m_vkDevCtx.EndCommandBuffer(commandBuffer) == VK_SUCCESS;
}

bool SyncFdExportTest::CreateImage(const VkImageCreateInfo& info, VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
    VkDevice device = m_vkDevCtx.getDevice();
    if (m_vkDevCtx.CreateImage(device, &info, nullptr, &image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements reqs{};
    m_vkDevCtx.GetImageMemoryRequirements(device, image, &reqs);
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = reqs.size;
    int32_t type = FindMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    allocInfo.memoryTypeIndex = (uint32_t)type;
    if ((type < 0) || (m_vkDevCtx.AllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) ||
        (m_vkDevCtx.BindImageMemory(device, image, memory, 0) != VK_SUCCESS)) {
        return false;
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = info.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return m_vkDevCtx.CreateImageView(device, &viewInfo, nullptr, &view) == VK_SUCCESS;
}

bool SyncFdExportTest::CreateDecodeResources(std::string& whyNot) {
    VkDevice device = m_vkDevCtx.getDevice();
    VkPhysicalDevice physDevice = m_vkDevCtx.getPhysicalDevice();

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
    if (m_vkDevCtx.GetPhysicalDeviceVideoCapabilitiesKHR(physDevice, &m_decProfile, &caps) != VK_SUCCESS) {
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
        m_vkDevCtx.GetPhysicalDeviceVideoFormatPropertiesKHR(physDevice, &formatInfo, &count, nullptr);
        std::vector<VkVideoFormatPropertiesKHR> formats(count, {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
        m_vkDevCtx.GetPhysicalDeviceVideoFormatPropertiesKHR(physDevice, &formatInfo, &count, formats.data());
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
    sessionInfo.queueFamilyIndex = (uint32_t)m_vkDevCtx.GetVideoDecodeQueueFamilyIdx();
    sessionInfo.pVideoProfile = &m_decProfile;
    sessionInfo.pictureFormat = dstFormat;
    sessionInfo.maxCodedExtent = m_extent;
    sessionInfo.referencePictureFormat = dpbFormat;
    sessionInfo.maxDpbSlots = 1;
    sessionInfo.maxActiveReferencePictures = std::min(caps.maxActiveReferencePictures, 1u);
    sessionInfo.pStdHeaderVersion = &caps.stdHeaderVersion;
    if (m_vkDevCtx.CreateVideoSessionKHR(device, &sessionInfo, nullptr, &m_decSession) != VK_SUCCESS) {
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
    if (m_vkDevCtx.CreateVideoSessionParametersKHR(device, &paramsInfo, nullptr, &m_decSessionParameters) != VK_SUCCESS) {
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
    const uint32_t families[2] = {(uint32_t)m_vkDevCtx.GetVideoEncodeQueueFamilyIdx(),
                                  (uint32_t)m_vkDevCtx.GetVideoDecodeQueueFamilyIdx()};
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &m_bothProfileList};
    bufferInfo.size = m_decodeBitstreamSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    bufferInfo.sharingMode = (families[0] != families[1]) ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    bufferInfo.queueFamilyIndexCount = (families[0] != families[1]) ? 2 : 0;
    bufferInfo.pQueueFamilyIndices = families;
    if (m_vkDevCtx.CreateBuffer(device, &bufferInfo, nullptr, &m_decodeBitstream) != VK_SUCCESS) {
        whyNot = "cannot create a bitstream buffer for both encode and decode";
        return false;
    }
    VkMemoryRequirements bufferReqs{};
    m_vkDevCtx.GetBufferMemoryRequirements(device, m_decodeBitstream, &bufferReqs);
    VkMemoryAllocateInfo bufferAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bufferAlloc.allocationSize = bufferReqs.size;
    int32_t bufferType =
        FindMemoryType(bufferReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    bufferAlloc.memoryTypeIndex = (uint32_t)bufferType;
    if ((bufferType < 0) || (m_vkDevCtx.AllocateMemory(device, &bufferAlloc, nullptr, &m_decodeBitstreamMemory) != VK_SUCCESS) ||
        (m_vkDevCtx.BindBufferMemory(device, m_decodeBitstream, m_decodeBitstreamMemory, 0) != VK_SUCCESS)) {
        whyNot = "cannot allocate host-visible memory for the shared bitstream buffer";
        return false;
    }
    return true;
}

void SyncFdExportTest::DestroyDecodeResources() {
    VkDevice device = m_vkDevCtx.getDevice();
    for (VkImageView view : {m_dstImageView, m_dpbImageView}) {
        if (view != VK_NULL_HANDLE) {
            m_vkDevCtx.DestroyImageView(device, view, nullptr);
        }
    }
    for (VkImage image : {m_dstImage, m_dpbImage}) {
        if (image != VK_NULL_HANDLE) {
            m_vkDevCtx.DestroyImage(device, image, nullptr);
        }
    }
    if (m_decodeBitstream != VK_NULL_HANDLE) {
        m_vkDevCtx.DestroyBuffer(device, m_decodeBitstream, nullptr);
    }
    for (VkDeviceMemory memory : {m_dstImageMemory, m_dpbImageMemory, m_decodeBitstreamMemory}) {
        if (memory != VK_NULL_HANDLE) {
            m_vkDevCtx.FreeMemory(device, memory, nullptr);
        }
    }
    if (m_decSessionParameters != VK_NULL_HANDLE) {
        m_vkDevCtx.DestroyVideoSessionParametersKHR(device, m_decSessionParameters, nullptr);
    }
    if (m_decSession != VK_NULL_HANDLE) {
        m_vkDevCtx.DestroyVideoSessionKHR(device, m_decSession, nullptr);
    }
    for (VkDeviceMemory memory : m_decSessionMemory) {
        m_vkDevCtx.FreeMemory(device, memory, nullptr);
    }
    m_dstImageView = m_dpbImageView = VK_NULL_HANDLE;
    m_dstImage = m_dpbImage = VK_NULL_HANDLE;
    m_dstImageMemory = m_dpbImageMemory = m_decodeBitstreamMemory = VK_NULL_HANDLE;
    m_decodeBitstream = VK_NULL_HANDLE;
    m_decSessionParameters = VK_NULL_HANDLE;
    m_decSession = VK_NULL_HANDLE;
    m_decSessionMemory.clear();
}

bool SyncFdExportTest::EncodeDecodeInput(const QueueUnderTest& encodeQueue, VkCommandBuffer commandBuffer, std::string& whyNot) {
    VkDevice device = m_vkDevCtx.getDevice();
    void* mapped = nullptr;
    if (m_vkDevCtx.MapMemory(device, m_decodeBitstreamMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        whyNot = "cannot map the shared bitstream buffer";
        return false;
    }
    // Zeroed first, so the end of the encoded data is the last nonzero byte: an H.264 slice NAL unit
    // ends with its stop bit, and cabac_zero_words end in 0x03.
    std::memset(mapped, 0, (size_t)m_decodeBitstreamSize);

    // The encode queue has encoded into the source picture before, so it is in the encode layout.
    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = commandBuffer;
    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cbInfo;
    if (!RecordEncode(commandBuffer, false, m_decodeBitstream, m_decodeBitstreamSize, true) ||
        (m_vkDevCtx.QueueSubmit2KHR(encodeQueue.queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) ||
        (m_vkDevCtx.QueueWaitIdle(encodeQueue.queue) != VK_SUCCESS)) {
        m_vkDevCtx.UnmapMemory(device, m_decodeBitstreamMemory);
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
    m_vkDevCtx.UnmapMemory(device, m_decodeBitstreamMemory);
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
bool SyncFdExportTest::RecordDecode(VkCommandBuffer commandBuffer) {
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (m_vkDevCtx.BeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
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
    m_vkDevCtx.CmdPipelineBarrier2KHR(commandBuffer, &dependency);

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
    m_vkDevCtx.CmdBeginVideoCodingKHR(commandBuffer, &beginCoding);

    VkVideoCodingControlInfoKHR control{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
    control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    m_vkDevCtx.CmdControlVideoCodingKHR(commandBuffer, &control);

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
    m_vkDevCtx.CmdDecodeVideoKHR(commandBuffer, &decodeInfo);

    VkVideoEndCodingInfoKHR endCoding{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    m_vkDevCtx.CmdEndVideoCodingKHR(commandBuffer, &endCoding);
    return m_vkDevCtx.EndCommandBuffer(commandBuffer) == VK_SUCCESS;
}

bool SyncFdExportTest::RunDecodeCases(const QueueUnderTest& decodeQueue, const QueueUnderTest& encodeQueue, std::string& whyNot) {
    VkDevice device = m_vkDevCtx.getDevice();
    if (!CreateDecodeResources(whyNot)) {
        return false;
    }

    VkCommandPool pools[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkCommandBuffer commandBuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    const QueueUnderTest* owners[2] = {&decodeQueue, &encodeQueue};
    bool ok = true;
    for (int i = 0; ok && (i < 2); i++) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = (uint32_t)owners[i]->family;
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        ok = (m_vkDevCtx.CreateCommandPool(device, &poolInfo, nullptr, &pools[i]) == VK_SUCCESS);
        allocInfo.commandPool = pools[i];
        ok = ok && (m_vkDevCtx.AllocateCommandBuffers(device, &allocInfo, &commandBuffers[i]) == VK_SUCCESS);
    }
    VkCommandBuffer decodeCommandBuffer = commandBuffers[0];
    VkCommandBuffer encodeCommandBuffer = commandBuffers[1];
    if (!ok) {
        whyNot = "cannot create the command buffers";
    } else {
        ok = EncodeDecodeInput(encodeQueue, encodeCommandBuffer, whyNot);
    }

    // The Vulkan decode queue decodes, then signals the exported semaphore.
    for (int r = 0; ok && (r < m_options.repeats); r++) {
        ok = RecordDecode(decodeCommandBuffer) && SubmitExportAndPoll(decodeQueue, decodeCommandBuffer, "h264-idr-decode", r);
    }

    // An encode batch exports a SYNC_FD while it is still pending; the decode batch imports it as its
    // wait and exports its own signal.
    for (int r = 0; ok && (r < m_options.repeats); r++) {
        VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkSemaphoreCreateInfo exportableInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exportInfo};
        VkSemaphoreCreateInfo plainInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore producer = VK_NULL_HANDLE, imported = VK_NULL_HANDLE;
        ok = (m_vkDevCtx.CreateSemaphore(device, &exportableInfo, nullptr, &producer) == VK_SUCCESS) &&
             (m_vkDevCtx.CreateSemaphore(device, &plainInfo, nullptr, &imported) == VK_SUCCESS) &&
             RecordEncode(encodeCommandBuffer, false, m_bitstream, m_bitstreamSize);

        VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signalInfo.semaphore = producer;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbInfo.commandBuffer = encodeCommandBuffer;
        VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &cbInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &signalInfo;
        ok = ok && (m_vkDevCtx.QueueSubmit2KHR(encodeQueue.queue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS);

        int fd = -1;
        VkSemaphoreGetFdInfoKHR getFdInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        getFdInfo.semaphore = producer;
        getFdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        ok = ok && (m_vkDevCtx.GetSemaphoreFdKHR(device, &getFdInfo, &fd) == VK_SUCCESS);

        // A successful import takes ownership of the fd.
        VkImportSemaphoreFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        importInfo.semaphore = imported;
        importInfo.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        importInfo.fd = fd;
        if (ok && (m_vkDevCtx.ImportSemaphoreFdKHR(device, &importInfo) != VK_SUCCESS)) {
            if (fd >= 0) {
                close(fd);
            }
            ok = false;
        }

        ok = ok && RecordDecode(decodeCommandBuffer) &&
             SubmitExportAndPoll(decodeQueue, decodeCommandBuffer, "h264-decode-after-import", r, imported);
        m_vkDevCtx.QueueWaitIdle(encodeQueue.queue);
        for (VkSemaphore semaphore : {producer, imported}) {
            if (semaphore != VK_NULL_HANDLE) {
                m_vkDevCtx.DestroySemaphore(device, semaphore, nullptr);
            }
        }
        if (!ok && whyNot.empty()) {
            whyNot = "a Vulkan call failed in the decode-after-import batch";
        }
    }
    if (!ok && whyNot.empty()) {
        whyNot = "a Vulkan call failed in the decode batch";
    }

    for (VkCommandPool pool : pools) {
        if (pool != VK_NULL_HANDLE) {
            m_vkDevCtx.DestroyCommandPool(device, pool, nullptr);
        }
    }
    return ok;
}

int SyncFdExportTest::Run() {
    VkDevice device = m_vkDevCtx.getDevice();
    const QueueUnderTest queues[] = {
        {"graphics", m_vkDevCtx.GetGfxQueue(), m_vkDevCtx.GetGfxQueueFamilyIdx()},
        {"compute", m_vkDevCtx.GetComputeQueue(), m_vkDevCtx.GetComputeQueueFamilyIdx()},
        {"transfer", m_vkDevCtx.GetTransferQueue(), m_vkDevCtx.GetTransferQueueFamilyIdx()},
        {"decode", m_vkDevCtx.GetVideoDecodeQueue(), m_vkDevCtx.GetVideoDecodeQueueFamilyIdx()},
        {"encode", m_vkDevCtx.GetVideoEncodeQueue(), m_vkDevCtx.GetVideoEncodeQueueFamilyIdx()},
    };

    for (const QueueUnderTest& q : queues) {
        if ((q.queue == VK_NULL_HANDLE) || (q.family < 0)) {
            std::printf("COULD NOT RUN: the device has no %s queue\n", q.name);
            return kExitCouldNotRun;
        }
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = (uint32_t)q.family;
        VkCommandPool pool = VK_NULL_HANDLE;
        if (m_vkDevCtx.CreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            std::printf("COULD NOT RUN: vkCreateCommandPool failed on the %s queue family\n", q.name);
            return kExitCouldNotRun;
        }
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        m_vkDevCtx.AllocateCommandBuffers(device, &allocInfo, &commandBuffer);

        bool ok = true;
        for (int r = 0; ok && (r < m_options.repeats); r++) {
            ok = SubmitExportAndPoll(q, VK_NULL_HANDLE, "no-cmdbuf", r);
        }
        for (int r = 0; ok && (r < m_options.repeats); r++) {
            VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            ok = (m_vkDevCtx.BeginCommandBuffer(commandBuffer, &beginInfo) == VK_SUCCESS) &&
                 (m_vkDevCtx.EndCommandBuffer(commandBuffer) == VK_SUCCESS) &&
                 SubmitExportAndPoll(q, commandBuffer, "empty-cmdbuf", r);
        }

        if (ok && (std::strcmp(q.name, "encode") == 0)) {
            std::string whyNot;
            if (!CreateEncodeResources(whyNot)) {
                std::printf("COULD NOT RUN: the encode case: %s\n", whyNot.c_str());
                m_vkDevCtx.DestroyCommandPool(device, pool, nullptr);
                return kExitCouldNotRun;
            }
            for (int r = 0; ok && (r < m_options.repeats); r++) {
                ok = RecordEncode(commandBuffer, r == 0, m_bitstream, m_bitstreamSize) &&
                     SubmitExportAndPoll(q, commandBuffer, "h264-idr-encode", r);
            }
        }
        m_vkDevCtx.DestroyCommandPool(device, pool, nullptr);
        if (!ok) {
            std::printf("COULD NOT RUN: a Vulkan call failed on the %s queue\n", q.name);
            return kExitCouldNotRun;
        }
    }

    std::string whyNot;
    if (!RunDecodeCases(queues[3], queues[4], whyNot)) {
        std::printf("COULD NOT RUN: the decode cases: %s\n", whyNot.c_str());
        return kExitCouldNotRun;
    }

    std::printf("%d exported SYNC_FDs, %d did not signal within %d ms\n", m_results.checks, m_results.failures,
                m_options.timeoutMs);
    std::printf("RESULT: %s\n", (m_results.failures == 0) ? "PASS" : "FAIL");
    return (m_results.failures == 0) ? kExitPass : kExitFail;
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
        "Usage: %s [--timeout-ms N] [--repeats N] [--device-id N] [--verbose]\n"
        "Exports a SYNC_FD from a semaphore each submission signals, on the graphics, compute,\n"
        "transfer, decode and encode queues, and checks that every fd becomes readable. The encode\n"
        "and decode queues also run an H.264 IDR encode and decode, and a decode that waits on a\n"
        "SYNC_FD imported from a pending encode.\n"
        "Exit codes: 0 pass, 1 an fd did not signal, 77 could not run, 64 bad usage.\n",
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
        } else if ((arg == "--timeout-ms") && (i + 1 < argc)) {
            options.timeoutMs = std::atoi(argv[++i]);
        } else if ((arg == "--repeats") && (i + 1 < argc)) {
            options.repeats = std::atoi(argv[++i]);
        } else if ((arg == "--device-id") && (i + 1 < argc)) {
            options.deviceId = std::atoi(argv[++i]);
        } else {
            PrintHelp(argv[0]);
            return kExitBadUsage;
        }
    }
    if ((options.timeoutMs <= 0) || (options.repeats <= 0)) {
        PrintHelp(argv[0]);
        return kExitBadUsage;
    }

    SyncFdExportTest test(options);
    std::string whyNot;
    int exitCode = kExitCouldNotRun;
    if (!test.Init(whyNot)) {
        std::printf("COULD NOT RUN: %s\n", whyNot.c_str());
    } else {
        exitCode = test.Run();
    }
    if ((exitCode == kExitCouldNotRun) && RequireGpu()) {
        std::printf("FAIL: the test could not run, and VVS_REQUIRE_GPU is set\n");
        return kExitFail;
    }
    return exitCode;
}

#else  // !defined(__linux__)

#include <cstdio>

int main() {
    std::printf("COULD NOT RUN: SYNC_FD is a Linux handle type\n");
    return 77;
}

#endif  // defined(__linux__)
