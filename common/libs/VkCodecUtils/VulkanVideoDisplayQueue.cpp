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

#include <assert.h>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <fstream>

#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VulkanVideoDisplayQueue.h"

template<class FrameDataType>
VkResult VulkanVideoDisplayQueue<FrameDataType>::Create(const VulkanDeviceContext* vkDevCtx,
                                                        int32_t defaultWidth, int32_t defaultHeight,
                                                        int32_t defaultBitDepth, VkFormat defaultImageFormat,
                                                        VkSharedBaseObj<VulkanVideoDisplayQueue>& vulkanVideoDisplayQueue)
{
    VkSharedBaseObj<VulkanVideoDisplayQueue> videoQueue(new VulkanVideoDisplayQueue(vkDevCtx,
                                                                                    defaultWidth,
                                                                                    defaultHeight,
                                                                                    defaultBitDepth,
                                                                                    defaultImageFormat));

    if (videoQueue) {
        vulkanVideoDisplayQueue = videoQueue;
        return VK_SUCCESS;
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

template<class FrameDataType>
VkFormat VulkanVideoDisplayQueue<FrameDataType>::GetFrameImageFormat(int32_t* pWidth, int32_t* pHeight, int32_t* pBitDepth)  const
{
    VkFormat frameImageFormat = VK_FORMAT_UNDEFINED;
    if (true) {
        if (GetBitDepth() == 8) {
            frameImageFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        } else if (GetBitDepth() == 10) {
            frameImageFormat = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
        } else if (GetBitDepth() == 12) {
            frameImageFormat = VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
        } else {
            assert(0);
        }

        if (pWidth) {
            *pWidth = GetWidth();
        }

        if (pHeight) {
            *pHeight = GetHeight();
        }

        if (pBitDepth) {
            *pBitDepth = GetBitDepth();
        }
    }

    return frameImageFormat;
}

template<class FrameDataType>
void VulkanVideoDisplayQueue<FrameDataType>::Deinit()
{

}

template<class FrameDataType>
int32_t VulkanVideoDisplayQueue<FrameDataType>::EnqueueFrame(FrameDataType* pFrame)
{
    if (!m_queueIsEnabled) {
        return -1;
    }

    m_queue.Push(*pFrame);

    return (int32_t)m_queue.Size();
}

template<class FrameDataType>
int32_t VulkanVideoDisplayQueue<FrameDataType>::GetNextFrame(FrameDataType* pFrame, bool* endOfStream)
{
    if (m_exitQueueRequested) {
        m_queue.SetFlushAndExit();
        m_queueIsEnabled = false;
    }

    *endOfStream = !m_queue.WaitAndPop(*pFrame) && !m_queueIsEnabled;

    if (*endOfStream) {
        return -1;
    }

    return 1;
}

template<class FrameDataType>
int32_t VulkanVideoDisplayQueue<FrameDataType>::ReleaseFrame(FrameDataType* pDisplayedFrame)
{
    return 1;
}

#include "VkCodecUtils/VulkanVideoEncodeDisplayQueue.h"

VkResult CreateVulkanVideoEncodeDisplayQueue(const VulkanDeviceContext* vkDevCtx,
                                             int32_t defaultWidth, int32_t defaultHeight,
                                             int32_t defaultBitDepth, VkFormat defaultImageFormat,
                                             VkSharedBaseObj<VulkanVideoDisplayQueue<VulkanEncoderInputFrame>>& vulkanVideoEncodeDisplayQueue)
{
    VkSharedBaseObj<VulkanVideoDisplayQueue<VulkanEncoderInputFrame>> vulkanVideoDisplayQueue;
    VkResult result = VulkanVideoDisplayQueue<VulkanEncoderInputFrame>::Create(vkDevCtx,
                                                                               defaultWidth,
                                                                               defaultHeight,
                                                                               defaultBitDepth,
                                                                               defaultImageFormat,
                                                                               vulkanVideoDisplayQueue);
    if (result != VK_SUCCESS) {
        return result;
    }

    if (vulkanVideoDisplayQueue) {
        vulkanVideoEncodeDisplayQueue = vulkanVideoDisplayQueue;
        return VK_SUCCESS;
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
