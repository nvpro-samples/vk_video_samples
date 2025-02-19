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

#include <iostream>
#include <sstream>
#include <cassert>
#include <vector>
#include <algorithm>

#include <thread>
#include <chrono>

#include "VkCodecUtils/Helpers.h"
#include "ShellDirect.h"

static const std::vector<VkExtensionProperties> directSurfaceExtensions {
    VkExtensionProperties{ VK_KHR_DISPLAY_EXTENSION_NAME, VK_KHR_DISPLAY_SPEC_VERSION },
    VkExtensionProperties{ VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME, VK_EXT_DIRECT_MODE_DISPLAY_SPEC_VERSION }
#if defined(VK_USE_PLATFORM_XLIB_XRANDR_EXT)
  , VkExtensionProperties{ VK_EXT_ACQUIRE_XLIB_DISPLAY_EXTENSION_NAME, VK_EXT_ACQUIRE_XLIB_DISPLAY_SPEC_VERSION }
#endif
};

const std::vector<VkExtensionProperties>& ShellDirect::GetRequiredInstanceExtensions()
{
    return directSurfaceExtensions;
}

ShellDirect::ShellDirect(const VulkanDeviceContext* vkDevCtx, const Configuration& configuration)
    : Shell(vkDevCtx, configuration),
      m_vkDisplay(), m_displayWidth(), m_displayHeight(), m_quitLoop(false)
{

}

ShellDirect::~ShellDirect()
{

}

const char* ShellDirect::GetRequiredInstanceExtension()
{
    return VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME;
}

