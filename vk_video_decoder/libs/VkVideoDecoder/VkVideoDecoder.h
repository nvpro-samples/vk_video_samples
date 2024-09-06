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
#include <queue>
#include <sstream>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include "vulkan_interfaces.h"
#include "VkCodecUtils/VulkanVideoReferenceCountedPool.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanFilterYuvCompute.h"
#include "VkCodecUtils/VulkanBistreamBufferImpl.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkCodecUtils/VulkanVideoSession.h"
#include "VulkanVideoFrameBuffer/VulkanVideoFrameBuffer.h"
#include "vkvideo_parser/VulkanVideoParser.h"
#include "vkvideo_parser/VulkanVideoParserIf.h"
#include "VkParserVideoPictureParameters.h"
#include "vkvideo_parser/StdVideoPictureParametersSet.h"

struct Rect {
    int32_t l;
    int32_t t;
    int32_t r;
    int32_t b;
};

struct Dim {
    int w, h;
};

struct NvVkDecodeFrameDataSlot {
    uint32_t                                            slot;
    VkCommandBuffer                                     commandBuffer;
};

class NvVkDecodeFrameData {

    using VulkanBitstreamBufferPool = VulkanVideoRefCountedPool<VulkanBitstreamBufferImpl, 64>;

public:
    NvVkDecodeFrameData(const VulkanDeviceContext* vkDevCtx)
       : m_vkDevCtx(vkDevCtx),
         m_videoCommandPool(),
         m_bitstreamBuffersQueue() {}

    void deinit() {

        if (m_videoCommandPool) {
            assert(m_vkDevCtx);
            m_vkDevCtx->FreeCommandBuffers(*m_vkDevCtx, m_videoCommandPool, (uint32_t)m_commandBuffers.size(), &m_commandBuffers[0]);
            m_vkDevCtx->DestroyCommandPool(*m_vkDevCtx, m_videoCommandPool, NULL);
            m_videoCommandPool = VkCommandPool();
        }
    }

    ~NvVkDecodeFrameData() {
        deinit();
    }

    size_t resize(size_t maxDecodeFramesCount) {

        assert(m_vkDevCtx);

        size_t allocatedCommandBuffers = 0;
        if (!m_videoCommandPool) {
            VkCommandPoolCreateInfo cmdPoolInfo = {};
            cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cmdPoolInfo.queueFamilyIndex = m_vkDevCtx->GetVideoDecodeQueueFamilyIdx();
            VkResult result = m_vkDevCtx->CreateCommandPool(*m_vkDevCtx, &cmdPoolInfo, nullptr, &m_videoCommandPool);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "\nERROR: CreateCommandPool() result: 0x%x\n", result);
            }

            VkCommandBufferAllocateInfo cmdInfo = {};
            cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cmdInfo.commandBufferCount = (uint32_t)maxDecodeFramesCount;
            cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmdInfo.commandPool = m_videoCommandPool;

            m_commandBuffers.resize(maxDecodeFramesCount);
            result = m_vkDevCtx->AllocateCommandBuffers(*m_vkDevCtx, &cmdInfo, &m_commandBuffers[0]);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "\nERROR: AllocateCommandBuffers() result: 0x%x\n", result);
            } else {
                allocatedCommandBuffers = maxDecodeFramesCount;
            }
        } else {
            allocatedCommandBuffers = m_commandBuffers.size();
            assert(maxDecodeFramesCount <= allocatedCommandBuffers);
        }

        return allocatedCommandBuffers;
    }

    VkCommandBuffer GetCommandBuffer(uint32_t slot) {
        assert(slot < m_commandBuffers.size());
        return m_commandBuffers[slot];
    }

    size_t size() {
        return m_commandBuffers.size();
    }

    VulkanBitstreamBufferPool& GetBitstreamBuffersQueue() { return m_bitstreamBuffersQueue; }

private:
    const VulkanDeviceContext*                                m_vkDevCtx;
    VkCommandPool                                             m_videoCommandPool;
    std::vector<VkCommandBuffer>                              m_commandBuffers;
    VulkanBitstreamBufferPool                                 m_bitstreamBuffersQueue;
};

/**
 * @brief Base class for decoder interface.
 */
class VkVideoDecoder : public IVulkanVideoDecoderHandler {
public:
    VkPhysicalDevice GetPhysDevice() { return m_vkDevCtx ? m_vkDevCtx->getPhysicalDevice() : VK_NULL_HANDLE; }
    enum { MAX_RENDER_TARGETS = 32 }; // Must be 32 or less (used as uint32_t bitmask of active render targets)

