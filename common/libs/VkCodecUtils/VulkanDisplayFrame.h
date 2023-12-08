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

#ifndef COMMON_LIBS_VKCODECUTILS_VULKANDISPLAYFRAME_H_
#define COMMON_LIBS_VKCODECUTILS_VULKANDISPLAYFRAME_H_

#include "stdint.h"
#include "VkCodecUtils/VkImageResource.h"

class VulkanDisplayFrame
{
public:
    int32_t  pictureIndex;
    uint32_t imageLayerIndex; // The layer of a multi-layered images. Always "0" for single layered images
    int32_t  displayWidth;    // Valid usable width of the image
    int32_t  displayHeight;   // Valid usable height of the image
    uint64_t decodeOrder;
    uint64_t displayOrder;
    uint64_t timestamp;
    VkSharedBaseObj<VkImageResourceView> imageView; // input or output image view resource to be displayed
    VkSharedBaseObj<VkImageResourceView> dpbImageView;   // dpb image view (optional)
    VkFence frameCompleteFence; // If valid, the fence is signaled when the decoder or encoder is done decoding / encoding the frame.
    VkFence frameConsumerDoneFence; // If valid, the fence is signaled when the consumer (graphics, compute or display) is done using the frame.
    VkSemaphore frameCompleteSemaphore; // If valid, the semaphore is signaled when the decoder or encoder is done decoding / encoding the frame.
    VkSemaphore frameConsumerDoneSemaphore; // If valid, the semaphore is signaled when the consumer (graphics, compute or display) is done using the frame.
    VkQueryPool queryPool;                  // queryPool handle used for the video queries.
    int32_t startQueryId;                   // query Id used for the this frame.
    uint32_t numQueries;                    // usually one query per frame
    // If multiple queues are available, submittedVideoQueueIndex is the queue index that the video frame was submitted to.
    // if only one queue is available, submittedVideoQueueIndex will always have a value of "0".
    int32_t  submittedVideoQueueIndex;
    uint32_t hasConsummerSignalFence : 1;
    uint32_t hasConsummerSignalSemaphore : 1;

    void Reset()
    {
        pictureIndex = -1;
        imageLayerIndex = 0;
        displayWidth = 0;
        displayHeight = 0;
        imageView  = nullptr;
        dpbImageView = nullptr;
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

    VulkanDisplayFrame()
        : pictureIndex(-1)
        , imageLayerIndex(0)
    , displayWidth()
    , displayHeight()
    , decodeOrder()
    , displayOrder()
    , timestamp()
    , imageView()
    , dpbImageView()
    , frameCompleteFence()
    , frameConsumerDoneFence()
    , frameCompleteSemaphore()
    , frameConsumerDoneSemaphore()
    , queryPool()
    , startQueryId()
    , numQueries()
    , submittedVideoQueueIndex()
    , hasConsummerSignalFence()
    , hasConsummerSignalSemaphore()
    {}

    virtual ~VulkanDisplayFrame() {
        Reset();
    }
};

#endif /* COMMON_LIBS_VKCODECUTILS_VULKANDISPLAYFRAME_H_ */
