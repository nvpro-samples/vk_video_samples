/*
 * Copyright 2025 NVIDIA Corporation.
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
#include "vulkan_video_decoder.h"

#include "VkCodecUtils/DecoderConfig.h"
#include "VkVideoDecoder/VkVideoDecoder.h"
#include "VkCodecUtils/VulkanVideoProcessor.h"

class VulkanVideoDecoderImpl : public VulkanVideoDecoder {
public:

    virtual int32_t  GetWidth() const
    {
        return m_vulkanVideoProcessor->GetWidth();
    }

    virtual int32_t  GetHeight() const
    {
        return m_vulkanVideoProcessor->GetHeight();
    }

    virtual int32_t  GetBitDepth() const
    {
        return m_vulkanVideoProcessor->GetBitDepth();
    }

    virtual VkFormat GetFrameImageFormat() const
    {
        return m_vulkanVideoProcessor->GetFrameImageFormat();
    }

    virtual int32_t  GetNextFrame(VulkanDecodedFrame* pNewDecodedFrame, bool* endOfStream)
    {
        return m_vulkanVideoProcessor->GetNextFrame(pNewDecodedFrame, endOfStream);
    }

    virtual int32_t  ReleaseFrame(VulkanDecodedFrame* pDoneDecodedFrame)
    {
        return m_vulkanVideoProcessor->ReleaseFrame(pDoneDecodedFrame);
    }

    virtual VkVideoProfileInfoKHR GetVkProfile() const
    {
        return m_vulkanVideoProcessor->GetVkProfile();
    }

    virtual uint32_t GetProfileIdc() const
    {
        return m_vulkanVideoProcessor->GetProfileIdc();
    }

    virtual VkExtent3D GetVideoExtent() const
    {
        VkExtent3D extent {
            (uint32_t)m_vulkanVideoProcessor->GetWidth(),
            (uint32_t)m_vulkanVideoProcessor->GetHeight(),
            1
        };
        return extent;
    }

    VulkanVideoDecoderImpl(const char* programName)
    : m_refCount(0)
    , m_vkDevCtxt()
    , m_decoderConfig(programName)
    , m_decoder()
    , m_vulkanVideoProcessor()
    { }

    VkResult Initialize(VkInstance vkInstance, VkPhysicalDevice vkPhysicalDevice, VkDevice vkDevice,
                        VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer,
                        VkSharedBaseObj<VkVideoFrameOutput>& frameToFile,
                        const VkWsiDisplay* pWsiDisplay,
                        int argc, const char** argv);

    virtual ~VulkanVideoDecoderImpl() { }

    void Deinitialize()
    {

        if (m_decoderConfig.verbose) {
            std::cout << "Done processing " << 0UL << " input frames!" << std::endl
                      << std::endl;
        }

        m_decoder       = nullptr;
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
    std::atomic<int32_t>                  m_refCount;
    VulkanDeviceContext                   m_vkDevCtxt;
    DecoderConfig                         m_decoderConfig;
    VkSharedBaseObj<VkVideoDecoder>       m_decoder;
    VkSharedBaseObj<VulkanVideoProcessor> m_vulkanVideoProcessor;
};

VkResult VulkanVideoDecoderImpl::Initialize(VkInstance vkInstance,
                                            VkPhysicalDevice vkPhysicalDevice,
                                            VkDevice vkDevice,
                                            VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer,
                                            VkSharedBaseObj<VkVideoFrameOutput>& frameToFile,
                                            const VkWsiDisplay* pWsiDisplay,
                                            int argc, const char** argv)
{
    bool configResult = m_decoderConfig.ParseArgs(argc, argv);
    if (!configResult && (m_decoderConfig.help == true)) {
        return VK_SUCCESS;
    } else if (!configResult) {
        return VK_NOT_READY;
    }

    VkResult result = m_vkDevCtxt.InitVulkanDecoderDevice(m_decoderConfig.appName.c_str(),
                                                          vkInstance,
                                                          !m_decoderConfig.noPresent,
                                                          m_decoderConfig.directMode,
                                                          m_decoderConfig.validate,
                                                          m_decoderConfig.validateVerbose,
                                                          m_decoderConfig.verbose);

    if (result != VK_SUCCESS) {
        printf("Could not initialize the Vulkan decoder device!\n");
        return result;
    }

    const int32_t numDecodeQueues = ((m_decoderConfig.queueId != 0) ||
                                     (m_decoderConfig.enableHwLoadBalancing != 0)) ?
                                     -1 : // all available HW decoders
                                      1;  // only one HW decoder instance

    VkQueueFlags requestVideoDecodeQueueMask = VK_QUEUE_VIDEO_DECODE_BIT_KHR;

    if (m_decoderConfig.selectVideoWithComputeQueue) {
        requestVideoDecodeQueueMask |= VK_QUEUE_COMPUTE_BIT;
    }

    VkQueueFlags requestVideoComputeQueueMask = 0;
    if (m_decoderConfig.enablePostProcessFilter != -1) {
        requestVideoComputeQueueMask = VK_QUEUE_COMPUTE_BIT;
    }

    VkVideoCodecOperationFlagsKHR videoCodecOperation = videoStreamDemuxer->GetVideoCodec();

    const bool supportsShellPresent = ((!m_decoderConfig.noPresent == false) && (pWsiDisplay != nullptr));
    const bool createGraphicsQueue = supportsShellPresent ? true  : false;
    const bool createDisplayQueue  = supportsShellPresent ? true  : false;

    VkQueueFlags requestGraphicsQueueMask = 0;
    if (createGraphicsQueue) {
        requestGraphicsQueueMask = VK_QUEUE_GRAPHICS_BIT;
    }

    result = m_vkDevCtxt.InitPhysicalDevice(m_decoderConfig.deviceId, m_decoderConfig.deviceUUID,
                                            ( VK_QUEUE_TRANSFER_BIT |
                                              requestGraphicsQueueMask |
                                              requestVideoComputeQueueMask |
                                              requestVideoDecodeQueueMask),
                                            pWsiDisplay,
                                            requestVideoDecodeQueueMask,
                                            videoCodecOperation,
                                              0,
                                            VK_VIDEO_CODEC_OPERATION_NONE_KHR,
                                            vkPhysicalDevice);

    if (result != VK_SUCCESS) {

        assert(!"Can't initialize the Vulkan physical device!");
        return result;
    }

    m_vkDevCtxt.CreateVulkanDevice(numDecodeQueues,
                                   0, // num encode queues
                                   videoCodecOperation,
                                   // If no graphics or compute queue is requested, only video queues
                                   // will be created. Not all implementations support transfer on video queues,
                                   // so request a separate transfer queue for such implementations.
                                   ((m_vkDevCtxt.GetVideoDecodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) == 0), //  createTransferQueue
                                   createGraphicsQueue,
                                   createDisplayQueue,
                                   (requestVideoComputeQueueMask != 0),  // createComputeQueue
                                   vkDevice
                                   );

    if (result != VK_SUCCESS) {

        assert(!"Failed to create Vulkan device!");
        return result;
    }

    result = VulkanVideoProcessor::Create(m_decoderConfig, &m_vkDevCtxt, m_vulkanVideoProcessor);
    if (result != VK_SUCCESS) {
        return result;
    }

    int32_t initStatus = m_vulkanVideoProcessor->Initialize(&m_vkDevCtxt,
                                                            videoStreamDemuxer,
                                                            frameToFile,
                                                            m_decoderConfig);
    if (initStatus != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return result;
}

VK_VIDEO_DECODER_EXPORT
VkResult CreateVulkanVideoDecoder(VkInstance vkInstance, VkPhysicalDevice vkPhysicalDevice, VkDevice vkDevice,
                                  VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer,
                                  VkSharedBaseObj<VkVideoFrameOutput>& frameToFile,
                                  const VkWsiDisplay* pWsiDisplay,
                                  int argc, const char** argv,
                                  VkSharedBaseObj<VulkanVideoDecoder>& vulkanVideoDecoder)
{
    switch((uint32_t)videoStreamDemuxer->GetVideoCodec())
    {
        case VK_VIDEO_CODEC_OPERATION_NONE_KHR: // auto
        case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR:
        {

        }
        break;

    default:
        assert(!"Unsupported codec type!!!\n");
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    VkSharedBaseObj<VulkanVideoDecoderImpl> vulkanVideoDecoderObj( new VulkanVideoDecoderImpl(argv[0]));
    if (!vulkanVideoDecoderObj) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkResult result = vulkanVideoDecoderObj->Initialize(vkInstance, vkPhysicalDevice, vkDevice,
                                                        videoStreamDemuxer, frameToFile, pWsiDisplay,
                                                        argc, argv);
    if (result != VK_SUCCESS) {
        vulkanVideoDecoderObj = nullptr;
    } else {
        vulkanVideoDecoder = vulkanVideoDecoderObj;
    }

    return result;
}
