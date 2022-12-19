/*
* Copyright 2022 NVIDIA Corporation.
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

#include <atomic>
#include "VkCodecUtils/HelpersDispatchTable.h"
#include "VkImageResource.h"

static VkResult MapMemoryTypeToIndex(VkPhysicalDevice gpuDevice, uint32_t typeBits,
                          VkFlags requirements_mask, uint32_t *typeIndex)
{
    VkPhysicalDeviceMemoryProperties memoryProperties;
    vk::GetPhysicalDeviceMemoryProperties(gpuDevice, &memoryProperties);
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

VkResult VkImageResource::Create(VkPhysicalDevice physicalDevice, VkDevice device,
                                 const VkImageCreateInfo* pImageCreateInfo,
                                 VkMemoryPropertyFlags requiredMemoryProperty,
                                 VkSharedBaseObj<VkImageResource>& imageResource)
{
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    do {

        result = vk::CreateImage(device, pImageCreateInfo, nullptr, &image);
        if (result != VK_SUCCESS) {
            break;
        }

        VkMemoryRequirements memReqs = { };
        vk::GetImageMemoryRequirements(device, image, &memReqs);

        // Find an available memory type that satisfies the requested properties.
        uint32_t memoryTypeIndex;
        result = MapMemoryTypeToIndex(physicalDevice, memReqs.memoryTypeBits,
                                      requiredMemoryProperty, &memoryTypeIndex);
        if (result != VK_SUCCESS) {
            break;
        }

        VkMemoryAllocateInfo memInfo = {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,          // sType
            NULL,                                            // pNext
            memReqs.size,                                    // allocationSize
            memoryTypeIndex,                                 // memoryTypeIndex
        };

        result = vk::AllocateMemory(device, &memInfo, 0, &memory);
        if (result != VK_SUCCESS) {
            break;
        }

        result = vk::BindImageMemory(device, image, memory, 0);
        if (result != VK_SUCCESS) {
            break;
        }

        imageResource = new VkImageResource(device, pImageCreateInfo,
                                            image, memory, memoryTypeIndex);
        return result;

    } while (0);

    if (device != VK_NULL_HANDLE) {

        if (memory != VK_NULL_HANDLE) {
            vk::FreeMemory(device, memory, 0);
        }

        if (image != VK_NULL_HANDLE) {
            vk::DestroyImage(device, image, nullptr);
        }
    }

    return result;
}


void VkImageResource::Destroy()
{
    assert(m_device != VK_NULL_HANDLE);

    if (m_image != VK_NULL_HANDLE) {
        vk::DestroyImage(m_device, m_image, nullptr);
        m_image = VK_NULL_HANDLE;
    }

    if (m_memory != VK_NULL_HANDLE) {
        vk::FreeMemory(m_device, m_memory, 0);
        m_memory = VK_NULL_HANDLE;
    }

    m_device = VK_NULL_HANDLE;
}

VkResult VkImageResourceView::Create(VkDevice device,
                                     VkSharedBaseObj<VkImageResource>& imageResource,
                                     VkImageSubresourceRange &imageSubresourceRange,
                                     VkSharedBaseObj<VkImageResourceView>& imageResourceView)
{
    VkImageView  imageView;
    VkImageViewCreateInfo viewInfo = VkImageViewCreateInfo();
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.pNext = nullptr;
    viewInfo.image = imageResource->GetImage();
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageResource->GetImageCreateInfo().format;
    viewInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange = imageSubresourceRange;
    viewInfo.flags = 0;
    VkResult result = vk::CreateImageView(device, &viewInfo, nullptr, &imageView);
    if (result != VK_SUCCESS) {
        return result;
    }

    imageResourceView = new VkImageResourceView(device, imageResource,
                                                imageView, imageSubresourceRange);

    return result;
}

VkImageResourceView::~VkImageResourceView()
{
    if (m_imageView != VK_NULL_HANDLE) {
        vk::DestroyImageView(m_device, m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }

    m_imageResource = nullptr;

    m_device = VK_NULL_HANDLE;;
}
