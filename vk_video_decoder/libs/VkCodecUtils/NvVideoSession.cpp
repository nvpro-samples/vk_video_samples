/*
* Copyright 2021 NVIDIA Corporation.
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

#include <assert.h>
#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/HelpersDispatchTable.h"
#include "VkCodecUtils/VulkanVideoUtils.h"
#include "VkCodecUtils/nvVideoProfile.h"
#include "VkCodecUtils/NvVideoSession.h"

VkResult NvVideoSession::Create(VkDevice         dev,
                                uint32_t         videoQueueFamily,
                                nvVideoProfile*  pVideoProfile,
                                VkFormat         pictureFormat,
                                VkExtent2D*      pMaxCodedExtent,
                                VkFormat         referencePicturesFormat,
                                uint32_t         maxReferencePicturesSlotsCount,
                                uint32_t         maxReferencePicturesActiveCount,
                                VkSharedBaseObj<NvVideoSession>& videoSession)
{
    NvVideoSession* pNewVideoSession = new NvVideoSession(pVideoProfile);

    static const VkExtensionProperties h264StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H264_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H264_SPEC_VERSION };
    static const VkExtensionProperties h265StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H265_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H265_SPEC_VERSION };

    VkVideoDecodeH264SessionCreateInfoEXT decodeSessionCreateInfoH264 = VkVideoDecodeH264SessionCreateInfoEXT();
    decodeSessionCreateInfoH264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_CREATE_INFO_EXT;
    decodeSessionCreateInfoH264.pStdExtensionVersion = &h264StdExtensionVersion;

    VkVideoDecodeH265SessionCreateInfoEXT decodeSessionCreateInfoH265 = VkVideoDecodeH265SessionCreateInfoEXT();
    decodeSessionCreateInfoH265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_CREATE_INFO_EXT;
    decodeSessionCreateInfoH265.pStdExtensionVersion = &h265StdExtensionVersion;

    VkVideoEncodeH264SessionCreateInfoEXT encodeSessionCreateInfoH264 = VkVideoEncodeH264SessionCreateInfoEXT();
    encodeSessionCreateInfoH264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_CREATE_INFO_EXT;
    encodeSessionCreateInfoH264.pStdExtensionVersion = &h264StdExtensionVersion;

    VkVideoEncodeH265SessionCreateInfoEXT encodeSessionCreateInfoH265 = VkVideoEncodeH265SessionCreateInfoEXT();
    encodeSessionCreateInfoH265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_CREATE_INFO_EXT;
    encodeSessionCreateInfoH265.pStdExtensionVersion = &h265StdExtensionVersion;

    VkVideoSessionCreateInfoKHR createInfo = VkVideoSessionCreateInfoKHR();
    createInfo.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    createInfo.flags = 0;
    createInfo.pVideoProfile = pVideoProfile->GetProfile();
    createInfo.queueFamilyIndex = videoQueueFamily;
    createInfo.pictureFormat = pictureFormat;
    createInfo.maxCodedExtent = *pMaxCodedExtent;
    createInfo.maxReferencePicturesSlotsCount = maxReferencePicturesSlotsCount;
    createInfo.maxReferencePicturesActiveCount = maxReferencePicturesActiveCount;
    createInfo.referencePicturesFormat = referencePicturesFormat;

    switch ((int32_t)pVideoProfile->GetCodecType()) {
    case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
        createInfo.pNext = &decodeSessionCreateInfoH264;
        break;
    case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
        createInfo.pNext = &decodeSessionCreateInfoH265;
        break;
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT:
        createInfo.pNext = &encodeSessionCreateInfoH264;
        break;
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT:
        createInfo.pNext = &encodeSessionCreateInfoH265;
        break;
    default:
        assert(0);
    }
    VkResult result = vk::CreateVideoSessionKHR(dev, &createInfo, NULL, &pNewVideoSession->m_videoSession);
    assert(result == VK_SUCCESS);
    pNewVideoSession->m_dev = dev;

    const uint32_t maxMemReq = 8;
    uint32_t videoSessionMemoryRequirementsCount = 0;
    VkMemoryRequirements2 memoryRequirements[maxMemReq];
    VkVideoGetMemoryPropertiesKHR decodeSessionMemoryRequirements[maxMemReq];
    // Get the count first
    result = vk::GetVideoSessionMemoryRequirementsKHR(dev, pNewVideoSession->m_videoSession,
        &videoSessionMemoryRequirementsCount, NULL);
    assert(result == VK_SUCCESS);
    assert(videoSessionMemoryRequirementsCount <= maxMemReq);

    memset(decodeSessionMemoryRequirements, 0x00, sizeof(decodeSessionMemoryRequirements));
    memset(memoryRequirements, 0x00, sizeof(memoryRequirements));
    for (uint32_t i = 0; i < videoSessionMemoryRequirementsCount; i++) {
        decodeSessionMemoryRequirements[i].sType = VK_STRUCTURE_TYPE_VIDEO_GET_MEMORY_PROPERTIES_KHR;
        decodeSessionMemoryRequirements[i].pMemoryRequirements = &memoryRequirements[i];
        memoryRequirements[i].sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    }

    result = vk::GetVideoSessionMemoryRequirementsKHR(dev, pNewVideoSession->m_videoSession,
                                                      &videoSessionMemoryRequirementsCount,
                                                      decodeSessionMemoryRequirements);
    assert(result == VK_SUCCESS);

    uint32_t decodeSessionBindMemoryCount = videoSessionMemoryRequirementsCount;
    VkVideoBindMemoryKHR decodeSessionBindMemory[8];

    for (uint32_t memIdx = 0; memIdx < decodeSessionBindMemoryCount; memIdx++) {
        result = pNewVideoSession->m_memoryBound[memIdx].AllocMemory(dev, &memoryRequirements[memIdx].memoryRequirements,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        assert(result == VK_SUCCESS);
        decodeSessionBindMemory[memIdx].pNext = NULL;
        decodeSessionBindMemory[memIdx].sType = VK_STRUCTURE_TYPE_VIDEO_BIND_MEMORY_KHR;
        decodeSessionBindMemory[memIdx].memory = pNewVideoSession->m_memoryBound[memIdx].memory;

        decodeSessionBindMemory[memIdx].memoryBindIndex = decodeSessionMemoryRequirements[memIdx].memoryBindIndex;
        decodeSessionBindMemory[memIdx].memoryOffset = 0;
        decodeSessionBindMemory[memIdx].memorySize = memoryRequirements[memIdx].memoryRequirements.size;
    }

    result = vk::BindVideoSessionMemoryKHR(dev, pNewVideoSession->m_videoSession, decodeSessionBindMemoryCount,
        decodeSessionBindMemory);
    assert(result == VK_SUCCESS);

    videoSession = pNewVideoSession;

    return result;
}




