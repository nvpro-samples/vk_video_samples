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

#include "VkParserVideoRefCountBase.h"
#include "VulkanVideoParser.h"
#include "vulkan_interfaces.h"

namespace vulkanVideoUtils {
    class ImageObject;
    class VulkanDeviceInfo;
}

struct DecodedFrame {
    int32_t pictureIndex;
    const vulkanVideoUtils::ImageObject* pDecodedImage;
    VkFence frameCompleteFence;
    VkFence frameConsumerDoneFence;
    VkSemaphore frameCompleteSemaphore;
    VkSemaphore frameConsumerDoneSemaphore;
    VkQueryPool queryPool;
    int32_t startQueryId;
    uint32_t numQueries;
    uint64_t timestamp;
    uint32_t hasConsummerSignalFence : 1;
    uint32_t hasConsummerSignalSemaphore : 1;
    // For debugging
    int32_t decodeOrder;
    int32_t displayOrder;
};

struct DecodedFrameRelease {
    int32_t pictureIndex;
    VkVideotimestamp timestamp;
    uint32_t hasConsummerSignalFence : 1;
    uint32_t hasConsummerSignalSemaphore : 1;
    // For debugging
    int32_t decodeOrder;
    int32_t displayOrder;
};

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

    struct PictureResourceInfo {
        VkImage image;
        VkImageLayout currentImageLayout;
    };

    virtual int32_t InitImagePool(uint32_t numImages, const VkImageCreateInfo* pImageCreateInfo, const VkVideoProfileKHR* pDecodeProfile = NULL) = 0;
    virtual int32_t QueuePictureForDecode(int8_t picId, VkParserDecodePictureInfo* pDecodePictureInfo, FrameSynchronizationInfo* pFrameSynchronizationInfo) = 0;
    virtual int32_t DequeueDecodedPicture(DecodedFrame* pDecodedFrame) = 0;
    virtual int32_t ReleaseDisplayedPicture(DecodedFrameRelease** pDecodedFramesRelease, uint32_t numFramesToRelease) = 0;
    virtual int32_t GetImageResourcesByIndex(uint32_t numResources, const int8_t* referenceSlotIndexes,
        VkVideoPictureResourceKHR* pictureResources,
        PictureResourceInfo* pictureResourcesInfo,
        VkImageLayout newImageLayout = VK_IMAGE_LAYOUT_MAX_ENUM)
        = 0;
    virtual int32_t ReleaseImageResources(uint32_t numResources, const uint32_t* indexes) = 0;
    virtual int32_t SetPicNumInDecodeOrder(int32_t picId, int32_t picNumInDecodeOrder) = 0;
    virtual int32_t SetPicNumInDisplayOrder(int32_t picId, int32_t picNumInDisplayOrder) = 0;
    virtual size_t GetSize() = 0;

    virtual ~VulkanVideoFrameBuffer() { }

    static VulkanVideoFrameBuffer* CreateInstance(vulkanVideoUtils::VulkanDeviceInfo* pVideoRendererDeviceInfo);
};

#endif /* _VULKANVIDEOFRAMEBUFFER_H_ */
