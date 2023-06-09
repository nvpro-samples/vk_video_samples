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

#ifndef _VULKANDEVICECONTEXT_H_
#define _VULKANDEVICECONTEXT_H_

#include <assert.h>
#include <vector>
#include <array>
#include <mutex>
#include <vulkan_interfaces.h>
#include <VkCodecUtils/HelpersDispatchTable.h>
#include "VkShell/VkWsiDisplay.h"

class VulkanDeviceContext : public vk::VkInterfaceFunctions {

public:

    enum LogPriority {
        LOG_DEBUG,
        LOG_INFO,
        LOG_WARN,
        LOG_ERR,
    };

    enum QueueFamilySubmitType {
        DECODE = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
        ENCODE = VK_QUEUE_VIDEO_ENCODE_BIT_KHR,
    };

    enum {
        MAX_QUEUE_INSTANCES = 8,
        MAX_QUEUE_FAMILIES = 4, // Gfx, Present, Decode, Encode
    };

    VulkanDeviceContext(int32_t deviceId,
                        const char* const* reqInstanceLayers,
                        const char* const* reqInstanceExtensions,
                        const char* const* requestedDeviceExtensions,
                        const char* const* optDeviceExtensions = nullptr);

    VkInstance getInstance() const {
        return m_instance;
    }

    VkPhysicalDevice getPhysicalDevice() const {
        return m_physDevice;
    }

    VkDevice getDevice() const {
        return m_device;
    }

    int32_t GetGfxQueueFamilyIdx() const { return m_gfxQueueFamily; }
    VkQueue GetGfxQueue() const { return m_gfxQueue; }
    int32_t GetComputeQueueFamilyIdx() const { return m_computeQueueFamily; }
    VkQueue GetComputeQueue() const { return m_computeQueue; }
    int32_t GetPresentQueueFamilyIdx() const { return m_presentQueueFamily; }
    VkQueue GetPresentQueue() const { return m_presentQueue; }
    int32_t GetVideoDecodeQueueFamilyIdx() const { return m_videoDecodeQueueFamily; }
    int32_t GetVideoDecodeDefaultQueueIndex() const { return m_videoDecodeDefaultQueueIndex; }
    int32_t GetVideoDecodeNumQueues() const { return m_videoDecodeNumQueues; }
    VkQueue GetVideoDecodeQueue(int32_t index = 0) const {
        if ((size_t)index >= m_videoDecodeQueues.size()) {
            return VK_NULL_HANDLE;
        }
        return m_videoDecodeQueues[index];
    }
    int32_t GetVideoEncodeQueueFamilyIdx() const { return m_videoEncodeQueueFamily; }
    int32_t GetVideoEncodeNumQueues() const { return m_videoEncodeNumQueues; }
    VkQueue GetVideoEncodeQueue(int32_t index = 0) const {
        if ((size_t)index >= m_videoEncodeQueues.size()) {
            return VK_NULL_HANDLE;
        }
        return m_videoEncodeQueues[index];
    }
    bool    GetVideoDecodeQueryResultStatusSupport() const { return m_videoDecodeQueryResultStatusSupport; }
    bool    GetVideoEncodeQueryResultStatusSupport() const { return m_videoEncodeQueryResultStatusSupport; }
    class MtQueueMutex {

    public:
        MtQueueMutex(const VulkanDeviceContext* devCtx, const QueueFamilySubmitType submitType, const int32_t queueIndex)
        {
            if (submitType == DECODE) {
                assert((queueIndex >= 0) && (queueIndex < devCtx->m_videoDecodeNumQueues));
                m_queue = &devCtx->m_videoDecodeQueues[queueIndex];
                m_mutex = &devCtx->m_videoDecodeQueueMutexes[queueIndex];
            } else if (submitType == ENCODE) {
                assert((queueIndex >= 0) && (queueIndex < devCtx->m_videoEncodeNumQueues));
                m_queue = &devCtx->m_videoEncodeQueues[queueIndex];
                m_mutex = &devCtx->m_videoEncodeQueueMutexes[queueIndex];
            } else {
                m_queue = nullptr;
                m_mutex = nullptr;
            }
            if (m_mutex) {
                m_mutex->lock();
            }
        }

        ~MtQueueMutex() {
            if (m_mutex) {
                m_mutex->unlock();
                m_mutex = nullptr;
            }
        }

        VkQueue GetQueue() {
            return *m_queue;
        }

        operator VkQueue() { return *m_queue; }

        operator bool() { return ((m_queue != nullptr) && (*m_queue != VK_NULL_HANDLE)); }

    private:
        const VkQueue*    m_queue;
        mutable std::mutex* m_mutex;
    };

