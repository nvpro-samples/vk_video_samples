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

#ifndef _VULKANBISTREAMBUFFERIMPL_H_
#define _VULKANBISTREAMBUFFERIMPL_H_

#include <atomic>
#include <iostream>
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VulkanDeviceMemoryImpl.h"
#include "vkvideo_parser/VulkanBitstreamBuffer.h"

class VulkanBitstreamBufferImpl : public VulkanBitstreamBuffer
{
public:

    static VkResult Create(const VulkanDeviceContext* vkDevCtx, uint32_t queueFamilyIndex,
             VkDeviceSize bufferSize, VkDeviceSize bufferOffsetAlignment, VkDeviceSize bufferSizeAlignment,
             const void* pInitializeBufferMemory, size_t initializeBufferMemorySize,
             VkSharedBaseObj<VulkanBitstreamBufferImpl>& vulkanBitstreamBuffer);

    virtual int32_t AddRef()
    {
        return ++m_refCount;
    }

    virtual int32_t Release()
    {
        uint32_t ret = --m_refCount;
        // Destroy the buffer if ref-count reaches zero
        if (ret == 0) {
            // std::cout << "Delete bitstream buffer " << this << " with size " << GetMaxSize() << std::endl;
            delete this;
        }
        return ret;
    }

    virtual int32_t GetRefCount()
    {
        assert(m_refCount > 0);
        return m_refCount;
    }

    virtual size_t GetMaxSize() const;
    virtual size_t GetOffsetAlignment() const;
    virtual size_t GetSizeAlignment() const;
    virtual size_t Resize(size_t newSize, size_t copySize = 0, size_t copyOffset = 0);

    virtual int64_t  MemsetData(uint32_t value, size_t offset, size_t size);
    virtual int64_t  CopyDataToBuffer(uint8_t *dstBuffer, size_t dstOffset,
                                      size_t srcOffset, size_t size) const;
    virtual int64_t  CopyDataToBuffer(VkSharedBaseObj<VulkanBitstreamBuffer>& dstBuffer, size_t dstOffset,
                                      size_t srcOffset, size_t size) const;
    virtual int64_t  CopyDataFromBuffer(const uint8_t *sourceBuffer, size_t srcOffset,
                                        size_t dstOffset, size_t size);
    virtual int64_t  CopyDataFromBuffer(const VkSharedBaseObj<VulkanBitstreamBuffer>& sourceBuffer, size_t srcOffset,
                                        size_t dstOffset, size_t size);
    virtual uint8_t* GetDataPtr(size_t offset, size_t &maxSize);
    virtual const uint8_t* GetReadOnlyDataPtr(size_t offset, size_t &maxSize) const;

    virtual void FlushRange(size_t offset, size_t size) const;
    virtual void InvalidateRange(size_t offset, size_t size) const;

    virtual VkBuffer GetBuffer() const { return m_buffer; }
    virtual VkDeviceMemory GetDeviceMemory() const { return *m_vulkanDeviceMemory; }

    virtual uint32_t  AddStreamMarker(uint32_t streamOffset);
    virtual uint32_t  SetStreamMarker(uint32_t streamOffset, uint32_t index);
    virtual uint32_t  GetStreamMarker(uint32_t index) const;
    virtual uint32_t  GetStreamMarkersCount() const;
    virtual const uint32_t* GetStreamMarkersPtr(uint32_t startIndex, uint32_t& maxCount) const;
    virtual uint32_t  ResetStreamMarkers();

    operator VkDeviceMemory() { return GetDeviceMemory(); }
    operator bool() { return m_buffer != VK_NULL_HANDLE; }

    VkResult CopyDataToBuffer(const uint8_t* pData, VkDeviceSize size,
                              VkDeviceSize &dstBufferOffset) const;

private:

    static VkResult CreateBuffer(const VulkanDeviceContext* vkDevCtx, uint32_t queueFamilyIndex,
                                 VkDeviceSize& bufferSize,
                                 VkDeviceSize bufferSizeAlignment,
                                 VkBuffer& buffer,
                                 VkDeviceSize& bufferOffset,
                                 VkMemoryPropertyFlags& memoryPropertyFlags,
                                 const void* pInitializeBufferMemory,
                                 size_t initializeBufferMemorySize,
                                 VkSharedBaseObj<VulkanDeviceMemoryImpl>& vulkanDeviceMemory);

    uint8_t* CheckAccess(size_t offset, size_t size) const;

    VkResult Initialize(VkDeviceSize bufferSize, const void* pInitializeBufferMemory, size_t initializeBufferMemorySize);

    VulkanBitstreamBufferImpl(const VulkanDeviceContext* vkDevCtx,
                              uint32_t m_queueFamilyIndex,
                              VkDeviceSize bufferOffsetAlignment,
                              VkDeviceSize bufferSizeAlignment)
        : VulkanBitstreamBuffer()
        , m_refCount(0)
        , m_vkDevCtx(vkDevCtx)
        , m_queueFamilyIndex(m_queueFamilyIndex)
        , m_memoryPropertyFlags()
        , m_buffer()
        , m_bufferOffset()
        , m_bufferSize()
        , m_bufferOffsetAlignment(bufferOffsetAlignment)
        , m_bufferSizeAlignment(bufferSizeAlignment)
        , m_vulkanDeviceMemory()
        , m_streamMarkers(256) { }

    void Deinitialize();

    virtual ~VulkanBitstreamBufferImpl() { Deinitialize(); }

private:
    std::atomic<int32_t>       m_refCount;
    const VulkanDeviceContext* m_vkDevCtx;
    uint32_t                   m_queueFamilyIndex;
    VkMemoryPropertyFlags      m_memoryPropertyFlags;
    VkBuffer                   m_buffer;
    VkDeviceSize               m_bufferOffset;
    VkDeviceSize               m_bufferSize;
    VkDeviceSize               m_bufferOffsetAlignment;
    VkDeviceSize               m_bufferSizeAlignment;
    VkSharedBaseObj<VulkanDeviceMemoryImpl> m_vulkanDeviceMemory;
    std::vector<uint32_t>      m_streamMarkers;
};

#endif /* _VULKANBISTREAMBUFFERIMPL_H_ */
