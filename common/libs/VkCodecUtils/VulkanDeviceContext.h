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
#include "VkCodecUtils/Helpers.h"
#include <VkCodecUtils/HelpersDispatchTable.h>
#include "VkShell/VkWsiDisplay.h"
#include "VkCodecUtils/VulkanSemaphoreDump.h"

class VulkanDeviceContext : public vk::VkInterfaceFunctions {

public:

    enum LogPriority {
        LOG_DEBUG,
        LOG_INFO,
        LOG_WARN,
        LOG_ERR,
    };

    enum QueueFamilySubmitType {
        GRAPHICS = VK_QUEUE_GRAPHICS_BIT,
        COMPUTE  = VK_QUEUE_COMPUTE_BIT,
        TRANSFER = VK_QUEUE_TRANSFER_BIT,
        DECODE   = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
        ENCODE   = VK_QUEUE_VIDEO_ENCODE_BIT_KHR,
        PRESENT,
    };

    enum {
        MAX_QUEUE_INSTANCES = 8,
        MAX_QUEUE_FAMILIES = 6, // Gfx, Present, Compute, Transfer, Decode, Encode
    };

    static const VkVideoCodecOperationFlagsKHR VIDEO_CODEC_OPERATIONS_DECODE =
        VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
        VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR |
        VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR |
        VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;

    static const VkVideoCodecOperationFlagsKHR VIDEO_CODEC_OPERATIONS_ENCODE =
        VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR |
        VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR |
        VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;

    static const VkVideoCodecOperationFlagsKHR VIDEO_CODEC_OPERATIONS_ALL =
        VIDEO_CODEC_OPERATIONS_DECODE |
        VIDEO_CODEC_OPERATIONS_ENCODE;

    VulkanDeviceContext();

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
    int32_t GetTransferQueueFamilyIdx() const { return m_transferQueueFamily; }
    VkQueue GetTransferQueue() const { return m_trasferQueue; }
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
    int32_t GetVideoEncodeDefaultQueueIndex() const { return m_videoEncodeDefaultQueueIndex; }
    int32_t GetVideoEncodeNumQueues() const { return m_videoEncodeNumQueues; }
    VkQueue GetVideoEncodeQueue(int32_t index = 0) const {
        if ((size_t)index >= m_videoEncodeQueues.size()) {
            return VK_NULL_HANDLE;
        }
        return m_videoEncodeQueues[index];
    }
    bool    GetVideoDecodeQueryResultStatusSupport() const { return m_videoDecodeQueryResultStatusSupport; }
    bool    GetVideoEncodeQueryResultStatusSupport() const { return m_videoEncodeQueryResultStatusSupport; }
    VkQueueFlags GetVideoDecodeQueueFlag() const { return m_videoDecodeQueueFlags; }
    VkQueueFlags GetVideoEncodeQueueFlag() const { return m_videoEncodeQueueFlags; }
    class MtQueueMutex {

