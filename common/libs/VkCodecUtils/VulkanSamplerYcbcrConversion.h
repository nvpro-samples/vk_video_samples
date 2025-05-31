/*
* Copyright 2023 NVIDIA Corporation.
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

#ifndef _VULKANSAMPLERYCBCRCONVERSION_H_
#define _VULKANSAMPLERYCBCRCONVERSION_H_

#include <vulkan_interfaces.h>
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VulkanSamplerYcbcrConversion.h"
#include "vulkan/vulkan_core.h"

class VulkanSamplerYcbcrConversion {

public:

    VulkanSamplerYcbcrConversion ()
        : m_vkDevCtx(),
          m_samplerInfo(),
          m_samplerYcbcrConversionCreateInfo(),
          m_samplerYcbcrConversion(),
          m_sampler()
    {

    }

    ~VulkanSamplerYcbcrConversion () {
        DestroyVulkanSampler();
    }

    void DestroyVulkanSampler() {
        if (m_sampler) {
            m_vkDevCtx->DestroySampler(*m_vkDevCtx, m_sampler, nullptr);
        }
        m_sampler = VkSampler();

        if(m_samplerYcbcrConversion) {
            m_vkDevCtx->DestroySamplerYcbcrConversion(*m_vkDevCtx, m_samplerYcbcrConversion, NULL);
        }

        m_samplerYcbcrConversion = VkSamplerYcbcrConversion(0);
    }

    VkResult CreateVulkanSampler(const VulkanDeviceContext* vkDevCtx,
                                 const VkSamplerCreateInfo* pSamplerCreateInfo,
                                 const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo);

    VkSampler GetSampler() const {
      return m_sampler;
    }

    const VkSamplerYcbcrConversionCreateInfo& GetSamplerYcbcrConversionCreateInfo() const
    {
        return m_samplerYcbcrConversionCreateInfo;
    }

    uint32_t GetCombinedImageSamplerDescriptorCount() const
    {
        VkSamplerYcbcrConversionImageFormatProperties samplerYcbcrConversionImageFormatProperties =
                                                    { VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES };

        VkImageFormatProperties2 imageFormatProperties = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                                                           &samplerYcbcrConversionImageFormatProperties };

        const VkPhysicalDeviceImageFormatInfo2 imageFormatInfo =
                VkPhysicalDeviceImageFormatInfo2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, nullptr,
                                                   m_samplerYcbcrConversionCreateInfo.format,
                                                   VK_IMAGE_TYPE_2D,
                                                   VK_IMAGE_TILING_OPTIMAL,
                                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                                                   0 };

        VkResult result = m_vkDevCtx->GetPhysicalDeviceImageFormatProperties2(m_vkDevCtx->getPhysicalDevice(),
                                                                              &imageFormatInfo,
                                                                              &imageFormatProperties);
        if (result != VK_SUCCESS) {
            assert(!"ERROR: vkGetPhysicalDeviceImageFormatProperties2!");
        }

        return samplerYcbcrConversionImageFormatProperties.combinedImageSamplerDescriptorCount;
    }

    // sampler requires update if the function were to return true.
    bool SamplerRequiresUpdate(const VkSamplerCreateInfo* pSamplerCreateInfo,
            const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo);

private:
    const VulkanDeviceContext*         m_vkDevCtx;
    VkSamplerCreateInfo                m_samplerInfo;
    VkSamplerYcbcrConversionCreateInfo m_samplerYcbcrConversionCreateInfo;
    VkSamplerYcbcrConversion           m_samplerYcbcrConversion;
    VkSampler                          m_sampler;
};

class VulkanSamplerResize {

public:
    ~VulkanSamplerResize () {
        // already explicitly destroyed in VulkanSamplerYcbcrConversion destructor
        /*
        if (m_sampler) {
            m_vkDevCtx->DestroySampler(*m_vkDevCtx, m_sampler, NULL);
        }
        m_sampler = VkSampler();
        */
    }

    VkSampler GetSampler() const {
        return m_sampler;
    }

    VkResult CreateVulkanSampler(const VulkanDeviceContext* vkDevCtx,
                                 const VkSamplerCreateInfo* pSamplerCreateInfo = NULL) {
        m_vkDevCtx = (VulkanDeviceContext*)vkDevCtx;
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_FALSE; //VK_TRUE;
        samplerInfo.maxAnisotropy = 1.0f; //m_vkDevCtx->GetMaxAnisotropy();
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        samplerInfo.pNext = NULL;
        if (vkDevCtx->CreateSampler(vkDevCtx->getDevice(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return VK_SUCCESS;
    }

private:
    VkSampler m_sampler;
    VulkanDeviceContext* m_vkDevCtx;
    VkSamplerCreateInfo m_samplerInfo;
};

#endif /* _VULKANSAMPLERYCBCRCONVERSION_H_ */
