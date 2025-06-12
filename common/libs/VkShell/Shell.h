/*
 * Copyright (C) 2016 Google, Inc.
 * Copyright 2022 NVIDIA Corporation.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SHELL_H
#define SHELL_H

#include <queue>
#include <vector>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <vulkan_interfaces.h>
#include "VkCodecUtils/DecoderConfig.h"

#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkCodecUtils/FrameProcessor.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkShell/VkWsiDisplay.h"

static VkSemaphore vkNullSemaphore = VkSemaphore(0);

class FrameProcessor;

class Shell : public VkWsiDisplay, public VkVideoRefCountBase {
public:

    struct Configuration {
        std::string m_windowName;
        int32_t     m_initialWidth;
        int32_t     m_initialHeight;
        int32_t     m_initialBitdepth;
        int32_t     m_backBufferCount;
        uint32_t    m_directToDisplayMode : 1;
        uint32_t    m_vsync : 1;
        uint32_t    m_verbose : 1;

        Configuration(const char* windowName, int32_t backBufferCount = 4, bool directToDisplayMode = false,
               int32_t initialWidth = 1920, int32_t initialHeight = 1080, int32_t initialBitdepth = 8,
               bool vsync = true, bool verbose = false)
            : m_windowName(windowName)
            , m_initialWidth(initialWidth)
            , m_initialHeight(initialHeight)
            , m_initialBitdepth(initialBitdepth)
            , m_backBufferCount(backBufferCount)
            , m_directToDisplayMode(false)
            , m_vsync(vsync)
            , m_verbose(verbose)
        {}

    };

    inline static VkResult AssertSuccess(VkResult res) {
        if ((res != VK_SUCCESS) && (res != VK_SUBOPTIMAL_KHR)) {
            std::stringstream ss;
            ss << "VkResult " << res << " returned";
#ifdef __cpp_exceptions
            throw std::runtime_error(ss.str());
#endif // __cpp_exceptions

        }

        return res;
    }

    Shell(const Shell &sh) = delete;
    Shell &operator=(const Shell &sh) = delete;

    static const std::vector<VkExtensionProperties>& GetRequiredInstanceExtensions(bool directToDisplayMode);
    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           const Configuration& configuration,
                           VkSharedBaseObj<Shell>& displayShell);

    virtual void AttachFrameProcessor(VkSharedBaseObj<FrameProcessor>& frameProcessor);

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

    typedef enum BACK_BUFFER_STATE { BACK_BUFFER_INIT, BACK_BUFFER_PREPARE, BACK_BUFFER_IN_SWAPCHAIN, BACK_BUFFER_CANCELED } BACK_BUFFER_STATE;

    struct AcquireBuffer {

        AcquireBuffer();
        ~AcquireBuffer();
        VkResult Create(const VulkanDeviceContext* vkDevCtx);

        const VulkanDeviceContext*                                  m_vkDevCtx;
        VkSemaphore                                                 m_semaphore;
        VkFence                                                     m_fence;
    };

    class BackBuffer {

    public:
        BackBuffer();
        ~BackBuffer();
        VkResult Create(const VulkanDeviceContext* vkDevCtx);


        AcquireBuffer* SetAcquireBuffer(uint32_t imageIndex, AcquireBuffer* acquireBuffer) {
            AcquireBuffer* oldAcquireBuffer = m_acquireBuffer;
            m_imageIndex = imageIndex;
            m_acquireBuffer = acquireBuffer;
            return oldAcquireBuffer;
        }

        const VkSemaphore& GetAcquireSemaphore() const {
            if (m_acquireBuffer){
                return m_acquireBuffer->m_semaphore;
            } else {
                return vkNullSemaphore;
            }
        }

        const VkSemaphore& GetRenderSemaphore() const {
            return m_renderSemaphore;
        }

        uint32_t GetImageIndex() const {
            return m_imageIndex;
        }

    private:
        const VulkanDeviceContext* m_vkDevCtx;
        uint32_t                   m_imageIndex;

        AcquireBuffer*             m_acquireBuffer;
        VkSemaphore                m_renderSemaphore;

    public:
        mutable std::chrono::nanoseconds                                    m_lastFrameTime;
        mutable std::chrono::time_point<std::chrono::high_resolution_clock> m_lastPresentTime;
        mutable std::chrono::nanoseconds                                    m_targetTimeDelta;
        mutable std::chrono::time_point<std::chrono::high_resolution_clock> m_framePresentAtTime;
    };

    struct Context {

        Context(const VulkanDeviceContext* vkDevCtx)
        : devCtx(vkDevCtx)
        , acquireBuffers()
        , backBuffers()
        , lastPresentTime()
        , lastFrameToFrameTimeNsec()
        , currentBackBuffer()
        , surface()
        , format()
        , swapchain()
        , extent()
        , acquiredFrameId() {}

        const VulkanDeviceContext* devCtx;

        std::queue<AcquireBuffer*>                                  acquireBuffers;
        std::vector<BackBuffer>                                     backBuffers;
        std::chrono::time_point<std::chrono::high_resolution_clock> lastPresentTime;
        std::chrono::nanoseconds                                    lastFrameToFrameTimeNsec;
        int32_t                                                     currentBackBuffer;

        VkSurfaceKHR surface;
        VkSurfaceFormatKHR format;

        VkSwapchainKHR swapchain;
        VkExtent2D extent;

        uint64_t acquiredFrameId;
    };
    const Context &GetContext() const { return m_ctx; }

    const BackBuffer* GetCurrentBackBuffer() const {
        if (m_ctx.currentBackBuffer >= 0) {
            return &m_ctx.backBuffers[m_ctx.currentBackBuffer];
        }
        return nullptr;
    }

    enum LogPriority {
        LOG_DEBUG,
        LOG_INFO,
        LOG_WARN,
        LOG_ERR,
    };

    virtual void Log(LogPriority priority, const char *msg);

    virtual void RunLoop() = 0;
    virtual void QuitLoop() = 0;

private:
    std::atomic<int32_t>       m_refCount;
protected:
    Shell(const VulkanDeviceContext* devCtx, const Configuration& configuration);
    virtual ~Shell() {}

    void CreateContext();
    void DestroyContext();

    void ResizeSwapchain(uint32_t width_hint, uint32_t height_hint);

    void AcquireBackBuffer(bool trainFrame = false);
    void PresentBackBuffer(bool trainFrame = false);

    const Configuration             m_settings;
    VkSharedBaseObj<FrameProcessor> m_frameProcessor;
private:

    // called by create_context
    void CreateBackBuffers();
    void DestroyBackBuffers();
    virtual VkSurfaceKHR CreateSurface(VkInstance instance) = 0;
    void CreateSwapchain();
    void DestroySwapchain();

protected:
    Context m_ctx;
};

#endif  // SHELL_H