    VkResult MultiThreadedQueueSubmit(const QueueFamilySubmitType submitType, const int32_t queueIndex,
                                      uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) const
    {
        MtQueueMutex queue(this, submitType, queueIndex);
        if (queue) {
            return QueueSubmit(queue, submitCount, pSubmits, fence);
        } else {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    VkResult MultiThreadedQueueWaitIdle(const QueueFamilySubmitType submitType, const int32_t queueIndex) const
    {
        MtQueueMutex queue(this, submitType, queueIndex);
        if (queue) {
            return QueueWaitIdle(queue);
        } else {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    void GetMemoryProperties(VkPhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties) const {
        if (m_physDevice) {
            GetPhysicalDeviceMemoryProperties(m_physDevice, &physicalDeviceMemoryProperties);
        }
    }

    operator VkDevice() const { return m_device; }

    void DeviceWaitIdle() const;

    ~VulkanDeviceContext();

    int32_t AddRequiredDeviceExtension(const char* deviceExtensionName) {
        m_reqDeviceExtensions.push_back(deviceExtensionName);
        return (int32_t)m_reqDeviceExtensions.size();
    }

    const VkExtensionProperties* FindExtension(
        const std::vector<VkExtensionProperties>& extensions,
        const char* name) const;

    const VkExtensionProperties* FindInstanceExtension(const char* name) const;

    const VkExtensionProperties* FindDeviceExtension(const char* name) const;
    const char * FindRequiredDeviceExtension(const char* name) const;

    void PrintExtensions(bool deviceExt = false) const;

#if !defined(VK_USE_PLATFORM_WIN32_KHR)
    typedef void* VulkanLibraryHandleType;
#else
    typedef HMODULE VulkanLibraryHandleType;
#endif

    VkResult InitVulkanDevice(const char * pAppName, bool verbose = false,
                              const char * pCustomLoader = nullptr);

    VkResult CheckAllInstanceLayers(bool verbose = false);
    VkResult CheckAllInstanceExtensions(bool verbose = false);
    bool HasAllDeviceExtensions(VkPhysicalDevice physDevice, bool printMissingExt = false);

    bool DebugReportCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT obj_type,
                             uint64_t object, size_t location,
                             int32_t msg_code, const char *layer_prefix, const char *msg);

    VkResult InitPhysicalDevice(const VkQueueFlags requestQueueTypes =  (VK_QUEUE_GRAPHICS_BIT |
                                                                   /*  VK_QUEUE_COMPUTE_BIT |  */
                                                                   /* VK_QUEUE_TRANSFER_BIT | */
                                                                   VK_QUEUE_VIDEO_DECODE_BIT_KHR |
                                                                   VK_QUEUE_VIDEO_ENCODE_BIT_KHR ),
                                const VkWsiDisplay* pWsiDisplay = nullptr,
                                const VkQueueFlags requestVideoDecodeQueueMask = VK_QUEUE_VIDEO_DECODE_BIT_KHR |
                                                                                 VK_QUEUE_TRANSFER_BIT,
                                const VkVideoCodecOperationFlagsKHR requestVideoDecodeQueueOperations =
                                                                  (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
                                                                   VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR),
                                const VkQueueFlags requestVideoEncodeQueueMask = VK_QUEUE_VIDEO_ENCODE_BIT_KHR |
                                                                                 VK_QUEUE_TRANSFER_BIT,
                                const VkVideoCodecOperationFlagsKHR requestVideoEncodeQueueOperations =
                                                                  (VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT |
                                                                   VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT));

    VkResult CreateVulkanDevice(int32_t numDecodeQueues = 1,
                                int32_t numEncodeQueues = 0,
                                bool createGraphicsQueue = false,
                                bool createPresentQueue = false,
                                bool createComputeQueue = false);
    VkResult InitDebugReport(bool validate = false, bool validateVerbose = false);
private:

    static PFN_vkGetInstanceProcAddr LoadVk(VulkanLibraryHandleType &vulkanLibHandle,
                                            const char * pCustomLoader = nullptr);

    VkResult InitVkInstance(const char * pAppName, bool verbose = false);

    VkResult PopulateInstanceExtensions();

    VkResult PopulateDeviceExtensions();

private:
    int32_t                 m_deviceId;
    VulkanLibraryHandleType m_libHandle;
    VkInstance m_instance;
    VkPhysicalDevice m_physDevice;
    int32_t  m_gfxQueueFamily;
    int32_t  m_computeQueueFamily;
    int32_t  m_presentQueueFamily;
    int32_t  m_transferQueueFamily;
    int32_t  m_transferNumQueues;
    int32_t  m_videoDecodeQueueFamily;
    int32_t  m_videoDecodeDefaultQueueIndex;
    int32_t  m_videoDecodeNumQueues;
    int32_t  m_videoEncodeQueueFamily;
    int32_t  m_videoEncodeNumQueues;
    int32_t  m_videoDecodeEncodeComputeQueueFamily;
    int32_t  m_videoDecodeEncodeComputeNumQueues;
    uint32_t m_videoDecodeQueueTransferSupport : 1;
    uint32_t m_videoEncodeQueueTransferSupport : 1;
    uint32_t m_videoDecodeQueueComputeSupport : 1;
    uint32_t m_videoEncodeQueueComputeSupport : 1;
    uint32_t m_videoDecodeQueryResultStatusSupport : 1;
    uint32_t m_videoEncodeQueryResultStatusSupport : 1;
    VkDevice                m_device;
    VkQueue                 m_gfxQueue;
    VkQueue                 m_computeQueue;
    VkQueue                 m_presentQueue;
    std::vector<VkQueue>    m_videoDecodeQueues;
    std::vector<VkQueue>    m_videoEncodeQueues;
    mutable std::mutex                                  m_gfxQueueMutexes;
    mutable std::array<std::mutex, MAX_QUEUE_INSTANCES> m_videoDecodeQueueMutexes;
    mutable std::array<std::mutex, MAX_QUEUE_INSTANCES> m_videoEncodeQueueMutexes;
    bool m_isExternallyManagedDevice;
    VkDebugReportCallbackEXT           m_debugReport;
    const char* const*                 m_reqInstanceLayers;
    uint32_t                           m_reqInstanceLayersSize;
    const char* const*                 m_reqInstanceExtensions;
    uint32_t                           m_reqInstanceExtensionsSize;
    const char* const*                 m_requestedDeviceExtensions;
    uint32_t                           m_requestedDeviceExtensionsSize;
    const char* const*                 m_optDeviceExtensions;
    uint32_t                           m_optDeviceExtensionsSize;
    std::vector<const char *>          m_reqDeviceExtensions;
    std::vector<VkExtensionProperties> m_instanceExtensions;
    std::vector<VkExtensionProperties> m_deviceExtensions;
};

#endif /* _VULKANDEVICECONTEXT_H_ */
