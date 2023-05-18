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
#include "VkCodecUtils/Helpers.h"
#include "Shell.h"

Shell::Shell(const VulkanDeviceContext* devCtx, VkSharedBaseObj<FrameProcessor>& frameProcessor)
    : m_refCount(0)
    , m_frameProcessor(frameProcessor)
    , m_settings(frameProcessor->GetSettings())
    , m_ctx(devCtx), m_tick(1.0f / m_settings.ticksPerSecond)
    , m_time(m_tick) { }

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
    vk::assert_success(m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &sem_info, nullptr, &m_semaphore));
    vk::assert_success(m_vkDevCtx->CreateFence(*m_vkDevCtx, &fence_info, nullptr, &m_fence));

    return VK_SUCCESS;
}

Shell::BackBuffer::BackBuffer()
    : m_vkDevCtx(nullptr)
    , m_imageIndex(0)
    , m_acquireBuffer(nullptr)
    , m_renderSemaphore(VkSemaphore())
{
}

VkResult Shell::BackBuffer::Create(const VulkanDeviceContext* vkDevCtx)
{
    VkSemaphoreCreateInfo sem_info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

    m_vkDevCtx = vkDevCtx;
    vk::assert_success(m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &sem_info, nullptr, &m_renderSemaphore));
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
    assert(m_ctx.devCtx->GetVideoDecodeQueueFamilyIdx() != -1);
    assert(m_ctx.devCtx->GetVideoDecodeNumQueues() > 0);

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
    const int count = m_settings.backBufferCount + 1;
    m_ctx.backBuffers.resize(count);
    for (auto &backBuffers : m_ctx.backBuffers) {
        vk::assert_success(backBuffers.Create(m_ctx.devCtx));
    }

    for (size_t i = 0; i < m_ctx.backBuffers.size(); i++) {
        AcquireBuffer* pAcquireBuffer = new AcquireBuffer();
        vk::assert_success(pAcquireBuffer->Create(m_ctx.devCtx));
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
    vk::assert_success(
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
    vk::assert_success(m_ctx.devCtx->GetPhysicalDeviceSurfaceCapabilitiesKHR(m_ctx.devCtx->getPhysicalDevice(), m_ctx.surface, &caps));

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

    uint32_t image_count = m_settings.backBufferCount;
    if (image_count < caps.minImageCount)
        image_count = caps.minImageCount;
    else if (image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

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
        if ((m_settings.vsync && m == VK_PRESENT_MODE_MAILBOX_KHR) || (!m_settings.vsync && m == VK_PRESENT_MODE_IMMEDIATE_KHR)) {
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

    vk::assert_success(m_ctx.devCtx->CreateSwapchainKHR(*m_ctx.devCtx,
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

void Shell::AcquireBackBuffer(bool trainFrame) {
    // acquire just once when not presenting
    if (m_settings.noPresent && GetCurrentBackBuffer().GetAcquireSemaphore() != VK_NULL_HANDLE) return;

    assert(!m_ctx.acquireBuffers.empty());

    AcquireBuffer* acquireBuf = m_ctx.acquireBuffers.front();

    uint32_t imageIndex = 0;
    vk::assert_success(
        m_ctx.devCtx->AcquireNextImageKHR(*m_ctx.devCtx, m_ctx.swapchain, UINT64_MAX, acquireBuf->m_semaphore, acquireBuf->m_fence, &imageIndex));

    assert(imageIndex < m_ctx.backBuffers.size());
    BackBuffer& back = m_ctx.backBuffers[imageIndex];

    // wait until acquire and render semaphores are waited/unsignaled
    vk::assert_success(m_ctx.devCtx->WaitForFences(*m_ctx.devCtx, 1, &acquireBuf->m_fence, true, UINT64_MAX));
    // reset the fence
    vk::assert_success(m_ctx.devCtx->ResetFences(*m_ctx.devCtx, 1, &acquireBuf->m_fence));

    m_ctx.currentBackBuffer = imageIndex;
    AcquireBuffer* oldAcquireBuffer = back.SetAcquireBuffer(imageIndex, acquireBuf);
    m_ctx.acquireBuffers.pop();
    if (oldAcquireBuffer) {
        m_ctx.acquireBuffers.push(oldAcquireBuffer);
    }
    m_ctx.acquiredFrameId++;
}

void Shell::PresentBackBuffer(bool trainFrame) {

    const BackBuffer& backBuffer = GetCurrentBackBuffer();

    if (!m_frameProcessor->OnFrame(trainFrame ? -(int32_t)backBuffer.GetImageIndex() :
                                                backBuffer.GetImageIndex(),
                                  1, // waitSemaphoreCount
                                  &backBuffer.GetAcquireSemaphore(),
                                  1, // signalSemaphoreCount
                                  &backBuffer.GetRenderSemaphore())) {
        QuitLoop();
    }

    if (m_settings.noPresent) {
        FakePresent();
        return;
    }

    uint32_t imageIndex = backBuffer.GetImageIndex();
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &backBuffer.GetRenderSemaphore();
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_ctx.swapchain;
    presentInfo.pImageIndices = &imageIndex;

    VkResult res = m_ctx.devCtx->QueuePresentKHR(m_ctx.devCtx->GetPresentQueue(), &presentInfo);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        std::cout << "Out of date Present Surface" << res << std::endl;
        return;
    }
}

void Shell::FakePresent() {
    const BackBuffer& backBuffer = GetCurrentBackBuffer();

    assert(m_settings.noPresent);

    // wait render semaphore and signal acquire semaphore
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &backBuffer.GetRenderSemaphore();
    submitInfo.pWaitDstStageMask = &stage;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &backBuffer.GetAcquireSemaphore();
    vk::assert_success(m_ctx.devCtx->QueueSubmit(m_ctx.devCtx->GetGfxQueue(), 1, &submitInfo, VK_NULL_HANDLE));
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
}

VkResult Shell::Create(const VulkanDeviceContext* vkDevCtx,
                       VkSharedBaseObj<FrameProcessor>& frameProcessor,
                       bool directToDisplayMode,
                       VkSharedBaseObj<Shell>& displayShell)
{
    if (directToDisplayMode) {
        displayShell = new ShellDirect(vkDevCtx, frameProcessor);
    } else {
#if defined(VK_USE_PLATFORM_XCB_KHR)
        displayShell = new ShellXcb(vkDevCtx, frameProcessor);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
        displayShell = new ShellWayland(vkDevCtx, frameProcessor);
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
        displayShell = new ShellWin32(vkDevCtx, frameProcessor);
#endif
    }

    if (displayShell) {
        return VK_SUCCESS;
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}
