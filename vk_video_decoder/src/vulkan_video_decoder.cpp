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
#include "VkCodecUtils/ProgramConfig.h"
#include "VkVideoDecoder/VkVideoDecoder.h"

// To remove
#include "VkCodecUtils/VulkanVideoProcessor.h"
#include "VkShell/Shell.h"
#include "VkCodecUtils/VulkanDecoderFrameProcessor.h"

class VulkanVideoDecoderImpl : public VulkanVideoDecoder {
public:
    virtual VkResult Initialize(VkInstance vkInstance, VkPhysicalDevice vkPhysicalDevice, VkDevice vkDevice,
                                VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer,
                                int argc, const char** argv);

    virtual int64_t GetMaxNumberOfFrames() const
    {
        return m_decoderConfig.maxFrameCount;
    }

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
        VkExtent3D extent ({ (uint32_t)m_vulkanVideoProcessor->GetWidth(),
                             (uint32_t)m_vulkanVideoProcessor->GetHeight(),
                             (uint32_t)1
                           });
        return extent;
    }

    virtual int32_t  ParserProcessNextDataChunk()
    {
        return m_vulkanVideoProcessor->ParserProcessNextDataChunk();
    }

    virtual uint32_t RestartStream(int64_t& bitstreamOffset)
    {
        return m_vulkanVideoProcessor->Restart(bitstreamOffset);
    }

    virtual size_t OutputFrameToFile(VulkanDecodedFrame* pNewDecodedFrame)
    {
        return m_vulkanVideoProcessor->OutputFrameToFile(pNewDecodedFrame);
    }

    VulkanVideoDecoderImpl(const char* programName)
    : m_refCount(0)
    , m_vkDevCtxt()
    , m_decoderConfig(programName)
    , m_decoder()
    , m_vulkanVideoProcessor()
    , m_frameProcessor()
    { }

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
    ProgramConfig                         m_decoderConfig;
    VkSharedBaseObj<VkVideoDecoder>       m_decoder;
    VkSharedBaseObj<VulkanVideoProcessor> m_vulkanVideoProcessor;
    DecoderFrameProcessorState            m_frameProcessor;
};

