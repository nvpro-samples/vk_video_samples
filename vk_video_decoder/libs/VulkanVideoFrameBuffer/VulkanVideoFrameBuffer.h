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

#ifndef _VULKANVIDEOFRAMEBUFFER_H_
#define _VULKANVIDEOFRAMEBUFFER_H_

#include <assert.h>
#include <stdint.h>

#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "vkvideo_parser/VulkanVideoParser.h"
#include "vulkan_interfaces.h"
#include "VkCodecUtils/VkImageResource.h"
#include "VkCodecUtils/VulkanDecodedFrame.h"

struct DecodedFrameRelease {
    int32_t pictureIndex;
    VkVideotimestamp timestamp;
    uint32_t hasConsummerSignalFence : 1;
    uint32_t hasConsummerSignalSemaphore : 1;
    // For debugging
    uint64_t displayOrder;
    uint64_t decodeOrder;
};

class VkParserVideoPictureParameters;

class VulkanVideoFrameBuffer : public IVulkanVideoFrameBufferParserCb {
public:
    // Synchronization
    struct FrameSynchronizationInfo {
        VkFence frameCompleteFence;
        VkSemaphore frameCompleteSemaphore;
        VkFence frameConsumerDoneFence;
        VkSemaphore frameConsumerDoneSemaphore;
        VkQueryPool queryPool;
        uint32_t startQueryId;
        uint32_t numQueries;
        uint32_t hasFrameCompleteSignalFence : 1;
        uint32_t hasFrameCompleteSignalSemaphore : 1;
        uint32_t syncOnFrameCompleteFence : 1;
        uint32_t syncOnFrameConsumerDoneFence : 1;
    };

    struct ReferencedObjectsInfo {

        // The bitstream Buffer
        const VkVideoRefCountBase*     pBitstreamData;
        // PPS
        const VkVideoRefCountBase*     pStdPps;
        // SPS
        const VkVideoRefCountBase*     pStdSps;
        // VPS
        const VkVideoRefCountBase*     pStdVps;

        ReferencedObjectsInfo(const VkVideoRefCountBase* pBitstreamDataRef,
                              const VkVideoRefCountBase* pStdPpsRef,
                              const VkVideoRefCountBase* pStdSpsRef,
                              const VkVideoRefCountBase* pStdVpsRef = nullptr)
        : pBitstreamData(pBitstreamDataRef)
        , pStdPps(pStdPpsRef)
        , pStdSps(pStdSpsRef)
        , pStdVps(pStdVpsRef) {}
    };

    struct PictureResourceInfo {
        VkImage  image;
        VkFormat imageFormat;
        VkImageLayout currentImageLayout;
    };

    virtual int32_t InitImagePool(const VkVideoProfileInfoKHR* pDecodeProfile,
                                  uint32_t                 numImages,
                                  VkFormat                 dpbImageFormat,
                                  VkFormat                 outImageFormat,
                                  const VkExtent2D&        codedExtent,
                                  const VkExtent2D&        maxImageExtent,
                                  VkImageUsageFlags        dpbImageUsage,
                                  VkImageUsageFlags        outImageUsage,
                                  uint32_t                 queueFamilyIndex,
                                  int32_t                  numImagesToPreallocate,
                                  bool                     useImageArray = false,
                                  bool                     useImageViewArray = false,
                                  bool                     useSeparateOutputImage = false,
                                  bool                     useLinearOutput = false) = 0;

    virtual int32_t QueuePictureForDecode(int8_t picId, VkParserDecodePictureInfo* pDecodePictureInfo,
                                          ReferencedObjectsInfo* pReferencedObjectsInfo,
                                          FrameSynchronizationInfo* pFrameSynchronizationInfo) = 0;
    virtual int32_t DequeueDecodedPicture(VulkanDecodedFrame* pDecodedFrame) = 0;
    virtual int32_t ReleaseDisplayedPicture(DecodedFrameRelease** pDecodedFramesRelease, uint32_t numFramesToRelease) = 0;
    virtual int32_t GetDpbImageResourcesByIndex(uint32_t numResources, const int8_t* referenceSlotIndexes,
                                                VkVideoPictureResourceInfoKHR* pictureResources,
                                                PictureResourceInfo* pictureResourcesInfo,
                                                VkImageLayout newDpbImageLayerLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) = 0;
    virtual int32_t GetCurrentImageResourceByIndex(int8_t referenceSlotIndex,
                                                   VkVideoPictureResourceInfoKHR* dpbPictureResource,
                                                   PictureResourceInfo* dpbPictureResourceInfo,
                                                   VkImageLayout newDpbImageLayerLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                                                   VkVideoPictureResourceInfoKHR* outputPictureResource = nullptr,
                                                   PictureResourceInfo* outputPictureResourceInfo = nullptr,
                                                   VkImageLayout newOutputImageLayerLayout = VK_IMAGE_LAYOUT_MAX_ENUM) = 0;
    virtual int32_t GetCurrentImageResourceByIndex(int8_t referenceSlotIndex,
                                                   VkSharedBaseObj<VkImageResourceView>& decodedImageView,
                                                   VkSharedBaseObj<VkImageResourceView>& outputImageView) = 0;
    virtual int32_t ReleaseImageResources(uint32_t numResources, const uint32_t* indexes) = 0;
    virtual uint64_t SetPicNumInDecodeOrder(int32_t picId, uint64_t picNumInDecodeOrder) = 0;
    virtual int32_t SetPicNumInDisplayOrder(int32_t picId, int32_t picNumInDisplayOrder) = 0;
    virtual size_t GetSize() = 0;

    virtual ~VulkanVideoFrameBuffer() { }

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VulkanVideoFrameBuffer>& vkVideoFrameBuffer);
};

static inline VkResult vkWaitAndResetFence(const VulkanDeviceContext* vkDevCtx, VkFence fence,
                                           const char* fenceName = "unknown", uint32_t fenceNum = 0,
                                           const uint64_t fenceWaitTimeout = 100 * 1000 * 1000 /* 100 mSec */) {

    assert(vkDevCtx != nullptr);
    assert(fence != VK_NULL_HANDLE);

    VkResult result = vkDevCtx->WaitForFences(*vkDevCtx, 1, &fence, true, fenceWaitTimeout);
    if (result != VK_SUCCESS) {
        std::cerr << "\t *************** WARNING: fence " << fenceName << " is not done after  " << fenceWaitTimeout <<
                         "nSec *************< " << fenceNum << " >**********************" << std::endl;
        assert(!"Fence is not signaled yet after more than 100 mSec wait");
    }

    result = vkDevCtx->GetFenceStatus(*vkDevCtx, fence);
    if (result == VK_NOT_READY) {
        std::cerr << "\t *************** WARNING: fence " << fenceName << " is VK_NOT_READY *************< " <<
                        fenceNum << " >**********************" << std::endl;
        assert(!"Fence is not signaled yet");
    }

    result = vkDevCtx->ResetFences(*vkDevCtx, 1, &fence);
    assert(result == VK_SUCCESS);

    result = vkDevCtx->GetFenceStatus(*vkDevCtx, fence);
    assert(result == VK_NOT_READY);

    return result;
}

#endif /* _VULKANVIDEOFRAMEBUFFER_H_ */
