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

#ifndef _VKBUFFERRESOURCE_H_
#define _VKBUFFERRESOURCE_H_

#include <vector>
#include <atomic>
#include <iostream>
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VulkanDeviceMemoryImpl.h"

/**
 * @brief Vulkan buffer resource with automatic memory management
 *
 * Manages Vulkan buffers with device memory, supporting host-visible and device-local memory.
 * Provides reference counting, automatic cleanup, and convenient data access methods.
 */
class VkBufferResource : public VkVideoRefCountBase
{
public:

    /**
     * @brief Create a Vulkan buffer with device memory
     *
     * Parameters:
     * - vkDevCtx                    - Vulkan device context
     * - usage                       - Buffer usage flags (VK_BUFFER_USAGE_*)
     * - memoryPropertyFlags         - Memory property flags (VK_MEMORY_PROPERTY_*)
     * - bufferSize                  - Buffer size in BYTES
     * - vkBufferResource            - Output buffer (VkSharedBaseObj reference)
     * - bufferOffsetAlignment       - [Optional] Offset alignment (default: 1)
     * - bufferSizeAlignment         - [Optional] Size alignment (default: 1)
     * - initializeBufferMemorySize  - [Optional] Size of initial data
     * - pInitializeBufferMemory     - [Optional] Initial data to copy
     * - queueFamilyCount           - [Optional] Number of queue families
     * - queueFamilyIndexes         - [Optional] Queue family indices
     *
     * Example usage:
     *   Create(ctx, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
     *          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
     *          bufferSize, buffer);
     *
     * Common usage patterns:
     * - Staging buffer (host→device):
     *     VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
     * - Readback buffer (device→host):
     *     VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
     * - Device-only buffer:
     *     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
     *
     * @return VK_SUCCESS on success, Vulkan error code otherwise
     */
    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryPropertyFlags,
                           VkDeviceSize bufferSize,
                           VkSharedBaseObj<VkBufferResource>& vkBufferResource,
                           VkDeviceSize bufferOffsetAlignment = 1, VkDeviceSize bufferSizeAlignment = 1,
                           VkDeviceSize initializeBufferMemorySize = 0, const void* pInitializeBufferMemory = nullptr,
                           uint32_t queueFamilyCount = 0, uint32_t* queueFamilyIndexes = nullptr);

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

    /** @brief Get maximum buffer size in bytes */
    virtual VkDeviceSize GetMaxSize() const;

    /** @brief Get buffer offset alignment requirement */
    virtual VkDeviceSize GetOffsetAlignment() const;

    /** @brief Get buffer size alignment requirement */
    virtual VkDeviceSize GetSizeAlignment() const;
    virtual VkDeviceSize Resize(VkDeviceSize newSize, VkDeviceSize copySize = 0, VkDeviceSize copyOffset = 0);
    virtual VkDeviceSize Clone(VkDeviceSize newSize, VkDeviceSize copySize, VkDeviceSize copyOffset,
                               VkSharedBaseObj<VkBufferResource>& vulkanBitstreamBuffer);

    /** @brief Fill buffer region with value (CPU memset) */
    virtual int64_t  MemsetData(uint32_t value, VkDeviceSize offset, VkDeviceSize size);

    /** @brief Copy data from this buffer to CPU buffer */
    virtual int64_t  CopyDataToBuffer(uint8_t *dstBuffer, VkDeviceSize dstOffset,
                                      VkDeviceSize srcOffset, VkDeviceSize size) const;
    virtual int64_t  CopyDataToBuffer(VkSharedBaseObj<VkBufferResource>& dstBuffer, VkDeviceSize dstOffset,
                                      VkDeviceSize srcOffset, VkDeviceSize size) const;

    /** @brief Copy data from CPU buffer to this buffer */
    virtual int64_t  CopyDataFromBuffer(const uint8_t *sourceBuffer, VkDeviceSize srcOffset,
                                        VkDeviceSize dstOffset, VkDeviceSize size);

    /** @brief Copy data from another VkBufferResource to this buffer */
    virtual int64_t  CopyDataFromBuffer(const VkSharedBaseObj<VkBufferResource>& sourceBuffer, VkDeviceSize srcOffset,
                                        VkDeviceSize dstOffset, VkDeviceSize size);

    /**
     * @brief Get writable pointer to buffer data (host-visible memory only)
     * @param offset Offset in bytes
     * @param maxSize Output: remaining bytes from offset
     * @return Pointer to data, or nullptr if not host-visible
     */
    virtual uint8_t* GetDataPtr(VkDeviceSize offset, VkDeviceSize &maxSize);

    /**
     * @brief Get read-only pointer to buffer data (host-visible memory only)
     * @param offset Offset in bytes
     * @param maxSize Output: remaining bytes from offset
     * @return Const pointer to data, or nullptr if not host-visible
     */
    virtual const uint8_t* GetReadOnlyDataPtr(VkDeviceSize offset, VkDeviceSize &maxSize) const;

    /** @brief Flush CPU writes to GPU (for non-coherent memory) */
    virtual void FlushRange(VkDeviceSize offset, VkDeviceSize size) const;

    /** @brief Invalidate GPU writes to CPU (for non-coherent memory) */
    virtual void InvalidateRange(VkDeviceSize offset, VkDeviceSize size) const;

    /** @brief Get underlying VkBuffer handle */
    virtual VkBuffer GetBuffer() const { return m_buffer; }

    /** @brief Get underlying VkDeviceMemory handle */
    virtual VkDeviceMemory GetDeviceMemory() const { return *m_vulkanDeviceMemory; }

    /** @brief Implicit conversion to VkDeviceMemory */
    operator VkDeviceMemory() { return GetDeviceMemory(); }

    /** @brief Check if buffer is valid */
    operator bool() { return m_buffer != VK_NULL_HANDLE; }

    /** @brief Copy data to buffer and return offset */
    VkResult CopyDataToBuffer(const uint8_t* pData, VkDeviceSize size,
                              VkDeviceSize &dstBufferOffset) const;

