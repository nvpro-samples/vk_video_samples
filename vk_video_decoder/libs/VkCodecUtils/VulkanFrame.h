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

#ifndef SMOKE_H
#define SMOKE_H

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "VkVideoDecoder/VkVideoDecoder.h"
#include "VkCodecUtils/FrameProcessor.h"
#include "VkCodecUtils/VulkanVideoProcessor.h"

class VulkanFrame : public FrameProcessor {
public:

    static VkResult Create(const ProgramConfig& programConfig,
                           const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VulkanVideoProcessor>& videoProcessor,
                           VkSharedBaseObj<VulkanFrame>& frameProcessor);

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

    virtual int AttachShell(const Shell& sh);
    virtual void DetachShell();

    virtual int AttachSwapchain(const Shell& sh);
    virtual void DetachSwapchain();

    virtual int CreateFrameData(int count);
    virtual void DestroyFrameData();

    virtual bool OnKey(Key key);
    virtual bool OnFrame(  int32_t           renderIndex,
                          uint32_t           waitSemaphoreCount = 0,
                          const VkSemaphore* pWaitSemaphores  = nullptr,
                          uint32_t           signalSemaphoreCount = 0,
                          const VkSemaphore* pSignalSemaphores = nullptr,
                          const DecodedFrame** ppOutFrame = nullptr);


    VkResult DrawFrame( int32_t           renderIndex,
                       uint32_t           waitSemaphoreCount,
                       const VkSemaphore* pWaitSemaphores,
                       uint32_t           signalSemaphoreCount,
                       const VkSemaphore* pSignalSemaphores,
                       DecodedFrame*      inFrame);

    int GetVideoWidth();
    int GetVideoHeight();

    // called by attach_swapchain
    void PrepareViewport(const VkExtent2D& extent);

private:
    VulkanFrame(const ProgramConfig& programConfig,
                const VulkanDeviceContext* vkDevCtx,
                VkSharedBaseObj<VulkanVideoProcessor>& videoProcessor);
    virtual ~VulkanFrame();

private:
    std::atomic<int32_t>                  m_refCount;
    const VulkanDeviceContext*            m_vkDevCtx;
    // Decoder specific members
    VkSharedBaseObj<VulkanVideoProcessor> m_videoProcessor;
public:

    VkSamplerYcbcrModelConversion         m_samplerYcbcrModelConversion;
    VkSamplerYcbcrRange                   m_samplerYcbcrRange;
    vulkanVideoUtils::VkVideoAppCtx*      m_videoRenderer;
    uint64_t                              m_lastRealTimeNsecs;
    bool                                  m_codecPaused;
    VkQueue                               m_gfxQueue;
    VkFormat                              m_vkFormat;

    VkPhysicalDeviceProperties            m_physicalDevProps;
    std::vector<VkMemoryPropertyFlags>    m_memFlags;

    struct FrameData {
        // signaled when this struct is ready for reuse
        DecodedFrame lastDecodedFrame;
    };

    std::vector<FrameData>                m_frameData;
    int                                   m_frameDataIndex;

    VkExtent2D                            m_extent;
    VkViewport                            m_viewport;
    VkRect2D                              m_scissor;
};

#endif // HOLOGRAM_H
