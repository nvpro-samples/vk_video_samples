/*
* Copyright 2020 NVIDIA Corporation.
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

#pragma once

#include <assert.h>
#include <atomic>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include "VkCodecUtils/VulkanVideoUtils.h"
#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/NvVideoProfile.h"
#include "VkCodecUtils/NvVideoSession.h"
#include "VulkanVideoFrameBuffer/VulkanVideoFrameBuffer.h"
#include "VulkanVideoParser.h"
#include "vulkan_interfaces.h"
#include "VulkanVideoParserIf.h"
#include "VkParserVideoPictureParameters.h"
#include "StdVideoPictureParametersSet.h"

struct Rect {
    int32_t l;
    int32_t t;
    int32_t r;
    int32_t b;
};

struct Dim {
    int w, h;
};

typedef struct VulkanDecodeContext {
    VkInstance instance;
    VkPhysicalDevice physicalDev;
    VkDevice dev;
    uint32_t videoDecodeQueueFamily;
    VkQueue videoQueue;
    uint32_t videoEncodeQueueFamily;
} VulkanDecodeContext;

struct NvVkDecodeFrameDataSlot {
    uint32_t                                            slot;
    VkCommandBuffer                                     commandBuffer;
    const vulkanVideoUtils::VulkanVideoBitstreamBuffer* bitstreamBuffer;
};

class NvVkDecodeFrameData {

public:
    NvVkDecodeFrameData(const VulkanDecodeContext* pVulkanDecodeContext)
       : m_pVulkanDecodeContext(*pVulkanDecodeContext),
         m_maxCodedWidth(),
         m_videoCommandPool() {}

    void deinit() {

        if (m_videoCommandPool) {
            vk::FreeCommandBuffers(m_pVulkanDecodeContext.dev, m_videoCommandPool, (uint32_t)m_commandBuffers.size(), &m_commandBuffers[0]);
            vk::DestroyCommandPool(m_pVulkanDecodeContext.dev, m_videoCommandPool, NULL);
            m_videoCommandPool = VkCommandPool();
        }

        for (size_t decodeFrameId = 0; decodeFrameId < m_bitstreamBuffers.size(); decodeFrameId++) {
            m_bitstreamBuffers[decodeFrameId].DestroyVideoBitstreamBuffer();
        }
    }

    ~NvVkDecodeFrameData() {
        deinit();
    }

    size_t resize(size_t maxDecodeFramesCount,
                  uint32_t maxCodedWidth,uint32_t maxCodedHeight,
                  VkVideoChromaSubsamplingFlagBitsKHR chromaSubsampling,
                  VkDeviceSize minBitstreamBufferOffsetAlignment,
                  VkDeviceSize minBitstreamBufferSizeAlignment) {

        const size_t oldMaxDecodeBuffersFrameCount = m_bitstreamBuffers.size();
        if (oldMaxDecodeBuffersFrameCount >= maxDecodeFramesCount) {
            assert(m_maxCodedWidth >= maxCodedWidth);
            return oldMaxDecodeBuffersFrameCount;
        }

        m_bitstreamBuffers.resize(maxDecodeFramesCount);

        unsigned int maxMbWidth  = (maxCodedWidth + 15) >> 4;
        unsigned int maxMbHeight = (maxCodedHeight + 15) >> 4;
        unsigned int maxMbCount  =  maxMbWidth * ((maxMbHeight + 1) & ~1);
        const VkDeviceSize bufferSize = chromaSubsampling == VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR ?
                                        std::max((maxMbCount << 8) * 3, (unsigned int)2048) :  // At least 3MB
                                        std::max((maxMbCount << 8) * 2, (unsigned int)2048);   // At least 2MB

        for (size_t decodeFrameId = oldMaxDecodeBuffersFrameCount; decodeFrameId < m_bitstreamBuffers.size(); decodeFrameId++) {
            VkResult result = m_bitstreamBuffers[decodeFrameId].CreateVideoBitstreamBuffer(
                m_pVulkanDecodeContext.physicalDev, m_pVulkanDecodeContext.dev, m_pVulkanDecodeContext.videoDecodeQueueFamily,
                bufferSize, minBitstreamBufferOffsetAlignment, minBitstreamBufferSizeAlignment);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "\nERROR: CreateVideoBitstreamBuffer() result: 0x%x\n", result);
            }
        }

        if (!m_videoCommandPool) {
            VkCommandPoolCreateInfo cmdPoolInfo = {};
            cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cmdPoolInfo.queueFamilyIndex = m_pVulkanDecodeContext.videoDecodeQueueFamily;
            VkResult result = vk::CreateCommandPool(m_pVulkanDecodeContext.dev, &cmdPoolInfo, nullptr, &m_videoCommandPool);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "\nERROR: CreateCommandPool() result: 0x%x\n", result);
            }
        }

        const size_t oldCommandBuffersCount = m_commandBuffers.size();
        VkCommandBufferAllocateInfo cmdInfo = {};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandBufferCount = (uint32_t)(maxDecodeFramesCount - oldCommandBuffersCount);
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandPool = m_videoCommandPool;

        m_commandBuffers.resize(maxDecodeFramesCount);
        VkResult result = vk::AllocateCommandBuffers(m_pVulkanDecodeContext.dev, &cmdInfo, &m_commandBuffers[oldCommandBuffersCount]);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: AllocateCommandBuffers() result: 0x%x\n", result);
        }

        m_maxCodedWidth = maxCodedWidth;

        return oldCommandBuffersCount;
    }

    const vulkanVideoUtils::VulkanVideoBitstreamBuffer* GetBitstreamBuffer(uint32_t slot) {
        assert(slot < m_bitstreamBuffers.size());
        return &m_bitstreamBuffers[slot];
    }

    VkCommandBuffer GetCommandBuffer(uint32_t slot) {
        assert(slot < m_commandBuffers.size());
        return m_commandBuffers[slot];
    }

    size_t size() {
        return m_commandBuffers.size();
    }

private:
    const VulkanDecodeContext                                 m_pVulkanDecodeContext;
    uint32_t                                                  m_maxCodedWidth;
    VkCommandPool                                             m_videoCommandPool;
    std::vector<VkCommandBuffer>                              m_commandBuffers;
    std::vector<vulkanVideoUtils::VulkanVideoBitstreamBuffer> m_bitstreamBuffers;
};

/**
 * @brief Base class for decoder interface.
 */
