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

#if !defined(VK_USE_PLATFORM_WIN32_KHR)
#include <dlfcn.h>
#endif

#include <iostream>
#include <sstream>
#include <cassert>
#include <vector>
#include <algorithm>

#include <thread>
#include <chrono>

#include "VkCodecUtils/Helpers.h"
#include "ShellDirect.h"

ShellDirect::ShellDirect(FrameProcessor& frameProcessor, uint32_t deviceID)
    : Shell(frameProcessor),
      lib_handle_(nullptr),
      quit_(false)
{
    instance_extensions_.push_back("VK_KHR_display");
    instance_extensions_.push_back("VK_EXT_direct_mode_display");
    instance_extensions_.push_back("VK_EXT_acquire_xlib_display");

    init_vk(deviceID);
    init_display();
}

ShellDirect::~ShellDirect()
{
    cleanup_vk();
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
    dlclose(lib_handle_);
#endif
}

void ShellDirect::run()
{
    create_context();
    resize_swapchain(ctx_.display_res_width_, ctx_.display_res_height_);
    vk::DeviceWaitIdle(ctx_.dev);
    uint64_t counter = 0;
    static const uint32_t waitForDisplayPowerOnSec = 5;

    while (!quit_)
    {
        acquire_back_buffer(counter == 0);
        present_back_buffer(counter == 0);

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
    destroy_context();
}

void ShellDirect::quit()
{
    quit_ = true;
}

void ShellDirect::init_display()
{
    uint32_t display_count = 0;
    vk::assert_success(vk::GetPhysicalDeviceDisplayPropertiesKHR(ctx_.physical_dev, &display_count, NULL));

    VkDisplayPropertiesKHR display_props[4];
    vk::assert_success(vk::GetPhysicalDeviceDisplayPropertiesKHR(ctx_.physical_dev, &display_count, display_props));

    const uint32_t display_index = 0;
    ctx_.display_ = display_props[display_index].display;
    printf("using display index %u ('%s')\n", display_index, display_props[display_index].displayName);

    // Display dpy = NULL;
    // Provided by VK_EXT_acquire_xlib_display
    // vk::assert_success(vk::AcquireXlibDisplayEXT(ctx_.physical_dev, &dpy, display_));
}

// called by init_vk
PFN_vkGetInstanceProcAddr ShellDirect::load_vk()
{
    void *handle = NULL, *symbol = NULL;
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
    const char filename[] = "libvulkan.so.1";

#ifdef UNINSTALLED_LOADER
    handle = dlopen(UNINSTALLED_LOADER, RTLD_LAZY);
    if (!handle) handle = dlopen(filename, RTLD_LAZY);
#else
    handle = dlopen(filename, RTLD_LAZY);
#endif

    if (handle) symbol = dlsym(handle, "vkGetInstanceProcAddr");

    if (!handle || !symbol) {
        std::stringstream ss;
        ss << "failed to load " << dlerror();

        if (handle) dlclose(handle);

        throw std::runtime_error(ss.str());
    }

    lib_handle_ = handle;
#endif
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(symbol);
}

// called by init_vk
bool ShellDirect::can_present(VkPhysicalDevice phy, uint32_t queue_family)
{
    // todo ?
    return true;
}

// called by create_context
VkSurfaceKHR ShellDirect::create_surface(VkInstance instance)
{
    assert(ctx_.display_ != VK_NULL_HANDLE);

    std::vector<VkDisplayModePropertiesKHR> mode_properties;

    // get the list of supported display modes
    uint32_t mode_count = 0;
    vk::assert_success(vk::GetDisplayModePropertiesKHR(ctx_.physical_dev, ctx_.display_, &mode_count, nullptr));
    mode_properties.resize(mode_count);
    vk::assert_success(vk::GetDisplayModePropertiesKHR(ctx_.physical_dev, ctx_.display_, &mode_count, &mode_properties[0]));

    // choose the first display mode
    assert(!mode_properties.empty());
    const auto& mode_props = mode_properties[0];

    // Get the list of planes
    uint32_t plane_count = 0;
    vk::assert_success(vk::GetPhysicalDeviceDisplayPlanePropertiesKHR(ctx_.physical_dev, &plane_count, nullptr));

    std::vector<VkDisplayPlanePropertiesKHR> plane_properties;
    plane_properties.resize(plane_count);
    vk::assert_success(vk::GetPhysicalDeviceDisplayPlanePropertiesKHR(ctx_.physical_dev, &plane_count, &plane_properties[0]));

    // find a plane compatible with the display
    uint32_t found_plane_index = 0;

    for (found_plane_index = 0; found_plane_index < plane_properties.size();
        ++found_plane_index) {

        // Disqualify planes that are bound to a different display
        if ((plane_properties[found_plane_index].currentDisplay != VK_NULL_HANDLE) &&
            (plane_properties[found_plane_index].currentDisplay != ctx_.display_)) {
            continue;
        }

        uint32_t supported_count = 0;
        vk::assert_success(vk::GetDisplayPlaneSupportedDisplaysKHR(ctx_.physical_dev, found_plane_index, &supported_count, nullptr));
        std::vector<VkDisplayKHR> supported_displays;
        supported_displays.resize(supported_count);
        vk::assert_success(vk::GetDisplayPlaneSupportedDisplaysKHR(ctx_.physical_dev, found_plane_index, &supported_count,
            &supported_displays[0]));

        // if the plane supports our current display we choose it
        auto it = std::find(std::begin(supported_displays), std::end(supported_displays),  ctx_.display_);
        if (it != std::end(supported_displays))
            break; // for loop
    }
    if (found_plane_index == plane_properties.size()) {
        printf("No plane found compatible with the display. Ooops.");
        return nullptr;
    }

    const VkExtent2D surface_extent = {
        mode_props.parameters.visibleRegion.width,
        mode_props.parameters.visibleRegion.height
    };

    VkDisplaySurfaceCreateInfoKHR surface_create_info = {};
    surface_create_info.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
    surface_create_info.pNext = NULL;
    surface_create_info.flags = 0;
    surface_create_info.displayMode = mode_props.displayMode;
    surface_create_info.planeIndex  = found_plane_index;
    surface_create_info.transform   = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    surface_create_info.alphaMode   = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    surface_create_info.globalAlpha = 1.0f;
    surface_create_info.imageExtent = surface_extent;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vk::assert_success(vk::CreateDisplayPlaneSurfaceKHR(ctx_.instance, &surface_create_info, nullptr, &surface));

    printf("Created display surface.\n"
           "display res: %ux%u\n", surface_extent.width, surface_extent.height);
    ctx_.display_res_width_ = surface_extent.width;
    ctx_.display_res_height_ = surface_extent.height;

    if (false && surface) {
        const VkDisplayPowerInfoEXT displayPowerInfo = {VK_STRUCTURE_TYPE_DISPLAY_POWER_INFO_EXT, NULL, VK_DISPLAY_POWER_STATE_ON_EXT};
        vk::assert_success(vk::DisplayPowerControlEXT(ctx_.dev, ctx_.display_,  &displayPowerInfo));
    }

    return surface;
}


