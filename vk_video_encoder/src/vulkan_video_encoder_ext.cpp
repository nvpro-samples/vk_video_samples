/*
 * Copyright 2024-2025 NVIDIA Corporation.
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

#include <atomic>
#include <iostream>
#include <cstring>
#include <deque>
#include <mutex>

#include "vulkan_video_encoder_ext.h"
#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkVideoEncoder/VkVideoEncoder.h"

//=============================================================================
// VulkanVideoEncoderExtImpl - Concrete implementation of VulkanVideoEncoderExt
//
// Wraps the internal VkVideoEncoder with the public service-oriented API.
// Uses VulkanDeviceContext for Vulkan init and VkVideoEncoder for encoding.
//=============================================================================

class VulkanVideoEncoderExtImpl : public VulkanVideoEncoderExt {
public:
    VulkanVideoEncoderExtImpl()
        : m_refCount(0)
        , m_vkDevCtx()
        , m_encoderConfig()
        , m_encoder()
        , m_initialized(false)
        , m_framesSubmitted(0)
    { }

    virtual ~VulkanVideoEncoderExtImpl() {
        Deinitialize();
    }

    //=========================================================================
    // VkVideoRefCountBase
    //=========================================================================
    int32_t AddRef() override { return ++m_refCount; }

    int32_t Release() override {
        uint32_t ret = --m_refCount;
        if (ret == 0) {
            delete this;
        }
        return ret;
    }

    //=========================================================================
    // VulkanVideoEncoder (base interface - file-based, backward compatible)
    //=========================================================================
    VkResult Initialize(VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
                        int argc, const char** argv) override;

    int64_t GetNumberOfFrames() override {
        return m_encoderConfig ? m_encoderConfig->numFrames : 0;
    }

    VkResult EncodeNextFrame(int64_t& frameNumEncoded) override;
    VkResult GetBitstream() override { return VK_SUCCESS; }

    //=========================================================================
    // VulkanVideoEncoderExt (extended interface - external frame input)
    //=========================================================================
    VkResult InitializeExt(const VkVideoEncoderConfig& config) override;
    VkResult SubmitExternalFrame(const VkVideoEncodeInputFrame& frame,
                                VkSemaphore* pStagingCompleteSemaphore = nullptr) override;
    VkResult PollEncodeComplete(uint64_t frameId) override;
    VkResult GetEncodedFrame(VkVideoEncodeResult& result) override;
    void     ReleaseEncodedFrame(uint64_t frameId) override;
    VkFence  GetEncodeFence(uint64_t frameId) override;
    VkResult Flush() override;
    VkResult Reconfigure(const VkVideoEncoderConfig& config) override;
    VkBool32 SupportsFormat(VkFormat inputFormat) const override;
    uint32_t GetMaxWidth() const override;
    uint32_t GetMaxHeight() const override;

private:
    void Deinitialize();

    // Build EncoderConfig from the structured VkVideoEncoderConfig
    VkResult BuildEncoderConfig(const VkVideoEncoderConfig& extConfig,
                                VkVideoCodecOperationFlagBitsKHR codecOp,
                                VkSharedBaseObj<EncoderConfig>& outConfig);

    // Initialize VulkanDeviceContext with encode queue support
    VkResult InitVulkanDevice(VkVideoCodecOperationFlagBitsKHR codecOp,
                              const VkVideoEncoderConfig& config);

    std::atomic<int32_t>             m_refCount;
    VulkanDeviceContext              m_vkDevCtx;
    VkSharedBaseObj<EncoderConfig>   m_encoderConfig;
    VkSharedBaseObj<VkVideoEncoder>  m_encoder;
    bool                             m_initialized;
    uint64_t                         m_framesSubmitted;

    // Tracking submitted frames for async retrieval
    struct PendingFrame {
        uint64_t frameId;
        uint64_t pts;
        VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> encodeFrameInfo;
    };
    std::deque<PendingFrame>  m_pendingFrames;
    std::mutex                m_pendingMutex;
};

//=============================================================================
// Map VkVideoEncoderConfig codec field to VkVideoCodecOperationFlagBitsKHR
//=============================================================================
static VkVideoCodecOperationFlagBitsKHR MapCodecOperation(
    VkVideoCodecOperationFlagBitsKHR codec)
{
    switch ((uint32_t)codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            return codec;
        default:
            return VK_VIDEO_CODEC_OPERATION_NONE_KHR;
    }
}

//=============================================================================
// Build EncoderConfig from structured config
//=============================================================================
VkResult VulkanVideoEncoderExtImpl::BuildEncoderConfig(
    const VkVideoEncoderConfig& extConfig,
    VkVideoCodecOperationFlagBitsKHR codecOp,
    VkSharedBaseObj<EncoderConfig>& outConfig)
{
    // Create the codec-specific config via the static factory
    // We'll build argc/argv from the structured config for now.
    // This is a bridge until EncoderConfig supports direct field assignment.

    std::vector<std::string> argStrings;
    argStrings.push_back("encoder"); // argv[0]

    // Codec (required by CreateCodecConfig to select H264/H265/AV1 subclass)
    switch ((uint32_t)codecOp) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            argStrings.push_back("-c"); argStrings.push_back("h264"); break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            argStrings.push_back("-c"); argStrings.push_back("h265"); break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            argStrings.push_back("-c"); argStrings.push_back("av1"); break;
        default: break;
    }

    // No -i flag: external frame input mode. ParseArguments handles this
    // by skipping file handler setup when inputFileHandler.HasFileName() is false.

    // Resolution
    argStrings.push_back("--inputWidth");
    argStrings.push_back(std::to_string(extConfig.inputWidth));
    argStrings.push_back("--inputHeight");
    argStrings.push_back(std::to_string(extConfig.inputHeight));
    argStrings.push_back("--encodeWidth");
    argStrings.push_back(std::to_string(extConfig.encodeWidth));
    argStrings.push_back("--encodeHeight");
    argStrings.push_back(std::to_string(extConfig.encodeHeight));

    // Frame rate: no CLI arg for this in ParseArguments — set via member directly after config

    // Bitrate (note: lowercase 'r' — --averageBitrate, not --averageBitRate)
    if (extConfig.averageBitrate > 0) {
        argStrings.push_back("--averageBitrate");
        argStrings.push_back(std::to_string(extConfig.averageBitrate));
    }
    if (extConfig.maxBitrate > 0) {
        argStrings.push_back("--maxBitrate");
        argStrings.push_back(std::to_string(extConfig.maxBitrate));
    }

    // GOP
    if (extConfig.gopLength > 0) {
        argStrings.push_back("--gopFrameCount");
        argStrings.push_back(std::to_string(extConfig.gopLength));
    }
    if (extConfig.consecutiveBFrames > 0) {
        argStrings.push_back("--consecutiveBFrameCount");
        argStrings.push_back(std::to_string(extConfig.consecutiveBFrames));
    }

    // QP — only pass when explicitly set (> 0); 0 is default/unused
    if (extConfig.constQpI > 0) {
        argStrings.push_back("--qpI");
        argStrings.push_back(std::to_string(extConfig.constQpI));
    }
    if (extConfig.constQpP > 0) {
        argStrings.push_back("--qpP");
        argStrings.push_back(std::to_string(extConfig.constQpP));
    }

    // Quality
    if (extConfig.qualityLevel > 0) {
        argStrings.push_back("--qualityLevel");
        argStrings.push_back(std::to_string(extConfig.qualityLevel));
    }

    // Verbose / Validate — these flags are not recognized by ParseArguments,
    // they would fall to the unknown-arg path. Skip for now.
    // if (extConfig.verbose) argStrings.push_back("--verbose");
    // if (extConfig.validate) argStrings.push_back("--validate");

    // Large frame count for streaming mode (not UINT32_MAX — some code paths overflow)
    argStrings.push_back("--numFrames");
    argStrings.push_back("1000000");
    argStrings.push_back("--repeatInputFrames");

    // Output file path (per-encoder isolation; when set, overrides encoder default)
    if (extConfig.outputPath && extConfig.outputPath[0] != '\0') {
        argStrings.push_back("--output");
        argStrings.push_back(extConfig.outputPath);
    }

    // No input file (external frame input)
    // Don't pass --input since we'll use SetExternalInputFrame

    // Build argc/argv
    std::vector<const char*> argv;
    for (auto& s : argStrings) {
        argv.push_back(s.c_str());
    }

    // Debug: dump argv for troubleshooting
    std::cout << "[VulkanVideoEncoderExt] CreateCodecConfig argv (" << argv.size() << "):\n";
    for (size_t i = 0; i < argv.size(); i++) {
        std::cout << "  [" << i << "] " << argv[i] << "\n";
    }

    return EncoderConfig::CreateCodecConfig(
        static_cast<int>(argv.size()), argv.data(), outConfig);
}

//=============================================================================
// Initialize VulkanDeviceContext
//=============================================================================
VkResult VulkanVideoEncoderExtImpl::InitVulkanDevice(
    VkVideoCodecOperationFlagBitsKHR codecOp,
    const VkVideoEncoderConfig& config)
{
    static const char* const requiredInstanceLayers[] = {
        "VK_LAYER_KHRONOS_validation",
        nullptr
    };

    static const char* const requiredInstanceExtensions[] = {
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
        nullptr
    };

    static const char* const requiredDeviceExtension[] = {
#if defined(__linux) || defined(__linux__) || defined(linux)
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
#endif
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        nullptr
    };

    static const char* const optionalDeviceExtension[] = {
        VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        nullptr
    };

    if (config.validate) {
        m_vkDevCtx.AddReqInstanceLayers(requiredInstanceLayers);
        m_vkDevCtx.AddReqInstanceExtensions(requiredInstanceExtensions);
    }

    m_vkDevCtx.AddReqDeviceExtensions(requiredDeviceExtension);
    m_vkDevCtx.AddOptDeviceExtensions(optionalDeviceExtension);

    VkResult result = m_vkDevCtx.InitVulkanDevice("VulkanVideoEncoderExt",
                                                    VK_NULL_HANDLE,
                                                    config.verbose);
    if (result != VK_SUCCESS) {
        std::cerr << "[EncoderExt] InitVulkanDevice failed: " << result << std::endl;
        return result;
    }

    fprintf(stderr, "[EncoderExt] InitDebugReport...\n"); fflush(stderr);
    result = m_vkDevCtx.InitDebugReport(config.validate, config.verbose && config.validate);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[EncoderExt] InitDebugReport failed: %d\n", (int)result); fflush(stderr);
        return result;
    }
    fprintf(stderr, "[EncoderExt] InitDebugReport OK\n"); fflush(stderr);

    VkQueueFlags requestVideoEncodeQueueMask = VK_QUEUE_VIDEO_ENCODE_BIT_KHR;
    VkQueueFlags requestVideoComputeQueueMask = 0;
    if (config.enablePreprocessFilter) {
        requestVideoComputeQueueMask = VK_QUEUE_COMPUTE_BIT;
    }

    // Use UUID if provided, otherwise auto-select
    const uint8_t* gpuUUID = nullptr;
    uint8_t zeroUUID[VK_UUID_SIZE] = {};
    if (memcmp(config.gpuUUID, zeroUUID, VK_UUID_SIZE) != 0) {
        gpuUUID = config.gpuUUID;
    }

    fprintf(stderr, "[EncoderExt] InitPhysicalDevice...\n"); fflush(stderr);
    result = m_vkDevCtx.InitPhysicalDevice(
        config.deviceId, gpuUUID,
        (requestVideoComputeQueueMask | requestVideoEncodeQueueMask | VK_QUEUE_TRANSFER_BIT),
        nullptr, 0,
        VK_VIDEO_CODEC_OPERATION_NONE_KHR,
        requestVideoEncodeQueueMask,
        codecOp);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[EncoderExt] InitPhysicalDevice failed: %d\n", (int)result); fflush(stderr);
        return result;
    }
    fprintf(stderr, "[EncoderExt] InitPhysicalDevice OK\n"); fflush(stderr);

    bool needTransferQueue = ((m_vkDevCtx.GetVideoEncodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) == 0);
    bool needComputeQueue = (config.enablePreprocessFilter == VK_TRUE);

    fprintf(stderr, "[EncoderExt] CreateVulkanDevice...\n"); fflush(stderr);
    result = m_vkDevCtx.CreateVulkanDevice(
        0,            // numDecodeQueues
        1,            // numEncodeQueues
        codecOp,
        needTransferQueue,
        false,        // createGraphicsQueue
        false,        // createDisplayQueue
        needComputeQueue);
    if (result != VK_SUCCESS) {
        std::cerr << "[EncoderExt] CreateVulkanDevice failed: " << result << std::endl;
        return result;
    }

    return VK_SUCCESS;
}

//=============================================================================
// VulkanVideoEncoder base interface (file-based, for backward compatibility)
//=============================================================================

VkResult VulkanVideoEncoderExtImpl::Initialize(
    VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
    int argc, const char** argv)
{
    // Delegate to the file-based path
    VkResult result = EncoderConfig::CreateCodecConfig(argc, argv, m_encoderConfig);
    if (result != VK_SUCCESS) return result;

    result = InitVulkanDevice(videoCodecOperation,
        VkVideoEncoderConfig{
            .codec = videoCodecOperation,
            .encodeWidth = m_encoderConfig->encodeWidth,
            .encodeHeight = m_encoderConfig->encodeHeight,
            .verbose = m_encoderConfig->verbose ? VK_TRUE : VK_FALSE,
            .validate = m_encoderConfig->validate ? VK_TRUE : VK_FALSE,
        });
    if (result != VK_SUCCESS) return result;

    result = VkVideoEncoder::CreateVideoEncoder(&m_vkDevCtx, m_encoderConfig, m_encoder);
    if (result != VK_SUCCESS) return result;

    m_initialized = true;
    return VK_SUCCESS;
}

VkResult VulkanVideoEncoderExtImpl::EncodeNextFrame(int64_t& frameNumEncoded)
{
    if (!m_initialized || !m_encoder) return VK_ERROR_NOT_PERMITTED_KHR;

    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> encodeFrameInfo;
    m_encoder->GetAvailablePoolNode(encodeFrameInfo);
    if (!encodeFrameInfo) return VK_ERROR_OUT_OF_POOL_MEMORY;

    VkResult result = m_encoder->LoadNextFrame(encodeFrameInfo);
    if (result != VK_SUCCESS) return result;

    frameNumEncoded = encodeFrameInfo->frameInputOrderNum;
    return VK_SUCCESS;
}

//=============================================================================
// VulkanVideoEncoderExt interface (external frame input)
//=============================================================================

VkResult VulkanVideoEncoderExtImpl::InitializeExt(const VkVideoEncoderConfig& config)
{
    VkVideoCodecOperationFlagBitsKHR codecOp = MapCodecOperation(config.codec);
    if (codecOp == VK_VIDEO_CODEC_OPERATION_NONE_KHR) {
        std::cerr << "[EncoderExt] Unsupported codec: " << (uint32_t)config.codec << std::endl;
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    // Build EncoderConfig from structured config
    fprintf(stderr, "[EncoderExt] Step 1: BuildEncoderConfig...\n"); fflush(stderr);
    VkResult result = BuildEncoderConfig(config, codecOp, m_encoderConfig);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[EncoderExt] BuildEncoderConfig failed: %d\n", (int)result); fflush(stderr);
        return result;
    }
    fprintf(stderr, "[EncoderExt] Step 1 OK\n"); fflush(stderr);

    // Initialize Vulkan device with encode queue
    fprintf(stderr, "[EncoderExt] Step 2a: InitVulkanDevice...\n"); fflush(stderr);
    result = InitVulkanDevice(codecOp, config);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[EncoderExt] InitVulkanDevice failed: %d\n", (int)result); fflush(stderr);
        return result;
    }
    fprintf(stderr, "[EncoderExt] Step 2a OK\n"); fflush(stderr);

    fprintf(stderr, "[EncoderExt] Step 2b: InitPhysicalDevice...\n"); fflush(stderr);
    // (InitPhysicalDevice and CreateVulkanDevice are called inside InitVulkanDevice above)
    // If we got here, the full device init completed.
    fprintf(stderr, "[EncoderExt] Step 2b: Device initialized with encode queues\n"); fflush(stderr);

    // Create the internal encoder
    fprintf(stderr, "[EncoderExt] Step 3: CreateVideoEncoder...\n"); fflush(stderr);
    result = VkVideoEncoder::CreateVideoEncoder(&m_vkDevCtx, m_encoderConfig, m_encoder);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[EncoderExt] CreateVideoEncoder failed: %d\n", (int)result); fflush(stderr);
        return result;
    }
    fprintf(stderr, "[EncoderExt] Step 3 OK\n"); fflush(stderr);

    m_initialized = true;

    if (config.verbose) {
        std::cout << "[EncoderExt] Initialized: "
                  << config.encodeWidth << "x" << config.encodeHeight
                  << " codec=0x" << std::hex << (uint32_t)codecOp << std::dec
                  << " bitrate=" << config.averageBitrate
                  << " gop=" << config.gopLength
                  << std::endl;
    }

    return VK_SUCCESS;
}

VkResult VulkanVideoEncoderExtImpl::SubmitExternalFrame(
    const VkVideoEncodeInputFrame& frame,
    VkSemaphore* pStagingCompleteSemaphore)
{
    if (!m_initialized || !m_encoder) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }

    // Get an available frame info node from the pool
    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> encodeFrameInfo;
    bool gotNode = m_encoder->GetAvailablePoolNode(encodeFrameInfo);
    if (!gotNode || !encodeFrameInfo) {
        return VK_NOT_READY; // Pool is full, try again later
    }

    // Build pipeline stage masks (default to TRANSFER for all)
    std::vector<VkPipelineStageFlags2> waitDstStageMasks(frame.waitSemaphoreCount,
                                                          VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR);

    VkResult result = m_encoder->SetExternalInputFrame(
        encodeFrameInfo,
        frame.image,
        VK_NULL_HANDLE,  // memory (non-owning, encoder doesn't need it)
        frame.format,
        frame.width, frame.height,
        frame.imageTiling,
        frame.frameId,
        frame.pts,
        false,  // isLastFrame (caller controls this externally)
        frame.waitSemaphoreCount,
        frame.pWaitSemaphores,
        frame.pWaitSemaphoreValues,
        waitDstStageMasks.data(),
        frame.signalSemaphoreCount,
        frame.pSignalSemaphores,
        frame.pSignalSemaphoreValues);

    if (result == VK_SUCCESS) {
        // Return the semaphore that signals when the encoder is done
        // reading the external input image. The caller (encoder service)
        // uses this to chain the display blit after the encode, then
        // signals the release semaphore from the display submit.
        //
        // Path A (direct encode): encodeCmdBuffer's semaphore, signaled
        //   after vkCmdEncodeVideoKHR reads the input.
        // Path B/C (staging): inputCmdBuffer's semaphore, signaled
        //   after the staging copy reads the input.
        if (pStagingCompleteSemaphore) {
            if (encodeFrameInfo->inputCmdBuffer) {
                // Paths B/C: staging copy was done, signal from inputCmdBuffer
                *pStagingCompleteSemaphore = encodeFrameInfo->inputCmdBuffer->GetSemaphore();
            } else if (encodeFrameInfo->encodeCmdBuffer) {
                // Path A: direct encode, signal from encodeCmdBuffer
                *pStagingCompleteSemaphore = encodeFrameInfo->encodeCmdBuffer->GetSemaphore();
            } else {
                *pStagingCompleteSemaphore = VK_NULL_HANDLE;
            }
        }

        // Track for async retrieval
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingFrames.push_back({frame.frameId, frame.pts, encodeFrameInfo});
        m_framesSubmitted++;
    }

    return result;
}

VkResult VulkanVideoEncoderExtImpl::PollEncodeComplete(uint64_t frameId)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto& pending : m_pendingFrames) {
        if (pending.frameId == frameId) {
            if (pending.encodeFrameInfo->encodeCmdBuffer) {
                VkFence fence = pending.encodeFrameInfo->encodeCmdBuffer->GetFence();
                if (fence != VK_NULL_HANDLE) {
                    VkResult status = m_vkDevCtx.GetFenceStatus(m_vkDevCtx, fence);
                    return (status == VK_SUCCESS) ? VK_SUCCESS : VK_NOT_READY;
                }
            }
            return VK_NOT_READY;
        }
    }
    return VK_ERROR_UNKNOWN; // frameId not found
}

VkResult VulkanVideoEncoderExtImpl::GetEncodedFrame(VkVideoEncodeResult& result)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    if (m_pendingFrames.empty()) {
        return VK_NOT_READY;
    }

    // Check the oldest pending frame (FIFO)
    auto& front = m_pendingFrames.front();
    if (!front.encodeFrameInfo->encodeCmdBuffer) {
        return VK_NOT_READY;
    }

    VkFence fence = front.encodeFrameInfo->encodeCmdBuffer->GetFence();
    if (fence == VK_NULL_HANDLE) {
        return VK_NOT_READY;
    }

    VkResult fenceStatus = m_vkDevCtx.GetFenceStatus(m_vkDevCtx, fence);
    if (fenceStatus != VK_SUCCESS) {
        return VK_NOT_READY;
    }

    // Frame is complete - fill the result
    result.frameId = front.frameId;
    result.pts = front.pts;
    result.dts = 0; // TODO: compute from encode order
    result.pBitstreamData = nullptr; // Bitstream is in file for now
    result.bitstreamSize = 0;
    result.pictureType = 0; // TODO: extract from GOP position
    result.isIDR = VK_FALSE;
    result.temporalLayerId = 0;
    result.status = VK_SUCCESS;

    // Note: the frame stays in m_pendingFrames until ReleaseEncodedFrame()
    return VK_SUCCESS;
}

void VulkanVideoEncoderExtImpl::ReleaseEncodedFrame(uint64_t frameId)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto it = m_pendingFrames.begin(); it != m_pendingFrames.end(); ++it) {
        if (it->frameId == frameId) {
            // Release the encodeFrameInfo ref - returns resources to pools
            it->encodeFrameInfo = nullptr;
            m_pendingFrames.erase(it);
            return;
        }
    }
}

VkFence VulkanVideoEncoderExtImpl::GetEncodeFence(uint64_t frameId)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto& pending : m_pendingFrames) {
        if (pending.frameId == frameId && pending.encodeFrameInfo->encodeCmdBuffer) {
            return pending.encodeFrameInfo->encodeCmdBuffer->GetFence();
        }
    }
    return VK_NULL_HANDLE;
}

VkResult VulkanVideoEncoderExtImpl::Flush()
{
    if (!m_initialized || !m_encoder) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }

    // Wait for all pending encodes to complete
    m_encoder->WaitForThreadsToComplete();

    // Drain the pending queue
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto& pending : m_pendingFrames) {
        if (pending.encodeFrameInfo && pending.encodeFrameInfo->encodeCmdBuffer) {
            pending.encodeFrameInfo->encodeCmdBuffer->ResetCommandBuffer(
                true, "EncoderExtFlush");
        }
    }

    return VK_SUCCESS;
}

VkResult VulkanVideoEncoderExtImpl::Reconfigure(const VkVideoEncoderConfig& config)
{
    // TODO: Implement dynamic reconfiguration
    // For now, rate control changes require session reset
    (void)config;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkBool32 VulkanVideoEncoderExtImpl::SupportsFormat(VkFormat inputFormat) const
{
    // Formats the encoder can handle (directly or via filter)
    switch (inputFormat) {
        // Directly encodable (YCbCr 4:2:0)
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
        // Via filter (RGBA)
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return VK_TRUE;
        default:
            return VK_FALSE;
    }
}

uint32_t VulkanVideoEncoderExtImpl::GetMaxWidth() const
{
    // TODO: query from device capabilities
    return 8192;
}

uint32_t VulkanVideoEncoderExtImpl::GetMaxHeight() const
{
    // TODO: query from device capabilities
    return 8192;
}

void VulkanVideoEncoderExtImpl::Deinitialize()
{
    if (m_encoder) {
        m_encoder->WaitForThreadsToComplete();
    }

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingFrames.clear();
    }

    m_encoder = nullptr;
    m_encoderConfig = nullptr;
    m_initialized = false;
}

//=============================================================================
// Factory function
//=============================================================================

VK_VIDEO_ENCODER_EXPORT
VkResult CreateVulkanVideoEncoderExt(
    VkSharedBaseObj<VulkanVideoEncoderExt>& vulkanVideoEncoder)
{
    VkSharedBaseObj<VulkanVideoEncoderExt> impl(new VulkanVideoEncoderExtImpl());
    if (!impl) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    vulkanVideoEncoder = impl;
    return VK_SUCCESS;
}