    public:
        MtQueueMutex(const VulkanDeviceContext* devCtx, const QueueFamilySubmitType submitType, const int32_t queueIndex)
        {
            switch (submitType) {
            case GRAPHICS:
                m_queue = &devCtx->m_gfxQueue;
                m_mutex = &devCtx->m_gfxQueueMutex;
                break;
            case COMPUTE:
                m_queue = &devCtx->m_computeQueue;
                m_mutex = &devCtx->m_computeQueueMutex;
                break;
            case TRANSFER:
                m_queue = &devCtx->m_trasferQueue;
                m_mutex = &devCtx->m_transferQueueMutex;
                break;
            case DECODE:
                assert((queueIndex >= 0) && (queueIndex < devCtx->m_videoDecodeNumQueues));
                m_queue = &devCtx->m_videoDecodeQueues[queueIndex];
                m_mutex = &devCtx->m_videoDecodeQueueMutexes[queueIndex];
                break;
            case ENCODE:
                assert((queueIndex >= 0) && (queueIndex < devCtx->m_videoEncodeNumQueues));
                m_queue = &devCtx->m_videoEncodeQueues[queueIndex];
                m_mutex = &devCtx->m_videoEncodeQueueMutexes[queueIndex];
                break;
            case PRESENT:
                m_queue = &devCtx->m_presentQueue;
                m_mutex = &devCtx->m_presentQueueMutex;
                break;
            default:
                assert(!"Invalid queue type!");
                m_queue = nullptr;
                m_mutex = nullptr;
                break;
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
                                      uint32_t submitCount, const VkSubmitInfo2KHR* pSubmits, VkFence fence,
                                      const char* submissionName = nullptr,
                                      uint64_t decodeEncodeOrder = UINT64_MAX,
                                      uint64_t displayInputOrder = UINT64_MAX) const
    {
        MtQueueMutex queue(this, submitType, queueIndex);
        if (queue) {

            // Dump semaphore info for debugging
            if (false) {
                for (uint32_t i = 0; i < submitCount; i++) {
                    VulkanSemaphoreDump::DumpSemaphoreInfo(pSubmits[i], submissionName, decodeEncodeOrder, displayInputOrder);
                }
            }

            return QueueSubmit2KHR(queue, submitCount, pSubmits, fence);
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

    VkResult InitVulkanDevice(const char * pAppName,
                              VkInstance vkInstance = VK_NULL_HANDLE,
                              bool verbose = false,
                              const char * pCustomLoader = nullptr);

    VkResult InitVulkanDecoderDevice(const char * pAppName,
                                     VkInstance vkInstance = VK_NULL_HANDLE,
                                     VkVideoCodecOperationFlagsKHR videoCodecs = VIDEO_CODEC_OPERATIONS_ALL,
                                     bool enableWsi = false,
                                     bool enableWsiDirectMode = false,
                                     bool enableValidation = false,
                                     bool enableVerboseValidation = false,
                                     bool enbaleVerboseDump = false,
                                     const char * pCustomLoader = nullptr);

    VkResult AddReqInstanceLayers(const char* const* requiredInstanceLayers, bool verbose = false);
    VkResult CheckAllInstanceLayers(bool verbose = false);
    VkResult AddReqInstanceExtensions(const char* const* requiredInstanceExtensions, bool verbose = false);
    VkResult AddReqInstanceExtension(const char* requiredInstanceExtension, bool verbose = false);
    VkResult CheckAllInstanceExtensions(bool verbose = false);
    VkResult AddReqDeviceExtensions(const char* const* requiredDeviceExtensions, bool verbose = false);
    VkResult AddReqDeviceExtension(const char* requiredDeviceExtension, bool verbose = false);
    VkResult AddOptDeviceExtensions(const char* const* optionalDeviceExtensions, bool verbose = false);
    bool HasAllDeviceExtensions(VkPhysicalDevice physDevice, const char* printMissingDeviceExt = nullptr);

    bool DebugReportCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT obj_type,
                             uint64_t object, size_t location,
                             int32_t msg_code, const char *layer_prefix, const char *msg);

    VkResult InitPhysicalDevice(int32_t deviceId, const vk::DeviceUuidUtils& deviceUuid,
                                const VkQueueFlags requestQueueTypes =  (VK_QUEUE_GRAPHICS_BIT |
                                                                   /*  VK_QUEUE_COMPUTE_BIT |  */
                                                                   /* VK_QUEUE_TRANSFER_BIT | */
                                                                   VK_QUEUE_VIDEO_DECODE_BIT_KHR |
                                                                   VK_QUEUE_VIDEO_ENCODE_BIT_KHR ),
                                const VkWsiDisplay* pWsiDisplay = nullptr,
                                const VkQueueFlags requestVideoDecodeQueueMask = VK_QUEUE_VIDEO_DECODE_BIT_KHR |
                                                                                 VK_QUEUE_TRANSFER_BIT,
                                const VkVideoCodecOperationFlagsKHR requestVideoDecodeQueueOperations =
                                                                  VIDEO_CODEC_OPERATIONS_DECODE,
                                const VkQueueFlags requestVideoEncodeQueueMask = VK_QUEUE_VIDEO_ENCODE_BIT_KHR |
                                                                                 VK_QUEUE_TRANSFER_BIT,
                                const VkVideoCodecOperationFlagsKHR requestVideoEncodeQueueOperations =
                                                                  VIDEO_CODEC_OPERATIONS_ENCODE,
                                VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE);

    VkResult CreateVulkanDevice(int32_t numDecodeQueues = 1,
                                int32_t numEncodeQueues = 0,
                                VkVideoCodecOperationFlagsKHR videoCodecs = VIDEO_CODEC_OPERATIONS_ALL,
                                bool createTransferQueue = false,
                                bool createGraphicsQueue = false,
                                bool createPresentQueue = false,
                                bool createComputeQueue = false,
                                VkDevice vkDevice = VK_NULL_HANDLE);
    VkResult InitDebugReport(bool validate = false, bool validateVerbose = false);
private:

    static PFN_vkGetInstanceProcAddr LoadVk(VulkanLibraryHandleType &vulkanLibHandle,
                                            const char * pCustomLoader = nullptr);

    VkResult InitVkInstance(const char * pAppName, VkInstance vkInstance = VK_NULL_HANDLE, bool verbose = false);

    VkResult PopulateInstanceExtensions();

    VkResult PopulateDeviceExtensions();

private:
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
    int32_t  m_videoEncodeDefaultQueueIndex;
    int32_t  m_videoEncodeNumQueues;
    int32_t  m_videoDecodeEncodeComputeQueueFamily;
    int32_t  m_videoDecodeEncodeComputeNumQueues;
    VkQueueFlags m_videoDecodeQueueFlags;
    VkQueueFlags m_videoEncodeQueueFlags;
    uint32_t m_importedInstanceHandle : 1;
    uint32_t m_importedDeviceHandle : 1;
    uint32_t m_videoDecodeQueryResultStatusSupport : 1;
    uint32_t m_videoEncodeQueryResultStatusSupport : 1;
    VkDevice                m_device;
    VkQueue                 m_gfxQueue;
    VkQueue                 m_computeQueue;
    VkQueue                 m_trasferQueue;
    VkQueue                 m_presentQueue;
    std::vector<VkQueue>    m_videoDecodeQueues;
    std::vector<VkQueue>    m_videoEncodeQueues;
    mutable std::mutex                                  m_gfxQueueMutex;
    mutable std::mutex                                  m_computeQueueMutex;
    mutable std::mutex                                  m_transferQueueMutex;
    mutable std::mutex                                  m_presentQueueMutex;
    mutable std::array<std::mutex, MAX_QUEUE_INSTANCES> m_videoDecodeQueueMutexes;
    mutable std::array<std::mutex, MAX_QUEUE_INSTANCES> m_videoEncodeQueueMutexes;
    VkDebugReportCallbackEXT           m_debugReport;
    std::vector<const char *>          m_reqInstanceLayers;
    std::vector<const char *>          m_reqInstanceExtensions;
    std::vector<const char *>          m_requestedDeviceExtensions;
    std::vector<const char *>          m_optDeviceExtensions;
    std::vector<const char *>          m_reqDeviceExtensions;
    std::vector<VkExtensionProperties> m_instanceExtensions;
    std::vector<VkExtensionProperties> m_deviceExtensions;
};

#endif /* _VULKANDEVICECONTEXT_H_ */