class NvVkDecoder : public IVulkanVideoDecoderHandler {
public:
    VkPhysicalDevice GetPhysDevice() { return m_pVulkanDecodeContext.physicalDev; }
    enum { MAX_RENDER_TARGETS = 32 }; // Must be 32 or less (used as uint32_t bitmask of active render targets)

    static const char* GetVideoCodecString(VkVideoCodecOperationFlagBitsKHR codec);
    static const char* GetVideoChromaFormatString(VkVideoChromaSubsamplingFlagBitsKHR chromaFormat);
    static uint32_t GetNumDecodeSurfaces(VkVideoCodecOperationFlagBitsKHR codec, uint32_t minNumDecodeSurfaces, uint32_t width,
        uint32_t height);

    VkVideoCodecOperationFlagsKHR GetSupportedCodecs(VkPhysicalDevice vkPhysicalDev, uint32_t vkVideoDecodeQueueFamily) const
    {
        int32_t videoDecodeQueueFamily = (int32_t)vkVideoDecodeQueueFamily;
        VkVideoCodecOperationFlagsKHR videoCodecs = vk::GetSupportedCodecs(vkPhysicalDev,
                &videoDecodeQueueFamily,
                VK_QUEUE_VIDEO_DECODE_BIT_KHR,
                (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
                        VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR));

        assert(videoCodecs != VK_VIDEO_CODEC_OPERATION_NONE_KHR);

        return videoCodecs;
    }

    bool IsCodecTypeSupported(VkPhysicalDevice vkPhysicalDev, uint32_t vkVideoDecodeQueueFamily,
                              VkVideoCodecOperationFlagBitsKHR videoCodec) const
    {
        VkVideoCodecOperationFlagsKHR videoCodecs = GetSupportedCodecs(vkPhysicalDev, vkVideoDecodeQueueFamily);

        if (videoCodecs & videoCodec) {
            return true;
        }

        return false;
    }