VkResult VulkanVideoDecoderImpl::Initialize(VkInstance vkInstance, VkPhysicalDevice vkPhysicalDevice, VkDevice vkDevice,
                                            VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer,
                                            int argc, const char** argv)
{
    const bool libraryMode = true;

    m_decoderConfig.ParseArgs(argc, argv);

    // In the regular application use case the CRC output variables are allocated here and also output as part of main.
    // In the library case it is up to the caller of the library to allocate the values and initialize them.
    std::vector<uint32_t> crcAllocation;
    crcAllocation.resize(m_decoderConfig.crcInitValue.size());
    if (crcAllocation.empty() == false) {
        m_decoderConfig.crcOutput = &crcAllocation[0];
        for (size_t i = 0; i < m_decoderConfig.crcInitValue.size(); i += 1) {
            crcAllocation[i] = m_decoderConfig.crcInitValue[i];
        }
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

    VkQueueFlags requestVideoEncodeQueueMask = 0;
    if (m_decoderConfig.enableVideoEncoder) {
        requestVideoEncodeQueueMask |= VK_QUEUE_VIDEO_ENCODE_BIT_KHR;
    }

    if (m_decoderConfig.selectVideoWithComputeQueue) {
        requestVideoDecodeQueueMask |= VK_QUEUE_COMPUTE_BIT;
        if (m_decoderConfig.enableVideoEncoder) {
            requestVideoEncodeQueueMask |= VK_QUEUE_COMPUTE_BIT;
        }
    }

    VkQueueFlags requestVideoComputeQueueMask = 0;
    if (m_decoderConfig.enablePostProcessFilter != -1) {
        requestVideoComputeQueueMask = VK_QUEUE_COMPUTE_BIT;
    }

    VkVideoCodecOperationFlagsKHR videoDecodeCodecs = (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR  |
                                                       VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR  |
                                                       VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR);

    VkVideoCodecOperationFlagsKHR videoEncodeCodecs = ( VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR  |
                                                        VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR  |
                                                        VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR);

    VkVideoCodecOperationFlagsKHR videoCodecs = videoDecodeCodecs |
                                        (m_decoderConfig.enableVideoEncoder ? videoEncodeCodecs : (VkVideoCodecOperationFlagsKHR) VK_VIDEO_CODEC_OPERATION_NONE_KHR);

    if (!m_decoderConfig.noPresent) {

        VkSharedBaseObj<Shell> displayShell;
        if (!libraryMode) {
            const Shell::Configuration configuration(m_decoderConfig.appName.c_str(),
                                                     m_decoderConfig.backBufferCount,
                                                     m_decoderConfig.directMode);

            result = Shell::Create(&m_vkDevCtxt, configuration, displayShell);
            if (result != VK_SUCCESS) {
                assert(!"Can't allocate display shell! Out of memory!");
                return result;
            }
        }

        result = m_vkDevCtxt.InitPhysicalDevice(m_decoderConfig.deviceId, m_decoderConfig.GetDeviceUUID(),
                                              (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT |
                                              requestVideoComputeQueueMask |
                                              requestVideoDecodeQueueMask |
                                              requestVideoEncodeQueueMask),
                                              displayShell,
                                              requestVideoDecodeQueueMask,
                                              (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
                                               VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR |
                                               VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR),
                                              requestVideoEncodeQueueMask,
                                              (VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR |
                                               VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR |
                                               VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR),
                                               vkPhysicalDevice);
        if (result != VK_SUCCESS) {

            assert(!"Can't initialize the Vulkan physical device!");
            return result;
        }

        if (!libraryMode) {
            assert(displayShell->PhysDeviceCanPresent(m_vkDevCtxt.getPhysicalDevice(),
                                                      m_vkDevCtxt.GetPresentQueueFamilyIdx()));
        }

        m_vkDevCtxt.CreateVulkanDevice(numDecodeQueues,
                                       m_decoderConfig.enableVideoEncoder ? 1 : 0, // num encode queues
                                       videoCodecs,
                                       false, //  createTransferQueue
                                       true,  // createGraphicsQueue
                                       true,  // createDisplayQueue
                                       (requestVideoComputeQueueMask != 0),  // createComputeQueue
                                       vkDevice
                                       );


        result = VulkanVideoProcessor::Create(m_decoderConfig, &m_vkDevCtxt, m_vulkanVideoProcessor);
        if (result != VK_SUCCESS) {
            return result;
        }

        m_vulkanVideoProcessor->Initialize(&m_vkDevCtxt, videoStreamDemuxer, m_decoderConfig);

        if (!libraryMode) {

            VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>> videoQueue(m_vulkanVideoProcessor);
            m_frameProcessor.Init(&m_vkDevCtxt, videoQueue, 0);

            displayShell->AttachFrameProcessor(m_frameProcessor);

            displayShell->RunLoop();
        }
    } else {

        result = m_vkDevCtxt.InitPhysicalDevice(m_decoderConfig.deviceId, m_decoderConfig.GetDeviceUUID(),
                                                ( VK_QUEUE_TRANSFER_BIT       |
                                                 requestVideoDecodeQueueMask  |
                                                 requestVideoComputeQueueMask |
                                                 requestVideoEncodeQueueMask),
                                                nullptr,
                                                requestVideoDecodeQueueMask,
                                                ( VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
                                                  VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR |
                                                  VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR),
                                                ( VK_QUEUE_VIDEO_ENCODE_BIT_KHR | VK_QUEUE_TRANSFER_BIT),
                                                ( VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR |
                                                  VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR |
                                                  VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR),
                                                vkPhysicalDevice);
        if (result != VK_SUCCESS) {

            assert(!"Can't initialize the Vulkan physical device!");
            return result;
        }


        result = m_vkDevCtxt.CreateVulkanDevice( numDecodeQueues,
                                                 0,     // num encode queues
                                                 videoCodecs,
                                                 // If no graphics or compute queue is requested, only video queues
                                                 // will be created. Not all implementations support transfer on video queues,
                                                 // so request a separate transfer queue for such implementations.
                                                 ((m_vkDevCtxt.GetVideoDecodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) == 0), //  createTransferQueue
                                                 false, // createGraphicsQueue
                                                 false, // createDisplayQueue
                                                 (requestVideoComputeQueueMask != 0),   // createComputeQueue
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

        m_vulkanVideoProcessor->Initialize(&m_vkDevCtxt, videoStreamDemuxer, m_decoderConfig);

        if (!libraryMode) {

            VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>> videoQueue(m_vulkanVideoProcessor);
            m_frameProcessor.Init(&m_vkDevCtxt, videoQueue, m_decoderConfig.decoderQueueSize);

            bool continueLoop = true;
            do {
                continueLoop = m_frameProcessor->OnFrame(0);
            } while (continueLoop);
        }
    }

    if (m_decoderConfig.outputcrc != 0) {
        fprintf(m_decoderConfig.crcOutputFile, "CRC: ");
        for (size_t i = 0; i < m_decoderConfig.crcInitValue.size(); i += 1) {
            fprintf(m_decoderConfig.crcOutputFile, "0x%08X ", crcAllocation[i]);
        }

        fprintf(m_decoderConfig.crcOutputFile, "\n");
        if (m_decoderConfig.crcOutputFile != stdout) {
            fclose(m_decoderConfig.crcOutputFile);
            m_decoderConfig.crcOutputFile = stdout;
        }
    }

    return result;
}

VK_VIDEO_DECODER_EXPORT
VkResult CreateVulkanVideoDecoder(VkInstance vkInstance, VkPhysicalDevice vkPhysicalDevice, VkDevice vkDevice,
                                  VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer,
                                  int argc, const char** argv,
                                  VkSharedBaseObj<VulkanVideoDecoder>& vulkanVideoDecoder)
{
    switch((uint32_t)videoStreamDemuxer->GetVideoCodec())
    {
        case VK_VIDEO_CODEC_OPERATION_NONE_KHR: // auto
        case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
        {

        }
        break;

    default:
        assert(!"Unsupported codec type!!!\n");
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    VkSharedBaseObj<VulkanVideoDecoder> vulkanVideoDecoderObj( new VulkanVideoDecoderImpl(argv[0]));
    if (!vulkanVideoDecoderObj) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkResult result = vulkanVideoDecoderObj->Initialize(vkInstance, vkPhysicalDevice, vkDevice, videoStreamDemuxer, argc, argv);
    if (result != VK_SUCCESS) {
        vulkanVideoDecoderObj = nullptr;
    } else {
        vulkanVideoDecoder = vulkanVideoDecoderObj;
    }

    return result;
}
