/*
 * Copyright (C) 2016 Google, Inc.
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

#ifndef HELPERS_H
#define HELPERS_H

#include <vector>
#include <sstream>
#include <stdexcept>
#include <assert.h>
#include "HelpersDispatchTable.h"

namespace vk {

inline VkResult assert_success(VkResult res) {
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        std::stringstream ss;
        ss << "VkResult " << res << " returned";
        throw std::runtime_error(ss.str());
    }

    return res;
}

inline VkResult enumerate(const VkInterfaceFunctions* vkIf, const char *layer, std::vector<VkExtensionProperties> &exts) {
    uint32_t count = 0;
    vkIf->EnumerateInstanceExtensionProperties(layer, &count, nullptr);

    exts.resize(count);
    return vkIf->EnumerateInstanceExtensionProperties(layer, &count, exts.data());
}

inline VkResult enumerate(const VkInterfaceFunctions* vkIf, VkPhysicalDevice phy, const char *layer, std::vector<VkExtensionProperties> &exts) {
    uint32_t count = 0;
    vkIf->EnumerateDeviceExtensionProperties(phy, layer, &count, nullptr);

    exts.resize(count);
    return vkIf->EnumerateDeviceExtensionProperties(phy, layer, &count, exts.data());
}

inline VkResult enumerate(const VkInterfaceFunctions* vkIf, VkInstance instance, std::vector<VkPhysicalDevice> &phys) {
    uint32_t count = 0;
    vkIf->EnumeratePhysicalDevices(instance, &count, nullptr);

    phys.resize(count);
    return vkIf->EnumeratePhysicalDevices(instance, &count, phys.data());
}

inline VkResult enumerate(const VkInterfaceFunctions* vkIf, std::vector<VkLayerProperties> &layer_props) {
    uint32_t count = 0;
    vkIf->EnumerateInstanceLayerProperties(&count, nullptr);

    layer_props.resize(count);
    return vkIf->EnumerateInstanceLayerProperties(&count, layer_props.data());
}

inline VkResult get(const VkInterfaceFunctions* vkIf,
                    VkPhysicalDevice phy, std::vector<VkQueueFamilyProperties2> &queues,
                    std::vector<VkQueueFamilyVideoPropertiesKHR> &videoQueues,
                    std::vector<VkQueueFamilyQueryResultStatusPropertiesKHR> &queryResultStatus) {
    uint32_t count = 0;
    vkIf->GetPhysicalDeviceQueueFamilyProperties2(phy, &count, nullptr);

    queues.resize(count);
    videoQueues.resize(count);
    queryResultStatus.resize(count);
    for (uint32_t i = 0; i < queues.size(); i++) {
        queues[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        videoQueues[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        queues[i].pNext = &videoQueues[i];
        queryResultStatus[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR;
        videoQueues[i].pNext = &queryResultStatus[i];
    }

    vkIf->GetPhysicalDeviceQueueFamilyProperties2(phy, &count, queues.data());

    return VK_SUCCESS;
}

inline VkResult get(const VkInterfaceFunctions* vkIf,
                    VkPhysicalDevice phy, VkSurfaceKHR surface, std::vector<VkSurfaceFormatKHR> &formats) {
    uint32_t count = 0;
    vkIf->GetPhysicalDeviceSurfaceFormatsKHR(phy, surface, &count, nullptr);

    formats.resize(count);
    return vkIf->GetPhysicalDeviceSurfaceFormatsKHR(phy, surface, &count, formats.data());
}

inline VkResult get(const VkInterfaceFunctions* vkIf,
                    VkPhysicalDevice phy, VkSurfaceKHR surface, std::vector<VkPresentModeKHR> &modes) {
    uint32_t count = 0;
    vkIf->GetPhysicalDeviceSurfacePresentModesKHR(phy, surface, &count, nullptr);

    modes.resize(count);
    return vkIf->GetPhysicalDeviceSurfacePresentModesKHR(phy, surface, &count, modes.data());
}

inline VkResult get(const VkInterfaceFunctions* vkIf,
                    VkDevice dev, VkSwapchainKHR swapchain, std::vector<VkImage> &images) {
    uint32_t count = 0;
    vkIf->GetSwapchainImagesKHR(dev, swapchain, &count, nullptr);

    images.resize(count);
    return vkIf->GetSwapchainImagesKHR(dev, swapchain, &count, images.data());
}

inline VkResult MapMemoryTypeToIndex(const VkInterfaceFunctions* vkIf,
                                     VkPhysicalDevice vkPhysicalDev,
                                     uint32_t typeBits,
                                     VkFlags requirements_mask, uint32_t *typeIndex)
{
    VkPhysicalDeviceMemoryProperties memoryProperties;
    vkIf->GetPhysicalDeviceMemoryProperties(vkPhysicalDev, &memoryProperties);
    // Search memtypes to find first index with those properties
    for (uint32_t i = 0; i < 32; i++) {
        if ((typeBits & 1) == 1) {
            // Type is available, does it match user properties?
            if ((memoryProperties.memoryTypes[i].propertyFlags & requirements_mask) ==
                    requirements_mask) {
                *typeIndex = i;
                return VK_SUCCESS;
            }
        }
        typeBits >>= 1;
    }
    return VK_ERROR_VALIDATION_FAILED_EXT;
}

}  // namespace vk

#endif  // HELPERS_H
