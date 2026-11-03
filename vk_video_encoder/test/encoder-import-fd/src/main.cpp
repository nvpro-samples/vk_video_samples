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

// fd-backed images through IFrameSubmitter::SubmitFrame.
//
// THE PROPERTY. An image whose memory another device exported as an OPAQUE_FD
// or a DMA_BUF is submitted by its fd alone: the session imports it onto its
// own device, encodes every frame, and treats the fd as ExternalImage::ownership
// says -- closed by the library under Transfer, on success and on refusal
// alike, and left to the caller under Borrow. A description the device cannot
// import is refused with a Result, never a crash.
//
// THE PRODUCER is a second VkDevice on the session's physical device. It
// creates each image with exportable memory, writes a pattern into it with a
// copy and releases it to VK_QUEUE_FAMILY_EXTERNAL, then exports a fresh fd for
// every submission. The session's device never sees the producer's VkImage.
//
// THE CASES. OPAQUE_FD with a LINEAR image (the staged copy), OPAQUE_FD with an
// OPTIMAL image the encoder reads directly, and DMA_BUF -- LINEAR when the
// device exports a LINEAR dma-buf, otherwise a DRM format modifier whose tiling
// the encoder reads. A case the device cannot export or import reports SKIP;
// so does the whole run when no device encodes H.264.
//
// A CLOSED fd IS ONE fcntl(F_GETFD) REFUSES WITH EBADF. Each fd is moved to a
// number at or above kParkedFdFloor before it is submitted, so a descriptor the
// library or the driver opens in the meantime takes a lower number and cannot
// make a closed fd look open.

#include "encoder_test_support.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vk::video::enc;

namespace {

const char* const kName = "encoder-import-fd";

const uint32_t kWidth = 320;
const uint32_t kHeight = 240;
const uint32_t kFrames = 12;
const uint32_t kImages = 2;
const int kParkedFdFloor = 512;

const VkFormat kFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

// What the encoder states for an image it reads directly
// (VkVideoEncoder::WrapExternalImage): the import must be created with them.
const VkImageUsageFlags kDirectUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                       VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
const VkImageCreateFlags kDirectFlags =
    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
const VkImageUsageFlags kStagedUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

// The formats a MUTABLE_FORMAT NV12 image declares: itself and its planes.
const VkFormat kViewFormats[] = {kFormat, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};

bool IsOpen(int fd) { return fcntl(fd, F_GETFD) != -1; }
bool IsClosed(int fd) { return (fcntl(fd, F_GETFD) == -1) && (errno == EBADF); }

// |fd| moved to a number at or above kParkedFdFloor; the original is closed.
int Park(int fd) {
    if (fd < 0) {
        return fd;
    }
    const int parked = fcntl(fd, F_DUPFD_CLOEXEC, kParkedFdFloor);
    if (parked < 0) {
        return fd;
    }
    close(fd);
    return parked;
}

//=============================================================================
// THE INSTANCE THE TEST OWNS AND THE LIBRARY ADOPTS
//=============================================================================

struct Instance {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t producerFamily = UINT32_MAX;
    uint8_t deviceUuid[VK_UUID_SIZE] = {};
    uint8_t driverUuid[VK_UUID_SIZE] = {};

    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2 = nullptr;

    ~Instance() {
        if (instance != VK_NULL_HANDLE) {
            DestroyInstance(instance, nullptr);
        }
    }

    // The first Vulkan 1.3 device with a queue family that encodes H.264 and
    // one that copies, which is where the producer runs.
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
        INSTANCE_LOAD(GetPhysicalDeviceProperties2);
        INSTANCE_LOAD(GetPhysicalDeviceMemoryProperties);
        INSTANCE_LOAD(GetPhysicalDeviceFormatProperties2);
        INSTANCE_LOAD(GetPhysicalDeviceImageFormatProperties2);
#undef INSTANCE_LOAD
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
        auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
            gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties2"));
        if (!DestroyInstance || !GetDeviceProcAddr || !CreateDevice || !EnumerateDeviceExtensionProperties ||
            !GetPhysicalDeviceFeatures2 || !GetPhysicalDeviceProperties2 || !GetPhysicalDeviceMemoryProperties ||
            !GetPhysicalDeviceFormatProperties2 || !GetPhysicalDeviceImageFormatProperties2 || !enumerate || !queues) {
            *why = "the instance entry points do not load";
            return false;
        }

