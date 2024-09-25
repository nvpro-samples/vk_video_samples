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

#include <assert.h>
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "NvVideoSession.h"

VkResult NvVideoSession::create(nvvk::MemAllocator* devAlloc,
                                nvvk::Context*      vkctx,
                                uint32_t            videoQueueFamily,
                                VkVideoCoreProfile* pVideoProfile,
                                VkFormat            pictureFormat,
                                const VkExtent2D&   maxCodedExtent,
                                VkFormat            referencePicturesFormat,
                                uint32_t            maxReferencePicturesSlotsCount,
                                uint32_t            maxReferencePicturesActiveCount,
                                NvVideoSession**    ppVideoSession)
{
    NvVideoSession* pNewVideoSession = new NvVideoSession(pVideoProfile);

    static const VkExtensionProperties h264StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION };
    static const VkExtensionProperties h265StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H265_ENCODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H265_ENCODE_SPEC_VERSION };
    static const VkExtensionProperties av1StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_AV1_ENCODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_AV1_ENCODE_SPEC_VERSION };

    VkDevice dev = vkctx->m_device;

    VkVideoSessionCreateInfoKHR createInfo = { VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR };
    createInfo.pVideoProfile = pVideoProfile->GetProfile();
    createInfo.queueFamilyIndex = videoQueueFamily;
    createInfo.pictureFormat = pictureFormat;
    createInfo.maxCodedExtent = maxCodedExtent;
    createInfo.maxDpbSlots = maxReferencePicturesSlotsCount;
    createInfo.maxActiveReferencePictures = maxReferencePicturesActiveCount;
    createInfo.referencePictureFormat = referencePicturesFormat;
    createInfo.pStdHeaderVersion = &h264StdExtensionVersion;

    VkResult result = vkCreateVideoSessionKHR(dev, &createInfo, NULL, &pNewVideoSession->m_videoSession);
    assert(result == VK_SUCCESS);
    pNewVideoSession->m_dev = dev;

    const uint32_t maxMemReq = 8;
    uint32_t videoSessionMemoryRequirementsCount = 0;
    VkVideoSessionMemoryRequirementsKHR encodeSessionMemoryRequirements[maxMemReq];
    // Get the count first
    result = vkGetVideoSessionMemoryRequirementsKHR(dev, pNewVideoSession->m_videoSession,
             &videoSessionMemoryRequirementsCount, NULL);
    assert(result == VK_SUCCESS);
    assert(videoSessionMemoryRequirementsCount <= maxMemReq);

    memset(encodeSessionMemoryRequirements, 0x00, sizeof(encodeSessionMemoryRequirements));
    for (uint32_t i = 0; i < videoSessionMemoryRequirementsCount; i++) {
        encodeSessionMemoryRequirements[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    }

    result = vkGetVideoSessionMemoryRequirementsKHR(dev, pNewVideoSession->m_videoSession,
             &videoSessionMemoryRequirementsCount,
             encodeSessionMemoryRequirements);
    assert(result == VK_SUCCESS);

    uint32_t encodeSessionBindMemoryCount = videoSessionMemoryRequirementsCount;
    VkBindVideoSessionMemoryInfoKHR encodeSessionBindMemory[8];

    pNewVideoSession->m_devAlloc = devAlloc;

    for (uint32_t memIdx = 0; memIdx < encodeSessionBindMemoryCount; memIdx++) {
        nvvk::MemAllocateInfo memAllocInfo(encodeSessionMemoryRequirements[memIdx].memoryRequirements, 0);
        nvvk::MemHandle handle = devAlloc->allocMemory(memAllocInfo);
        if(!handle) {
            assert(0 && "could not allocate buffer\n");
        }

        pNewVideoSession->m_boundMemory[memIdx] = handle;

        nvvk::MemAllocator::MemInfo memInfo = devAlloc->getMemoryInfo(handle);

        encodeSessionBindMemory[memIdx].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        encodeSessionBindMemory[memIdx].pNext = NULL;
        encodeSessionBindMemory[memIdx].memory = memInfo.memory;

        encodeSessionBindMemory[memIdx].memoryBindIndex = encodeSessionMemoryRequirements[memIdx].memoryBindIndex;
        encodeSessionBindMemory[memIdx].memoryOffset = memInfo.offset;
        encodeSessionBindMemory[memIdx].memorySize = memInfo.size;
    }

    pNewVideoSession->m_boundMemoryCount = encodeSessionBindMemoryCount;

    result = vkBindVideoSessionMemoryKHR(dev, pNewVideoSession->m_videoSession, encodeSessionBindMemoryCount,
                                         encodeSessionBindMemory);
    assert(result == VK_SUCCESS);

    *ppVideoSession = pNewVideoSession;

    return result;
}

NvVideoSession::~NvVideoSession()
{
    for (uint32_t i = 0; i < m_boundMemoryCount; i++) {
        m_devAlloc->freeMemory(m_boundMemory[i]);
        m_boundMemory[i] = nvvk::NullMemHandle;
    }

    m_devAlloc = NULL;

    if (m_videoSession) {
        assert(m_dev != VkDevice());
        vkDestroyVideoSessionKHR(m_dev, m_videoSession, NULL);
        m_videoSession = VkVideoSessionKHR();
        m_dev = VkDevice();
    }
}