    VkResult GetDecodeH264Capabilities(VkPhysicalDevice vkPhysicalDev, uint32_t vkVideoDecodeQueueFamily,
                                       const VkVideoProfileInfoKHR* pVideoProfile,
                                       VkVideoCapabilitiesKHR &videoDecodeCapabilities) const
    {
        videoDecodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        return vk::GetPhysicalDeviceVideoCapabilitiesKHR(vkPhysicalDev,
                                                         pVideoProfile,
                                                         &videoDecodeCapabilities);
    }

    VkResult GetDecodeH265Capabilities(VkPhysicalDevice vkPhysicalDev, uint32_t vkVideoDecodeQueueFamily,
                                       const VkVideoProfileInfoKHR* pVideoProfile,
                                       VkVideoCapabilitiesKHR &videoDecodeCapabilities) const
    {
        videoDecodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        return vk::GetPhysicalDeviceVideoCapabilitiesKHR(vkPhysicalDev,
                                                         pVideoProfile,
                                                         &videoDecodeCapabilities);
    }

    VkResult GetEncodeH264Capabilities(VkPhysicalDevice vkPhysicalDev, uint32_t vkVideoDecodeQueueFamily,
                                       const VkVideoProfileInfoKHR* pVideoProfile,
                                       VkVideoCapabilitiesKHR &videoEncodeCapabilities,
                                       VkVideoEncodeH264CapabilitiesEXT &encode264Capabilities) const
    {
        encode264Capabilities.sType   = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT;
        videoEncodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
        videoEncodeCapabilities.pNext = &encode264Capabilities;
        return vk::GetPhysicalDeviceVideoCapabilitiesKHR(vkPhysicalDev,
                                                         pVideoProfile,
                                                         &videoEncodeCapabilities);

    }

    VkResult GetEncodeH264Capabilities(VkPhysicalDevice vkPhysicalDev, uint32_t vkVideoDecodeQueueFamily,
                          const NvVideoProfile* pProfile) const
    {
        const bool isEncode = pProfile->IsEncodeCodecType();

        VkVideoEncodeH264CapabilitiesEXT encode264Capabilities = VkVideoEncodeH264CapabilitiesEXT();
        encode264Capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT;
        VkVideoCapabilitiesKHR videoDecodeCapabilities = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
                                                           isEncode ? &encode264Capabilities : NULL };
        return vk::GetPhysicalDeviceVideoCapabilitiesKHR(vkPhysicalDev,
                                                                    pProfile->GetProfile(),
                                                                    &videoDecodeCapabilities);
    }

    VkResult GetVideoFormats(NvVideoProfile* pVideoProfile, VkImageUsageFlags imageUsage,
                             uint32_t& formatCount, VkFormat* formats);

    VkResult GetVideoCapabilities(NvVideoProfile* pVideoProfile,
                                  VkVideoCapabilitiesKHR* pVideoDecodeCapabilities);

    NvVkDecoder(const VulkanDecodeContext* pVulkanDecodeContext, VulkanVideoFrameBuffer* pVideoFrameBuffer, bool useLinearOutput = false)
        : m_pVulkanDecodeContext(*pVulkanDecodeContext)
        , m_refCount(1)
        , m_videoFormat {}
        , m_numDecodeSurfaces()
        , m_maxDecodeFramesCount(0)
        , m_capabilityFlags()
        , m_videoSession(nullptr)
        , m_pVideoFrameBuffer(pVideoFrameBuffer)
        , m_decodeFramesData(pVulkanDecodeContext)
        , m_decodePicCount(0)
        , m_lastIdInQueue{-1, -1, -1}
        , m_useImageArray(false)
        , m_useImageViewArray(false)
        , m_useSeparateOutputImages(useLinearOutput)
        , m_useLinearOutput(useLinearOutput)
        , m_resetDecoder(true)
        , m_dumpDecodeData(false)
    {

        if (m_pVideoFrameBuffer) {
            m_pVideoFrameBuffer->AddRef();
        }
    }

    ~NvVkDecoder();
    void Deinitialize();

    virtual int32_t AddRef();
    virtual int32_t Release();

    /**
     *   @brief  This function is used to get information about the video stream (codec, display parameters etc)
     */
    const VkParserDetectedVideoFormat* GetVideoFormatInfo()
    {
        assert(m_videoFormat.coded_width);
        return &m_videoFormat;
    }

    /**
    *   @brief  This callback function gets called when when decoding of sequence starts,
    */
    virtual int32_t StartVideoSequence(VkParserDetectedVideoFormat* pVideoFormat);

    virtual bool UpdatePictureParameters(VkPictureParameters* pPictureParameters,
                                         VkSharedBaseObj<VkParserVideoRefCountBase>& pictureParametersObject,
                                         uint64_t updateSequenceCount);

    /**
     *   @brief  This callback function gets called when a picture is ready to be decoded.
     */
    virtual int32_t DecodePictureWithParameters(VkParserPerFrameDecodeParameters* pPicParams, VkParserDecodePictureInfo* pDecodePictureInfo);