private:

    static VkResult CreateBuffer(const VulkanDeviceContext* vkDevCtx,
                                 VkBufferUsageFlags usage,
                                 VkDeviceSize& bufferSize,
                                 VkDeviceSize bufferSizeAlignment,
                                 VkBuffer& buffer,
                                 VkDeviceSize& bufferOffset,
                                 VkMemoryPropertyFlags& memoryPropertyFlags,
                                 VkDeviceSize initializeBufferMemorySize,
                                 const void* pInitializeBufferMemory,
                                 const std::vector<uint32_t>& queueFamilyIndexes,
                                 VkSharedBaseObj<VulkanDeviceMemoryImpl>& vulkanDeviceMemory);

    uint8_t* CheckAccess(VkDeviceSize offset, VkDeviceSize size) const;

    VkResult Initialize(VkDeviceSize bufferSize,
                        const void* pInitializeBufferMemory, VkDeviceSize initializeBufferMemorySize);

    VkBufferResource(const VulkanDeviceContext* vkDevCtx,
                     VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags memoryPropertyFlags,
                     VkDeviceSize bufferOffsetAlignment = 1,
                     VkDeviceSize bufferSizeAlignment = 1,
                     uint32_t queueFamilyCount = 0,
                     uint32_t* queueFamilyIndexes = nullptr)
        : m_refCount(0)
        , m_vkDevCtx(vkDevCtx)
        , m_usage(usage)
        , m_memoryPropertyFlags(memoryPropertyFlags)
        , m_buffer()
        , m_bufferOffset()
        , m_bufferSize()
        , m_bufferOffsetAlignment(bufferOffsetAlignment)
        , m_bufferSizeAlignment(bufferSizeAlignment)
        , m_queueFamilyIndexes(queueFamilyIndexes, queueFamilyIndexes + queueFamilyCount)
        , m_vulkanDeviceMemory() { }

    void Deinitialize();

    virtual ~VkBufferResource() { Deinitialize(); }

private:
    std::atomic<int32_t>       m_refCount;
    const VulkanDeviceContext* m_vkDevCtx;
    VkBufferUsageFlags         m_usage;
    VkMemoryPropertyFlags      m_memoryPropertyFlags;
    VkBuffer                   m_buffer;
    VkDeviceSize               m_bufferOffset;
    VkDeviceSize               m_bufferSize;
    VkDeviceSize               m_bufferOffsetAlignment;
    VkDeviceSize               m_bufferSizeAlignment;
    std::vector<uint32_t>      m_queueFamilyIndexes;
    VkSharedBaseObj<VulkanDeviceMemoryImpl> m_vulkanDeviceMemory;
};

#endif /* _VKBUFFERRESOURCE_H_ */
