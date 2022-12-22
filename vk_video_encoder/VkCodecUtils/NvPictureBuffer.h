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

#define VK_ENABLE_BETA_EXTENSIONS 1
#include "vulkan/vulkan.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string.h>
#include <string>
#include <vector>

#include "VkVideoCore/VkVideoCoreProfile.h"
#include "nvvk/resourceallocator_vk.hpp"
#include "nvvk/context_vk.hpp"
#include "nvvk/images_vk.hpp"
#include "nvvk/buffers_vk.hpp"
#include "nvvk/commands_vk.hpp"

#define INPUT_FRAME_BUFFER_SIZE 16
#define DECODED_PICTURE_BUFFER_SIZE 16

class Picture {
public:
    Picture()
        : m_image()
        , m_imageView()
        , m_imageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
    {
    }

    Picture(nvvk::Image refImage, nvvk::Texture refImageView, VkImageLayout refImageLayout)
    {
        m_image = refImage;
        m_imageView = refImageView;
        m_imageLayout = refImageLayout;
    };
    nvvk::Image m_image;
    nvvk::Texture m_imageView;
    VkImageLayout m_imageLayout;
private:

};
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
        : m_picture()
        , m_RefPics{}
        , m_usedDpbMask()
        , m_refCount(0)
        , m_device()
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

    void deinitFramePool(nvvk::ResourceAllocatorDedicated* m_resAlloc);

    Picture m_picture;
    ReferenceFrameData m_RefPics[DECODED_PICTURE_BUFFER_SIZE];
    uint32_t m_usedDpbMask; // binary mask for m_RefPics
    int32_t m_refCount; // num of ref pictures for this pic
    VkDevice m_device;
    // nvvk::ResourceAllocatorDedicated* m_resAlloc;
    VkFence m_frameCompleteFence;
    VkSemaphore m_frameEncodedSemaphore;
    VkFence m_frameConsumerDoneFence;
    VkSemaphore m_frameProducerDoneSemaphore;
    uint32_t m_queueFamilyIndex;
    VkVideoCoreProfile m_videoProfile;
    VkExtent2D m_extent;
    nvvk::Buffer m_outBitstreamBuffer;
    nvvk::Buffer m_inputStagingBuffer;
    VkCommandBuffer m_cmdBufVideoEncode;
    uint32_t m_frameSubmitted:1;
};

class NvPictureBuffer {
public:
    NvPictureBuffer()
        : m_pCtx()
        , m_queueFamilyIndex((uint32_t)-1)
        , m_videoProfile()
        , m_imageCreateInfo()
        , m_encodeFrameData()
        , m_queryPool()
        , m_resAlloc()
        , m_extent { 0, 0 }
        , m_frameBufferSize(0)
        , m_dpbSize(0)
        , m_fullImageSize(0)
        , m_maxBitstreamSize(0)
        , m_imageFormat()
    {
    }
    VkResult createVideoQueries(uint32_t numSlots, nvvk::Context* deviceInfo, const VkVideoProfileInfoKHR* pEncodeProfile);

    int32_t initFramePool( nvvk::Context* ctx,
                           const VkVideoProfileInfoKHR* pEncodeProfile,
                           uint32_t                 numImages,
                           VkFormat                 imageFormat,
                           uint32_t                 maxImageWidth,
                           uint32_t                 maxImageHeight,
                           uint32_t                 fullImageSize,
                           VkImageTiling            tiling,
                           VkImageUsageFlags        usage,
                           nvvk::ResourceAllocatorDedicated* rAlloc,
                           nvvk::CommandPool*        cmdPoolVideoEncode,
                           uint32_t                 queueFamilyIndex);
    void prepareInputImages(VkCommandBuffer cmdBuf);

    void initReferenceFramePool( uint32_t                   numImages,
                                 VkFormat                   imageFormat,
                                 nvvk::ResourceAllocator*   rAlloc);
    void prepareReferenceImages(VkCommandBuffer cmdBuf);

    void getFrameResourcesByIndex( int8_t referenceSlotIndexes,
                                   VkVideoPictureResourceInfoKHR* pictureResources);
    void getReferenceFrameResourcesByIndex( int8_t dpbSlotIdx,
                                            VkVideoPictureResourceInfoKHR* pictureResources);
    uint32_t initFrame( uint32_t numImages,
                        VkDevice dev,
                        const VkImageCreateInfo* pImageCreateInfo,
                        nvvk::ResourceAllocatorDedicated* rAlloc,
                        nvvk::CommandPool*        cmdPoolVideoEncode,
                        VkMemoryPropertyFlags requiredMemProps = 0,
                        int32_t initWithPattern = -1,
                        VkExternalMemoryHandleTypeFlagBitsKHR exportMemHandleTypes = VkExternalMemoryHandleTypeFlagBitsKHR());

    void addRefPic(uint8_t inImageIdx, int8_t dpbIdx, uint32_t poc);
    void removeRefPic(uint8_t inImageIdx);
    int32_t configRefPics(uint8_t distBetweenAnchors, uint8_t distanceBetweenIntras, uint32_t currentPoc, uint8_t currentEncodeFrameIdx);

    VkResult copyToVkImage(uint32_t index, uint32_t bufferOffset, VkCommandBuffer cmdBuf);

    EncodeFrameData* getEncodeFrameData(uint32_t index);
    VkQueryPool getQueryPool();
    size_t size();

    void deinitReferenceFramePool();
    void deinitFramePool();

    // debug only
    VkResult copyToVkBuffer(VkBuffer yuvInput, VkImage image, uint32_t width, uint32_t height, VkCommandBuffer cmdBuf);
    VkResult copyToBuffer(VkImage* image, VkBuffer* buffer, VkImageLayout layout, std::vector<VkBufferImageCopy> &copyRegions, VkCommandBuffer* cmdBuf);

private:
    static void initImageLayout(VkCommandBuffer cmdBuf, Picture* picture, VkImageLayout layout);

private:
    nvvk::Context*                      m_pCtx;
    uint32_t                            m_queueFamilyIndex;
    VkVideoCoreProfile                  m_videoProfile;
    VkImageCreateInfo                   m_imageCreateInfo;
    size_t                              m_frameBufferSize;
    size_t                              m_dpbSize;
    uint32_t                            m_maxBitstreamSize;
    EncodeFrameData                     m_encodeFrameData[INPUT_FRAME_BUFFER_SIZE];
    Picture                             m_dpb[DECODED_PICTURE_BUFFER_SIZE];
    nvvk::ResourceAllocatorDedicated*   m_resAlloc;
    VkQueryPool                         m_queryPool;
    VkExtent2D                          m_extent;
    uint32_t                            m_fullImageSize;
    VkFormat                            m_imageFormat;
};

#endif
