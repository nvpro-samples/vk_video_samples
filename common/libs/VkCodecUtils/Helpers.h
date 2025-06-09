/*
 * Copyright (C) 2016 Google, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef HELPERS_H
#define HELPERS_H

#include <vector>
#include <sstream>
#include <stdexcept>
#include <assert.h>
#include <iostream>
#include <cstring>
#include <iomanip>
#include "HelpersDispatchTable.h"

namespace vk {

struct Vertex {
    float position[2];
    float texCoord[2];
};

struct Vec2 {
    Vec2(float val0, float val1)
        : val{val0, val1} {}
    float val[2];
};

struct Vec4 {
    Vec4(float val0, float val1, float val2, float val3)
        : val{val0, val1, val2, val3} {}
    float val[4];
};

struct TransformPushConstants {
    TransformPushConstants()
        : posMatrix {{1.0f, 0.0f, 0.0f, 0.0f},
                     {0.0f, 1.0f, 0.0f, 0.0f},
                     {0.0f, 0.0f, 1.0f, 0.0f},
                     {0.0f, 0.0f, 0.0f, 1.0f}}
          , texMatrix {{1.0f, 0.0f},
                       {0.0f, 1.0f}}
    {
    }
    Vec4 posMatrix[4];
    Vec2 texMatrix[2];
};

template <class valueType, class alignmentType>
valueType alignedSize(valueType value, alignmentType alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

#if defined(VK_USE_PLATFORM_XCB_KHR) || defined (VK_USE_PLATFORM_XLIB_KHR) || defined(VK_USE_PLATFORM_WAYLAND_KHR)
#define VK_PLATFORM_IS_UNIX 1
#endif

class NativeHandle {
public:
    static NativeHandle InvalidNativeHandle;

    NativeHandle(void);
    NativeHandle(const NativeHandle& other);
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    NativeHandle(int fd);
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    NativeHandle (AHardwareBufferHandle buffer);
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
    ~NativeHandle (void);

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    NativeHandle& operator= (int fd);
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    NativeHandle& operator= (AHardwareBufferHandle buffer);
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    int getFd(void) const;
    operator int() const { return getFd(); }
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    AHardwareBufferHandle getAndroidHardwareBuffer(void) const;
    operator AHardwareBufferHandle() const { return getAndroidHardwareBuffer(); }
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
    VkExternalMemoryHandleTypeFlagBits getExternalMemoryHandleType (void) const
    {
        return m_externalMemoryHandleType;
    }
    void disown(void);
    bool isValid(void) const;
    operator bool() const { return isValid(); }
    // This should only be called on an import error or on handle replacement.
    void releaseReference(void);

private:
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    int                                 m_fd;
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    AHardwareBufferHandle               m_androidHardwareBuffer;
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
    VkExternalMemoryHandleTypeFlagBits  m_externalMemoryHandleType;

    // Disabled
    NativeHandle& operator= (const NativeHandle&) = delete;
};


inline VkResult enumerate(const VkInterfaceFunctions* vkIf, const char *layer, std::vector<VkExtensionProperties> &exts) {
    uint32_t count = 0;
    vkIf->EnumerateInstanceExtensionProperties(layer, &count, nullptr);

    exts.resize(count);
    return vkIf->EnumerateInstanceExtensionProperties(layer, &count, exts.data());
}

inline VkResult enumerate(const VkInterfaceFunctions* vkIf, VkPhysicalDevice phy, const char *layer, std::vector<VkExtensionProperties> &exts) {
    uint32_t count = 0;
    vkIf->EnumerateDeviceExtensionProperties(phy, layer, &count, nullptr);

    exts.resize(count);
    return vkIf->EnumerateDeviceExtensionProperties(phy, layer, &count, exts.data());
}

inline VkResult enumerate(const VkInterfaceFunctions* vkIf, VkInstance instance, std::vector<VkPhysicalDevice> &phys) {
    uint32_t count = 0;
    vkIf->EnumeratePhysicalDevices(instance, &count, nullptr);

    phys.resize(count);
    return vkIf->EnumeratePhysicalDevices(instance, &count, phys.data());
}

inline VkResult enumerate(const VkInterfaceFunctions* vkIf, std::vector<VkLayerProperties> &layer_props) {
    uint32_t count = 0;
    vkIf->EnumerateInstanceLayerProperties(&count, nullptr);

    layer_props.resize(count);
    return vkIf->EnumerateInstanceLayerProperties(&count, layer_props.data());
}

inline VkResult get(const VkInterfaceFunctions* vkIf,
                    VkPhysicalDevice phy, std::vector<VkQueueFamilyProperties2> &queues,
                    std::vector<VkQueueFamilyVideoPropertiesKHR> &videoQueues,
                    std::vector<VkQueueFamilyQueryResultStatusPropertiesKHR> &queryResultStatus) {
    uint32_t count = 0;
    vkIf->GetPhysicalDeviceQueueFamilyProperties2(phy, &count, nullptr);

    queues.resize(count);
    videoQueues.resize(count);
    queryResultStatus.resize(count);
    for (uint32_t i = 0; i < queues.size(); i++) {
        queues[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        videoQueues[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        queues[i].pNext = &videoQueues[i];
        queryResultStatus[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR;
        videoQueues[i].pNext = &queryResultStatus[i];
    }

    vkIf->GetPhysicalDeviceQueueFamilyProperties2(phy, &count, queues.data());

    return VK_SUCCESS;
}

inline VkResult get(const VkInterfaceFunctions* vkIf,
                    VkPhysicalDevice phy, VkSurfaceKHR surface, std::vector<VkSurfaceFormatKHR> &formats) {
    uint32_t count = 0;
    vkIf->GetPhysicalDeviceSurfaceFormatsKHR(phy, surface, &count, nullptr);

    formats.resize(count);
    return vkIf->GetPhysicalDeviceSurfaceFormatsKHR(phy, surface, &count, formats.data());
}

inline VkResult get(const VkInterfaceFunctions* vkIf,
                    VkPhysicalDevice phy, VkSurfaceKHR surface, std::vector<VkPresentModeKHR> &modes) {
    uint32_t count = 0;
    vkIf->GetPhysicalDeviceSurfacePresentModesKHR(phy, surface, &count, nullptr);

    modes.resize(count);
    return vkIf->GetPhysicalDeviceSurfacePresentModesKHR(phy, surface, &count, modes.data());
}

inline VkResult get(const VkInterfaceFunctions* vkIf,
                    VkDevice dev, VkSwapchainKHR swapchain, std::vector<VkImage> &images) {
    uint32_t count = 0;
    vkIf->GetSwapchainImagesKHR(dev, swapchain, &count, nullptr);

    images.resize(count);
    return vkIf->GetSwapchainImagesKHR(dev, swapchain, &count, images.data());
}

inline VkResult MapMemoryTypeToIndex(const VkInterfaceFunctions* vkIf,
                                     VkPhysicalDevice vkPhysicalDev,
                                     uint32_t typeBits,
                                     VkFlags requirements_mask, uint32_t *typeIndex)
{
    VkPhysicalDeviceMemoryProperties memoryProperties;
    vkIf->GetPhysicalDeviceMemoryProperties(vkPhysicalDev, &memoryProperties);
    // Search memtypes to find first index with those properties
    for (uint32_t i = 0; i < 32; i++) {
        if ((typeBits & 1) == 1) {
            // Type is available, does it match user properties?
            if ((memoryProperties.memoryTypes[i].propertyFlags & requirements_mask) ==
                    requirements_mask) {
                *typeIndex = i;
                return VK_SUCCESS;
            }
        }
        typeBits >>= 1;
    }
    return VK_ERROR_VALIDATION_FAILED_EXT;
}

inline VkResult WaitAndResetFence(const VkInterfaceFunctions* vkIf, VkDevice device, VkFence fence,
                                  bool resetAfterWait = true, const char* fenceName = "unknown",
                                  const uint64_t fenceWaitTimeout = 100ULL * 1000ULL * 1000ULL /* 100 mSec */,
                                  const uint64_t fenceTotalWaitTimeout = 5ULL * 1000ULL * 1000ULL * 1000ULL /* 5 sec */) {

    assert(vkIf != nullptr);
    assert(device != VK_NULL_HANDLE);
    assert(fence != VK_NULL_HANDLE);

    uint64_t fenceCurrentWaitTimeout = 0;

    VkResult result = VK_SUCCESS;

    while (fenceTotalWaitTimeout >= fenceCurrentWaitTimeout) {

        fenceCurrentWaitTimeout += fenceWaitTimeout;

        result = vkIf->WaitForFences(device, 1, &fence, true, fenceWaitTimeout);
        if (result == VK_TIMEOUT) {
            fprintf(stderr, "\t **** WARNING: fence  %s(%llu) is not done after %llu mSec with result 0x%x ****\n",
                            fenceName, (long long unsigned int)fence, (long long unsigned int)fenceCurrentWaitTimeout/(1000ULL * 1000ULL), result);
        } else {
            break; // either success or an error occured
        }

    }

    if (result != VK_SUCCESS) {
        fprintf(stderr, "\t **** ERROR: fence  %s(%llu) is not done after %llu mSec with result 0x%x ****\n",
                        fenceName, (long long unsigned int)fence, (long long unsigned int)fenceTotalWaitTimeout/(1000ULL * 1000ULL), vkIf->GetFenceStatus(device, fence));
        assert(!"Fence is not signaled yet after more than 100 mSec wait");
    }

    if (resetAfterWait) {
        result = vkIf->ResetFences(device, 1, &fence);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: ResetFences() result: 0x%x\n", result);
            assert(result == VK_SUCCESS);
        }

        assert(vkIf->GetFenceStatus(device, fence) == VK_NOT_READY);
    }
    return result;
}