    static VkSharedBaseObj<VkVideoDecoder> invalidVkDecoder;

    enum DecoderFeatures { ENABLE_LINEAR_OUTPUT       = (1 << 0),
                           ENABLE_HW_LOAD_BALANCING   = (1 << 1),
                           ENABLE_POST_PROCESS_FILTER = (1 << 2),
                         };

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VulkanVideoFrameBuffer>& videoFrameBuffer,
                           int32_t videoQueueIndx = 0,
                           uint32_t enableDecoderFeatures = 0, // a list of DecoderFeatures
                           VulkanFilterYuvCompute::FilterType filterType = VulkanFilterYuvCompute::YCBCRCOPY,
                           int32_t numDecodeImagesInFlight = 8,
                           int32_t numDecodeImagesToPreallocate = -1, // preallocate the maximum required
                           int32_t numBitstreamBuffersToPreallocate = 8,
                           VkSharedBaseObj<VkVideoDecoder>& vkVideoDecoder = invalidVkDecoder);

    static const char* GetVideoCodecString(VkVideoCodecOperationFlagBitsKHR codec);
    static const char* GetVideoChromaFormatString(VkVideoChromaSubsamplingFlagBitsKHR chromaFormat);

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

    virtual bool UpdatePictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersObject,
                                         VkSharedBaseObj<VkVideoRefCountBase>& client);

    /**
     *   @brief  This callback function gets called when a picture is ready to be decoded.
     */
    virtual int32_t DecodePictureWithParameters(VkParserPerFrameDecodeParameters* pPicParams, VkParserDecodePictureInfo* pDecodePictureInfo);

    virtual VkDeviceSize GetBitstreamBuffer(VkDeviceSize size,
                                      VkDeviceSize minBitstreamBufferOffsetAlignment,
                                      VkDeviceSize minBitstreamBufferSizeAlignment,
                                      const uint8_t* pInitializeBufferMemory,
                                      VkDeviceSize initializeBufferMemorySize,
                                      VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer);
private:

    VkVideoDecoder(const VulkanDeviceContext* vkDevCtx,
                   VkSharedBaseObj<VulkanVideoFrameBuffer>& videoFrameBuffer,
                   int32_t  videoQueueIndx = 0,
                   uint32_t enableDecoderFeatures = 0, // a list of DecoderFeatures
                   VulkanFilterYuvCompute::FilterType filterType = VulkanFilterYuvCompute::YCBCRCOPY,
                   int32_t  numDecodeImagesInFlight = 8,
                   int32_t  numDecodeImagesToPreallocate = -1, // preallocate the maximum required
                   int32_t  numBitstreamBuffersToPreallocate = 8)
        : m_vkDevCtx(vkDevCtx)
        , m_currentVideoQueueIndx(videoQueueIndx)
        , m_refCount(0)
        , m_codedExtent {}
        , m_videoFormat {}
        , m_numDecodeImagesInFlight(numDecodeImagesInFlight)
        , m_numDecodeImagesToPreallocate(numDecodeImagesToPreallocate)
        , m_numDecodeSurfaces()
        , m_maxDecodeFramesCount(0)
        , m_capabilityFlags()
        , m_videoSession(nullptr)
        , m_videoFrameBuffer(videoFrameBuffer)
        , m_decodeFramesData(vkDevCtx)
        , m_decodePicCount(0)
        , m_hwLoadBalancingTimelineSemaphore()
        , m_dpbAndOutputCoincide(true)
        , m_videoMaintenance1FeaturesSupported(false)
        , m_enableDecodeFilter((enableDecoderFeatures & ENABLE_POST_PROCESS_FILTER) != 0)
        , m_useImageArray(false)
        , m_useImageViewArray(false)
        , m_useSeparateOutputImages(false)
        , m_useLinearOutput((enableDecoderFeatures & ENABLE_LINEAR_OUTPUT) != 0)
        , m_resetDecoder(true)
        , m_dumpDecodeData(false)
        , m_numBitstreamBuffersToPreallocate(numBitstreamBuffersToPreallocate)
        , m_maxStreamBufferSize()
        , m_filterType(filterType)
    {

        assert(m_vkDevCtx->GetVideoDecodeQueueFamilyIdx() != -1);
        assert(m_vkDevCtx->GetVideoDecodeNumQueues() > 0);

        if (m_currentVideoQueueIndx < 0) {
            m_currentVideoQueueIndx = m_vkDevCtx->GetVideoDecodeDefaultQueueIndex();
        } else if (m_vkDevCtx->GetVideoDecodeNumQueues() > 1) {
            m_currentVideoQueueIndx %= m_vkDevCtx->GetVideoDecodeNumQueues();
            assert(m_currentVideoQueueIndx < m_vkDevCtx->GetVideoDecodeNumQueues());
            assert(m_currentVideoQueueIndx >= 0);
        } else {
            m_currentVideoQueueIndx = 0;
        }

        if (enableDecoderFeatures & ENABLE_HW_LOAD_BALANCING) {

            if (m_vkDevCtx->GetVideoDecodeNumQueues() < 2) {
                std::cout << "\t WARNING: Enabling HW Load Balancing for device with only " <<
                        m_vkDevCtx->GetVideoDecodeNumQueues() << " queue!!!" << std::endl;
            }

            // Create the timeline semaphore object for the HW LoadBalancing Timeline Semaphore
            VkSemaphoreTypeCreateInfo timelineCreateInfo;
            timelineCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            timelineCreateInfo.pNext = NULL;
            timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            timelineCreateInfo.initialValue = (uint64_t)-1; // assuming m_decodePicCount starts at 0.

            VkSemaphoreCreateInfo createInfo;
            createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            createInfo.pNext = &timelineCreateInfo;
            createInfo.flags = 0;

            VkResult result = m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &createInfo, NULL, &m_hwLoadBalancingTimelineSemaphore);
            if (result == VK_SUCCESS) {
                m_currentVideoQueueIndx = 0; // start with index zero
            }
            std::cout << "\t Enabling HW Load Balancing for device with "
                      << m_vkDevCtx->GetVideoDecodeNumQueues() << " queues" << std::endl;
        }

    }

    virtual ~VkVideoDecoder();
    void Deinitialize();

    int CopyOptimalToLinearImage(VkCommandBuffer& commandBuffer,
                                 VkVideoPictureResourceInfoKHR& srcPictureResource,
                                 VulkanVideoFrameBuffer::PictureResourceInfo& srcPictureResourceInfo,
                                 VkVideoPictureResourceInfoKHR& dstPictureResource,
                                 VulkanVideoFrameBuffer::PictureResourceInfo& dstPictureResourceInfo,
                                 VulkanVideoFrameBuffer::FrameSynchronizationInfo *pFrameSynchronizationInfo);

    int32_t GetCurrentFrameData(uint32_t slotId, NvVkDecodeFrameDataSlot& frameDataSlot)
    {
        if (slotId < m_decodeFramesData.size()) {
            frameDataSlot.commandBuffer   = m_decodeFramesData.GetCommandBuffer(slotId);
            frameDataSlot.slot = slotId;
            return slotId;
        }
        return -1;
    }

