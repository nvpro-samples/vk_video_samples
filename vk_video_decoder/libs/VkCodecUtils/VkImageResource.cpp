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
#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkImageResource.h"

VkResult VkImageResource::Create(const VulkanDeviceContext* vkDevCtx,
                                 const VkImageCreateInfo* pImageCreateInfo,
                                 VkMemoryPropertyFlags memoryPropertyFlags,
                                 VkSharedBaseObj<VkImageResource>& imageResource)
{
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;

    VkDevice device = vkDevCtx->getDevice();
    VkImage image = VK_NULL_HANDLE;

    do {

        result = vkDevCtx->CreateImage(device, pImageCreateInfo, nullptr, &image);
        if (result != VK_SUCCESS) {
            assert(!"CreateImage Failed!");
            break;
        }

        VkMemoryRequirements memoryRequirements = { };
        vkDevCtx->GetImageMemoryRequirements(device, image, &memoryRequirements);

        // Allocate memory for the image
        VkSharedBaseObj<VulkanDeviceMemoryImpl> vkDeviceMemory;
        result = VulkanDeviceMemoryImpl::Create(vkDevCtx,
                                                memoryRequirements,
                                                memoryPropertyFlags,
                                                nullptr, // pInitializeMemory
                                                0ULL,     // initializeMemorySize
                                                false,   // clearMemory
                                                vkDeviceMemory);
        if (result != VK_SUCCESS) {
            assert(!"Create Memory Failed!");
            break;
        }

        VkDeviceSize imageOffset = 0;
        result = vkDevCtx->BindImageMemory(device, image, *vkDeviceMemory, imageOffset);
        if (result != VK_SUCCESS) {
            assert(!"BindImageMemory Failed!");
            break;
        }

        imageResource = new VkImageResource(vkDevCtx,
                                            pImageCreateInfo,
                                            image,
                                            imageOffset,
                                            memoryRequirements.size,
                                            vkDeviceMemory);
        if (imageResource == nullptr) {
            break;
        }
        return result;

    } while (0);

    if (device != VK_NULL_HANDLE) {

        if (image != VK_NULL_HANDLE) {
            vkDevCtx->DestroyImage(device, image, nullptr);
        }
    }

    return result;
}


void VkImageResource::Destroy()
{
    assert(m_vkDevCtx != nullptr);

    if (m_image != VK_NULL_HANDLE) {
        m_vkDevCtx->DestroyImage(*m_vkDevCtx, m_image, nullptr);
        m_image = VK_NULL_HANDLE;
    }

    m_vulkanDeviceMemory = nullptr;
    m_vkDevCtx = nullptr;
}

VkResult VkImageResourceView::Create(const VulkanDeviceContext* vkDevCtx,
                                     VkSharedBaseObj<VkImageResource>& imageResource,
                                     VkImageSubresourceRange &imageSubresourceRange,
                                     VkSharedBaseObj<VkImageResourceView>& imageResourceView)
{
    VkDevice device = vkDevCtx->getDevice();
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
    VkResult result = vkDevCtx->CreateImageView(device, &viewInfo, nullptr, &imageView);
    if (result != VK_SUCCESS) {
        return result;
    }

    imageResourceView = new VkImageResourceView(vkDevCtx, imageResource,
                                                imageView, imageSubresourceRange);

    return result;
}

VkImageResourceView::~VkImageResourceView()
{
    if (m_imageView != VK_NULL_HANDLE) {
        m_vkDevCtx->DestroyImageView(*m_vkDevCtx, m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }

    m_imageResource = nullptr;
    m_vkDevCtx = nullptr;
}