inline VkResult WaitAndGetStatus(const VkInterfaceFunctions* vkIf, VkDevice device, VkFence fence,
                                 VkQueryPool queryPool, int32_t startQueryId, uint32_t pictureIndex,
                                  bool resetAfterWait = true, const char* fenceName = "unknown",
                                  const uint64_t fenceWaitTimeout = 100ULL * 1000ULL * 1000ULL /* 100 mSec */,
                                  const uint64_t fenceTotalWaitTimeout = 5ULL * 1000ULL * 1000ULL * 1000ULL /* 5 sec */,
                                  uint32_t retryCount = 6 /* 30s */) {

    VkResult result;

    do {
        result = WaitAndResetFence(vkIf, device, fence, resetAfterWait, fenceName, fenceWaitTimeout, fenceTotalWaitTimeout);
        if (result != VK_SUCCESS) {
            std::cout << "WaitForFences timeout " << fenceWaitTimeout
                    << " result " << result << " retry " << retryCount << std::endl << std::flush;

            VkQueryResultStatusKHR decodeStatus = VK_QUERY_RESULT_STATUS_NOT_READY_KHR;
            VkResult queryResult = vkIf->GetQueryPoolResults(device,
                                                     queryPool,
                                                     startQueryId,
                                                     1,
                                                     sizeof(decodeStatus),
                                                     &decodeStatus,
                                                     sizeof(decodeStatus),
                                                     VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);

            printf("\nERROR: GetQueryPoolResults() result: 0x%x\n", queryResult);
            std::cout << "\t +++++++++++++++++++++++++++< " << pictureIndex
                    << " >++++++++++++++++++++++++++++++" << std::endl;
            std::cout << "\t => Decode Status for CurrPicIdx: " << pictureIndex << std::endl
                    << "\t\tdecodeStatus: " << decodeStatus << std::endl;

            if (queryResult == VK_ERROR_DEVICE_LOST) {
                std::cout << "\t Dropping frame" << std::endl;
                break;
            }

            if ((queryResult == VK_SUCCESS) && (decodeStatus == VK_QUERY_RESULT_STATUS_ERROR_KHR)) {
                std::cout << "\t Decoding of the frame failed." << std::endl;
                break;
            }
        }

        retryCount--;
    } while ((result == VK_TIMEOUT) && (retryCount > 0));

    return result;
 }