void ShellDirect::RunLoop()
{
    InitDisplay();
    CreateContext();
    ResizeSwapchain(m_displayWidth, m_displayHeight);
    m_ctx.devCtx->DeviceWaitIdle();
    uint64_t counter = 0;
    static const uint32_t waitForDisplayPowerOnSec = 5;

    while (!m_quitLoop) {

        AcquireBackBuffer(counter == 0);
        PresentBackBuffer(counter == 0);

        if (counter == 0) {
            // Waiting for the display to wake-up
            std::cout << "Waiting for the display to wake-up for " << waitForDisplayPowerOnSec << " seconds: " << std::flush;
            for (uint32_t waitForDisplay = 0; waitForDisplay < waitForDisplayPowerOnSec; waitForDisplay++) {
                std::cout << waitForDisplay << " " << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
            std::cout << std::endl << "Done Waiting for the display" << std::endl;
        }

        counter++;
    }
    DestroyContext();
}

void ShellDirect::QuitLoop()
{
    m_quitLoop = true;
}

void ShellDirect::InitDisplay()
{
    uint32_t displayCount = 0;
    AssertSuccess(m_ctx.devCtx->GetPhysicalDeviceDisplayPropertiesKHR(m_ctx.devCtx->getPhysicalDevice(), &displayCount, NULL));

    VkDisplayPropertiesKHR displayProps[4];
    AssertSuccess(m_ctx.devCtx->GetPhysicalDeviceDisplayPropertiesKHR(m_ctx.devCtx->getPhysicalDevice(), &displayCount, displayProps));

    const uint32_t displayIndex = 0;
    m_vkDisplay = displayProps[displayIndex].display;
    printf("using display index %u ('%s')\n", displayIndex, displayProps[displayIndex].displayName);

    // Display dpy = NULL;
    // Provided by VK_EXT_acquire_xlib_display
    // AssertSuccess(vk::AcquireXlibDisplayEXT(ctx_.physical_dev, &dpy, display_));
}

bool ShellDirect::PhysDeviceCanPresent(VkPhysicalDevice, uint32_t) const
{
    // Each WSI platform extension should implement this function
    return false;
}

// called by create_context
VkSurfaceKHR ShellDirect::CreateSurface(VkInstance)
{
    assert(m_vkDisplay != VK_NULL_HANDLE);

    std::vector<VkDisplayModePropertiesKHR> modeProperties;

    // get the list of supported display modes
    uint32_t modeCount = 0;
    AssertSuccess(m_ctx.devCtx->GetDisplayModePropertiesKHR(m_ctx.devCtx->getPhysicalDevice(), m_vkDisplay, &modeCount, nullptr));
    modeProperties.resize(modeCount);
    AssertSuccess(m_ctx.devCtx->GetDisplayModePropertiesKHR(m_ctx.devCtx->getPhysicalDevice(), m_vkDisplay, &modeCount, &modeProperties[0]));

    // choose the first display mode
    assert(!modeProperties.empty());
    const auto& modeProps = modeProperties[0];

    // Get the list of planes
    uint32_t planeCount = 0;
    AssertSuccess(m_ctx.devCtx->GetPhysicalDeviceDisplayPlanePropertiesKHR(m_ctx.devCtx->getPhysicalDevice(), &planeCount, nullptr));

    std::vector<VkDisplayPlanePropertiesKHR> planeProperties;
    planeProperties.resize(planeCount);
    AssertSuccess(m_ctx.devCtx->GetPhysicalDeviceDisplayPlanePropertiesKHR(m_ctx.devCtx->getPhysicalDevice(), &planeCount, &planeProperties[0]));

    // find a plane compatible with the display
    uint32_t foundPlaneIndex = 0;

    for (foundPlaneIndex = 0; foundPlaneIndex < planeProperties.size(); ++foundPlaneIndex) {

        // Disqualify planes that are bound to a different display
        if ((planeProperties[foundPlaneIndex].currentDisplay != VK_NULL_HANDLE) &&
            (planeProperties[foundPlaneIndex].currentDisplay != m_vkDisplay)) {
            continue;
        }

        uint32_t supported_count = 0;
        AssertSuccess(m_ctx.devCtx->GetDisplayPlaneSupportedDisplaysKHR(m_ctx.devCtx->getPhysicalDevice(), foundPlaneIndex, &supported_count, nullptr));
        std::vector<VkDisplayKHR> supported_displays;
        supported_displays.resize(supported_count);
        AssertSuccess(m_ctx.devCtx->GetDisplayPlaneSupportedDisplaysKHR(m_ctx.devCtx->getPhysicalDevice(), foundPlaneIndex, &supported_count,
            &supported_displays[0]));

        // if the plane supports our current display we choose it
        auto it = std::find(std::begin(supported_displays), std::end(supported_displays),  m_vkDisplay);
        if (it != std::end(supported_displays))
            break; // for loop
    }

    if (foundPlaneIndex == planeProperties.size()) {
        printf("No plane found compatible with the display. Ooops.");
        assert(false);
    }

    const VkExtent2D surfaceExtent = {
        modeProps.parameters.visibleRegion.width,
        modeProps.parameters.visibleRegion.height
    };

    VkDisplaySurfaceCreateInfoKHR surfaceCreateInfo = {};
    surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
    surfaceCreateInfo.pNext = NULL;
    surfaceCreateInfo.flags = 0;
    surfaceCreateInfo.displayMode = modeProps.displayMode;
    surfaceCreateInfo.planeIndex  = foundPlaneIndex;
    surfaceCreateInfo.transform   = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    surfaceCreateInfo.alphaMode   = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    surfaceCreateInfo.globalAlpha = 1.0f;
    surfaceCreateInfo.imageExtent = surfaceExtent;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    AssertSuccess(m_ctx.devCtx->CreateDisplayPlaneSurfaceKHR(m_ctx.devCtx->getInstance(), &surfaceCreateInfo, nullptr, &surface));

    printf("Created display surface.\n"
           "display res: %ux%u\n", surfaceExtent.width, surfaceExtent.height);
    m_displayWidth = surfaceExtent.width;
    m_displayHeight = surfaceExtent.height;

    if (false && surface) {
        const VkDisplayPowerInfoEXT displayPowerInfo = {VK_STRUCTURE_TYPE_DISPLAY_POWER_INFO_EXT, NULL, VK_DISPLAY_POWER_STATE_ON_EXT};
        AssertSuccess(m_ctx.devCtx->DisplayPowerControlEXT(*m_ctx.devCtx, m_vkDisplay,  &displayPowerInfo));
    }

    // Destroy with vkDestroySurfaceKHR
    return surface;
}


