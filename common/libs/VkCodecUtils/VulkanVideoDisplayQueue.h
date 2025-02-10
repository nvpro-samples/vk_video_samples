/*
* Copyright 2024 NVIDIA Corporation.
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

#ifndef _VULKANVIDEODISPLAYQUEUE_H_
#define _VULKANVIDEODISPLAYQUEUE_H_

#include <vector>
#include <atomic>
#include <condition_variable>
#include "VkCodecUtils/VkVideoQueue.h"
#include "VkCodecUtils/VkThreadSafeQueue.h"

template<class FrameDataType>
class VulkanVideoDisplayQueue : public VkVideoQueue<FrameDataType> {
public:

    virtual int32_t GetWidth()    const { return m_defaultWidth; }
    virtual int32_t GetHeight()   const { return m_defaultHeight; }
    virtual int32_t GetBitDepth() const { return m_defaultBitDepth; }
    virtual VkFormat GetFrameImageFormat()  const;
    virtual VkVideoProfileInfoKHR GetVkProfile() const
    {
        VkVideoProfileInfoKHR videoProfile{};
        return videoProfile;
    }

    virtual uint32_t GetProfileIdc() const { return 0; }
    virtual VkExtent3D GetVideoExtent() const;
    virtual int32_t GetNextFrame(FrameDataType* pFrame, bool* endOfStream);
    virtual int32_t ReleaseFrame(FrameDataType* pDisplayedFrame);

    static VkSharedBaseObj<VulkanVideoDisplayQueue>& invalidVulkanVideoDisplayQueue;

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           int32_t defaultWidth, int32_t defaultHeight,
                           int32_t defaultBitDepth, VkFormat defaultImageFormat,
                           VkSharedBaseObj<VulkanVideoDisplayQueue>& VulkanVideoDisplayQueue = invalidVulkanVideoDisplayQueue);

    void Deinit();

    virtual int32_t AddRef()
    {
        return ++m_refCount;
    }

    virtual int32_t Release()
    {
        uint32_t ret = --m_refCount;
        // Destroy the device if ref-count reaches zero
        if (ret == 0) {
            delete this;
        }
        return ret;
    }

    bool StopQueue() {
        m_exitQueueRequested = true;
        return m_queueIsEnabled;
    }

    bool QueueIsEnabled() {
        return m_queueIsEnabled;
    }

    virtual int32_t EnqueueFrame(FrameDataType* pFrame);

private:

    VulkanVideoDisplayQueue(const VulkanDeviceContext* vkDevCtx,
                            int32_t defaultWidth = 1920, int32_t defaultHeight = 1080,
                            int32_t defaultBitDepth = 8,
                            VkFormat defaultImageFormat = VK_FORMAT_UNDEFINED,
                            uint32_t maxPendingQueueNodes = 4)
        : m_refCount(0)
        , m_vkDevCtx(vkDevCtx)
        , m_defaultWidth(defaultWidth)
        , m_defaultHeight(defaultHeight)
        , m_defaultBitDepth(defaultBitDepth)
        , m_defaultImageFormat((defaultImageFormat != VK_FORMAT_UNDEFINED) ? defaultImageFormat : GetFrameImageFormat())
        , m_queueIsEnabled(true)
        , m_exitQueueRequested(false)
        , m_queue(maxPendingQueueNodes)
    {
    }

    virtual ~VulkanVideoDisplayQueue() { Deinit(); }

private:
    std::atomic<int32_t>       m_refCount;
    const VulkanDeviceContext* m_vkDevCtx;
    int32_t                    m_defaultWidth;
    int32_t                    m_defaultHeight;
    int32_t                    m_defaultBitDepth;
    VkFormat                   m_defaultImageFormat;
    uint32_t                   m_queueIsEnabled : 1;
    uint32_t                   m_exitQueueRequested : 1;
    VkThreadSafeQueue<FrameDataType> m_queue;
};

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
VkFormat VulkanVideoDisplayQueue<FrameDataType>::GetFrameImageFormat()  const
{
    VkFormat frameImageFormat = VK_FORMAT_UNDEFINED;
    if (GetBitDepth() == 8) {
        frameImageFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    } else if (GetBitDepth() == 10) {
        frameImageFormat = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    } else if (GetBitDepth() == 12) {
        frameImageFormat = VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
    } else {
        assert(0);
    }

    return frameImageFormat;
}

template<class FrameDataType>
VkExtent3D VulkanVideoDisplayQueue<FrameDataType>::GetVideoExtent() const
{
    VkExtent3D extent ({ (uint32_t)m_defaultWidth,
                         (uint32_t)m_defaultHeight,
                         (uint32_t)1
                       });
    return extent;
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

#endif /* _VULKANVIDEODISPLAYQUEUE_H_ */
