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

#pragma once

#include <atomic>
#include "VkParserVideoRefCountBase.h"

class NvVideoSession : public VkParserVideoRefCountBase
{
public:
    static VkResult Create(VkDevice            dev,
                           uint32_t            videoQueueFamily,
                           VkVideoCoreProfile* pVideoProfile,
                           VkFormat            pictureFormat,
                           const VkExtent2D&   maxCodedExtent,
                           VkFormat            referencePicturesFormat,
                           uint32_t            maxDpbSlots,
                           uint32_t            maxActiveReferencePictures,
                           VkSharedBaseObj<NvVideoSession>& videoSession);

    bool IsCompatible ( VkDevice            dev,
                        uint32_t            videoQueueFamily,
                        VkVideoCoreProfile* pVideoProfile,
                        VkFormat            pictureFormat,
                        const VkExtent2D&   maxCodedExtent,
                        VkFormat            referencePicturesFormat,
                        uint32_t            maxDpbSlots,
                        uint32_t            maxActiveReferencePictures)
    {
        if (*pVideoProfile != m_profile) {
            return false;
        }

        if (maxCodedExtent.width > m_createInfo.maxCodedExtent.width) {
            return false;
        }

        if (maxCodedExtent.height > m_createInfo.maxCodedExtent.height) {
            return false;
        }

        if (maxDpbSlots > m_createInfo.maxDpbSlots) {
            return false;
        }

        if (maxActiveReferencePictures > m_createInfo.maxActiveReferencePictures) {
            return false;
        }

        if (m_createInfo.referencePictureFormat != referencePicturesFormat) {
            return false;
        }

        if (m_createInfo.pictureFormat != pictureFormat) {
            return false;
        }

        if (m_dev != dev) {
            return false;
        }

        if (m_createInfo.queueFamilyIndex != videoQueueFamily) {
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
        // Destroy the device if refcount reaches zero
        if (ret == 0) {
            delete this;
        }
        return ret;
    }

    VkVideoSessionKHR GetVideoSession() const { return m_videoSession; }

private:
    std::atomic<int32_t>                 m_refCount;
    VkVideoCoreProfile                   m_profile;
    VkDevice                             m_dev;
    VkVideoSessionCreateInfoKHR          m_createInfo;
    VkVideoSessionKHR                    m_videoSession;
    vulkanVideoUtils::DeviceMemoryObject m_memoryBound[8];

    NvVideoSession(VkVideoCoreProfile* pVideoProfile)
       : m_refCount(0), m_profile(*pVideoProfile), m_dev(VkDevice()),
         m_createInfo{ VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR, NULL },
         m_videoSession(VkVideoSessionKHR()), m_memoryBound{}
    {

    }

    ~NvVideoSession()
    {
        if (m_videoSession) {
            assert(m_dev != VkDevice());
            vk::DestroyVideoSessionKHR(m_dev, m_videoSession, NULL);
            m_videoSession = VkVideoSessionKHR();
            m_dev = VkDevice();
        }
    }
};
