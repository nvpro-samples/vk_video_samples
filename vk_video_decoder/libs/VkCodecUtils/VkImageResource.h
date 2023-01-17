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

#pragma once

#include <atomic>
#include <vulkan_interfaces.h>
#include "VkVideoCore/VkVideoRefCountBase.h"
#include "VkCodecUtils/VulkanDeviceMemoryImpl.h"

class VkImageResource : public VkVideoRefCountBase
{
public:
    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           const VkImageCreateInfo* pImageCreateInfo,
                           VkMemoryPropertyFlags memoryPropertyFlags,
                           VkSharedBaseObj<VkImageResource>& imageResource);

    bool IsCompatible ( VkDevice dev,
                        const VkImageCreateInfo* pImageCreateInfo)
    {

        if (pImageCreateInfo->extent.width > m_imageCreateInfo.extent.width) {
            return false;
        }

        if (pImageCreateInfo->extent.height > m_imageCreateInfo.extent.height) {
            return false;
        }

        if (pImageCreateInfo->arrayLayers > m_imageCreateInfo.arrayLayers) {
            return false;
        }

        if (pImageCreateInfo->tiling != m_imageCreateInfo.tiling) {
            return false;
        }

        if (pImageCreateInfo->imageType != m_imageCreateInfo.imageType) {
            return false;
        }

        if (pImageCreateInfo->format != m_imageCreateInfo.format) {
            return false;
        }

        return true;
    }


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

    operator VkImage() const { return m_image; }
    VkImage GetImage() const { return m_image; }
    VkDevice GetDevice() const { return *m_vkDevCtx; }
    VkDeviceMemory GetDeviceMemory() const { return *m_vulkanDeviceMemory; }

    VkSharedBaseObj<VulkanDeviceMemoryImpl>& GetMemory() { return m_vulkanDeviceMemory; }

    VkDeviceSize GetImageDeviceMemorySize() const { return m_imageSize; }
    VkDeviceSize GetImageDeviceMemoryOffset() const { return m_imageOffset; }

    const VkImageCreateInfo& GetImageCreateInfo() const { return m_imageCreateInfo; }

private:
    std::atomic<int32_t>    m_refCount;
    const VkImageCreateInfo m_imageCreateInfo;
    const VulkanDeviceContext* m_vkDevCtx;
    VkImage                 m_image;
    VkDeviceSize            m_imageOffset;
    VkDeviceSize            m_imageSize;
    VkSharedBaseObj<VulkanDeviceMemoryImpl> m_vulkanDeviceMemory;

    VkImageResource(const VulkanDeviceContext* vkDevCtx,
                    const VkImageCreateInfo* pImageCreateInfo,
                    VkImage image, VkDeviceSize imageOffset, VkDeviceSize imageSize,
                    VkSharedBaseObj<VulkanDeviceMemoryImpl>& vulkanDeviceMemory)
       : m_refCount(0), m_imageCreateInfo(*pImageCreateInfo), m_vkDevCtx(vkDevCtx),
         m_image(image), m_imageOffset(imageOffset), m_imageSize(imageSize),
         m_vulkanDeviceMemory(vulkanDeviceMemory) { }

    void Destroy();

    virtual ~VkImageResource() { Destroy(); }
};

class VkImageResourceView : public VkVideoRefCountBase
{
public:
    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VkImageResource>& imageResource,
                           VkImageSubresourceRange &imageSubresourceRange,
                           VkSharedBaseObj<VkImageResourceView>& imageResourceView);


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

    operator VkImageView() const { return m_imageView; }
    VkImageView GetImageView() const { return m_imageView; }
    VkDevice GetDevice() const { return *m_vkDevCtx; }

    const VkImageSubresourceRange& GetImageSubresourceRange() const
    { return m_imageSubresourceRange; }

    const VkSharedBaseObj<VkImageResource>& GetImageResource()
    {
        return m_imageResource;
    }

private:
    std::atomic<int32_t>             m_refCount;
    const VulkanDeviceContext*       m_vkDevCtx;
    VkSharedBaseObj<VkImageResource> m_imageResource;
    VkImageView                      m_imageView;
    VkImageSubresourceRange          m_imageSubresourceRange;


    VkImageResourceView(const VulkanDeviceContext* vkDevCtx,
                        VkSharedBaseObj<VkImageResource>& imageResource,
                        VkImageView imageView, VkImageSubresourceRange &imageSubresourceRange)
       : m_refCount(0), m_vkDevCtx(vkDevCtx), m_imageResource(imageResource),
         m_imageView(imageView), m_imageSubresourceRange(imageSubresourceRange)
    {}

    virtual ~VkImageResourceView();
};
