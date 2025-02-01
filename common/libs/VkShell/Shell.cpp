/*
 * Copyright (C) 2016 Google, Inc.
 * Copyright 2020 NVIDIA Corporation.
 *
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

#include <cassert>
#include <array>
#include <iostream>
#include <string>
#include <sstream>
#include <set>
#include <thread>
#include "VkCodecUtils/Helpers.h"
#include "Shell.h"

Shell::Shell(const VulkanDeviceContext* devCtx, const Configuration& configuration)
    : m_refCount(0)
    , m_settings(configuration)
    , m_frameProcessor()
    , m_ctx(devCtx) { }

Shell::AcquireBuffer::AcquireBuffer()
    : m_vkDevCtx(nullptr)
    , m_semaphore(VkSemaphore())
    , m_fence(VkFence())
{
}

Shell::AcquireBuffer::~AcquireBuffer()
{
    if (m_semaphore) {
        m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_semaphore, nullptr);
        m_semaphore = VkSemaphore(0);
    }

    if (m_fence) {
        m_vkDevCtx->DestroyFence(*m_vkDevCtx, m_fence, nullptr);
        m_fence = VkFence(0);
    }
}

VkResult Shell::AcquireBuffer::Create(const VulkanDeviceContext* vkDevCtx)
{
    VkSemaphoreCreateInfo sem_info = {};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    // Fence for vkAcquireNextImageKHR must be unsignaled

    m_vkDevCtx = vkDevCtx;
    AssertSuccess(m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &sem_info, nullptr, &m_semaphore));
    AssertSuccess(m_vkDevCtx->CreateFence(*m_vkDevCtx, &fence_info, nullptr, &m_fence));

    return VK_SUCCESS;
}

Shell::BackBuffer::BackBuffer()
    : m_vkDevCtx(nullptr)
    , m_imageIndex(0)
    , m_acquireBuffer(nullptr)
    , m_renderSemaphore(VkSemaphore())
    , m_lastFrameTime()
    , m_lastPresentTime()
    , m_targetTimeDelta()
    , m_framePresentAtTime()
{
}

VkResult Shell::BackBuffer::Create(const VulkanDeviceContext* vkDevCtx)
{
    VkSemaphoreCreateInfo sem_info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

    m_vkDevCtx = vkDevCtx;
    AssertSuccess(m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &sem_info, nullptr, &m_renderSemaphore));
    return VK_SUCCESS;
}

Shell::BackBuffer::~BackBuffer()
{
    if (m_renderSemaphore) {
        m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_renderSemaphore, nullptr);
        m_renderSemaphore = VkSemaphore(0);
    }

    if (m_acquireBuffer) {
        delete m_acquireBuffer;
        m_acquireBuffer = nullptr;
    }
}

void Shell::Log(LogPriority priority, const char *msg) {
    std::ostream &st = (priority >= LOG_ERR) ? std::cerr : std::cout;
    st << msg << "\n";
}

void Shell::CreateContext() {

    m_ctx.currentBackBuffer = 0;
    m_ctx.acquiredFrameId = 0;

    assert(m_ctx.devCtx->GetPresentQueueFamilyIdx() != -1);
    assert(m_ctx.devCtx->GetGfxQueueFamilyIdx() != -1);
    assert((m_ctx.devCtx->GetVideoDecodeQueueFamilyIdx() != -1) ||
           (m_ctx.devCtx->GetVideoEncodeQueueFamilyIdx() != -1));
    assert((m_ctx.devCtx->GetVideoDecodeNumQueues() > 0) ||
            m_ctx.devCtx->GetVideoEncodeNumQueues() > 0);

    CreateBackBuffers();

    // initialize ctx_.{surface,format} before attach_shell
    CreateSwapchain();

    m_frameProcessor->AttachShell(*this);
}

void Shell::DestroyContext() {

    if (*m_ctx.devCtx == VK_NULL_HANDLE) {
        return;
    }

    m_ctx.devCtx->DeviceWaitIdle();

    DestroySwapchain();

    m_frameProcessor->DetachShell();

    DestroyBackBuffers();

    m_ctx.devCtx = nullptr;
}

void Shell::CreateBackBuffers() {

    // BackBuffer is used to track which swapchain image and its associated
    // sync primitives are busy.  Having more BackBuffer's than swapchain
    // images may allows us to replace CPU wait on present_fence by GPU wait
    // on acquire_semaphore.
    const int count = m_settings.m_backBufferCount + 1;
    m_ctx.backBuffers.resize(count);
    for (auto &backBuffers : m_ctx.backBuffers) {
        AssertSuccess(backBuffers.Create(m_ctx.devCtx));
    }

    for (size_t i = 0; i < m_ctx.backBuffers.size(); i++) {
        AcquireBuffer* pAcquireBuffer = new AcquireBuffer();
        AssertSuccess(pAcquireBuffer->Create(m_ctx.devCtx));
        m_ctx.acquireBuffers.push(pAcquireBuffer);
    }

    m_ctx.currentBackBuffer = 0;
}

void Shell::DestroyBackBuffers() {

    m_ctx.backBuffers.clear();

    while (!m_ctx.acquireBuffers.empty()) {
        AcquireBuffer* pAcquireBuffer = m_ctx.acquireBuffers.front();
        m_ctx.acquireBuffers.pop();
        delete pAcquireBuffer;
    }

    m_ctx.currentBackBuffer = 0;
}

void Shell::CreateSwapchain() {
    m_ctx.surface = CreateSurface(m_ctx.devCtx->getInstance());
    assert(m_ctx.surface);

    VkBool32 supported;
    AssertSuccess(
        m_ctx.devCtx->GetPhysicalDeviceSurfaceSupportKHR(m_ctx.devCtx->getPhysicalDevice(),
                                                             m_ctx.devCtx->GetPresentQueueFamilyIdx(),
                                                             m_ctx.surface, &supported));
    // this should be guaranteed by the platform-specific PhysDeviceCanPresent() call
    assert(supported);

    std::vector<VkSurfaceFormatKHR> formats;
    vk::get(m_ctx.devCtx, m_ctx.devCtx->getPhysicalDevice(), m_ctx.surface, formats);
    m_ctx.format = formats[0];

    // Tegra hack __VkModesetApiNvdc::vkFormatToNvColorFormat() does not mapp the correct formats.
#ifdef NV_RMAPI_TEGRA
    m_ctx.format.format = VK_FORMAT_A8B8G8R8_UNORM_PACK32;
#endif // NV_RMAPI_TEGRA

    // defer to resize_swapchain()
    m_ctx.swapchain = VK_NULL_HANDLE;
    m_ctx.extent.width = (uint32_t)-1;
    m_ctx.extent.height = (uint32_t)-1;
}

void Shell::DestroySwapchain() {
    if (m_ctx.swapchain != VK_NULL_HANDLE) {
        m_frameProcessor->DetachSwapchain();

        m_ctx.devCtx->DestroySwapchainKHR(*m_ctx.devCtx, m_ctx.swapchain, nullptr);
        m_ctx.swapchain = VK_NULL_HANDLE;
    }

    m_ctx.devCtx->DestroySurfaceKHR(m_ctx.devCtx->getInstance(), m_ctx.surface, nullptr);
    m_ctx.surface = VK_NULL_HANDLE;
}

void Shell::ResizeSwapchain(uint32_t width_hint, uint32_t height_hint) {
    VkSurfaceCapabilitiesKHR caps;
    AssertSuccess(m_ctx.devCtx->GetPhysicalDeviceSurfaceCapabilitiesKHR(m_ctx.devCtx->getPhysicalDevice(), m_ctx.surface, &caps));

    VkExtent2D extent = caps.currentExtent;
    // use the hints
    if (extent.width == (uint32_t)-1) {
        extent.width = width_hint;
        extent.height = height_hint;
    }
    // clamp width; to protect us from broken hints?
    if (extent.width < caps.minImageExtent.width)
        extent.width = caps.minImageExtent.width;
    else if (extent.width > caps.maxImageExtent.width)
        extent.width = caps.maxImageExtent.width;
    // clamp height
    if (extent.height < caps.minImageExtent.height)
        extent.height = caps.minImageExtent.height;
    else if (extent.height > caps.maxImageExtent.height)
        extent.height = caps.maxImageExtent.height;

    if (m_ctx.extent.width == extent.width && m_ctx.extent.height == extent.height) return;

    uint32_t image_count = m_settings.m_backBufferCount;
    if (image_count < caps.minImageCount) {
        image_count = caps.minImageCount;
    }
    if ((caps.maxImageCount > 0) && (image_count > caps.maxImageCount)) {
        image_count = caps.maxImageCount;
    }

    assert(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    assert(caps.supportedTransforms & caps.currentTransform);
    assert(caps.supportedCompositeAlpha & (VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR));
    VkCompositeAlphaFlagBitsKHR composite_alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                                                      ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                                                      : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    std::vector<VkPresentModeKHR> modes;
    vk::get(m_ctx.devCtx, m_ctx.devCtx->getPhysicalDevice(), m_ctx.surface, modes);

    // FIFO is the only mode universally supported
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : modes) {
        if ((m_settings.m_vsync && (m == VK_PRESENT_MODE_MAILBOX_KHR)) ||
            (!m_settings.m_vsync && (m == VK_PRESENT_MODE_IMMEDIATE_KHR))) {
            mode = m;
            break;
        }
    }

    VkSwapchainCreateInfoKHR swapchain_info = {};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = m_ctx.surface;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = m_ctx.format.format;
    swapchain_info.imageColorSpace = m_ctx.format.colorSpace;
    swapchain_info.imageExtent = extent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    std::vector<uint32_t> queueFamilies(1, m_ctx.devCtx->GetGfxQueueFamilyIdx());
    if (m_ctx.devCtx->GetGfxQueueFamilyIdx() != m_ctx.devCtx->GetPresentQueueFamilyIdx()) {
        queueFamilies.push_back(m_ctx.devCtx->GetPresentQueueFamilyIdx());

        swapchain_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_info.queueFamilyIndexCount = (uint32_t)queueFamilies.size();
        swapchain_info.pQueueFamilyIndices = queueFamilies.data();
    } else {
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    swapchain_info.preTransform = caps.currentTransform;
    swapchain_info.compositeAlpha = composite_alpha;
    swapchain_info.presentMode = mode;
    swapchain_info.clipped = true;
    swapchain_info.oldSwapchain = m_ctx.swapchain;

    AssertSuccess(m_ctx.devCtx->CreateSwapchainKHR(*m_ctx.devCtx,
                                                         &swapchain_info, nullptr,
                                                         &m_ctx.swapchain));
    m_ctx.extent = extent;

    // destroy the old swapchain
    if (swapchain_info.oldSwapchain != VK_NULL_HANDLE) {
        m_frameProcessor->DetachSwapchain();

        m_ctx.devCtx->DeviceWaitIdle();
        m_ctx.devCtx->DestroySwapchainKHR(*m_ctx.devCtx, swapchain_info.oldSwapchain, nullptr);
    }

    m_frameProcessor->AttachSwapchain(*this);
}

void Shell::AcquireBackBuffer(bool) {

    if(!m_ctx.acquireBuffers.empty()) {

        AcquireBuffer* acquireBuf = m_ctx.acquireBuffers.front();

        assert(acquireBuf != nullptr);

        uint32_t imageIndex = 0;
        AssertSuccess(
            m_ctx.devCtx->AcquireNextImageKHR(*m_ctx.devCtx, m_ctx.swapchain,
                                              UINT64_MAX,
                                              acquireBuf->m_semaphore, acquireBuf->m_fence,
                                              &imageIndex));

        assert(imageIndex < m_ctx.backBuffers.size());
        BackBuffer& backBuffer = m_ctx.backBuffers[imageIndex];

        // wait until acquire and render semaphores are waited/unsignaled
        AssertSuccess(m_ctx.devCtx->WaitForFences(*m_ctx.devCtx, 1, &acquireBuf->m_fence, true, UINT64_MAX));
        // reset the fence
        AssertSuccess(m_ctx.devCtx->ResetFences(*m_ctx.devCtx, 1, &acquireBuf->m_fence));

        // 16 milliseconds in nanoseconds
        static const std::chrono::nanoseconds targetDuration(16 * 1000 * 1000); // 16 mSec targeting ~60 FPS
        auto timeNow = std::chrono::high_resolution_clock::now();
        if (false) {
            m_ctx.lastFrameToFrameTimeNsec = timeNow - m_ctx.lastPresentTime;
            std::cout << "Last Present Time: " << m_ctx.lastFrameToFrameTimeNsec.count() << " nSec, " << std::endl;
            if (m_ctx.lastFrameToFrameTimeNsec.count() < 16000000) {

            }
        }

        if (false) {
            backBuffer.m_lastFrameTime = timeNow - backBuffer.m_lastPresentTime;
            std::cout << "Frame Present Time: " << backBuffer.m_lastFrameTime.count() << " nSec, " << std::endl;

            if (backBuffer.m_lastFrameTime / 8 < targetDuration) {
                std::this_thread::sleep_for(targetDuration - backBuffer.m_lastFrameTime / 8);
            }
        }

        if (false) {
            std::cout << "Frame diff: " << (timeNow - backBuffer.m_framePresentAtTime).count() << " nSec, "
                    << "m_targetTimeDelta: " << backBuffer.m_targetTimeDelta.count() << std::endl;
        }

        if (false && (backBuffer.m_targetTimeDelta.count() > 0) && (backBuffer.m_framePresentAtTime < timeNow)) {
            auto timeDiff = timeNow - backBuffer.m_framePresentAtTime;
            if (timeDiff < backBuffer.m_targetTimeDelta) {
                std::this_thread::sleep_for(timeDiff);
            } else {
                std::this_thread::sleep_for(backBuffer.m_targetTimeDelta);
            }
        }

        // std::this_thread::sleep_for (std::chrono::milliseconds(16));

        m_ctx.currentBackBuffer = imageIndex;
        m_ctx.acquireBuffers.pop();
        // Now return to the queue the old frame.
        AcquireBuffer* oldAcquireBuffer = backBuffer.SetAcquireBuffer(imageIndex, acquireBuf);
        if (oldAcquireBuffer) {
            m_ctx.acquireBuffers.push(oldAcquireBuffer);
        }
        m_ctx.acquiredFrameId++;

    } else {
        // If the queue is empty - the is nothing that can be done here.
        assert(!"Swapchain queue is empty!");
        m_ctx.currentBackBuffer = -1;
    }
}

void Shell::PresentBackBuffer(bool trainFrame) {

    const BackBuffer* backBuffer = GetCurrentBackBuffer();

    bool contintueLoop = false;
    if (backBuffer != nullptr) {
        contintueLoop = m_frameProcessor->OnFrame(trainFrame ?
                                                      -(int32_t)backBuffer->GetImageIndex() :
                                                      backBuffer->GetImageIndex(),
                                                  1, // waitSemaphoreCount
                                                  &backBuffer->GetAcquireSemaphore(),
                                                  1, // signalSemaphoreCount
                                                  &backBuffer->GetRenderSemaphore());
    } else {
        contintueLoop = m_frameProcessor->OnFrame(-1);
    }

    if (!contintueLoop) {
        QuitLoop();
    }

    if (backBuffer == nullptr) {
        return;
    }

    uint32_t imageIndex = backBuffer->GetImageIndex();
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &backBuffer->GetRenderSemaphore();
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_ctx.swapchain;
    presentInfo.pImageIndices = &imageIndex;

    VkResult res = m_ctx.devCtx->QueuePresentKHR(m_ctx.devCtx->GetPresentQueue(), &presentInfo);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        std::cout << "Out of date Present Surface" << res << std::endl;
        return;
    }

    m_ctx.lastPresentTime = backBuffer->m_lastPresentTime = std::chrono::high_resolution_clock::now();
    static const std::chrono::nanoseconds targetDuration(12 * 1000 * 1000); // 16 mSec targeting ~60 FPS
    backBuffer->m_targetTimeDelta = targetDuration;
    backBuffer->m_framePresentAtTime = backBuffer->m_lastPresentTime + targetDuration;
}

#include "ShellDirect.h"
#if defined(VK_USE_PLATFORM_XCB_KHR)
#include "ShellXcb.h"
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
#include "wayland-client-protocol.h"
#include "ShellWayland.h"
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
#include "ShellWin32.h"
#endif

const std::vector<VkExtensionProperties>& Shell::GetRequiredInstanceExtensions(bool directToDisplayMode)
{
    if (directToDisplayMode) {
        return ShellDirect::GetRequiredInstanceExtensions();
    } else {
#if defined(VK_USE_PLATFORM_XCB_KHR)
        return ShellXcb::GetRequiredInstanceExtensions();
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
        return  ShellWayland::GetRequiredInstanceExtensions();
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
        return  ShellWin32::GetRequiredInstanceExtensions();
#endif
    }
    return ShellDirect::GetRequiredInstanceExtensions();
}

void Shell::AttachFrameProcessor(VkSharedBaseObj<FrameProcessor>& frameProcessor)
{
    m_frameProcessor = frameProcessor;
}

VkResult Shell::Create(const VulkanDeviceContext* vkDevCtx,
                       const Configuration& configuration,
                       VkSharedBaseObj<Shell>& displayShell)
{
    if (configuration.m_directToDisplayMode) {
        displayShell = new ShellDirect(vkDevCtx, configuration);
    } else {
#if defined(VK_USE_PLATFORM_XCB_KHR)
        displayShell = new ShellXcb(vkDevCtx, configuration);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
        displayShell = new ShellWayland(vkDevCtx, configuration);
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
        displayShell = new ShellWin32(vkDevCtx, configuration);
#endif
    }

    if (displayShell) {
        return VK_SUCCESS;
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}