private:
    const VulkanDeviceContext*  m_vkDevCtx;
    int32_t                     m_currentVideoQueueIndx;
    std::atomic<int32_t>        m_refCount;
    // The current decoder coded extent
    VkExtent2D                  m_codedExtent;
    // dimension of the output
    VkParserDetectedVideoFormat m_videoFormat;
    int32_t                     m_numDecodeImagesInFlight; // driven by how deep is the decoder queue
    int32_t                     m_numDecodeImagesToPreallocate; // -1 means pre-allocate all required images on setup
    uint32_t                    m_numDecodeSurfaces;
    uint32_t                    m_maxDecodeFramesCount;

    VkVideoDecodeCapabilityFlagsKHR         m_capabilityFlags;
    VkSharedBaseObj<VulkanVideoSession>     m_videoSession;
    VkSharedBaseObj<VulkanVideoFrameBuffer> m_videoFrameBuffer;
    NvVkDecodeFrameData                     m_decodeFramesData;

    uint64_t                                         m_decodePicCount; // Also used for the HW load balancing timeline semaphore
    VkSharedBaseObj<VkParserVideoPictureParameters>  m_currentPictureParameters;
    VkSemaphore m_hwLoadBalancingTimelineSemaphore;
    uint32_t m_dpbAndOutputCoincide : 1;
    uint32_t m_videoMaintenance1FeaturesSupported : 1;
    uint32_t m_enableDecodeFilter : 1;
    uint32_t m_useImageArray : 1;
    uint32_t m_useImageViewArray : 1;
    uint32_t m_useSeparateOutputImages : 1;
    uint32_t m_useLinearOutput : 1;
    uint32_t m_resetDecoder : 1;
    uint32_t m_dumpDecodeData : 1;
    int32_t  m_numBitstreamBuffersToPreallocate;
    VkDeviceSize   m_maxStreamBufferSize;
    VulkanFilterYuvCompute::FilterType m_filterType;
    VkSharedBaseObj<VulkanFilter> m_yuvFilter;
};
