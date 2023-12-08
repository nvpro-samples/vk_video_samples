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
#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkCodecUtils/VulkanDeviceContext.h"

class VulkanVideoSession : public VkVideoRefCountBase
{
    enum { MAX_BOUND_MEMORY = 8 };
public:
    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkVideoSessionCreateFlagsKHR sessionCreateFlags,
                           uint32_t            videoQueueFamily,
                           VkVideoCoreProfile* pVideoProfile,
                           VkFormat            pictureFormat,
                           const VkExtent2D&   maxCodedExtent,
                           VkFormat            referencePicturesFormat,
                           uint32_t            maxDpbSlots,
                           uint32_t            maxActiveReferencePictures,
                           VkSharedBaseObj<VulkanVideoSession>& videoSession);

    bool IsCompatible ( const VulkanDeviceContext* vkDevCtx,
                        VkVideoSessionCreateFlagsKHR sessionCreateFlags,
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

        if (sessionCreateFlags != m_flags) {
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

        if (m_vkDevCtx->getDevice() != vkDevCtx->getDevice()) {
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

    operator VkVideoSessionKHR() const {
        assert(m_videoSession != VK_NULL_HANDLE);
        return m_videoSession;
    }

private:

    VulkanVideoSession(const VulkanDeviceContext* vkDevCtx,
                   VkVideoCoreProfile* pVideoProfile)
       : m_refCount(0), m_flags(), m_profile(*pVideoProfile), m_vkDevCtx(vkDevCtx),
         m_createInfo{ VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR, NULL },
         m_videoSession(VkVideoSessionKHR()), m_memoryBound{}
    {

    }

    ~VulkanVideoSession()
    {
        if (m_videoSession) {
            assert(m_vkDevCtx != nullptr);
            m_vkDevCtx->DestroyVideoSessionKHR(*m_vkDevCtx, m_videoSession, NULL);
            m_videoSession = VkVideoSessionKHR();
        }

        for (uint32_t memIdx = 0; memIdx < MAX_BOUND_MEMORY; memIdx++) {
            if (m_memoryBound[memIdx] != VK_NULL_HANDLE) {
                m_vkDevCtx->FreeMemory(*m_vkDevCtx, m_memoryBound[memIdx], 0);
                m_memoryBound[memIdx] = VK_NULL_HANDLE;
            }
        }
        m_vkDevCtx = nullptr;
    }

private:
    std::atomic<int32_t>                   m_refCount;
    VkVideoSessionCreateFlagsKHR           m_flags;
    VkVideoCoreProfile                     m_profile;
    const VulkanDeviceContext*             m_vkDevCtx;
    VkVideoSessionCreateInfoKHR            m_createInfo;
    VkVideoSessionKHR                      m_videoSession;
    VkDeviceMemory                         m_memoryBound[MAX_BOUND_MEMORY];
};