        uint32_t count = 0;
        enumerate(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        enumerate(instance, &count, devices.data());
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p.pNext = &ids;
            GetPhysicalDeviceProperties2(candidate, &p);
            if (p.properties.apiVersion < VK_API_VERSION_1_3) {
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
                std::memcpy(deviceUuid, ids.deviceUUID, VK_UUID_SIZE);
                std::memcpy(driverUuid, ids.driverUUID, VK_UUID_SIZE);
                return true;
            }
        }
        *why = "no Vulkan 1.3 device with an H.264 encode queue and a queue that copies";
        return false;
    }

    bool HasExtension(const char* name) const {
        uint32_t count = 0;
        EnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        EnumerateDeviceExtensionProperties(phys, nullptr, &count, available.data());
        for (const VkExtensionProperties& e : available) {
            if (std::strcmp(e.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    }

    bool VideoMaintenance1() const {
        VkPhysicalDeviceVideoMaintenance1FeaturesKHR maintenance1{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &maintenance1;
        GetPhysicalDeviceFeatures2(phys, &features);
        return maintenance1.videoMaintenance1 == VK_TRUE;
    }

    // The modifiers of kFormat whose tiling the encoder reads and the producer
    // writes, in the order the device lists them.
    std::vector<VkDrmFormatModifierPropertiesEXT> EncodableModifiers() const {
        VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
        VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        props.pNext = &list;
        GetPhysicalDeviceFormatProperties2(phys, kFormat, &props);
        std::vector<VkDrmFormatModifierPropertiesEXT> all(list.drmFormatModifierCount);
        list.pDrmFormatModifierProperties = all.data();
        GetPhysicalDeviceFormatProperties2(phys, kFormat, &props);
        std::vector<VkDrmFormatModifierPropertiesEXT> usable;
        const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_VIDEO_ENCODE_INPUT_BIT_KHR;
        for (const VkDrmFormatModifierPropertiesEXT& m : all) {
            if ((m.drmFormatModifierTilingFeatures & needed) == needed) {
                usable.push_back(m);
            }
        }
        return usable;
    }
};

//=============================================================================
// WHAT ONE CASE EXPORTS
//=============================================================================

struct ImageSpec {
    const char* name = "";
    VkExternalMemoryHandleTypeFlagBits handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageTiling tiling = VK_IMAGE_TILING_LINEAR;
    VkImageUsageFlags usage = kStagedUsage;
    VkImageCreateFlags flags = 0;
    bool hasModifier = false;
    uint64_t modifier = 0;
    uint32_t modifierPlaneCount = 0;
    // Where the producer leaves the image, and what the frame declares.
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
};

// Whether the device creates |spec|'s image with memory it both exports and
// imports through the spec's handle type.
bool Shareable(const Instance& instance, const ImageSpec& spec) {
    VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    formatList.viewFormatCount = 3;
    formatList.pViewFormats = kViewFormats;
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
    modifier.drmFormatModifier = spec.modifier;
    modifier.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    external.handleType = spec.handleType;
    const void* chain = nullptr;
    if ((spec.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) {
        formatList.pNext = chain;
        chain = &formatList;
    }
    if (spec.hasModifier) {
        modifier.pNext = chain;
        chain = &modifier;
    }
    external.pNext = chain;
    VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    info.pNext = &external;
    info.format = kFormat;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = spec.tiling;
    info.usage = spec.usage;
    info.flags = spec.flags;
    VkExternalImageFormatProperties externalProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    props.pNext = &externalProps;
    if (instance.GetPhysicalDeviceImageFormatProperties2(instance.phys, &info, &props) != VK_SUCCESS) {
        return false;
    }
    const VkExternalMemoryFeatureFlags both = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    return (externalProps.externalMemoryProperties.externalMemoryFeatures & both) == both;
}

//=============================================================================
// THE PRODUCER: A SECOND DEVICE THAT WRITES AND EXPORTS THE IMAGES
//=============================================================================

#define DEVICE_FUNCTIONS(X)        \
    X(DestroyDevice)               \
    X(GetDeviceQueue)              \
    X(DeviceWaitIdle)              \
    X(CreateImage)                 \
    X(DestroyImage)                \
    X(GetImageMemoryRequirements2) \
    X(GetImageSubresourceLayout)   \
    X(AllocateMemory)              \
    X(FreeMemory)                  \
    X(BindImageMemory)             \
    X(MapMemory)                   \
    X(CreateBuffer)                \
    X(DestroyBuffer)               \
    X(GetBufferMemoryRequirements) \
    X(BindBufferMemory)            \
    X(CreateCommandPool)           \
    X(DestroyCommandPool)          \
    X(AllocateCommandBuffers)      \
    X(BeginCommandBuffer)          \
    X(EndCommandBuffer)            \
    X(CmdPipelineBarrier)          \
    X(CmdCopyBufferToImage)        \
    X(QueueSubmit)                 \
    X(QueueWaitIdle)               \
    X(GetMemoryFdKHR)

class Producer {
   public:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        uint32_t memoryType = UINT32_MAX;
        uint32_t planeCount = 0;
        PlaneLayout planes[kMaxPlanes] = {};
    };

    explicit Producer(const Instance& instance) : m_instance(instance) {}

    ~Producer() {
        if (m_device == VK_NULL_HANDLE) {
            return;
        }
        DeviceWaitIdle(m_device);
        for (Image& i : m_images) {
            DestroyImage(m_device, i.image, nullptr);
            FreeMemory(m_device, i.memory, nullptr);
        }
        DestroyCommandPool(m_device, m_pool, nullptr);
        DestroyBuffer(m_device, m_staging, nullptr);
        FreeMemory(m_device, m_stagingMemory, nullptr);
        DestroyDevice(m_device, nullptr);
    }

    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    // Every extension the cases may need that the device has: the image
    // memory exports, and the video extensions and videoMaintenance1 an image
    // the encoder reads directly declares.
    bool Create(const char** why) {
        std::vector<const char*> extensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
        for (const char* optional : {VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                                     VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
                                     VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME}) {
            if (m_instance.HasExtension(optional)) {
                extensions.push_back(optional);
            }
        }
        if (!m_instance.HasExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
            *why = "the device lacks " VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
            return false;
        }
        m_videoMaintenance1 = m_instance.HasExtension(VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME) && m_instance.VideoMaintenance1();

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = m_instance.producerFamily;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkPhysicalDeviceVideoMaintenance1FeaturesKHR maintenance1{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR};
        maintenance1.videoMaintenance1 = VK_TRUE;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = m_videoMaintenance1 ? &maintenance1 : nullptr;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &queue;
        dci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        dci.ppEnabledExtensionNames = extensions.data();
        if (m_instance.CreateDevice(m_instance.phys, &dci, nullptr, &m_device) != VK_SUCCESS) {
            m_device = VK_NULL_HANDLE;
            *why = "the producer's vkCreateDevice failed";
            return false;
        }
        bool loaded = true;
#define DEVICE_LOAD(name)                                                                      \
    name = reinterpret_cast<PFN_vk##name>(m_instance.GetDeviceProcAddr(m_device, "vk" #name)); \
    loaded = loaded && (name != nullptr);
        DEVICE_FUNCTIONS(DEVICE_LOAD)
#undef DEVICE_LOAD
        m_getModifier = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
            m_instance.GetDeviceProcAddr(m_device, "vkGetImageDrmFormatModifierPropertiesEXT"));
        if (!loaded) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(m_instance.GetDeviceProcAddr(m_device, "vkDestroyDevice"));
            if (destroy != nullptr) {
                destroy(m_device, nullptr);
            }
            m_device = VK_NULL_HANDLE;
            *why = "the producer's device entry points do not load";
            return false;
        }
        GetDeviceQueue(m_device, m_instance.producerFamily, 0, &m_queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = m_instance.producerFamily;
        if (CreateCommandPool(m_device, &pci, nullptr, &m_pool) != VK_SUCCESS) {
            *why = "the producer's command pool";
            return false;
        }
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = m_pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (AllocateCommandBuffers(m_device, &cai, &m_cmd) != VK_SUCCESS) {
            *why = "the producer's command buffer";
            return false;
        }

        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = kWidth * kHeight * 3 / 2;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (CreateBuffer(m_device, &bci, nullptr, &m_staging) != VK_SUCCESS) {
            *why = "the producer's staging buffer";
            return false;
        }
        VkMemoryRequirements breq{};
        GetBufferMemoryRequirements(m_device, m_staging, &breq);
        VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bai.allocationSize = breq.size;
        bai.memoryTypeIndex =
            FindType(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if ((bai.memoryTypeIndex == UINT32_MAX) || (AllocateMemory(m_device, &bai, nullptr, &m_stagingMemory) != VK_SUCCESS) ||
            (BindBufferMemory(m_device, m_staging, m_stagingMemory, 0) != VK_SUCCESS) ||
            (MapMemory(m_device, m_stagingMemory, 0, VK_WHOLE_SIZE, 0, &m_mapped) != VK_SUCCESS)) {
            *why = "the producer's host-visible staging memory";
            return false;
        }
        return true;
    }

    bool VideoMaintenance1() const { return m_videoMaintenance1; }
    bool HasModifierQuery() const { return m_getModifier != nullptr; }

    // Create |spec|'s image with exportable memory and write frame
    // |pattern|'s picture into it, leaving it in spec.layout and released to
    // VK_QUEUE_FAMILY_EXTERNAL.
    bool AddImage(const ImageSpec& spec, uint32_t pattern, const char** why) {
        Image image;
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = spec.handleType;
        VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        formatList.viewFormatCount = 3;
        formatList.pViewFormats = kViewFormats;
        VkImageDrmFormatModifierListCreateInfoEXT modifierList{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
        modifierList.drmFormatModifierCount = 1;
        modifierList.pDrmFormatModifiers = &spec.modifier;
        const void* chain = nullptr;
        if ((spec.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) {
            formatList.pNext = chain;
            chain = &formatList;
        }
        if (spec.hasModifier) {
            modifierList.pNext = chain;
            chain = &modifierList;
        }
        external.pNext = chain;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.pNext = &external;
        ci.flags = spec.flags;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kFormat;
        ci.extent = {kWidth, kHeight, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = spec.tiling;
        ci.usage = spec.usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (CreateImage(m_device, &ci, nullptr, &image.image) != VK_SUCCESS) {
            *why = "the producer's vkCreateImage refused the image";
            return false;
        }
        m_images.push_back(image);
        Image& made = m_images.back();

        VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        reqInfo.image = made.image;
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        GetImageMemoryRequirements2(m_device, &reqInfo, &req);
        made.memoryType = FindType(req.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (made.memoryType == UINT32_MAX) {
            made.memoryType = FindType(req.memoryRequirements.memoryTypeBits, 0);
        }
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = made.image;
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportInfo.pNext = &dedicated;
        exportInfo.handleTypes = spec.handleType;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &exportInfo;
        ai.allocationSize = req.memoryRequirements.size;
        ai.memoryTypeIndex = made.memoryType;
        if ((made.memoryType == UINT32_MAX) || (AllocateMemory(m_device, &ai, nullptr, &made.memory) != VK_SUCCESS) ||
            (BindImageMemory(m_device, made.image, made.memory, 0) != VK_SUCCESS)) {
            *why = "the producer's exportable image memory";
            return false;
        }
        made.size = req.memoryRequirements.size;

        if (spec.hasModifier) {
            // The memory planes the modifier lays the image out in, which is
            // what an importer names back.
            VkImageDrmFormatModifierPropertiesEXT chosen{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
            if ((m_getModifier == nullptr) || (m_getModifier(m_device, made.image, &chosen) != VK_SUCCESS) ||
                (chosen.drmFormatModifier != spec.modifier) || (spec.modifierPlaneCount > kMaxPlanes)) {
                *why = "the producer's image does not report the modifier it was created with";
                return false;
            }
            made.planeCount = spec.modifierPlaneCount;
            for (uint32_t p = 0; p < made.planeCount; p++) {
                VkImageSubresource sub{};
                sub.aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p);
                VkSubresourceLayout layout{};
                GetImageSubresourceLayout(m_device, made.image, &sub, &layout);
                made.planes[p].offset = layout.offset;
                made.planes[p].rowPitch = layout.rowPitch;
                made.planes[p].size = layout.size;
            }
        }
        return Write(made, spec.layout, pattern, why);
    }

    const Image& ImageAt(uint32_t index) const { return m_images[index % m_images.size()]; }

    // A new fd for image |index|'s memory, parked above kParkedFdFloor.
    int Export(uint32_t index, VkExternalMemoryHandleTypeFlagBits handleType) const {
        VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        info.memory = ImageAt(index).memory;
        info.handleType = handleType;
        int fd = -1;
        if (GetMemoryFdKHR(m_device, &info, &fd) != VK_SUCCESS) {
            return -1;
        }
        return Park(fd);
    }

   private:
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

    // A bar whose position depends on |pattern|, over a luma ramp, on
    // neutral chroma; copied in, then released in |layout|.
    bool Write(const Image& image, VkImageLayout layout, uint32_t pattern, const char** why) {
        uint8_t* luma = static_cast<uint8_t*>(m_mapped);
        uint8_t* chroma = luma + kWidth * kHeight;
        const uint32_t bar = (pattern * 96) % kWidth;
        for (uint32_t y = 0; y < kHeight; y++) {
            for (uint32_t x = 0; x < kWidth; x++) {
                const bool inBar = (x >= bar) && (x < bar + 32);
                luma[y * kWidth + x] = inBar ? 235 : static_cast<uint8_t>(16 + (x * 200) / kWidth);
            }
        }
        std::memset(chroma, 128, kWidth * kHeight / 2);

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (BeginCommandBuffer(m_cmd, &begin) != VK_SUCCESS) {
            *why = "the producer's command buffer does not begin";
            return false;
        }
        VkImageMemoryBarrier toCopy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toCopy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.image = image.image;
        toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &toCopy);
        VkBufferImageCopy regions[2]{};
        regions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
        regions[0].imageExtent = {kWidth, kHeight, 1};
        regions[1].bufferOffset = kWidth * kHeight;
        regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
        regions[1].imageExtent = {kWidth / 2, kHeight / 2, 1};
        CmdCopyBufferToImage(m_cmd, m_staging, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);
        VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        release.newLayout = layout;
        release.srcQueueFamilyIndex = m_instance.producerFamily;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        release.image = image.image;
        release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &release);
        if (EndCommandBuffer(m_cmd) != VK_SUCCESS) {
            *why = "the producer's command buffer does not end";
            return false;
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &m_cmd;
        if ((QueueSubmit(m_queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) || (QueueWaitIdle(m_queue) != VK_SUCCESS)) {
            *why = "the producer's copy did not complete";
            return false;
        }
        return true;
    }

    const Instance& m_instance;
    VkDevice m_device = VK_NULL_HANDLE;
#define DEVICE_DECLARE(name) PFN_vk##name name = nullptr;
    DEVICE_FUNCTIONS(DEVICE_DECLARE)
#undef DEVICE_DECLARE
    PFN_vkGetImageDrmFormatModifierPropertiesEXT m_getModifier = nullptr;
    bool m_videoMaintenance1 = false;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkBuffer m_staging = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    void* m_mapped = nullptr;
    std::vector<Image> m_images;
};

//=============================================================================
// ONE CASE
//=============================================================================

// How |spec|'s image |index| is described to SubmitFrame, carrying |fd|.
ExternalImage Describe(const Instance& instance, const ImageSpec& spec, const Producer::Image& image, int fd,
                       HandleOwnership ownership) {
    ExternalImage d;
    d.handleType = (spec.handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) ? ExternalHandleType::DmaBuf
                                                                                       : ExternalHandleType::OpaqueFd;
    d.fd = fd;
    d.ownership = ownership;
    d.format = kFormat;
    d.width = kWidth;
    d.height = kHeight;
    d.tiling = spec.tiling;
    d.layout = spec.layout;
    d.colorModel = ColorModel::YCbCr;
    d.usage = spec.usage;
    d.createFlags = spec.flags;
    // The producer released the image to VK_QUEUE_FAMILY_EXTERNAL on the same
    // physical device; the encoder takes it without a FOREIGN acquire.
    d.residency = Residency::Local;
    d.hasDrmFormatModifier = spec.hasModifier;
    d.drmFormatModifier = spec.modifier;
    d.planeCount = image.planeCount;
    for (uint32_t p = 0; p < image.planeCount; p++) {
        d.planeLayouts[p] = image.planes[p];
    }
    d.allocationSize = image.size;
    d.memoryTypeIndex = image.memoryType;
    std::memcpy(d.deviceUUID, instance.deviceUuid, VK_UUID_SIZE);
    std::memcpy(d.driverUUID, instance.driverUuid, VK_UUID_SIZE);
    return d;
}

// Take every frame that is ready; count those that carry an encode.
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

std::string Label(const ImageSpec& spec, const char* what) { return std::string(spec.name) + ": " + what; }

// One refusal: |image| is submitted, must be refused, and its fd must end up
// as |expectClosed| says.
void ExpectRefused(enctest::Report& report, enctest::Session& s, const ImageSpec& spec, const ExternalImage& image,
                   uint64_t frameId, const char* what, bool expectClosed) {
    FrameSubmit frame;
    frame.frameId = frameId;
    frame.pts = frameId * 3000;
    frame.image = image;
    const Result result = s.submitter->SubmitFrame(frame);
    fprintf(stderr, "        %s: %s -> code %d (%s)\n", spec.name, what, static_cast<int>(result.code()), result.detail());
    report.Check(!result.ok(), Label(spec, (std::string(what) + " is refused").c_str()).c_str());
    if (expectClosed) {
        report.Check(IsClosed(image.fd), Label(spec, (std::string(what) + ": Transfer closed the fd").c_str()).c_str());
    } else {
        report.Check(IsOpen(image.fd), Label(spec, (std::string(what) + ": the fd is still open").c_str()).c_str());
        close(image.fd);
    }
}

void RunCase(enctest::Report& report, const Instance& instance, const ImageSpec& spec, HandleOwnership ownership, bool refusals) {
    const char* why = "";
    Producer producer(instance);
    if (!producer.Create(&why)) {
        report.Check(false, why);
        return;
    }
    for (uint32_t i = 0; i < kImages; i++) {
        if (!producer.AddImage(spec, i, &why)) {
            report.Check(false, Label(spec, why).c_str());
            return;
        }
    }

    // Declared after the producer, so the session -- and every image it
    // imported -- goes first.
    PlatformCreateInfo adopt;
    adopt.instance = instance.instance;
    adopt.physicalDevice = instance.phys;
    enctest::Session s;
    if (s.Open(kWidth, kHeight, &why, adopt) != enctest::Session::Ready) {
        report.Check(false, why);
        return;
    }
    uint8_t sessionUuid[VK_UUID_SIZE] = {};
    report.Check(s.binding->DeviceUuid(sessionUuid) && (std::memcmp(sessionUuid, instance.deviceUuid, VK_UUID_SIZE) == 0),
                 Label(spec, "the session runs on the exporting physical device").c_str());

    const bool transfer = (ownership == HandleOwnership::Transfer);
    uint32_t accepted = 0;
    uint32_t submissions = 0;
    uint32_t ownershipBroken = 0;
    uint32_t encoded = 0;
    uint64_t frameId = 0;
    for (uint32_t f = 0; f < kFrames; f++) {
        Result result(ResultCode::NotReady);
        // NotReady is flow control: retire what is ready and submit again,
        // with a new fd under Transfer because the refused one is gone.
        for (uint32_t attempt = 0; (attempt < 100) && (result.code() == ResultCode::NotReady); attempt++) {
            const int fd = producer.Export(f, spec.handleType);
            if (fd < 0) {
                report.Check(false, Label(spec, "the producer exports an fd").c_str());
                return;
            }
            FrameSubmit frame;
            frame.frameId = frameId;
            frame.pts = frameId * 3000;
            frame.image = Describe(instance, spec, producer.ImageAt(f), fd, ownership);
            result = s.submitter->SubmitFrame(frame);
            ++submissions;
            if (!(transfer ? IsClosed(fd) : IsOpen(fd))) {
                ++ownershipBroken;
                fprintf(stderr, "        %s: frame %u: the fd is %s after SubmitFrame\n", spec.name, f,
                        transfer ? "still open" : "closed");
            }
            if (!transfer) {
                close(fd);
            }
            if (result.code() == ResultCode::NotReady) {
                encoded += Retire(s);
            }
        }
        if (result.ok()) {
            ++accepted;
            ++frameId;
        } else {
            fprintf(stderr, "        %s: frame %u refused: code %d (%s)\n", spec.name, f, static_cast<int>(result.code()),
                    result.detail());
        }
        encoded += Retire(s);
    }
    report.Check(accepted == kFrames, Label(spec, "every frame is accepted").c_str());
    fprintf(stderr, "        %s: %u submissions for %u frames\n", spec.name, submissions, kFrames);
    report.Check((submissions >= kFrames) && (ownershipBroken == 0),
                 Label(spec, transfer ? "Transfer closes the fd of every submission, NotReady included"
                                      : "Borrow leaves the fd of every submission open")
                     .c_str());

    if (refusals) {
        // Each description below is the accepted one with one thing wrong.
        ExternalImage small = Describe(instance, spec, producer.ImageAt(0), producer.Export(0, spec.handleType), ownership);
        small.allocationSize = 1;
        ExpectRefused(report, s, spec, small, frameId, "an allocation smaller than the image", transfer);

        ExternalImage otherDevice = Describe(instance, spec, producer.ImageAt(0), producer.Export(0, spec.handleType), ownership);
        otherDevice.deviceUUID[0] ^= 0xFF;
        ExpectRefused(report, s, spec, otherDevice, frameId, "an image of another device", transfer);

        ExternalImage modifierOnLinear =
            Describe(instance, spec, producer.ImageAt(0), producer.Export(0, spec.handleType), ownership);
        modifierOnLinear.hasDrmFormatModifier = !modifierOnLinear.hasDrmFormatModifier;
        ExpectRefused(report, s, spec, modifierOnLinear, frameId, "a modifier that disagrees with the tiling", transfer);

        ExternalImage wrongFormat = Describe(instance, spec, producer.ImageAt(0), producer.Export(0, spec.handleType), ownership);
        wrongFormat.format = VK_FORMAT_R8G8B8A8_UNORM;
        ExpectRefused(report, s, spec, wrongFormat, frameId, "a format other than the exporter's", transfer);

        ExternalImage borrowed =
            Describe(instance, spec, producer.ImageAt(0), producer.Export(0, spec.handleType), HandleOwnership::Borrow);
        borrowed.allocationSize = 1;
        ExpectRefused(report, s, spec, borrowed, frameId, "a Borrow description with a short allocation", false);

        ExternalImage unknownOwnership =
            Describe(instance, spec, producer.ImageAt(0), producer.Export(0, spec.handleType), ownership);
        unknownOwnership.ownership = static_cast<HandleOwnership>(7);
        ExpectRefused(report, s, spec, unknownOwnership, frameId, "an ownership outside the enumeration", false);

        FrameSubmit noFd;
        noFd.frameId = frameId;
        noFd.image = Describe(instance, spec, producer.ImageAt(0), -1, ownership);
        report.Check(s.submitter->SubmitFrame(noFd) == ResultCode::InvalidArgument,
                     Label(spec, "an fd-backed image with no fd is refused as an invalid argument").c_str());

        // The session is unharmed by the refusals: one more frame encodes.
        const int fd = producer.Export(1, spec.handleType);
        FrameSubmit after;
        after.frameId = frameId;
        after.pts = frameId * 3000;
        after.image = Describe(instance, spec, producer.ImageAt(1), fd, ownership);
        const Result afterResult = s.submitter->SubmitFrame(after);
        report.Check(afterResult.ok(), Label(spec, "a valid frame after the refusals is accepted").c_str());
        if (!transfer) {
            close(fd);
        }
        if (afterResult.ok()) {
            ++accepted;
            ++frameId;
        }
    }

    report.Check(s.session->Drain().ok(), Label(spec, "the drain completes").c_str());
    encoded += Retire(s);
    fprintf(stderr, "        %s: %u of %u accepted frames encoded\n", spec.name, encoded, accepted);
    report.Check((accepted > 0) && (encoded == accepted), Label(spec, "every accepted frame is encoded").c_str());
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

    // ---- OPAQUE_FD, LINEAR: the staged copy, Transfer, and the refusals ----
    ImageSpec opaqueLinear;
    opaqueLinear.name = "OPAQUE_FD LINEAR";
    fprintf(stderr, "%s (Transfer)\n", opaqueLinear.name);
    if (!Shareable(instance, opaqueLinear)) {
        report.Skip(opaqueLinear.name, "a LINEAR NV12 image with OPAQUE_FD memory is not both exportable and importable");
    } else {
        RunCase(report, instance, opaqueLinear, HandleOwnership::Transfer, true);
    }

    // ---- OPAQUE_FD, OPTIMAL: read by the encode itself, Borrow ----
    ImageSpec opaqueOptimal;
    opaqueOptimal.name = "OPAQUE_FD OPTIMAL";
    opaqueOptimal.tiling = VK_IMAGE_TILING_OPTIMAL;
    opaqueOptimal.usage = kDirectUsage;
    opaqueOptimal.flags = kDirectFlags;
    opaqueOptimal.layout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
    fprintf(stderr, "%s (Borrow)\n", opaqueOptimal.name);
    if (!instance.VideoMaintenance1() || !Shareable(instance, opaqueOptimal)) {
        report.Skip(opaqueOptimal.name,
                    "videoMaintenance1, or an OPTIMAL encode-source image with OPAQUE_FD memory, is not supported");
    } else {
        RunCase(report, instance, opaqueOptimal, HandleOwnership::Borrow, false);
    }

    // ---- DMA_BUF: LINEAR when the device exports one, else a modifier ----
    ImageSpec dmaBuf;
    dmaBuf.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    dmaBuf.name = "DMA_BUF LINEAR";
    bool haveDmaBuf = instance.HasExtension(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) && Shareable(instance, dmaBuf);
    if (!haveDmaBuf && instance.HasExtension(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
        instance.HasExtension(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) && instance.VideoMaintenance1()) {
        for (const VkDrmFormatModifierPropertiesEXT& m : instance.EncodableModifiers()) {
            ImageSpec candidate;
            candidate.name = "DMA_BUF DRM modifier";
            candidate.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            candidate.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            candidate.usage = kDirectUsage;
            candidate.flags = kDirectFlags;
            candidate.hasModifier = true;
            candidate.modifier = m.drmFormatModifier;
            candidate.modifierPlaneCount = m.drmFormatModifierPlaneCount;
            candidate.layout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
            if (Shareable(instance, candidate)) {
                dmaBuf = candidate;
                haveDmaBuf = true;
                break;
            }
        }
    }
    fprintf(stderr, "%s (Transfer)\n", dmaBuf.name);
    if (!haveDmaBuf) {
        report.Skip("DMA_BUF", "the device exports no NV12 dma-buf the encoder can read, LINEAR or by DRM format modifier");
    } else {
        if (dmaBuf.hasModifier) {
            fprintf(stderr, "        modifier 0x%016llx, %u memory plane(s)\n", static_cast<unsigned long long>(dmaBuf.modifier),
                    dmaBuf.modifierPlaneCount);
        }
        RunCase(report, instance, dmaBuf, HandleOwnership::Transfer, true);
    }

    return report.Summarise(kName);
}
