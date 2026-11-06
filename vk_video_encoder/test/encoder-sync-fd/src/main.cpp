/*
 * Copyright 2026 NVIDIA Corporation.
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

// SYNC_FD fences around the encoder's submissions.
//
// THE PROPERTY. A binary semaphore that a frame's submission signals, exported
// as a SYNC_FD, becomes readable once that submission completes; and a SYNC_FD
// imported into a semaphore the frame waits on holds the frame until the
// producer's work signals it. This is how a producer on another device, or in
// another API, hands frames to the encoder and gets its images back.
//
// TWO PATHS, TWO QUEUES. An OPTIMAL NV12 image is read by vkCmdEncodeVideoKHR
// itself, so the frame's waits and signals ride on the ENCODE-queue
// submission. A LINEAR image is copied first, so they ride on the staging
// submission. A driver can break either alone, and nothing else in the suite
// exports a fence from the encode queue.
//
// THE DIRECT CASE IS A PRODUCER LOOP. A second VkDevice on the same physical
// device writes each frame into an image whose memory it exports (OPAQUE_FD),
// releases it to VK_QUEUE_FAMILY_EXTERNAL and signals a SYNC_FD. The encoder's
// device imports the memory, waits on that SYNC_FD, encodes, and signals its
// own, which the producer waits on before it writes the image again. Each
// release is therefore checked twice: by poll() on the host, and by the
// producer's next submission waiting on it on the GPU.
//
// The staged case keeps the image on the encoder's device and written from the
// host; the producer only signals the acquire fence and waits on the release.
//
// A RELEASE THAT NEVER SIGNALS ENDS THE PROCESS. The frame is still in flight,
// and tearing the session down would wait on it forever, so the test reports
// and exits without destroying anything.

#include "encoder_test_support.h"

#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace vk::video::enc;

namespace {

const char* const kName = "encoder-sync-fd";

const uint32_t kWidth = 320;
const uint32_t kHeight = 240;
const uint32_t kFrames = 64;
const int kSignalTimeoutMs = 5000;

// Semaphores per role. A semaphore is reused only after the frame that used it
// has released, which the loop waits for before submitting the next frame.
const uint32_t kRing = 4;

const VkFormat kFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

// The usage and flags the encoder states for an OPTIMAL image it reads directly
// (VkVideoEncoder::WrapExternalImage). ExternalImage requires them to match
// vkCreateImage exactly, and an OPAQUE_FD import requires the two devices'
// images to match each other.
const VkImageUsageFlags kDirectUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                       VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
const VkImageCreateFlags kDirectFlags =
    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;

// "<what> of frame <n><rest>", for a check that names its frame.
std::string OfFrame(const std::string& what, uint32_t frame, const std::string& rest) {
    return what + " of frame " + std::to_string(frame) + rest;
}

//=============================================================================
// THE INSTANCE THE TEST OWNS AND THE LIBRARY ADOPTS
//=============================================================================

struct Instance {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t producerFamily = UINT32_MAX;

    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties GetPhysicalDeviceExternalSemaphoreProperties = nullptr;

    ~Instance() {
        if (instance != VK_NULL_HANDLE) {
            DestroyInstance(instance, nullptr);
        }
    }

    // The first physical device with a queue family that encodes H.264 and one
    // that can copy, which is where the producer runs. False with |why| when
    // there is none; the loader stays loaded, as the library loads it too.
    bool Create(const char** why) {
        void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) {
            lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        }
        gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")) : nullptr;
        if (gipa == nullptr) {
            *why = "no Vulkan loader";
            return false;
        }
        auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = kName;
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        if ((createInstance == nullptr) || (createInstance(&ici, nullptr, &instance) != VK_SUCCESS)) {
            instance = VK_NULL_HANDLE;
            *why = "vkCreateInstance failed";
            return false;
        }

#define INSTANCE_LOAD(name) name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name))
        INSTANCE_LOAD(DestroyInstance);
        INSTANCE_LOAD(GetDeviceProcAddr);
        INSTANCE_LOAD(CreateDevice);
        INSTANCE_LOAD(EnumerateDeviceExtensionProperties);
        INSTANCE_LOAD(GetPhysicalDeviceFeatures2);
        INSTANCE_LOAD(GetPhysicalDeviceMemoryProperties);
        INSTANCE_LOAD(GetPhysicalDeviceImageFormatProperties2);
        INSTANCE_LOAD(GetPhysicalDeviceExternalSemaphoreProperties);
#undef INSTANCE_LOAD
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
        auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
        auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
            gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties2"));
        if (!DestroyInstance || !GetDeviceProcAddr || !CreateDevice || !EnumerateDeviceExtensionProperties ||
            !GetPhysicalDeviceFeatures2 || !GetPhysicalDeviceMemoryProperties || !GetPhysicalDeviceImageFormatProperties2 ||
            !GetPhysicalDeviceExternalSemaphoreProperties || !enumerate || !properties || !queues) {
            *why = "the instance entry points do not load";
            return false;
        }

        uint32_t count = 0;
        enumerate(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        enumerate(instance, &count, devices.data());
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties p{};
            properties(candidate, &p);
            if (p.apiVersion < VK_API_VERSION_1_3) {
                continue;
            }
            uint32_t families = 0;
            queues(candidate, &families, nullptr);
            std::vector<VkQueueFamilyVideoPropertiesKHR> video(families, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR});
            std::vector<VkQueueFamilyProperties2> props(families, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
            for (uint32_t f = 0; f < families; f++) {
                props[f].pNext = &video[f];
            }
            queues(candidate, &families, props.data());
            bool encodes = false;
            uint32_t copier = UINT32_MAX;
            for (uint32_t f = 0; f < families; f++) {
                const VkQueueFlags flags = props[f].queueFamilyProperties.queueFlags;
                if (((flags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) != 0) &&
                    ((video[f].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) != 0)) {
                    encodes = true;
                }
                if ((copier == UINT32_MAX) && ((flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0)) {
                    copier = f;
                }
            }
            if (encodes && (copier != UINT32_MAX)) {
                phys = candidate;
                producerFamily = copier;
                return true;
            }
        }
        *why = "no Vulkan 1.3 device with an H.264 encode queue and a queue that copies";
        return false;
    }

    bool SyncFdSemaphores() const {
        VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkExternalSemaphoreProperties props{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        GetPhysicalDeviceExternalSemaphoreProperties(phys, &info, &props);
        const VkExternalSemaphoreFeatureFlags both =
            VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
        return (props.externalSemaphoreFeatures & both) == both;
    }

    // Whether the direct-path image can be created and shared through an
    // OPAQUE_FD allocation, with |why| saying what is missing.
    bool DirectImageShareable(const char** why) const {
        VkPhysicalDeviceVideoMaintenance1FeaturesKHR maintenance1{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &maintenance1;
        GetPhysicalDeviceFeatures2(phys, &features);
        if (!maintenance1.videoMaintenance1) {
            *why = "videoMaintenance1 is not supported";
            return false;
        }

        VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.pNext = &external;
        info.format = kFormat;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = kDirectUsage;
        info.flags = kDirectFlags;
        VkExternalImageFormatProperties externalProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        props.pNext = &externalProps;
        if (GetPhysicalDeviceImageFormatProperties2(phys, &info, &props) != VK_SUCCESS) {
            *why = "an OPTIMAL NV12 encode-source image with OPAQUE_FD memory is not supported";
            return false;
        }
        const VkExternalMemoryFeatureFlags both =
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
        if ((externalProps.externalMemoryProperties.externalMemoryFeatures & both) != both) {
            *why = "OPAQUE_FD image memory is not both exportable and importable";
            return false;
        }
        return true;
    }
};

//=============================================================================
// DEVICE ENTRY POINTS, FOR THE PRODUCER'S DEVICE AND THE ENCODER'S
//=============================================================================

#define DEVICE_FUNCTIONS(X)        \
    X(DestroyDevice)               \
    X(GetDeviceQueue)              \
    X(DeviceWaitIdle)              \
    X(CreateImage)                 \
    X(DestroyImage)                \
    X(GetImageMemoryRequirements2) \
    X(AllocateMemory)              \
    X(FreeMemory)                  \
    X(BindImageMemory)             \
    X(MapMemory)                   \
    X(UnmapMemory)                 \
    X(CreateBuffer)                \
    X(DestroyBuffer)               \
    X(GetBufferMemoryRequirements) \
    X(BindBufferMemory)            \
    X(CreateCommandPool)           \
    X(DestroyCommandPool)          \
    X(AllocateCommandBuffers)      \
    X(BeginCommandBuffer)          \
    X(EndCommandBuffer)            \
    X(ResetCommandBuffer)          \
    X(CmdPipelineBarrier)          \
    X(CmdCopyBufferToImage)        \
    X(QueueSubmit)                 \
    X(CreateFence)                 \
    X(DestroyFence)                \
    X(WaitForFences)               \
    X(ResetFences)                 \
    X(CreateSemaphore)             \
    X(DestroySemaphore)            \
    X(GetMemoryFdKHR)              \
    X(GetSemaphoreFdKHR)           \
    X(ImportSemaphoreFdKHR)

struct Device {
    VkDevice device = VK_NULL_HANDLE;
#define DEVICE_DECLARE(name) PFN_vk##name name = nullptr;
    DEVICE_FUNCTIONS(DEVICE_DECLARE)
#undef DEVICE_DECLARE

    bool Load(PFN_vkGetDeviceProcAddr gdpa, VkDevice handle) {
        device = handle;
        bool loaded = true;
#define DEVICE_LOAD(name)                                            \
    name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name)); \
    loaded = loaded && (name != nullptr);
        DEVICE_FUNCTIONS(DEVICE_LOAD)
#undef DEVICE_LOAD
        return loaded;
    }

    VkSemaphore CreateBinary(bool exportSyncFd) const {
        VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ci.pNext = exportSyncFd ? &exportInfo : nullptr;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        CreateSemaphore(device, &ci, nullptr, &semaphore);
        return semaphore;
    }

    // A SYNC_FD of the signal operation pending on |semaphore|. -1 with
    // VK_SUCCESS is a valid answer: the payload has already signalled.
    VkResult ExportSyncFd(VkSemaphore semaphore, int* fd) const {
        VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        info.semaphore = semaphore;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        *fd = -1;
        return GetSemaphoreFdKHR(device, &info, fd);
    }

    // Takes |fd| whatever the outcome: SYNC_FD imports are temporary, and on
    // failure the descriptor is closed here.
    VkResult ImportSyncFd(VkSemaphore semaphore, int fd) const {
        VkImportSemaphoreFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        info.semaphore = semaphore;
        info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        info.fd = fd;
        const VkResult result = ImportSemaphoreFdKHR(device, &info);
        if ((result != VK_SUCCESS) && (fd >= 0)) {
            close(fd);
        }
        return result;
    }
};

// Whether |fd| becomes readable -- its fence signalled -- within the timeout.
// -1 is a fence that had signalled when it was exported.
bool Signals(int fd) {
    if (fd < 0) {
        return true;
    }
    pollfd p{fd, POLLIN, 0};
    for (;;) {
        const int ready = poll(&p, 1, kSignalTimeoutMs);
        if ((ready < 0) && (errno == EINTR)) {
            continue;
        }
        return (ready > 0) && ((p.revents & POLLIN) != 0);
    }
}

//=============================================================================
// THE PRODUCER: A SECOND DEVICE THAT WRITES THE FRAMES AND OWNS THE FENCES
//=============================================================================

class Producer {
   public:
    explicit Producer(const Instance& instance) : m_instance(instance) {}

    ~Producer() {
        if (m_dev.device == VK_NULL_HANDLE) {
            return;
        }
        m_dev.DeviceWaitIdle(m_dev.device);
        for (VkSemaphore s : m_signal) {
            m_dev.DestroySemaphore(m_dev.device, s, nullptr);
        }
        for (VkSemaphore s : m_wait) {
            m_dev.DestroySemaphore(m_dev.device, s, nullptr);
        }
        m_dev.DestroyFence(m_dev.device, m_fence, nullptr);
        m_dev.DestroyCommandPool(m_dev.device, m_pool, nullptr);
        m_dev.DestroyImage(m_dev.device, m_image, nullptr);
        m_dev.FreeMemory(m_dev.device, m_imageMemory, nullptr);
        m_dev.DestroyBuffer(m_dev.device, m_staging, nullptr);
        m_dev.FreeMemory(m_dev.device, m_stagingMemory, nullptr);
        m_dev.DestroyDevice(m_dev.device, nullptr);
    }

    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    // |withImage| adds what the direct case needs: the video extensions the
    // image's usage names, and videoMaintenance1 for its profile independence.
    bool Create(bool withImage, const char** why) {
        std::vector<const char*> extensions = {VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
                                               VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
        if (withImage) {
            extensions.push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
            extensions.push_back(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME);
            extensions.push_back(VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME);
        }
        uint32_t count = 0;
        m_instance.EnumerateDeviceExtensionProperties(m_instance.phys, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        m_instance.EnumerateDeviceExtensionProperties(m_instance.phys, nullptr, &count, available.data());
        for (const char* name : extensions) {
            bool found = false;
            for (const VkExtensionProperties& e : available) {
                found = found || (std::strcmp(e.extensionName, name) == 0);
            }
            if (!found) {
                m_missing = std::string("the device lacks ") + name;
                *why = m_missing.c_str();
                return false;
            }
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = m_instance.producerFamily;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkPhysicalDeviceVideoMaintenance1FeaturesKHR maintenance1{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR};
        maintenance1.videoMaintenance1 = VK_TRUE;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = withImage ? &maintenance1 : nullptr;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &queue;
        dci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        dci.ppEnabledExtensionNames = extensions.data();
        VkDevice device = VK_NULL_HANDLE;
        if (m_instance.CreateDevice(m_instance.phys, &dci, nullptr, &device) != VK_SUCCESS) {
            *why = "the producer's vkCreateDevice failed";
            return false;
        }
        if (!m_dev.Load(m_instance.GetDeviceProcAddr, device)) {
            // Destroyed here: the destructor needs every entry point.
            auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(m_instance.GetDeviceProcAddr(device, "vkDestroyDevice"));
            if (destroy != nullptr) {
                destroy(device, nullptr);
            }
            m_dev.device = VK_NULL_HANDLE;
            *why = "the producer's device entry points do not load";
            return false;
        }
        m_dev.GetDeviceQueue(device, m_instance.producerFamily, 0, &m_queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = m_instance.producerFamily;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if ((m_dev.CreateCommandPool(device, &pci, nullptr, &m_pool) != VK_SUCCESS) ||
            (m_dev.CreateFence(device, &fci, nullptr, &m_fence) != VK_SUCCESS)) {
            *why = "the producer's command pool or fence";
            return false;
        }
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = m_pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (m_dev.AllocateCommandBuffers(device, &cai, &m_cmd) != VK_SUCCESS) {
            *why = "the producer's command buffer";
            return false;
        }
        for (uint32_t i = 0; i < kRing; i++) {
            m_signal.push_back(m_dev.CreateBinary(true));
            m_wait.push_back(m_dev.CreateBinary(false));
            if ((m_signal.back() == VK_NULL_HANDLE) || (m_wait.back() == VK_NULL_HANDLE)) {
                *why = "the producer's semaphores";
                return false;
            }
        }
        return !withImage || CreateImage(why);
    }

    // The image's memory, exported for the encoder's device to import. The
    // caller owns |*fd|.
    bool ExportImageMemory(int* fd, const char** why) const {
        VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        info.memory = m_imageMemory;
        info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (m_dev.GetMemoryFdKHR(m_dev.device, &info, fd) != VK_SUCCESS) {
            *why = "vkGetMemoryFdKHR refused the image memory";
            return false;
        }
        return true;
    }

    VkDeviceSize ImageAllocationSize() const { return m_imageSize; }
    uint32_t ImageMemoryType() const { return m_imageType; }

    // Frame |frame|'s submission: wait on |wait| (the previous frame's
    // release, when there is one), write the image when there is one, and
    // signal ring slot |slot|, whose SYNC_FD is returned in |*fd|.
    bool Submit(uint32_t frame, VkSemaphore wait, uint32_t slot, int* fd, const char** why) {
        m_dev.WaitForFences(m_dev.device, 1, &m_fence, VK_TRUE, UINT64_MAX);
        m_dev.ResetFences(m_dev.device, 1, &m_fence);

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (wait != VK_NULL_HANDLE) {
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &wait;
            submit.pWaitDstStageMask = &waitStage;
        }
        if (m_image != VK_NULL_HANDLE) {
            WritePattern(frame);
            if (!Record(frame)) {
                *why = "recording the producer's copy failed";
                return false;
            }
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &m_cmd;
        }
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &m_signal[slot];
        if (m_dev.QueueSubmit(m_queue, 1, &submit, m_fence) != VK_SUCCESS) {
            *why = "the producer's vkQueueSubmit failed";
            return false;
        }
        if (m_dev.ExportSyncFd(m_signal[slot], fd) != VK_SUCCESS) {
            *why = "the producer's acquire fence does not export as a SYNC_FD";
            return false;
        }
        return true;
    }

    // Make the producer's next submission wait on |fd| (taken).
    VkSemaphore ImportRelease(uint32_t slot, int fd) const {
        return (m_dev.ImportSyncFd(m_wait[slot], fd) == VK_SUCCESS) ? m_wait[slot] : VK_NULL_HANDLE;
    }

   private:
    bool CreateImage(const char** why) {
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ci = DirectImageInfo(&external);
        if (m_dev.CreateImage(m_dev.device, &ci, nullptr, &m_image) != VK_SUCCESS) {
            *why = "the producer's vkCreateImage refused the direct-path image";
            return false;
        }
        VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        reqInfo.image = m_image;
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        m_dev.GetImageMemoryRequirements2(m_dev.device, &reqInfo, &req);
        m_imageType = FindType(req.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (m_imageType == UINT32_MAX) {
            *why = "no device-local memory type for the direct-path image";
            return false;
        }
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = m_image;
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportInfo.pNext = &dedicated;
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &exportInfo;
        ai.allocationSize = req.memoryRequirements.size;
        ai.memoryTypeIndex = m_imageType;
        if ((m_dev.AllocateMemory(m_dev.device, &ai, nullptr, &m_imageMemory) != VK_SUCCESS) ||
            (m_dev.BindImageMemory(m_dev.device, m_image, m_imageMemory, 0) != VK_SUCCESS)) {
            *why = "the producer's exportable image memory";
            return false;
        }
        m_imageSize = req.memoryRequirements.size;

        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = kWidth * kHeight * 3 / 2;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (m_dev.CreateBuffer(m_dev.device, &bci, nullptr, &m_staging) != VK_SUCCESS) {
            *why = "the producer's staging buffer";
            return false;
        }
        VkMemoryRequirements breq{};
        m_dev.GetBufferMemoryRequirements(m_dev.device, m_staging, &breq);
        VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bai.allocationSize = breq.size;
        bai.memoryTypeIndex =
            FindType(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if ((bai.memoryTypeIndex == UINT32_MAX) ||
            (m_dev.AllocateMemory(m_dev.device, &bai, nullptr, &m_stagingMemory) != VK_SUCCESS) ||
            (m_dev.BindBufferMemory(m_dev.device, m_staging, m_stagingMemory, 0) != VK_SUCCESS) ||
            (m_dev.MapMemory(m_dev.device, m_stagingMemory, 0, VK_WHOLE_SIZE, 0, &m_mapped) != VK_SUCCESS)) {
            *why = "the producer's host-visible staging memory";
            return false;
        }
        return true;
    }

    uint32_t FindType(uint32_t bits, VkMemoryPropertyFlags want) const {
        VkPhysicalDeviceMemoryProperties props{};
        m_instance.GetPhysicalDeviceMemoryProperties(m_instance.phys, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
            if (((bits & (1u << i)) != 0) && ((props.memoryTypes[i].propertyFlags & want) == want)) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    // A bar that moves each frame, so successive frames differ.
    void WritePattern(uint32_t frame) {
        uint8_t* luma = static_cast<uint8_t*>(m_mapped);
        uint8_t* chroma = luma + kWidth * kHeight;
        const uint32_t bar = (frame * 24) % kWidth;
        for (uint32_t y = 0; y < kHeight; y++) {
            for (uint32_t x = 0; x < kWidth; x++) {
                const bool inBar = (x >= bar) && (x < bar + 24);
                luma[y * kWidth + x] = inBar ? 235 : static_cast<uint8_t>(16 + (x * 200) / kWidth);
            }
        }
        std::memset(chroma, 128, kWidth * kHeight / 2);
    }

    // Acquire the image back from the encoder's device (after the first
    // frame), copy the frame in, and release it to that device in the layout
    // the frame declares.
    bool Record(uint32_t frame) {
        m_dev.ResetCommandBuffer(m_cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (m_dev.BeginCommandBuffer(m_cmd, &begin) != VK_SUCCESS) {
            return false;
        }

        VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acquire.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        acquire.oldLayout = (frame == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
        acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        acquire.srcQueueFamilyIndex = (frame == 0) ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_EXTERNAL;
        acquire.dstQueueFamilyIndex = (frame == 0) ? VK_QUEUE_FAMILY_IGNORED : m_instance.producerFamily;
        acquire.image = m_image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        m_dev.CmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &acquire);

        VkBufferImageCopy regions[2]{};
        regions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
        regions[0].imageExtent = {kWidth, kHeight, 1};
        regions[1].bufferOffset = kWidth * kHeight;
        regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
        regions[1].imageExtent = {kWidth / 2, kHeight / 2, 1};
        m_dev.CmdCopyBufferToImage(m_cmd, m_staging, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);

        VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        release.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
        release.srcQueueFamilyIndex = m_instance.producerFamily;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        release.image = m_image;
        release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        m_dev.CmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &release);
        return m_dev.EndCommandBuffer(m_cmd) == VK_SUCCESS;
    }

    const Instance& m_instance;
    Device m_dev;
    std::string m_missing;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    std::vector<VkSemaphore> m_signal;
    std::vector<VkSemaphore> m_wait;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_imageMemory = VK_NULL_HANDLE;
    VkDeviceSize m_imageSize = 0;
    uint32_t m_imageType = UINT32_MAX;
    VkBuffer m_staging = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    void* m_mapped = nullptr;

   public:
    static VkImageCreateInfo DirectImageInfo(const void* pNext) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.pNext = pNext;
        ci.flags = kDirectFlags;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kFormat;
        ci.extent = {kWidth, kHeight, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = kDirectUsage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return ci;
    }
};

//=============================================================================
// THE ENCODER'S SIDE OF ONE CASE
//=============================================================================

// The image the producer wrote, on the encoder's device, bound to the
// producer's memory.
struct ImportedImage {
    const Device* dev = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    ~ImportedImage() {
        if (dev != nullptr) {
            dev->DestroyImage(dev->device, image, nullptr);
            dev->FreeMemory(dev->device, memory, nullptr);
        }
    }

    bool Import(const Device& encoder, const Producer& producer, const char** why) {
        dev = &encoder;
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ci = Producer::DirectImageInfo(&external);
        if (dev->CreateImage(dev->device, &ci, nullptr, &image) != VK_SUCCESS) {
            *why = "the encoder's device refused the direct-path image";
            return false;
        }
        VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        reqInfo.image = image;
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        dev->GetImageMemoryRequirements2(dev->device, &reqInfo, &req);
        if ((req.memoryRequirements.memoryTypeBits & (1u << producer.ImageMemoryType())) == 0) {
            *why = "the encoder's image does not accept the producer's memory type";
            return false;
        }
        int fd = -1;
        if (!producer.ExportImageMemory(&fd, why)) {
            return false;
        }
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = image;
        VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        importInfo.pNext = &dedicated;
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        importInfo.fd = fd;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &importInfo;
        ai.allocationSize = producer.ImageAllocationSize();
        ai.memoryTypeIndex = producer.ImageMemoryType();
        if (dev->AllocateMemory(dev->device, &ai, nullptr, &memory) != VK_SUCCESS) {
            close(fd);
            *why = "the encoder's device refused the producer's image memory";
            return false;
        }
        if (dev->BindImageMemory(dev->device, image, memory, 0) != VK_SUCCESS) {
            *why = "binding the imported memory failed";
            return false;
        }
        return true;
    }

    ExternalImage Describe() const {
        ExternalImage d;
        d.handleType = ExternalHandleType::VkImageHandle;
        d.existingImage = image;
        d.format = kFormat;
        d.width = kWidth;
        d.height = kHeight;
        d.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Where the producer's release leaves it, and where the encode reads
        // it: the encoder transitions nothing on a local image.
        d.layout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
        d.colorModel = ColorModel::YCbCr;
        d.usage = kDirectUsage;
        d.createFlags = kDirectFlags;
        d.residency = Residency::Local;
        return d;
    }
};

// The encoder-side semaphores of one case: what each frame waits on (the
// producer's SYNC_FD, and the previous frame's release, both imported) and what
// it signals (exported).
struct EncoderFences {
    const Device* dev = nullptr;
    std::vector<VkSemaphore> wait;
    std::vector<VkSemaphore> chain;
    std::vector<VkSemaphore> signal;

    ~EncoderFences() {
        if (dev == nullptr) {
            return;
        }
        for (VkSemaphore s : wait) {
            dev->DestroySemaphore(dev->device, s, nullptr);
        }
        for (VkSemaphore s : chain) {
            dev->DestroySemaphore(dev->device, s, nullptr);
        }
        for (VkSemaphore s : signal) {
            dev->DestroySemaphore(dev->device, s, nullptr);
        }
    }

    bool Create(const Device& encoder) {
        dev = &encoder;
        for (uint32_t i = 0; i < kRing; i++) {
            wait.push_back(dev->CreateBinary(false));
            chain.push_back(dev->CreateBinary(false));
            signal.push_back(dev->CreateBinary(true));
            if ((wait.back() == VK_NULL_HANDLE) || (chain.back() == VK_NULL_HANDLE) || (signal.back() == VK_NULL_HANDLE)) {
                return false;
            }
        }
        return true;
    }
};

// Take every frame that is ready, returning how many carry a bitstream. The
// session pins a frame's storage until it is released, and stops accepting
// frames when it has none left.
uint32_t Retire(enctest::Session& s) {
    uint32_t encoded = 0;
    for (;;) {
        Expected<EncodedFrame> got = s.bitstream->AcquireNext();
        if (!got) {
            return encoded;
        }
        if ((got->outcome == ResultCode::Ok) && !got->bitstream.empty()) {
            ++encoded;
        }
        s.bitstream->Release(got->frameId);
    }
}

// One case: |kFrames| frames, each acquired from the producer through a
// SYNC_FD and released back through another.
//
// THE FENCES ARE STILL PENDING WHEN THEY ARE IMPORTED. Frame N's release is
// polled only after frame N+1 is submitted, and frame N+1 waits on it as well
// as on the producer's acquire, so the encoder's queue waits on a fence its
// own submission exported before that fence has signalled, and so does the
// producer's. A fence imported after it signalled tests less: a driver can
// treat it as no wait at all.
//
// |describe| gives frame N's image, |prepare| runs before the producer's
// submission of frame N, and |submitted| is told that the session accepted it.
void RunFrames(enctest::Report& report, const char* path, enctest::Session& s, const Device& encoder, Producer& producer,
               const std::function<ExternalImage(uint32_t)>& describe, const std::function<void(uint32_t)>& prepare,
               const std::function<void(uint32_t)>& submitted) {
    EncoderFences fences;
    if (!fences.Create(encoder)) {
        report.Check(false, "the encoder-side semaphores are created");
        return;
    }

    uint32_t acquired = 0;
    uint32_t released = 0;
    uint32_t encoded = 0;
    VkSemaphore producerWait = VK_NULL_HANDLE;
    bool chained = false;
    int pendingFd = -1;
    bool pending = false;

    // Frame |frame|'s release, exported as |fd|, signals; the process ends if
    // it does not, since the session cannot be torn down around it.
    auto settle = [&](int fd, uint32_t frame) {
        const bool signalled = Signals(fd);
        if (fd >= 0) {
            close(fd);
        }
        if (!signalled) {
            report.Check(false, OfFrame(std::string("the ") + path + " release fence", frame, " signals within 5 s").c_str());
            std::fflush(stderr);
            std::_Exit(report.Summarise(kName));
        }
        ++released;
        encoded += Retire(s);
    };

    for (uint32_t f = 0; f < kFrames; f++) {
        const uint32_t slot = f % kRing;
        const char* why = "";
        prepare(f);

        int acquireFd = -1;
        if (!producer.Submit(f, producerWait, slot, &acquireFd, &why)) {
            report.Check(false, why);
            break;
        }
        if (encoder.ImportSyncFd(fences.wait[slot], acquireFd) != VK_SUCCESS) {
            report.Check(false, OfFrame("the acquire fence", f, " imports into the encoder's device").c_str());
            break;
        }
        ++acquired;

        SemaphoreWait waitOn[2];
        uint32_t waits = 0;
        waitOn[waits++].semaphore = fences.wait[slot];
        if (chained) {
            waitOn[waits++].semaphore = fences.chain[slot];
        }
        SemaphoreWait signalOn[1];
        signalOn[0].semaphore = fences.signal[slot];
        FrameSubmit frame;
        frame.frameId = f;
        frame.pts = static_cast<uint64_t>(f) * 3000;
        frame.image = describe(f);
        frame.waitSemaphores = ArrayView<SemaphoreWait>(waitOn, waits);
        frame.signalSemaphores = ArrayView<SemaphoreWait>(signalOn, 1);
        const Result result = s.submitter->SubmitFrame(frame);
        if (!result) {
            report.Check(false, OfFrame("the submission", f, std::string(" is accepted: ") + result.detail()).c_str());
            // The imported acquire is waited on by nothing now; the semaphore
            // is destroyed with it pending, which a temporary import allows.
            break;
        }
        submitted(f);

        int releaseFd = -1;
        const VkResult exported = encoder.ExportSyncFd(fences.signal[slot], &releaseFd);
        if (exported != VK_SUCCESS) {
            report.Check(false, OfFrame(std::string("the ") + path + " release fence", f,
                                        " exports as a SYNC_FD (VkResult " + std::to_string(exported) + ")")
                                    .c_str());
            std::fflush(stderr);
            std::_Exit(report.Summarise(kName));
        }

        // The producer's next submission and the encoder's next frame each
        // wait on a copy on the GPU; the host polls the original once the
        // next frame is submitted.
        const int toProducer = (releaseFd >= 0) ? dup(releaseFd) : -1;
        const int toEncoder = (releaseFd >= 0) ? dup(releaseFd) : -1;
        producerWait = producer.ImportRelease(slot, toProducer);
        chained = (encoder.ImportSyncFd(fences.chain[(f + 1) % kRing], toEncoder) == VK_SUCCESS);

        if (pending) {
            settle(pendingFd, f - 1);
        }
        pendingFd = releaseFd;
        pending = true;

        if ((producerWait == VK_NULL_HANDLE) || !chained) {
            report.Check(false, OfFrame("the release fence", f, " imports into the producer's and the encoder's devices").c_str());
            break;
        }
    }
    if (pending) {
        settle(pendingFd, acquired - 1);
    }
    fprintf(stderr, "        %s: %u of %u acquire fences imported, %u of %u release fences signalled\n", path, acquired, kFrames,
            released, kFrames);
    report.Check(acquired == kFrames, "every acquire fence is imported and waited on");
    report.Check(released == kFrames, "every release fence signals");

    report.Check(static_cast<bool>(s.session->Drain()), "the drain completes");
    encoded += Retire(s);
    fprintf(stderr, "        %s: %u of %u frames encoded\n", path, encoded, released);
    report.Check((released > 0) && (encoded == released), "every released frame is encoded");
}

}  // namespace

int main(int argc, const char** argv) {
    (void)argc;
    (void)argv;

    enctest::Report report;

    Instance instance;
    const char* why = "";
    if (!instance.Create(&why)) {
        return enctest::NoDeviceOrFail(why, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR);
    }
    if (!instance.SyncFdSemaphores()) {
        report.Skip("every check", "SYNC_FD semaphores are not both exportable and importable");
        return report.Summarise(kName);
    }

    PlatformCreateInfo adopt;
    adopt.instance = instance.instance;
    adopt.physicalDevice = instance.phys;

    // ---- direct: the encode queue waits and signals ----
    fprintf(stderr, "direct (OPTIMAL image, encode queue)\n");
    const char* direct = "";
    if (!instance.DirectImageShareable(&direct)) {
        report.Skip("the direct path", direct);
    } else {
        Producer producer(instance);
        enctest::Session s;
        Device encoder;
        ImportedImage image;
        if (!producer.Create(true, &why)) {
            report.Check(false, why);
        } else if (s.Open(kWidth, kHeight, &why, adopt) != enctest::Session::Ready) {
            report.Check(false, why);
        } else if (!encoder.Load(instance.GetDeviceProcAddr, s.binding->Device())) {
            report.Check(false, "the encoder's device entry points load");
        } else if (!image.Import(encoder, producer, &why)) {
            report.Check(false, why);
        } else {
            report.Check(s.binding->PhysicalDevice() == instance.phys, "the session runs on the adopted physical device");
            RunFrames(
                report, "direct", s, encoder, producer, [&image](uint32_t) { return image.Describe(); }, [](uint32_t) {},
                [](uint32_t) {});
            encoder.DeviceWaitIdle(encoder.device);
        }
    }

    // ---- staged: the staging submission waits and signals ----
    fprintf(stderr, "staged (LINEAR image, staging queue)\n");
    {
        Producer producer(instance);
        enctest::Session s;
        Device encoder;
        enctest::DeviceFns fns;
        if (!producer.Create(false, &why)) {
            report.Check(false, why);
        } else if (s.Open(kWidth, kHeight, &why, adopt) != enctest::Session::Ready) {
            report.Check(false, why);
        } else if (!encoder.Load(instance.GetDeviceProcAddr, s.binding->Device()) || !fns.Load(s.binding)) {
            report.Check(false, "the encoder's device entry points load");
        } else {
            // Two images, used in turn: the host writes frame N+1 while frame
            // N's release is still pending, and only frame N-1's has been seen.
            enctest::HostImage first(fns, s.binding->PhysicalDevice(), s.binding->Device(), kWidth, kHeight);
            enctest::HostImage second(fns, s.binding->PhysicalDevice(), s.binding->Device(), kWidth, kHeight);
            enctest::HostImage* images[2] = {&first, &second};
            if (!first.Create(&why) || !second.Create(&why)) {
                report.Check(false, why);
            } else {
                RunFrames(
                    report, "staged", s, encoder, producer, [&images](uint32_t f) { return images[f % 2]->Describe(); },
                    [&images](uint32_t f) {
                        const char* ignored = "";
                        images[f % 2]->WritePattern(f, &ignored);
                    },
                    [&images](uint32_t f) { images[f % 2]->MarkSubmitted(); });
                encoder.DeviceWaitIdle(encoder.device);
            }
        }
    }

    return report.Summarise(kName);
}
