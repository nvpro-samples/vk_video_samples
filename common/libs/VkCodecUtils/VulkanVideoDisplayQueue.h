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

    virtual bool IsValid(void)    const { return true; }
    virtual int32_t GetWidth()    const { return m_defaultWidth; }
    virtual int32_t GetHeight()   const { return m_defaultHeight; }
    virtual int32_t GetBitDepth() const { return m_defaultBitDepth; }
    virtual VkFormat GetFrameImageFormat(int32_t* pWidth = NULL, int32_t* pHeight = NULL,
                                         int32_t* pBitDepth = NULL)  const;
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

#endif /* _VULKANVIDEODISPLAYQUEUE_H_ */
