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
#include "VkVideoCore/DecodeFrameBufferIf.h"

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
        uint32_t optimalOutputIndex : 4;
        uint32_t linearOutputIndex : 4;
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
        VkImage       image;
        VkFormat      imageFormat;
        VkImageLayout currentImageLayout;
    };

    struct ImageSpec {

        ImageSpec()
        : imageTypeIdx((uint32_t)-1)
        , usesImageArray(false)
        , usesImageViewArray(false)
        , deferCreate(false)
        , createInfo()
        , memoryProperty()
        , imageArray()
        , imageViewArray() {}

        uint32_t              imageTypeIdx;  // -1 is an invalid index and the entry is skipped
        uint32_t              usesImageArray     : 1;
        uint32_t              usesImageViewArray : 1;
        uint32_t              deferCreate        : 1;
        VkImageCreateInfo     createInfo;
        VkMemoryPropertyFlags memoryProperty;
        // must be valid if m_usesImageArray is true
        VkSharedBaseObj<VkImageResource>     imageArray;
        // must be valid if m_usesImageViewArray is true
        VkSharedBaseObj<VkImageResourceView> imageViewArray;
    };

    virtual int32_t InitImagePool(const VkVideoProfileInfoKHR* pDecodeProfile,
                                  uint32_t                 numImages,
                                  uint32_t                 maxNumImageTypeIdx,
                                  std::array<VulkanVideoFrameBuffer::ImageSpec, DecodeFrameBufferIf::MAX_PER_FRAME_IMAGE_TYPES>& imageSpecs,
                                  uint32_t                 queueFamilyIndex,
                                  int32_t                  numImagesToPreallocate) = 0;

    virtual int32_t QueuePictureForDecode(int8_t picId, VkParserDecodePictureInfo* pDecodePictureInfo,
                                          ReferencedObjectsInfo* pReferencedObjectsInfo,
                                          FrameSynchronizationInfo* pFrameSynchronizationInfo) = 0;
    virtual int32_t DequeueDecodedPicture(VulkanDecodedFrame* pDecodedFrame) = 0;
    virtual int32_t ReleaseDisplayedPicture(DecodedFrameRelease** pDecodedFramesRelease, uint32_t numFramesToRelease) = 0;
    virtual int32_t GetImageResourcesByIndex(uint32_t numResources, const int8_t* referenceSlotIndexes, uint32_t imageTypeIdx,
                                             VkVideoPictureResourceInfoKHR* pictureResources,
                                             PictureResourceInfo* pictureResourcesInfo,
                                             VkImageLayout newImageLayerLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) = 0;
    virtual int32_t GetCurrentImageResourceByIndex(int8_t referenceSlotIndex, uint32_t imageTypeIdx,
                                                   VkVideoPictureResourceInfoKHR* pPictureResource,
                                                   PictureResourceInfo* pPictureResourceInfo,
                                                   VkImageLayout newImageLayerLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) = 0;
    virtual int32_t GetCurrentImageResourceByIndex(int8_t referenceSlotIndex, uint32_t imageTypeIdx,
                                                   VkSharedBaseObj<VkImageResourceView>& imageView) = 0;
    virtual int32_t ReleaseImageResources(uint32_t numResources, const uint32_t* indexes) = 0;
    virtual uint64_t SetPicNumInDecodeOrder(int32_t picId, uint64_t picNumInDecodeOrder) = 0;
    virtual int32_t SetPicNumInDisplayOrder(int32_t picId, int32_t picNumInDisplayOrder) = 0;
    virtual size_t GetSize() = 0;

    virtual ~VulkanVideoFrameBuffer() { }

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VulkanVideoFrameBuffer>& vkVideoFrameBuffer);
};

#endif /* _VULKANVIDEOFRAMEBUFFER_H_ */