template<typename NodeType, typename ChainedNodeType>
inline void ChainNextVkStruct(NodeType& node, ChainedNodeType& nextChainedNode) {
    // make sure the node is of type VkBaseInStructure
    static_assert(offsetof(NodeType, sType) == offsetof(VkBaseInStructure, sType),
                  "NodeType does not have sType at the same offset as VkBaseInStructure");

    static_assert(offsetof(NodeType, pNext) == offsetof(VkBaseInStructure, pNext),
                  "NodeType does not have pNext at the same offset as VkBaseInStructure");

    static_assert(std::is_standard_layout<NodeType>::value,
                  "NodeType must be a standard-layout type");

    static_assert(offsetof(ChainedNodeType, sType) == offsetof(VkBaseInStructure, sType),
                  "ChainedNodeType does not have sType at the same offset as VkBaseInStructure");

    static_assert(offsetof(ChainedNodeType, pNext) == offsetof(VkBaseInStructure, pNext),
                  "ChainedNodeType does not have pNext at the same offset as VkBaseInStructure");

    static_assert(std::is_standard_layout<ChainedNodeType>::value,
                  "ChainedNodeType must be a standard-layout type");

    assert(node.sType > 0);
    VkBaseInStructure* pNode = (VkBaseInStructure*)(&node);
    VkBaseInStructure* pNextNode = (VkBaseInStructure*)(&nextChainedNode);

    // The incoming object may not have anything chained.
    assert(pNextNode->pNext == nullptr);

    // Inserts the incoming object at the beginning of the list.
    pNextNode->pNext = pNode->pNext;
    pNode->pNext = pNextNode;
}

