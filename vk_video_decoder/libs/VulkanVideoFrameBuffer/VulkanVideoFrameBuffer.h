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

#include "VkVideoCore/VkVideoRefCountBase.h"
#include "VulkanVideoParser.h"
#include "vulkan_interfaces.h"
#include "VkCodecUtils/VkImageResource.h"

struct DecodedFrame {
    int32_t pictureIndex;
    int32_t displayWidth;
    int32_t displayHeight;
    VkSharedBaseObj<VkImageResourceView> decodedImageView;
    VkSharedBaseObj<VkImageResourceView> outputImageView;
    VkFence frameCompleteFence; // If valid, the fence is signaled when the decoder is done decoding the frame.
    VkFence frameConsumerDoneFence; // If valid, the fence is signaled when the consumer (graphics, compute or display) is done using the frame.
    VkSemaphore frameCompleteSemaphore; // If valid, the semaphore is signaled when the decoder is done decoding the frame.
    VkSemaphore frameConsumerDoneSemaphore; // If valid, the semaphore is signaled when the consumer (graphics, compute or display) is done using the frame.
    VkQueryPool queryPool; // queryPool handle used for the video queries.
    int32_t startQueryId;  // query Id used for the this frame.
    uint32_t numQueries;   // usually one query per frame
    // If multiple queues are available, submittedVideoQueueIndex is the queue index that the video frame was submitted to.
    // if only one queue is available, submittedVideoQueueIndex will always have a value of "0".
    int32_t  submittedVideoQueueIndex;
    uint64_t timestamp;
    uint64_t decodeOrder;
    uint32_t hasConsummerSignalFence : 1;
    uint32_t hasConsummerSignalSemaphore : 1;
    // For debugging
    int32_t displayOrder;

    void Reset()
    {
        pictureIndex = -1;
        displayWidth = 0;
        displayHeight = 0;
        decodedImageView  = nullptr;
        outputImageView = nullptr;
        frameCompleteFence = VkFence();
        frameConsumerDoneFence = VkFence();
        frameCompleteSemaphore = VkSemaphore();
        frameConsumerDoneSemaphore = VkSemaphore();
        queryPool = VkQueryPool();
        startQueryId = 0;
        numQueries = 0;
        submittedVideoQueueIndex = 0;
        timestamp = 0;
        hasConsummerSignalFence = false;
        hasConsummerSignalSemaphore = false;
        // For debugging
        decodeOrder = 0;
        displayOrder = 0;
    }

};

struct DecodedFrameRelease {
    int32_t pictureIndex;
    VkVideotimestamp timestamp;
    uint32_t hasConsummerSignalFence : 1;
    uint32_t hasConsummerSignalSemaphore : 1;
    // For debugging
    int32_t displayOrder;
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
        int32_t startQueryId;
        uint32_t numQueries;
        uint32_t hasFrameCompleteSignalFence : 1;
        uint32_t hasFrameCompleteSignalSemaphore : 1;
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

        const VkVideoRefCountBase*     pStdAv1Sps;

        ReferencedObjectsInfo(const VkVideoRefCountBase* pBitstreamDataRef,
                              const VkVideoRefCountBase* pStdPpsRef,
                              const VkVideoRefCountBase* pStdSpsRef,
                              const VkVideoRefCountBase* pStdVpsRef = nullptr,
                              const VkVideoRefCountBase* pStdAv1Sps = nullptr)
        : pBitstreamData(pBitstreamDataRef)
        , pStdPps(pStdPpsRef)
        , pStdSps(pStdSpsRef)
        , pStdVps(pStdVpsRef)
        , pStdAv1Sps(pStdAv1Sps) {}
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
    virtual int32_t DequeueDecodedPicture(DecodedFrame* pDecodedFrame) = 0;
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
    virtual int32_t ReleaseImageResources(uint32_t numResources, const uint32_t* indexes) = 0;
    virtual uint64_t SetPicNumInDecodeOrder(int32_t picId, uint64_t picNumInDecodeOrder) = 0;
    virtual int32_t SetPicNumInDisplayOrder(int32_t picId, int32_t picNumInDisplayOrder) = 0;
    virtual size_t GetSize() = 0;

    virtual ~VulkanVideoFrameBuffer() { }

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VulkanVideoFrameBuffer>& vkVideoFrameBuffer);
};

#endif /* _VULKANVIDEOFRAMEBUFFER_H_ */