private:

    VkParserVideoPictureParameters*  AddPictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>& vpsStdPictureParametersSet,
                                                          VkSharedBaseObj<StdVideoPictureParametersSet>& spsStdPictureParametersSet,
                                                          VkSharedBaseObj<StdVideoPictureParametersSet>& ppsStdPictureParametersSet);

    bool CheckStdObjectBeforeUpdate(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersSet);
    VkParserVideoPictureParameters* CheckStdObjectAfterUpdate(VkSharedBaseObj<StdVideoPictureParametersSet>& stdPictureParametersSet, VkParserVideoPictureParameters* pNewPictureParametersObject);
    uint32_t AddPictureParametersToQueue(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersSet);
    uint32_t FlushPictureParametersQueue();

    int CopyOptimalToLinearImage(VkCommandBuffer& commandBuffer,
                                 VkVideoPictureResourceInfoKHR& srcPictureResource,
                                 VulkanVideoFrameBuffer::PictureResourceInfo& srcPictureResourceInfo,
                                 VkVideoPictureResourceInfoKHR& dstPictureResource,
                                 VulkanVideoFrameBuffer::PictureResourceInfo& dstPictureResourceInfo,
                                 VulkanVideoFrameBuffer::FrameSynchronizationInfo *pFrameSynchronizationInfo);

    int32_t GetCurrentFrameData(uint32_t slotId, NvVkDecodeFrameDataSlot& frameDataSlot)
    {
        if (slotId < m_decodeFramesData.size()) {
            frameDataSlot.bitstreamBuffer = m_decodeFramesData.GetBitstreamBuffer(slotId);
            frameDataSlot.commandBuffer   = m_decodeFramesData.GetCommandBuffer(slotId);
            frameDataSlot.slot = slotId;
            return slotId;
        }
        return -1;
    }

private:
    const VulkanDecodeContext m_pVulkanDecodeContext;
    std::atomic<int32_t> m_refCount;
    // dimension of the output
    VkParserDetectedVideoFormat m_videoFormat;
    uint32_t                    m_numDecodeSurfaces;
    uint32_t                    m_maxDecodeFramesCount;

    VkVideoDecodeCapabilityFlagBitsKHR   m_capabilityFlags;
    VkSharedBaseObj<NvVideoSession>      m_videoSession;
    VulkanVideoFrameBuffer*              m_pVideoFrameBuffer;

    NvVkDecodeFrameData                  m_decodeFramesData;

    int32_t                                                    m_decodePicCount;
    int32_t                                                    m_lastIdInQueue[StdVideoPictureParametersSet::NUM_OF_TYPES];
    std::queue<VkSharedBaseObj<StdVideoPictureParametersSet>>  m_pictureParametersQueue;
    VkSharedBaseObj<StdVideoPictureParametersSet>              m_lastPictParamsQueue[StdVideoPictureParametersSet::NUM_OF_TYPES];
    VkSharedBaseObj<VkParserVideoPictureParameters>            m_currentPictureParameters;
    uint32_t m_useImageArray : 1;
    uint32_t m_useImageViewArray : 1;
    uint32_t m_useSeparateOutputImages : 1;
    uint32_t m_useLinearOutput : 1;
    uint32_t m_resetDecoder : 1;
    uint32_t m_dumpDecodeData : 1;
};
