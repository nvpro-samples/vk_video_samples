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
#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkCodecUtils/VulkanDeviceMemoryImpl.h"

class VkImageResource : public VkVideoRefCountBase
{
public:
    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           const VkImageCreateInfo* pImageCreateInfo,
                           VkMemoryPropertyFlags memoryPropertyFlags,
                           VkSharedBaseObj<VkImageResource>& imageResource);

    bool IsCompatible ( VkDevice,
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

    const VkSubresourceLayout* GetSubresourceLayout() const {
        return m_isLinearImage ? m_layouts : nullptr;
    }

private:
    std::atomic<int32_t>    m_refCount;
    const VkImageCreateInfo m_imageCreateInfo;
    const VulkanDeviceContext* m_vkDevCtx;
    VkImage                 m_image;
    VkDeviceSize            m_imageOffset;
    VkDeviceSize            m_imageSize;
    VkSharedBaseObj<VulkanDeviceMemoryImpl> m_vulkanDeviceMemory;
    VkSubresourceLayout     m_layouts[3]; // per plane layout for linear images
    uint32_t                m_isLinearImage : 1;
    uint32_t                m_is16Bit : 1;
    uint32_t                m_isSubsampledX : 1;
    uint32_t                m_isSubsampledY : 1;

    VkImageResource(const VulkanDeviceContext* vkDevCtx,
                    const VkImageCreateInfo* pImageCreateInfo,
                    VkImage image, VkDeviceSize imageOffset, VkDeviceSize imageSize,
                    VkSharedBaseObj<VulkanDeviceMemoryImpl>& vulkanDeviceMemory);

    void Destroy();

    virtual ~VkImageResource();
};

class VkImageResourceView : public VkVideoRefCountBase
{
public:
    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VkImageResource>& imageResource,
                           VkImageSubresourceRange &imageSubresourceRange,
                           VkSharedBaseObj<VkImageResourceView>& imageResourceView);

    // Overload with optional plane usage override for storage-compatible plane views
    // When planeUsageOverride is non-zero, plane views will be created with VkImageViewUsageCreateInfo
    // specifying that usage. This is needed when the base format doesn't support storage but
    // individual planes (R8, RG8) do via VK_IMAGE_CREATE_EXTENDED_USAGE_BIT.
    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VkImageResource>& imageResource,
                           VkImageSubresourceRange &imageSubresourceRange,
                           VkImageUsageFlags planeUsageOverride,
                           VkSharedBaseObj<VkImageResourceView>& imageResourceView);

    /**
     * @brief Create image view with YCbCr sampler conversion support
     *
     * Creates both:
     * 1. A combined YCbCr view with VkSamplerYcbcrConversionInfo attached (for display sampling)
     * 2. Per-plane views with optional usage override (for compute storage)
     *
     * This is needed when an image needs to be both:
     * - Written by a compute shader (storage views per-plane)
     * - Sampled with YCbCr conversion for display (combined sampled view)
     *
     * @param vkDevCtx              Vulkan device context
     * @param imageResource         Image resource to create view for
     * @param imageSubresourceRange Subresource range for the view
     * @param planeUsageOverride    Usage flags for per-plane views (e.g., VK_IMAGE_USAGE_STORAGE_BIT)
     * @param ycbcrConversion       YCbCr sampler conversion object (or VK_NULL_HANDLE if none)
     * @param combinedViewUsage     Usage flags for combined view (e.g., VK_IMAGE_USAGE_SAMPLED_BIT)
     * @param imageResourceView     Output image resource view
     * @return VK_SUCCESS on success
     */
    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VkImageResource>& imageResource,
                           VkImageSubresourceRange &imageSubresourceRange,
                           VkImageUsageFlags planeUsageOverride,
                           VkSamplerYcbcrConversion ycbcrConversion,
                           VkImageUsageFlags combinedViewUsage,
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

    operator VkImageView() const {
        // Fall back to first plane view if combined view is null (storage-only case)
        return m_imageViews[0] ? m_imageViews[0] : (m_numPlanes > 0 ? m_imageViews[1] : VK_NULL_HANDLE);
    }
    VkImageView GetImageView() const {
        // Fall back to first plane view if combined view is null (storage-only case)
        return m_imageViews[0] ? m_imageViews[0] : (m_numPlanes > 0 ? m_imageViews[1] : VK_NULL_HANDLE);
    }
    uint32_t GetNumberOfPlanes() const { return m_numPlanes; }
    VkImageView GetPlaneImageView(uint32_t planeIndex = 0) const {
        if (m_numPlanes == 1) {
            return m_imageViews[0];
        }
        assert(planeIndex < m_numPlanes);
        return m_imageViews[planeIndex + 1];
    }
    VkDevice GetDevice() const { return *m_vkDevCtx; }

    const VkImageSubresourceRange& GetImageSubresourceRange() const
    { return m_imageSubresourceRange; }

    const VkSharedBaseObj<VkImageResource>& GetImageResource() const
    {
        return m_imageResource;
    }

private:
    std::atomic<int32_t>             m_refCount;
    const VulkanDeviceContext*       m_vkDevCtx;
    VkSharedBaseObj<VkImageResource> m_imageResource;
    VkImageView                      m_imageViews[4];
    VkImageSubresourceRange          m_imageSubresourceRange;
    uint32_t                         m_numViews;
    uint32_t                         m_numPlanes;


    VkImageResourceView(const VulkanDeviceContext* vkDevCtx,
                        VkSharedBaseObj<VkImageResource>& imageResource,
                        uint32_t numViews, uint32_t numPlanes,
                        VkImageView imageViews[4], VkImageSubresourceRange &imageSubresourceRange)
       : m_refCount(0), m_vkDevCtx(vkDevCtx), m_imageResource(imageResource),
         m_imageViews{VK_NULL_HANDLE}, m_imageSubresourceRange(imageSubresourceRange),
         m_numViews(numViews), m_numPlanes(numPlanes)
    {
        for (uint32_t imageViewIndx = 0; imageViewIndx < m_numViews; imageViewIndx++) {
            m_imageViews[imageViewIndx] = imageViews[imageViewIndx];
        }
    }

    virtual ~VkImageResourceView();
};
