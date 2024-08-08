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
#include "nvvk/resourceallocator_vk.hpp"
#include "nvvk/memorymanagement_vk.hpp"
#include "nvvk/memallocator_vk.hpp"
#include "nvvk/memallocator_dedicated_vk.hpp"
#include "vulkan/vulkan.hpp"
#include "nvvk/context_vk.hpp"

class NvVideoSession {
public:
    static VkResult create(nvvk::MemAllocator*  devAlloc,
                           nvvk::Context*       vkctx,
                           uint32_t             videoQueueFamily,
                           VkVideoCoreProfile*  pVideoProfile,
                           VkFormat             pictureFormat,
                           const VkExtent2D&    maxCodedExtent,
                           VkFormat             referencePicturesFormat,
                           uint32_t             maxReferencePicturesSlotsCount,
                           uint32_t             maxReferencePicturesActiveCount,
                           NvVideoSession**     ppVideoSession);

    VkVideoSessionKHR getVideoSession() const
    {
        return m_videoSession;
    }

    ~NvVideoSession();

private:
    VkVideoCoreProfile                   m_profile;
    VkDevice                             m_dev;
    VkVideoSessionKHR                    m_videoSession;
    nvvk::MemAllocator*                  m_devAlloc;
    nvvk::MemHandle                      m_boundMemory[8];
    uint32_t                             m_boundMemoryCount;

    NvVideoSession(VkVideoCoreProfile* pVideoProfile)
        : m_profile(*pVideoProfile), m_dev(VkDevice()),
          m_videoSession(VkVideoSessionKHR()),
          m_devAlloc(), m_boundMemory{}, m_boundMemoryCount(0)
    {

    }

};
