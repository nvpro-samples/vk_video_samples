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
    if (res != VK_SUCCESS) {
        std::stringstream ss;
        ss << "VkResult " << res << " returned";
        throw std::runtime_error(ss.str());
    }

    return res;
}

inline VkResult enumerate(const char *layer, std::vector<VkExtensionProperties> &exts) {
    uint32_t count = 0;
    vk::EnumerateInstanceExtensionProperties(layer, &count, nullptr);

    exts.resize(count);
    return vk::EnumerateInstanceExtensionProperties(layer, &count, exts.data());
}

inline VkResult enumerate(VkPhysicalDevice phy, const char *layer, std::vector<VkExtensionProperties> &exts) {
    uint32_t count = 0;
    vk::EnumerateDeviceExtensionProperties(phy, layer, &count, nullptr);

    exts.resize(count);
    return vk::EnumerateDeviceExtensionProperties(phy, layer, &count, exts.data());
}

inline VkResult enumerate(VkInstance instance, std::vector<VkPhysicalDevice> &phys) {
    uint32_t count = 0;
    vk::EnumeratePhysicalDevices(instance, &count, nullptr);

    phys.resize(count);
    return vk::EnumeratePhysicalDevices(instance, &count, phys.data());
}

inline VkResult enumerate(std::vector<VkLayerProperties> &layer_props) {
    uint32_t count = 0;
    vk::EnumerateInstanceLayerProperties(&count, nullptr);

    layer_props.resize(count);
    return vk::EnumerateInstanceLayerProperties(&count, layer_props.data());
}

inline VkResult get(VkPhysicalDevice phy, std::vector<VkQueueFamilyProperties2> &queues,
                                          std::vector<VkVideoQueueFamilyProperties2KHR> &videoQueues,
                                          std::vector<VkQueueFamilyQueryResultStatusProperties2KHR> &queryResultStatus) {
    uint32_t count = 0;
    vk::GetPhysicalDeviceQueueFamilyProperties2(phy, &count, nullptr);

    queues.resize(count);
    videoQueues.resize(count);
    queryResultStatus.resize(count);
    for (uint32_t i = 0; i < queues.size(); i++) {
        queues[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        videoQueues[i].sType = VK_STRUCTURE_TYPE_VIDEO_QUEUE_FAMILY_PROPERTIES_2_KHR;
        queues[i].pNext = &videoQueues[i];
        queryResultStatus[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_2_KHR;
        videoQueues[i].pNext = &queryResultStatus[i];
    }

    vk::GetPhysicalDeviceQueueFamilyProperties2(phy, &count, queues.data());

    return VK_SUCCESS;
}

inline VkResult get(VkPhysicalDevice phy, VkSurfaceKHR surface, std::vector<VkSurfaceFormatKHR> &formats) {
    uint32_t count = 0;
    vk::GetPhysicalDeviceSurfaceFormatsKHR(phy, surface, &count, nullptr);

    formats.resize(count);
    return vk::GetPhysicalDeviceSurfaceFormatsKHR(phy, surface, &count, formats.data());
}

inline VkResult get(VkPhysicalDevice phy, VkSurfaceKHR surface, std::vector<VkPresentModeKHR> &modes) {
    uint32_t count = 0;
    vk::GetPhysicalDeviceSurfacePresentModesKHR(phy, surface, &count, nullptr);

    modes.resize(count);
    return vk::GetPhysicalDeviceSurfacePresentModesKHR(phy, surface, &count, modes.data());
}

inline VkResult get(VkDevice dev, VkSwapchainKHR swapchain, std::vector<VkImage> &images) {
    uint32_t count = 0;
    vk::GetSwapchainImagesKHR(dev, swapchain, &count, nullptr);

    images.resize(count);
    return vk::GetSwapchainImagesKHR(dev, swapchain, &count, images.data());
}

inline VkVideoCodecOperationFlagsKHR GetSupportedCodecs(VkPhysicalDevice vkPhysicalDev, int32_t* pVideoQueueFamily,
        VkQueueFlags queueFlagsRequired = ( VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_VIDEO_ENCODE_BIT_KHR),
        VkVideoCodecOperationFlagsKHR videoCodeOperations =
                                          ( VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT |
                                            VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT | VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT))
{
    std::vector<VkQueueFamilyProperties2> queues;
    std::vector<VkVideoQueueFamilyProperties2KHR> videoQueues;
    std::vector<VkQueueFamilyQueryResultStatusProperties2KHR> queryResultStatus;
    vk::get(vkPhysicalDev, queues, videoQueues, queryResultStatus);

    for (uint32_t queueIndx = 0; queueIndx < queues.size(); queueIndx++) {
        const VkQueueFamilyProperties2 &q = queues[queueIndx];
        const VkVideoQueueFamilyProperties2KHR &videoQueue = videoQueues[queueIndx];

        if (pVideoQueueFamily && (*pVideoQueueFamily >= 0) && (*pVideoQueueFamily != (int32_t)queueIndx)) {
            continue;
        }

        if ((q.queueFamilyProperties.queueFlags & queueFlagsRequired) &&
            (videoQueue.videoCodecOperations & videoCodeOperations)) {
            if (pVideoQueueFamily && (*pVideoQueueFamily < 0)) {
                *pVideoQueueFamily = (int32_t)queueIndx;
            }
            // The video queues must support queryResultStatus
            assert(queryResultStatus[queueIndx].supported);
            return videoQueue.videoCodecOperations;
        }
    }

    return VK_VIDEO_CODEC_OPERATION_INVALID_BIT_KHR;
}

}  // namespace vk

#endif  // HELPERS_H