class DeviceUuidUtils
{

public:
    DeviceUuidUtils() {
        clear();
    }

    DeviceUuidUtils(const uint8_t deviceUUID[VK_UUID_SIZE]) {
        memcpy(m_deviceUUID, deviceUUID, VK_UUID_SIZE);
        m_deviceUuidIsValid = 1;
    }

    // Function to convert a UUID string to a byte array
    size_t StringToUUID(const char* uuidStr) {

        size_t numHexDigits = 0;

        // Validate basic format: 36 characters (32 hex digits + 4 hyphens)
        if (strlen(uuidStr) != 36) {
            std::cerr << "Error: UUID string must be 36 characters long" << std::endl;
            return numHexDigits;
        }

        for (size_t i = 0; i < 36; ++i) {

            if (*uuidStr == '-') {
                uuidStr++;
                continue;
            }

            uint8_t hexByte = 0;
            sscanf(uuidStr, "%2hhx", &hexByte);
            m_deviceUUID[numHexDigits] = hexByte;
            uuidStr += 2;
            numHexDigits++;
            if (numHexDigits == VK_UUID_SIZE) {
                m_deviceUuidIsValid = 1;
                break;
            }
        }

        return numHexDigits;
    }

    const uint8_t* GetDeviceUUID() const {
        return m_deviceUuidIsValid ? m_deviceUUID : nullptr;
    }

    std::string ToString() const {
        std::stringstream ss;

        // Format: 8-4-4-4-12 hex digits (32 hex digits total with 4 hyphens)
        for (uint32_t i = 0; i < VK_UUID_SIZE; ++i) {
            // Add hyphens at positions 4, 6, 8, and 10 (after 8, 4, 4, and 4 hex digits)
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                ss << "-";
            }

            // Convert byte to hex and ensure it's always two digits
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(m_deviceUUID[i]);
        }

        return ss.str();
    }

    explicit operator bool() const {
        return m_deviceUuidIsValid;
    }

    bool Compare(const uint8_t deviceUUID[VK_UUID_SIZE]) const {

        if (m_deviceUuidIsValid == 0) {
            return false;
        }
        if ( 0 == memcmp(m_deviceUUID, deviceUUID, VK_UUID_SIZE)) {
            return true;
        }

        return false;
    }
private:
    void clear() {
        m_deviceUuidIsValid = false;
        memset(m_deviceUUID, 0x00, sizeof(m_deviceUUID));
    }

private:
    uint8_t  m_deviceUUID[VK_UUID_SIZE];
    uint32_t m_deviceUuidIsValid : 1;
};

}  // namespace vk

#endif  // HELPERS_H
