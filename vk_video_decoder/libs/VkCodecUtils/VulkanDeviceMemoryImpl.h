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

#ifndef _VULKANDEVICEMEMORYIMPL_H_
#define _VULKANDEVICEMEMORYIMPL_H_

#include <atomic>
#include "VkVideoCore/VkVideoRefCountBase.h"
#include "VkCodecUtils/VulkanDeviceContext.h"

class VulkanDeviceMemoryImpl : public VkVideoRefCountBase
{
public:

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           const VkMemoryRequirements& memoryRequirements,
                           VkMemoryPropertyFlags& memoryPropertyFlags,
                           const void* pInitializeMemory, size_t initializeMemorySize, bool clearMemory,
                           VkSharedBaseObj<VulkanDeviceMemoryImpl>& vulkanDeviceMemory);

    virtual int32_t AddRef()
    {
        return ++m_refCount;
    }

    virtual int32_t Release()
    {
        uint32_t ret = --m_refCount;
        // Destroy the buffer if ref-count reaches zero
        if (ret == 0) {
            delete this;
        }
        return ret;
    }

    virtual size_t GetMaxSize() const;
    virtual size_t GetSizeAlignment() const;
    virtual size_t Resize(size_t newSize, size_t copySize = 0, size_t copyOffset = 0);

    virtual int64_t  MemsetData(uint32_t value, size_t offset, size_t size);
    virtual int64_t  CopyDataToBuffer(uint8_t *dstBuffer, size_t dstOffset,
                                      size_t srcOffset, size_t size) const;
    virtual int64_t  CopyDataToBuffer(VkSharedBaseObj<VulkanDeviceMemoryImpl>& dstBuffer, size_t dstOffset,
                                      size_t srcOffset, size_t size) const;
    virtual int64_t  CopyDataFromBuffer(const uint8_t *sourceBuffer, size_t srcOffset,
                                        size_t dstOffset, size_t size);
    virtual int64_t  CopyDataFromBuffer(const VkSharedBaseObj<VulkanDeviceMemoryImpl>& sourceMemory, size_t srcOffset,
                                        size_t dstOffset, size_t size);
    virtual uint8_t* GetDataPtr(size_t offset, size_t &maxSize);
    virtual const uint8_t* GetReadOnlyDataPtr(size_t offset, size_t &maxSize) const;

    virtual void FlushRange(size_t offset, size_t size) const;
    virtual void InvalidateRange(size_t offset, size_t size) const;

    virtual VkDeviceMemory GetDeviceMemory() const { return m_deviceMemory; }
    operator VkDeviceMemory() { return m_deviceMemory; }
    operator bool() { return m_deviceMemory != VK_NULL_HANDLE; }

    const VkMemoryRequirements& GetMemoryRequirements() const { return m_memoryRequirements; }

    VkResult FlushInvalidateMappedMemoryRange(VkDeviceSize offset, VkDeviceSize size, bool flush = true)  const;

    VkResult CopyDataToMemory(const uint8_t* pData, VkDeviceSize size,
                              VkDeviceSize memoryOffset) const;

    uint8_t* CheckAccess(size_t offset, size_t size) const;

private:

    static VkResult CreateDeviceMemory(const VulkanDeviceContext* vkDevCtx,
                                       const VkMemoryRequirements& memoryRequirements,
                                       VkMemoryPropertyFlags& memoryPropertyFlags,
                                       VkDeviceMemory& deviceMemory,
                                       VkDeviceSize&   deviceMemoryOffset);


    VkResult Initialize(const VkMemoryRequirements& memoryRequirements,
                        VkMemoryPropertyFlags& memoryPropertyFlags,
                        const void* pInitializeMemory,
                        size_t initializeMemorySize,
                        bool clearMemory);

    VulkanDeviceMemoryImpl(const VulkanDeviceContext* vkDevCtx)
        : m_refCount(0)
        , m_vkDevCtx(vkDevCtx)
        , m_memoryRequirements()
        , m_memoryPropertyFlags()
        , m_deviceMemory()
        , m_deviceMemoryOffset()
        , m_deviceMemoryDataPtr(nullptr) { }

    void Deinitialize();

    virtual ~VulkanDeviceMemoryImpl() { Deinitialize(); }

private:
    std::atomic<int32_t>       m_refCount;
    const VulkanDeviceContext* m_vkDevCtx;
    VkMemoryRequirements       m_memoryRequirements;
    VkMemoryPropertyFlags      m_memoryPropertyFlags;
    VkDeviceMemory             m_deviceMemory;
    VkDeviceSize               m_deviceMemoryOffset;
    uint8_t*                   m_deviceMemoryDataPtr;
};

#endif /* _VULKANDEVICEMEMORYIMPL_H_ */
