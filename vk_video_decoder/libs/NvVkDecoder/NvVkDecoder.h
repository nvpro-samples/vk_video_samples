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
#include "VulkanVideoFrameBuffer/VulkanVideoFrameBuffer.h"
#include "VulkanVideoParser.h"
#include "vulkan_interfaces.h"

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
    static VkFormat CodecGetVkFormat(VkVideoChromaSubsamplingFlagBitsKHR chromaFormatIdc, int bitDepthLumaMinus8, bool isSemiPlanar = true);
    static const char* CodecToName(VkVideoCodecOperationFlagBitsKHR codec);

    NvVkDecoder(const VulkanDecodeContext* pVulkanDecodeContext, VulkanVideoFrameBuffer* pVideoFrameBuffer)
        : m_pVulkanDecodeContext(*pVulkanDecodeContext)
        , m_refCount(1)
        , m_vkVideoDecoder()
        , m_codecType(VK_VIDEO_CODEC_OPERATION_INVALID_BIT_KHR)
        , m_rtFormat()
        , m_numDecodeSurfaces()
        , m_videoCommandPool()
        , m_pVideoFrameBuffer(pVideoFrameBuffer)
        , m_decodeFramesData(NULL)
        , m_maxDecodeFramesCount(0)
        , m_width(0)
        , m_height(0)
        , m_codedWidth()
        , m_codedHeight()
        , m_surfaceHeight(0)
        , m_surfaceWidth(0)
        , m_chromaFormat()
        , m_bitLumaDepthMinus8(0)
        , m_bitChromaDepthMinus8(0)
        , m_decodePicCount(0)
        , m_endDecodeDone(false)
        , m_videoFormat {}
        , m_cropRect {}
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
        assert(m_width);
        return &m_videoFormat;
    }

    /**
    *   @brief  This callback function gets called when when decoding of sequence starts,
    */
    virtual int32_t StartVideoSequence(VkParserDetectedVideoFormat* pVideoFormat);

    /**
     *   @brief  This callback function gets called when a picture is ready to be decoded.
     */
    virtual int32_t DecodePictureWithParameters(VkParserPerFrameDecodeParameters* pPicParams, VkParserDecodePictureInfo* pDecodePictureInfo);

private:
    NvVkDecodeFrameData* GetCurrentFrameData(uint32_t currentSlotId)
    {
        assert(currentSlotId < m_maxDecodeFramesCount);
        return &m_decodeFramesData[currentSlotId];
    }

private:
    const VulkanDecodeContext m_pVulkanDecodeContext;
    std::atomic<int32_t> m_refCount;
    VkVideoSessionKHR m_vkVideoDecoder;
    VkVideoCodecOperationFlagBitsKHR m_codecType;
    uint32_t m_rtFormat;
    uint32_t m_numDecodeSurfaces;
    vulkanVideoUtils::DeviceMemoryObject memoryDecoderBound[8];
    VkCommandPool m_videoCommandPool;
    VulkanVideoFrameBuffer* m_pVideoFrameBuffer;
    NvVkDecodeFrameData* m_decodeFramesData;
    uint32_t m_maxDecodeFramesCount;
    // dimension of the output
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_codedWidth;
    uint32_t m_codedHeight;
    // height of the mapped surface
    uint32_t m_surfaceHeight;
    uint32_t m_surfaceWidth;
    VkVideoChromaSubsamplingFlagBitsKHR m_chromaFormat;
    uint8_t m_bitLumaDepthMinus8;
    uint8_t m_bitChromaDepthMinus8;
    int32_t m_decodePicCount;
    bool m_endDecodeDone;
    VkParserDetectedVideoFormat m_videoFormat;
    Rect m_cropRect;
    uint32_t m_dumpDecodeData : 1;
};
