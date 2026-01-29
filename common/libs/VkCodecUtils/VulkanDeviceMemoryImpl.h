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
#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkCodecUtils/VulkanDeviceContext.h"

class VulkanDeviceMemoryImpl : public VkVideoRefCountBase
{
public:

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           const VkMemoryRequirements& memoryRequirements,
                           VkMemoryPropertyFlags& memoryPropertyFlags,
                           const void* pInitializeMemory, VkDeviceSize initializeMemorySize, bool clearMemory,
                           VkSharedBaseObj<VulkanDeviceMemoryImpl>& vulkanDeviceMemory);

    /**
     * @brief Create device memory with optional external memory export support
     *
     * This overload accepts a pNext chain that can include VkExportMemoryAllocateInfo
     * for cross-process sharing via DMA-BUF (Linux) or NT handles (Windows).
     *
     * @param vkDevCtx              Vulkan device context
     * @param memoryRequirements    Memory requirements from vkGetImageMemoryRequirements
     * @param memoryPropertyFlags   Required memory property flags
     * @param pAllocateInfoPNext    pNext chain to append to VkMemoryAllocateInfo (e.g., VkExportMemoryAllocateInfo)
     * @param pInitializeMemory     Optional data to initialize memory with
     * @param initializeMemorySize  Size of initialization data
     * @param clearMemory           Whether to clear memory after initialization
     * @param vulkanDeviceMemory    Output device memory object
     * @return VK_SUCCESS on success
     */
    static VkResult CreateWithExport(const VulkanDeviceContext* vkDevCtx,
                                     const VkMemoryRequirements& memoryRequirements,
                                     VkMemoryPropertyFlags& memoryPropertyFlags,
                                     const void* pAllocateInfoPNext,
                                     const void* pInitializeMemory, VkDeviceSize initializeMemorySize, bool clearMemory,
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

    virtual VkDeviceSize GetMaxSize() const;
    virtual VkDeviceSize GetSizeAlignment() const;
    virtual VkDeviceSize Resize(VkDeviceSize newSize, VkDeviceSize copySize = 0, VkDeviceSize copyOffset = 0);

    virtual int64_t  MemsetData(uint32_t value, VkDeviceSize offset, VkDeviceSize size);
    virtual int64_t  CopyDataToBuffer(uint8_t *dstBuffer, VkDeviceSize dstOffset,
                                      VkDeviceSize srcOffset, VkDeviceSize size);
    virtual int64_t  CopyDataToBuffer(VkSharedBaseObj<VulkanDeviceMemoryImpl>& dstBuffer, VkDeviceSize dstOffset,
                                      VkDeviceSize srcOffset, VkDeviceSize size);
    virtual int64_t  CopyDataFromBuffer(const uint8_t *sourceBuffer, VkDeviceSize srcOffset,
                                        VkDeviceSize dstOffset, VkDeviceSize size);
    virtual int64_t  CopyDataFromBuffer(const VkSharedBaseObj<VulkanDeviceMemoryImpl>& sourceMemory, VkDeviceSize srcOffset,
                                        VkDeviceSize dstOffset, VkDeviceSize size);
    virtual uint8_t* GetDataPtr(VkDeviceSize offset, VkDeviceSize &maxSize);
    virtual const uint8_t* GetReadOnlyDataPtr(VkDeviceSize offset, VkDeviceSize &maxSize);

    virtual void FlushRange(VkDeviceSize offset, VkDeviceSize size) const;
    virtual void InvalidateRange(VkDeviceSize offset, VkDeviceSize size) const;

    virtual VkDeviceMemory GetDeviceMemory() const { return m_deviceMemory; }
    operator VkDeviceMemory() { return m_deviceMemory; }
    operator bool() { return m_deviceMemory != VK_NULL_HANDLE; }

    VkMemoryPropertyFlags GetMemoryPropertyFlags() const { return m_memoryPropertyFlags; }

    const VkMemoryRequirements& GetMemoryRequirements() const { return m_memoryRequirements; }

    /**
     * @brief Get the memory type index used for this allocation
     */
    uint32_t GetMemoryTypeIndex() const { return m_memoryTypeIndex; }

    /**
     * @brief Check if this memory was created with export capabilities
     */
    bool IsExportable() const { return m_exportHandleTypes != 0; }

    /**
     * @brief Get the external memory handle types this memory was created with
     */
    VkExternalMemoryHandleTypeFlags GetExportHandleTypes() const { return m_exportHandleTypes; }

    /**
     * @brief Export native handle (DMA-BUF FD on Linux, NT handle on Windows)
     *
     * The memory must have been created with CreateWithExport() and appropriate handle types.
     * The caller is responsible for closing the returned handle when done.
     *
     * @param handleType The handle type to export (e.g., VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
     * @param outHandle  Output native handle
     * @return VK_SUCCESS on success, VK_ERROR_FEATURE_NOT_PRESENT if not exportable
     */
#ifdef _WIN32
    VkResult ExportNativeHandle(VkExternalMemoryHandleTypeFlagBits handleType, void** outHandle) const;
#else
    VkResult ExportNativeHandle(VkExternalMemoryHandleTypeFlagBits handleType, int* outHandle) const;
#endif

    VkResult FlushInvalidateMappedMemoryRange(VkDeviceSize offset, VkDeviceSize size, bool flush = true)  const;

    VkResult CopyDataToMemory(const uint8_t* pData, VkDeviceSize size,
                              VkDeviceSize memoryOffset) const;

    uint8_t* CheckAccess(VkDeviceSize offset, VkDeviceSize size);

private:

    static VkResult CreateDeviceMemory(const VulkanDeviceContext* vkDevCtx,
                                       const VkMemoryRequirements& memoryRequirements,
                                       VkMemoryPropertyFlags& memoryPropertyFlags,
                                       const void* pAllocateInfoPNext,
                                       VkDeviceMemory& deviceMemory,
                                       VkDeviceSize&   deviceMemoryOffset,
                                       VkExternalMemoryHandleTypeFlags& outExportHandleTypes,
                                       uint32_t& outMemoryTypeIndex);


    VkResult Initialize(const VkMemoryRequirements& memoryRequirements,
                        VkMemoryPropertyFlags& memoryPropertyFlags,
                        const void* pAllocateInfoPNext,
                        const void* pInitializeMemory,
                        VkDeviceSize initializeMemorySize,
                        bool clearMemory);

    VulkanDeviceMemoryImpl(const VulkanDeviceContext* vkDevCtx)
        : m_refCount(0)
        , m_vkDevCtx(vkDevCtx)
        , m_memoryRequirements()
        , m_memoryPropertyFlags()
        , m_exportHandleTypes(0)
        , m_memoryTypeIndex(0)
        , m_deviceMemory()
        , m_deviceMemoryOffset()
        , m_deviceMemoryDataPtr(nullptr) { }

    void Deinitialize();

    virtual ~VulkanDeviceMemoryImpl();

private:
    std::atomic<int32_t>            m_refCount;
    const VulkanDeviceContext*      m_vkDevCtx;
    VkMemoryRequirements            m_memoryRequirements;
    VkMemoryPropertyFlags           m_memoryPropertyFlags;
    VkExternalMemoryHandleTypeFlags m_exportHandleTypes;   // Handle types this memory was created with
    uint32_t                        m_memoryTypeIndex;     // Memory type index used for allocation
    VkDeviceMemory                  m_deviceMemory;
    VkDeviceSize                    m_deviceMemoryOffset;
    uint8_t*                        m_deviceMemoryDataPtr;
};

#endif /* _VULKANDEVICEMEMORYIMPL_H_ */
