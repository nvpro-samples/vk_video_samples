/*
 * Copyright 2022 NVIDIA Corporation.
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

#ifndef _NVPICTUREBUFFER_H_
#define _NVPICTUREBUFFER_H_

#include "vulkan/vulkan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string.h>
#include <string>
#include <vector>

#include "VkCodecUtils/VkBufferResource.h"
#include "VkCodecUtils/VkImageResource.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VulkanCommandBuffersSet.h"
#include "VkVideoCore/VkVideoCoreProfile.h"

#define INPUT_FRAME_BUFFER_SIZE 16
#define DECODED_PICTURE_BUFFER_SIZE 16

class ReferenceFrameData {
public:
    ReferenceFrameData()
        : m_dpbIdx(-1)
        , m_stdRefPicData()
        , m_poc(-1)
    {
    }

    int8_t m_dpbIdx; // -1 when invalid //
    StdVideoEncodeH264RefPicMarkingEntry m_stdRefPicData;
    int32_t m_poc; // index in video sequence - picture order count
};

class EncodeFrameData {
public:
    EncodeFrameData()
        : m_vkDevCtx()
        , m_inputImageView()
        , m_currentImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
        , m_RefPics{}
        , m_usedDpbMask()
        , m_refCount(0)
        , m_frameCompleteFence()
        , m_frameEncodedSemaphore()
        , m_frameConsumerDoneFence()
        , m_frameProducerDoneSemaphore()
        , m_queueFamilyIndex()
        , m_videoProfile()
        , m_extent { 0, 0 }
        , m_cmdBufVideoEncode()
        , m_frameSubmitted(false)
    {
    }

    ~EncodeFrameData() {
        DeinitFramePool();
    }

    void DeinitFramePool();

    const VulkanDeviceContext*           m_vkDevCtx;
    VkSharedBaseObj<VkImageResourceView> m_inputImageView;
    VkImageLayout                        m_currentImageLayout;
    ReferenceFrameData m_RefPics[DECODED_PICTURE_BUFFER_SIZE];
    uint32_t m_usedDpbMask; // binary mask for m_RefPics
    std::atomic<int32_t>  m_refCount; // num of ref pictures for this pic
    VkFence m_frameCompleteFence;
    VkSemaphore m_frameEncodedSemaphore;
    VkFence m_frameConsumerDoneFence;
    VkSemaphore m_frameProducerDoneSemaphore;
    uint32_t m_queueFamilyIndex;
    VkVideoCoreProfile m_videoProfile;
    VkExtent2D m_extent;
    VkSharedBaseObj<VkBufferResource>    m_outBitstreamBuffer;
    VkSharedBaseObj<VkImageResourceView> m_linearInputImage;
    VkCommandBuffer m_cmdBufVideoEncode;
    uint32_t m_frameSubmitted:1;
};

class VkEncoderPictureBuffer {
public:
    VkEncoderPictureBuffer()
        : m_vkDevCtx()
        , m_queueFamilyIndex((uint32_t)-1)
        , m_videoProfile()
        , m_imageCreateInfo()
        , m_frameBufferSize(0)
        , m_dpbSize(0)
        , m_maxBitstreamSize(0)
        , m_encodeFrameData()
        , m_queryPool()
        , m_extent { 0, 0 }
        , m_fullImageSize(0)
        , m_imageFormat()
    {
    }

    ~VkEncoderPictureBuffer() {
        DeinitReferenceFramePool();
        DeinitFramePool();
    }

    VkResult CreateVideoQueries(uint32_t numSlots, const VulkanDeviceContext* vkDevCtx, const VkVideoProfileInfoKHR* pEncodeProfile);

    VkResult InitFramePool(const VulkanDeviceContext* vkDevCtx,
                           const VkVideoProfileInfoKHR* pEncodeProfile,
                           uint32_t                 numImages,
                           VkFormat                 imageFormat,
                           uint32_t                 maxImageWidth,
                           uint32_t                 maxImageHeight,
                           uint32_t                 fullImageSize,
                           VkImageTiling            tiling,
                           VkImageUsageFlags        usage,
                           uint32_t                 queueFamilyIndex);
    void PrepareInputImages(VkCommandBuffer cmdBuf);

    VkResult InitReferenceFramePool( uint32_t                   numImages,
                                     VkFormat                   imageFormat,
                                     VkMemoryPropertyFlags memoryPropertyFlags);
    void PrepareReferenceImages(VkCommandBuffer cmdBuf);

    void GetFrameResourcesByIndex( int8_t referenceSlotIndexes,
                                   VkVideoPictureResourceInfoKHR* pictureResources);
    void GetReferenceFrameResourcesByIndex( int8_t dpbSlotIdx,
                                            VkVideoPictureResourceInfoKHR* pictureResources);
    VkResult InitFrame( uint32_t numImages,
                        const VulkanDeviceContext* vkDevCtx,
                        const VkImageCreateInfo* pImageCreateInfo,
                        VkMemoryPropertyFlags requiredMemProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    void AddRefPic(uint8_t inImageIdx, int8_t dpbIdx, uint32_t poc);
    void ReleaseRefPic(uint8_t inImageIdx);
    int32_t ConfigRefPics(uint8_t distBetweenAnchors, uint8_t distanceBetweenIntras, uint32_t currentPoc, uint8_t currentEncodeFrameIdx);

    VkResult CopyToVkImage(uint32_t index, uint32_t bufferOffset, VkCommandBuffer cmdBuf);

    EncodeFrameData* GetEncodeFrameData(uint32_t index);
    VkQueryPool GetQueryPool();

    void DeinitReferenceFramePool();
    void DeinitFramePool();

    // debug only
    VkResult CopyToVkBuffer(VkBuffer yuvInput, VkImage image, uint32_t width, uint32_t height, VkCommandBuffer cmdBuf);
    VkResult CopyLinearToOptimalImage(VkCommandBuffer& commandBuffer,
                                 VkSharedBaseObj<VkImageResourceView>& srcImageView,
                                 VkSharedBaseObj<VkImageResourceView>& dstImageView,
                                 uint32_t srcCopyArrayLayer = 0,
                                 uint32_t dstCopyArrayLayer = 0,
                                 VkImageLayout srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VkImageLayout dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkResult CopyToBuffer(VkImage* image, VkBuffer* buffer, VkImageLayout layout, std::vector<VkBufferImageCopy> &copyRegions, VkCommandBuffer* cmdBuf);

private:
    VkImageLayout TransitionLayout(VkCommandBuffer cmdBuf, VkSharedBaseObj<VkImageResourceView>& imageView, VkImageLayout layout);

private:
    const VulkanDeviceContext*           m_vkDevCtx;
    uint32_t                             m_queueFamilyIndex;
    VkVideoCoreProfile                   m_videoProfile;
    VkImageCreateInfo                    m_imageCreateInfo;
    size_t                               m_frameBufferSize;
    size_t                               m_dpbSize;
    uint32_t                             m_maxBitstreamSize;
    VulkanCommandBuffersSet              m_commandBuffersSet;
    EncodeFrameData                      m_encodeFrameData[INPUT_FRAME_BUFFER_SIZE];
    VkSharedBaseObj<VkImageResourceView> m_dpb[DECODED_PICTURE_BUFFER_SIZE];
    VkQueryPool                          m_queryPool;
    VkExtent2D                           m_extent;
    uint32_t                             m_fullImageSize;
    VkFormat                             m_imageFormat;
};

#endif
