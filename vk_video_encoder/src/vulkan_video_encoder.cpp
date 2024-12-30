/*
 * Copyright 2024 NVIDIA Corporation.
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
#include "vulkan_video_encoder.h"

#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkVideoEncoder/VkVideoEncoder.h"

class VulkanVideoEncoderImpl : public VulkanVideoEncoder {
public:
    virtual VkResult Initialize(VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
                                int argc, const char** argv);
    virtual int64_t GetNumberOfFrames()
    {
        return m_encoderConfig->numFrames;
    }
    virtual VkResult EncodeNextFrame(int64_t& frameNumEncoded);
    virtual VkResult GetBitstream() { return VK_SUCCESS; }

    VulkanVideoEncoderImpl()
    : m_refCount(0)
    , m_vkDevCtxt()
    , m_encoderConfig()
    , m_encoder()
    , m_lastFrameIndex(0)
    { }

    virtual ~VulkanVideoEncoderImpl() { }

    void Deinitialize()
    {
        m_encoder->WaitForThreadsToComplete();

        if (m_encoderConfig->verbose) {
            std::cout << "Done processing " << m_lastFrameIndex << " input frames!" << std::endl
                      << "Encoded file's location is at " << m_encoderConfig->outputFileHandler.GetFileName()
                      << std::endl;
        }

        m_encoder       = nullptr;
        m_encoderConfig = nullptr;
    }

    int32_t AddRef()
    {
        return ++m_refCount;
    }

    int32_t Release()
    {
        uint32_t ret;
        ret = --m_refCount;
        // Destroy the device if refcount reaches zero
        if (ret == 0) {
            Deinitialize();
            delete this;
        }
        return ret;
    }

private:
    std::atomic<int32_t>             m_refCount;
    VulkanDeviceContext              m_vkDevCtxt;
    VkSharedBaseObj<EncoderConfig>   m_encoderConfig;
    VkSharedBaseObj<VkVideoEncoder>  m_encoder;
    uint32_t                         m_lastFrameIndex;
};

VkResult VulkanVideoEncoderImpl::Initialize(VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
                                            int argc, const char** argv)
{
    VkResult result = EncoderConfig::CreateCodecConfig(argc, argv, m_encoderConfig);
    if (VK_SUCCESS != result) {
        return result;
    }

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

    static const char* const optinalDeviceExtension[] = {
        VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        nullptr
    };

    if (m_encoderConfig->validate) {
        m_vkDevCtxt.AddReqInstanceLayers(requiredInstanceLayers);
        m_vkDevCtxt.AddReqInstanceExtensions(requiredInstanceExtensions);
    }

    m_vkDevCtxt.AddReqDeviceExtensions(requiredDeviceExtension);
    m_vkDevCtxt.AddOptDeviceExtensions(optinalDeviceExtension);

    result = m_vkDevCtxt.InitVulkanDevice(m_encoderConfig->appName.c_str(), VK_NULL_HANDLE,
                                          m_encoderConfig->verbose);
    if (result != VK_SUCCESS) {
        printf("Could not initialize the Vulkan device!\n");
        return result;
    }

    result = m_vkDevCtxt.InitDebugReport(m_encoderConfig->validate,
                                         m_encoderConfig->validateVerbose);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkQueueFlags requestVideoEncodeQueueMask = VK_QUEUE_VIDEO_ENCODE_BIT_KHR;

    if (m_encoderConfig->selectVideoWithComputeQueue) {
        requestVideoEncodeQueueMask |= VK_QUEUE_COMPUTE_BIT;
    }

    VkQueueFlags requestVideoComputeQueueMask = 0;
    if (m_encoderConfig->enablePreprocessComputeFilter == VK_TRUE) {
        requestVideoComputeQueueMask = VK_QUEUE_COMPUTE_BIT;
    }

    // No display presentation and no decoder - just the encoder
    result = m_vkDevCtxt.InitPhysicalDevice(m_encoderConfig->deviceId, m_encoderConfig->deviceUUID,
                                            ( requestVideoComputeQueueMask |
                                              requestVideoEncodeQueueMask  |
                                              VK_QUEUE_TRANSFER_BIT),
                                            nullptr,
                                            0,
                                            VK_VIDEO_CODEC_OPERATION_NONE_KHR,
                                            requestVideoEncodeQueueMask,
                                            videoCodecOperation);
    if (result != VK_SUCCESS) {

        assert(!"Can't initialize the Vulkan physical device!");
        return result;
    }

    const int32_t numEncodeQueues = ((m_encoderConfig->queueId != 0) ||
                                     (m_encoderConfig->enableHwLoadBalancing != 0)) ?
                                     -1 : // all available HW encoders
                                      1;  // only one HW encoder instance

    result = m_vkDevCtxt.CreateVulkanDevice(0, // num decode queues
                                            numEncodeQueues,     // num encode queues
                                            videoCodecOperation,
                                            // If no graphics or compute queue is requested, only video queues
                                            // will be created. Not all implementations support transfer on video queues,
                                            // so request a separate transfer queue for such implementations.
                                            ((m_vkDevCtxt.GetVideoEncodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) == 0), //  createTransferQueue
                                            false, // createGraphicsQueue
                                            false, // createDisplayQueue
                                            ((m_encoderConfig->selectVideoWithComputeQueue == 1) ||  // createComputeQueue
                                             (m_encoderConfig->enablePreprocessComputeFilter == VK_TRUE))
                                          );
    if (result != VK_SUCCESS) {

        assert(!"Failed to create Vulkan device!");
        return result;
    }

    result = VkVideoEncoder::CreateVideoEncoder(&m_vkDevCtxt, m_encoderConfig, m_encoder);
    if (result != VK_SUCCESS) {
        assert(!"Can't initialize the Vulkan physical device!");
        return result;
    }

    return result;
}

VkResult VulkanVideoEncoderImpl::EncodeNextFrame(int64_t& frameNumEncoded)
{
    if (m_lastFrameIndex >= m_encoderConfig->numFrames) {
        return VK_ERROR_TOO_MANY_OBJECTS;
    }

    if (m_encoderConfig->verboseFrameStruct) {
        std::cout << "####################################################################################" << std::endl
                  << "Start processing current input frame index: " << m_lastFrameIndex << std::endl;
    }

    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> encodeFrameInfo;
    m_encoder->GetAvailablePoolNode(encodeFrameInfo);
    assert(encodeFrameInfo);
    // load frame data from the file
    VkResult result = m_encoder->LoadNextFrame(encodeFrameInfo);
    if (result != VK_SUCCESS) {
        std::cout << "ERROR processing input frame index: " << m_lastFrameIndex << std::endl;
        return result;
    }

    frameNumEncoded = encodeFrameInfo->frameInputOrderNum;

    if (m_encoderConfig->verboseFrameStruct) {
        std::cout << "End processing current input frame index: " << m_lastFrameIndex << std::endl;
    }

    m_lastFrameIndex++;

    return result;
}

VK_VIDEO_ENCODER_EXPORT
VkResult CreateVulkanVideoEncoder(VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
                                  int argc, const char** argv,
                                  VkSharedBaseObj<VulkanVideoEncoder>& vulkanVideoEncoder)
{
    switch((uint32_t)videoCodecOperation)
    {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
        {

        }
        break;

    default:
        assert(!"Unsupported codec type!!!\n");
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    VkSharedBaseObj<VulkanVideoEncoder> vulkanVideoEncoderObj( new VulkanVideoEncoderImpl());
    if (!vulkanVideoEncoderObj) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkResult result = vulkanVideoEncoderObj->Initialize(videoCodecOperation, argc, argv);
    if (result != VK_SUCCESS) {
        vulkanVideoEncoderObj = nullptr;
    } else {
        vulkanVideoEncoder = vulkanVideoEncoderObj;
    }

    return result;
}
