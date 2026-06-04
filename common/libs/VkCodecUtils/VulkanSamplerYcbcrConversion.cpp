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

#include <stdio.h>
#include <string.h>
#include "VulkanSamplerYcbcrConversion.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

bool VulkanSamplerYcbcrConversion::SamplerRequiresUpdate(const VkSamplerCreateInfo* pSamplerCreateInfo,
                                                         const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo)
{
    if (pSamplerCreateInfo) {
        if (memcmp(&m_samplerInfo, pSamplerCreateInfo, sizeof(m_samplerInfo))) {
            return true;
        }
    }

    if (pSamplerYcbcrConversionCreateInfo) {
        if (memcmp(&m_samplerYcbcrConversionCreateInfo, pSamplerYcbcrConversionCreateInfo, sizeof(m_samplerYcbcrConversionCreateInfo))) {
            return true;
        }
    }

    return false;
}


VkResult VulkanSamplerYcbcrConversion::CreateVulkanSampler(const VulkanDeviceContext* vkDevCtx,
                                                           const VkSamplerCreateInfo* pSamplerCreateInfo,
                                                           const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo)
{
    m_vkDevCtx = vkDevCtx;
    VkResult result = VK_SUCCESS;

    DestroyVulkanSampler();

    VkSamplerYcbcrConversionInfo* pSamplerColorConversion = NULL;
    VkSamplerYcbcrConversionInfo samplerColorConversion = VkSamplerYcbcrConversionInfo();
    const VkMpFormatInfo* mpInfo = pSamplerYcbcrConversionCreateInfo ? YcbcrVkFormatInfo(pSamplerYcbcrConversionCreateInfo->format) : nullptr;

    // Always retain the requested conversion create-info so callers that only
    // need the color-model/range (e.g. the RGBA2YCBCR compute filter, which
    // writes YCbCr via a storage image and never samples through a conversion)
    // can read it back via GetSamplerYcbcrConversionCreateInfo(). Previously
    // this was only stored inside the mpInfo branch, so packed/color output
    // formats silently fell back to a default (RGB-identity) matrix.
    if (pSamplerYcbcrConversionCreateInfo) {
        memcpy(&m_samplerYcbcrConversionCreateInfo, pSamplerYcbcrConversionCreateInfo, sizeof(m_samplerYcbcrConversionCreateInfo));
    }

    // Only create an actual VkSamplerYcbcrConversion object for multi-planar
    // YCbCr formats that are meant to be *sampled* through the conversion.
    // Single-plane interleaved 4:2:2 formats (G8B8G8R8_422 / G10X6...422 /
    // G16B16G16R16_422) are consumed as packed byte blobs (NVCUVID/NVENC input)
    // and, on NVIDIA, don't advertise COSITED/MIDPOINT chroma sampling features,
    // so creating a conversion for them fails validation. Skip the object for
    // those; the stored create-info above still carries the color matrix.
    const bool isSinglePlaneInterleaved =
        mpInfo && (mpInfo->planesLayout.layout == YCBCR_SINGLE_PLANE_INTERLEAVED);
    if (mpInfo && !isSinglePlaneInterleaved) {

        result = m_vkDevCtx->CreateSamplerYcbcrConversion(*m_vkDevCtx, &m_samplerYcbcrConversionCreateInfo, NULL, &m_samplerYcbcrConversion);
        if (result != VK_SUCCESS) {
            return result;
        }

        pSamplerColorConversion = &samplerColorConversion;
        pSamplerColorConversion->sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        pSamplerColorConversion->conversion = m_samplerYcbcrConversion;
    }

    const VkSamplerCreateInfo defaultSamplerInfo = {
            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR,  VK_FILTER_LINEAR,  VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            // mipLodBias  anisotropyEnable  maxAnisotropy  compareEnable      compareOp         minLod  maxLod          borderColor                   unnormalizedCoordinates
                 0.0,          false,            0.00,         false,       VK_COMPARE_OP_NEVER,   0.0,   16.0,    VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,        false };

    if (pSamplerCreateInfo) {
        memcpy(&m_samplerInfo, pSamplerCreateInfo, sizeof(m_samplerInfo));
    } else {
        memcpy(&m_samplerInfo, &defaultSamplerInfo, sizeof(defaultSamplerInfo));
    }
    m_samplerInfo.pNext = pSamplerColorConversion;
    result = m_vkDevCtx->CreateSampler(*m_vkDevCtx, &m_samplerInfo, nullptr, &m_sampler);

    m_samplerInfo.pNext = 0;

    return result;
}
