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
#include "VkCodecUtils/nvVideoProfile.h"
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

class NvVkDecodeFrameData {
public:
    vulkanVideoUtils::VulkanVideoBistreamBuffer bistreamBuffer;
    VkCommandBuffer commandBuffer;
};

/**
 * @brief Base class for decoder interface.
 */
class NvVkDecoder : public IVulkanVideoDecoderHandler {
public:
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
                (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT |
                        VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT));

        assert(videoCodecs != VK_VIDEO_CODEC_OPERATION_INVALID_BIT_KHR);

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
                                       const VkVideoProfileKHR* pVideoProfile,
                                       VkVideoCapabilitiesKHR &videoDecodeCapabilities) const
    {
        videoDecodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        return vk::GetPhysicalDeviceVideoCapabilitiesKHR(vkPhysicalDev,
                                                         pVideoProfile,
                                                         &videoDecodeCapabilities);
    }

    VkResult GetDecodeH265Capabilities(VkPhysicalDevice vkPhysicalDev, uint32_t vkVideoDecodeQueueFamily,
                                       const VkVideoProfileKHR* pVideoProfile,
                                       VkVideoCapabilitiesKHR &videoDecodeCapabilities) const
    {
        videoDecodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        return vk::GetPhysicalDeviceVideoCapabilitiesKHR(vkPhysicalDev,
                                                         pVideoProfile,
                                                         &videoDecodeCapabilities);
    }

    VkResult GetEncodeH264Capabilities(VkPhysicalDevice vkPhysicalDev, uint32_t vkVideoDecodeQueueFamily,
                                       const VkVideoProfileKHR* pVideoProfile,
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
                          const nvVideoProfile* pProfile) const
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

    VkResult GetVideoFormats(nvVideoProfile* pVideoProfile, VkImageUsageFlags imageUsage,
                             uint32_t& formatCount, VkFormat* formats);

    VkResult GetVideoCapabilities(nvVideoProfile* pVideoProfile,
                                  VkVideoCapabilitiesKHR* pVideoDecodeCapabilities);

    NvVkDecoder(const VulkanDecodeContext* pVulkanDecodeContext, VulkanVideoFrameBuffer* pVideoFrameBuffer)
        : m_pVulkanDecodeContext(*pVulkanDecodeContext)
        , m_refCount(1)
        , m_videoFormat {}
        , m_numDecodeSurfaces()
        , m_maxDecodeFramesCount(0)
        , m_videoSession(nullptr)
        , m_videoCommandPool()
        , m_pVideoFrameBuffer(pVideoFrameBuffer)
        , m_decodeFramesData(NULL)
        , m_decodePicCount(0)
        , m_lastSpsIdInQueue(-1)
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

    VkParserVideoPictureParameters*  AddPictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>& spsStdPictureParametersSet,
                                                          VkSharedBaseObj<StdVideoPictureParametersSet>& ppsStdPictureParametersSet);

    bool CheckStdObjectBeforeUpdate(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersSet);
    VkParserVideoPictureParameters* CheckStdObjectAfterUpdate(VkSharedBaseObj<StdVideoPictureParametersSet>& stdPictureParametersSet, VkParserVideoPictureParameters* pNewPictureParametersObject);
    uint32_t AddPictureParametersToQueue(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersSet, bool& hasSpsPpsPair);
    uint32_t FlushPictureParametersQueue();

    NvVkDecodeFrameData* GetCurrentFrameData(uint32_t currentSlotId)
    {
        assert(currentSlotId < m_maxDecodeFramesCount);
        return &m_decodeFramesData[currentSlotId];
    }

private:
    const VulkanDecodeContext m_pVulkanDecodeContext;
    std::atomic<int32_t> m_refCount;
    // dimension of the output
    VkParserDetectedVideoFormat m_videoFormat;
    uint32_t                    m_numDecodeSurfaces;
    uint32_t                    m_maxDecodeFramesCount;

    VkSharedBaseObj<NvVideoSession>      m_videoSession;
    VkCommandPool                        m_videoCommandPool;
    VulkanVideoFrameBuffer*              m_pVideoFrameBuffer;
    NvVkDecodeFrameData*                 m_decodeFramesData;

    int32_t                                                    m_decodePicCount;
    int32_t                                                    m_lastSpsIdInQueue;
    std::queue<VkSharedBaseObj<StdVideoPictureParametersSet>>  m_pictureParametersQueue;
    VkSharedBaseObj<StdVideoPictureParametersSet>              m_lastSpsPictureParametersQueue;
    VkSharedBaseObj<StdVideoPictureParametersSet>              m_lastPpsPictureParametersQueue;
    VkSharedBaseObj<VkParserVideoPictureParameters>            currentPictureParameters;
    uint32_t m_dumpDecodeData : 1;
};
