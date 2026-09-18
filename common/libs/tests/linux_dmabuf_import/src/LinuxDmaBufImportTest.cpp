/*
 * Copyright 2024-2026 NVIDIA Corporation.
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

#if defined(__linux__)

#include "LinuxDmaBufImportTest.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace linux_dmabuf_import_test {

//=============================================================================
// Naming helpers - a failure has to be readable without a debugger
//=============================================================================

const char* stepName(Step s) {
    switch (s) {
        case Step::NotStarted:                 return "-";
        case Step::ExportDeviceInit:     return "ExportDeviceInit";
        case Step::ImportDeviceCreate:   return "ImportDeviceCreate";
        case Step::ImportDeviceDispatch: return "ImportDeviceDispatch";
        case Step::ModifierEnumerate:    return "ModifierEnumerate";
        case Step::ModifierCapability:   return "ModifierCapability";
        case Step::ExportImageCreate:    return "ExportImageCreate";
        case Step::ExportModifierReadback: return "ExportModifierReadback";
        case Step::ExportMemoryAllocate: return "ExportMemoryAllocate";
        case Step::ExportBindMemory:     return "ExportBindMemory";
        case Step::ExportPlaneLayouts:   return "ExportPlaneLayouts";
        case Step::ExportFd:             return "ExportFd";
        case Step::UploadPattern:        return "UploadPattern";
        case Step::ReleaseToForeign:     return "ReleaseToForeign";
        case Step::ImportImageCreate:    return "ImportImageCreate";
        case Step::ImportFdMemoryTypes:  return "ImportFdMemoryTypes";
        case Step::ImportMemoryTypeSelect: return "ImportMemoryTypeSelect";
        case Step::ImportMemoryAllocate: return "ImportMemoryAllocate";
        case Step::ImportBindMemory:     return "ImportBindMemory";
        case Step::AcquireFromForeign:   return "AcquireFromForeign";
        case Step::Readback:             return "Readback";
        case Step::ContentCompare:       return "ContentCompare";
        case Step::Complete:             return "Complete";
    }
    return "?";
}

const char* vkResultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS:                            return "VK_SUCCESS";
        case VK_NOT_READY:                          return "VK_NOT_READY";
        case VK_TIMEOUT:                            return "VK_TIMEOUT";
        case VK_INCOMPLETE:                         return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:           return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:         return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:        return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:                  return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:            return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:            return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:        return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:          return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:          return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:             return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:         return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:              return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN:                      return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY:           return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE:      return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
            return "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
        default: {
            static thread_local char buf[32];
            snprintf(buf, sizeof(buf), "VkResult(%d)", (int)r);
            return buf;
        }
    }
}

const char* formatName(VkFormat format) {
    switch (format) {
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: return "NV12";
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM: return "NV16";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: return "P010";
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16: return "P012";
        case VK_FORMAT_R8G8B8A8_UNORM:           return "RGBA8";
        case VK_FORMAT_B8G8R8A8_UNORM:           return "BGRA8";
        default: {
            static thread_local char buf[32];
            snprintf(buf, sizeof(buf), "VkFormat(%d)", (int)format);
            return buf;
        }
    }
}

// Plane geometry for the content round-trip.
//
// Deliberately excludes VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM (I420): the
// device-independence plan's section 8.1 records that path hanging the GPU on
// hardware, and a test that wedges the machine it runs on is worse than no
// test. An unknown format is reported as "content check unavailable", never
// silently treated as verified.
FormatDesc describeFormat(VkFormat format) {
    FormatDesc d;
    switch (format) {
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
            d.known = true;
            d.planes = {{VK_IMAGE_ASPECT_PLANE_0_BIT, 1, 1, 1},
                        {VK_IMAGE_ASPECT_PLANE_1_BIT, 2, 2, 2}};
            break;
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
            d.known = true;
            d.planes = {{VK_IMAGE_ASPECT_PLANE_0_BIT, 1, 1, 1},
                        {VK_IMAGE_ASPECT_PLANE_1_BIT, 2, 1, 2}};
            break;
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
            d.known = true;
            d.planes = {{VK_IMAGE_ASPECT_PLANE_0_BIT, 1, 1, 2},
                        {VK_IMAGE_ASPECT_PLANE_1_BIT, 2, 2, 4}};
            break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
            d.known = true;
            d.planes = {{VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 4}};
            break;
        default:
            break;
    }
    return d;
}

static std::string modifierToString(uint64_t mod) {
    if (mod == DRM_FORMAT_MOD_LINEAR)  return "LINEAR";
    if (mod == DRM_FORMAT_MOD_INVALID) return "INVALID";
    std::ostringstream os;
    os << "0x" << std::hex << mod;
    return os.str();
}

// Deterministic, position-dependent, and not a constant: a copy that never ran
// cannot pass the comparison by accident.
static inline uint8_t patternByte(uint64_t i) {
    return static_cast<uint8_t>(((i * 131u) + 17u) ^ (i >> 8));
}

//=============================================================================
// DeviceFns
//=============================================================================

bool DeviceFns::loadAll(PFN_vkGetDeviceProcAddr gdpa, VkDevice device,
                        std::string& missingOut)
{
    missingOut.clear();
    if ((gdpa == nullptr) || (device == VK_NULL_HANDLE)) {
        missingOut = "vkGetDeviceProcAddr";
        return false;
    }

    bool ok = true;
    auto get = [&](const char* name) -> PFN_vkVoidFunction {
        PFN_vkVoidFunction p = gdpa(device, name);
        if ((p == nullptr) && ok) {
            ok = false;
            missingOut = name;
        }
        return p;
    };

    GetDeviceQueue              = (PFN_vkGetDeviceQueue)get("vkGetDeviceQueue");
    DestroyDevice               = (PFN_vkDestroyDevice)get("vkDestroyDevice");
    CreateImage                 = (PFN_vkCreateImage)get("vkCreateImage");
    DestroyImage                = (PFN_vkDestroyImage)get("vkDestroyImage");
    GetImageMemoryRequirements  = (PFN_vkGetImageMemoryRequirements)get("vkGetImageMemoryRequirements");
    GetImageSubresourceLayout   = (PFN_vkGetImageSubresourceLayout)get("vkGetImageSubresourceLayout");
    AllocateMemory              = (PFN_vkAllocateMemory)get("vkAllocateMemory");
    FreeMemory                  = (PFN_vkFreeMemory)get("vkFreeMemory");
    BindImageMemory             = (PFN_vkBindImageMemory)get("vkBindImageMemory");
    CreateBuffer                = (PFN_vkCreateBuffer)get("vkCreateBuffer");
    DestroyBuffer               = (PFN_vkDestroyBuffer)get("vkDestroyBuffer");
    GetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)get("vkGetBufferMemoryRequirements");
    BindBufferMemory            = (PFN_vkBindBufferMemory)get("vkBindBufferMemory");
    MapMemory                   = (PFN_vkMapMemory)get("vkMapMemory");
    UnmapMemory                 = (PFN_vkUnmapMemory)get("vkUnmapMemory");
    FlushMappedMemoryRanges     = (PFN_vkFlushMappedMemoryRanges)get("vkFlushMappedMemoryRanges");
    InvalidateMappedMemoryRanges= (PFN_vkInvalidateMappedMemoryRanges)get("vkInvalidateMappedMemoryRanges");
    CreateCommandPool           = (PFN_vkCreateCommandPool)get("vkCreateCommandPool");
    DestroyCommandPool          = (PFN_vkDestroyCommandPool)get("vkDestroyCommandPool");
    AllocateCommandBuffers      = (PFN_vkAllocateCommandBuffers)get("vkAllocateCommandBuffers");
    FreeCommandBuffers          = (PFN_vkFreeCommandBuffers)get("vkFreeCommandBuffers");
    BeginCommandBuffer          = (PFN_vkBeginCommandBuffer)get("vkBeginCommandBuffer");
    EndCommandBuffer            = (PFN_vkEndCommandBuffer)get("vkEndCommandBuffer");
    CmdPipelineBarrier          = (PFN_vkCmdPipelineBarrier)get("vkCmdPipelineBarrier");
    CmdCopyBufferToImage        = (PFN_vkCmdCopyBufferToImage)get("vkCmdCopyBufferToImage");
    CmdCopyImageToBuffer        = (PFN_vkCmdCopyImageToBuffer)get("vkCmdCopyImageToBuffer");
    QueueSubmit                 = (PFN_vkQueueSubmit)get("vkQueueSubmit");
    QueueWaitIdle               = (PFN_vkQueueWaitIdle)get("vkQueueWaitIdle");
    DeviceWaitIdle              = (PFN_vkDeviceWaitIdle)get("vkDeviceWaitIdle");
    CreateFence                 = (PFN_vkCreateFence)get("vkCreateFence");
    DestroyFence                = (PFN_vkDestroyFence)get("vkDestroyFence");
    WaitForFences               = (PFN_vkWaitForFences)get("vkWaitForFences");

    // These three are the extension surface the import path is built on. A
    // null here means the extension was not ENABLED on this device (not merely
    // absent from the physical device), which is precisely the failure mode
    // the plan's section 3.2 calls out as EXTENSION_MISSING.
    GetMemoryFdKHR              = (PFN_vkGetMemoryFdKHR)get("vkGetMemoryFdKHR");
    GetMemoryFdPropertiesKHR    = (PFN_vkGetMemoryFdPropertiesKHR)get("vkGetMemoryFdPropertiesKHR");
    GetImageDrmFormatModifierPropertiesEXT =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)get("vkGetImageDrmFormatModifierPropertiesEXT");

    return ok;
}

//=============================================================================
// Lifecycle
//=============================================================================

LinuxDmaBufImportTest::~LinuxDmaBufImportTest() {
    tearDownDevice(m_devB);
    tearDownDevice(m_devA);
}

void LinuxDmaBufImportTest::tearDownDevice(TestDevice& dev) {
    if (dev.device == VK_NULL_HANDLE) {
        return;
    }
    if (dev.fn.DeviceWaitIdle != nullptr) {
        dev.fn.DeviceWaitIdle(dev.device);
    }
    if ((dev.cmdPool != VK_NULL_HANDLE) && (dev.fn.DestroyCommandPool != nullptr)) {
        dev.fn.DestroyCommandPool(dev.device, dev.cmdPool, nullptr);
        dev.cmdPool = VK_NULL_HANDLE;
    }
    if (dev.ownsDevice && (dev.fn.DestroyDevice != nullptr)) {
        dev.fn.DestroyDevice(dev.device, nullptr);
    }
    dev.device = VK_NULL_HANDLE;
}

//=============================================================================
// init
//=============================================================================

VkResult LinuxDmaBufImportTest::init(const TestConfig& config) {
    m_config = config;
    m_initFailedAt = Step::ExportDeviceInit;

    // 4:2:0 needs even dimensions; say so rather than failing obscurely later.
    if ((m_config.width & 1u) || (m_config.height & 1u)) {
        std::cout << "[INFO] rounding " << m_config.width << "x" << m_config.height
                  << " down to even dimensions for chroma subsampling\n";
        m_config.width  &= ~1u;
        m_config.height &= ~1u;
    }
    if ((m_config.width == 0) || (m_config.height == 0)) {
        m_initFailureDetail = "width/height must be non-zero";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    m_formatDesc = describeFormat(m_config.format);

    static const char* const instanceExtensions[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        nullptr
    };
    m_vkDevCtx.AddReqInstanceExtensions(instanceExtensions);

    if (m_config.validation) {
        static const char* const layers[] = { "VK_LAYER_KHRONOS_validation", nullptr };
        static const char* const debugExt[] = { VK_EXT_DEBUG_REPORT_EXTENSION_NAME, nullptr };
        m_vkDevCtx.AddReqInstanceLayers(layers);
        m_vkDevCtx.AddReqInstanceExtensions(debugExt);
        std::cout << "[INFO] Validation layers enabled\n";
    }

    // The first four are exactly the precondition set the library's own
    // logical device must enable -- VK_KHR_external_memory_fd,
    // VK_EXT_external_memory_dma_buf, VK_EXT_queue_family_foreign and
    // VK_EXT_image_drm_format_modifier -- or a registration is refused with
    // EXTENSION_MISSING. The library gates on the same set.
    // Requiring them here means a host that cannot possibly support the design
    // fails at init with a name, instead of failing an import later.
    static const char* const requiredDeviceExtensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        nullptr
    };
    m_vkDevCtx.AddReqDeviceExtensions(requiredDeviceExtensions, m_config.verbose);

    static const char* const videoExtensions[] = {
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        nullptr
    };
    if (m_config.videoUsage) {
        m_vkDevCtx.AddOptDeviceExtensions(videoExtensions, m_config.verbose);
    }

    VkResult result = m_vkDevCtx.InitVulkanDevice("LinuxDmaBufImportTest",
                                                  VK_NULL_HANDLE, m_config.verbose);
    if (result != VK_SUCCESS) {
        m_initFailureDetail = "InitVulkanDevice failed (no Vulkan loader/ICD?)";
        return result;
    }

    if (m_config.validation) {
        m_vkDevCtx.InitDebugReport(true, true);
    }

    vk::DeviceUuidUtils deviceUuid;
    result = m_vkDevCtx.InitPhysicalDevice(
        -1, deviceUuid,
        VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT,
        nullptr,
        0, VK_VIDEO_CODEC_OPERATION_NONE_KHR,
        0, VK_VIDEO_CODEC_OPERATION_NONE_KHR);
    if (result != VK_SUCCESS) {
        m_initFailureDetail =
            "InitPhysicalDevice failed - no physical device offering the "
            "required dma-buf/DRM-modifier extension set";
        return result;
    }

    VkPhysicalDeviceProperties props{};
    m_vkDevCtx.GetPhysicalDeviceProperties(m_vkDevCtx.getPhysicalDevice(), &props);
    m_deviceName = props.deviceName;
    std::cout << "[INFO] Physical device: " << props.deviceName
              << "  driver=" << VK_VERSION_MAJOR(props.driverVersion) << "."
              << VK_VERSION_MINOR(props.driverVersion) << "."
              << VK_VERSION_PATCH(props.driverVersion) << "\n";

    // Re-check enablement by name. AddReqDeviceExtensions influences device
    // selection, but the point of the check is to be able to SAY which
    // extension is missing when the run cannot proceed.
    for (const char* const* p = requiredDeviceExtensions; *p != nullptr; ++p) {
        if (m_vkDevCtx.FindRequiredDeviceExtension(*p) == nullptr) {
            m_initFailureDetail = std::string("required device extension not available: ") + *p;
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    result = m_vkDevCtx.CreateVulkanDevice(
        0,                                  // numDecodeQueues
        0,                                  // numEncodeQueues
        VK_VIDEO_CODEC_OPERATION_NONE_KHR,
        true,                               // transfer queue
        false,                              // graphics queue
        false,                              // present queue
        true);                              // compute queue
    if (result != VK_SUCCESS) {
        m_initFailureDetail = "CreateVulkanDevice (device A) failed";
        return result;
    }

    m_devA.label      = "A(export)";
    m_devA.device     = m_vkDevCtx.getDevice();
    m_devA.queue      = m_vkDevCtx.GetComputeQueue();
    m_devA.queueFamily = static_cast<uint32_t>(m_vkDevCtx.GetComputeQueueFamilyIdx());
    if (m_devA.queue == VK_NULL_HANDLE) {
        m_devA.queue       = m_vkDevCtx.GetTransferQueue();
        m_devA.queueFamily = static_cast<uint32_t>(m_vkDevCtx.GetTransferQueueFamilyIdx());
    }
    if ((m_devA.queue == VK_NULL_HANDLE) || (m_devA.queueFamily == UINT32_MAX)) {
        m_initFailureDetail = "device A has no compute or transfer queue";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    m_devA.ownsDevice = false;   // VulkanDeviceContext owns it

    std::string missing;
    if (!m_devA.fn.loadAll(m_vkDevCtx.GetDeviceProcAddr, m_devA.device, missing)) {
        m_initFailedAt = Step::ExportDeviceInit;
        m_initFailureDetail = "device A is missing entry point " + missing;
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    m_vkDevCtx.GetPhysicalDeviceMemoryProperties(m_vkDevCtx.getPhysicalDevice(), &m_memProps);

    m_initFailedAt = Step::ImportDeviceCreate;
    result = createImportDevice();
    if (result != VK_SUCCESS) {
        return result;
    }

    m_initFailedAt = Step::ExportDeviceInit;
    result = setUpDevice(m_devA);
    if (result != VK_SUCCESS) {
        m_initFailureDetail = "command pool creation failed on device A";
        return result;
    }
    m_initFailedAt = Step::ImportDeviceCreate;
    result = setUpDevice(m_devB);
    if (result != VK_SUCCESS) {
        m_initFailureDetail = "command pool creation failed on device B";
        return result;
    }

    // The whole test is meaningless if these are the same VkDevice.
    if (m_devA.device == m_devB.device) {
        m_initFailureDetail = "device A and device B are the same VkDevice handle";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    std::cout << "[INFO] VkDevice A = 0x" << std::hex
              << (uint64_t)(uintptr_t)m_devA.device
              << "   VkDevice B = 0x" << (uint64_t)(uintptr_t)m_devB.device
              << std::dec << "   (one VkPhysicalDevice, queue family "
              << m_devA.queueFamily << ")\n";

    m_initFailedAt = Step::ModifierEnumerate;
    result = enumerateModifiers();
    if (result != VK_SUCCESS) {
        return result;
    }

    m_initFailedAt = Step::NotStarted;
    return VK_SUCCESS;
}

//=============================================================================
// Second VkDevice on the SAME VkPhysicalDevice.
//
// This is the Linux counterpart of Win32OpaqueImportTest::createImportDevice()
// (win32_opaque_import/src/Win32OpaqueImportTest.cpp:172-176). Same idea, and
// the same physical device by construction: m_vkDevCtx.getPhysicalDevice() is
// used verbatim, so a "different GPU" confound is impossible.
//=============================================================================

VkResult LinuxDmaBufImportTest::createImportDevice() {
    VkPhysicalDevice physDev = m_vkDevCtx.getPhysicalDevice();

    const char* wanted[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
    };
    const uint32_t wantedCount = sizeof(wanted) / sizeof(wanted[0]);

    // Everything from this index on is only wanted when the image carries video
    // usage, and is optional even then. Everything BEFORE it is the import
    // contract itself and its absence is fatal. Index 11 is
    // VK_KHR_synchronization2 - keep this in step with the array above.
    const uint32_t firstVideoIdx = 11;

    uint32_t availCount = 0;
    m_vkDevCtx.EnumerateDeviceExtensionProperties(physDev, nullptr, &availCount, nullptr);
    std::vector<VkExtensionProperties> avail(availCount);
    if (availCount != 0) {
        m_vkDevCtx.EnumerateDeviceExtensionProperties(physDev, nullptr, &availCount, avail.data());
    }

    auto haveExt = [&](const char* name) {
        for (const auto& a : avail) {
            if (strcmp(name, a.extensionName) == 0) return true;
        }
        return false;
    };

    std::vector<const char*> enabled;
    for (uint32_t i = 0; i < wantedCount; ++i) {
        if ((i >= firstVideoIdx) && !m_config.videoUsage) {
            continue;
        }
        if (haveExt(wanted[i])) {
            enabled.push_back(wanted[i]);
        } else if (i < firstVideoIdx) {
            // Non-negotiable: without these the import cannot be attempted at all.
            m_initFailureDetail =
                std::string("device B cannot enable required extension: ") + wanted[i];
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    const bool haveVideoMaintenance1 =
        m_config.videoUsage && haveExt(VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME);
    const bool haveSync2  = haveExt(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) && m_config.videoUsage;
    const bool haveTimeline = haveExt(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) && m_config.videoUsage;

    // Which of the features the physical device actually offers.
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR vm1F{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2Features sync2F{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineF{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrF{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};

    ycbcrF.pNext    = &timelineF;
    timelineF.pNext = &sync2F;
    sync2F.pNext    = haveVideoMaintenance1 ? (void*)&vm1F : nullptr;

    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &ycbcrF;
    m_vkDevCtx.GetPhysicalDeviceFeatures2(physDev, &query);

    // Rebuild the chain from scratch, enabling only what is both supported and
    // backed by an enabled extension. VUID-VkDeviceCreateInfo-pNext-pNext.
    void* featureChain = nullptr;
    ycbcrF.pNext = nullptr;  timelineF.pNext = nullptr;
    sync2F.pNext = nullptr;  vm1F.pNext = nullptr;

    if (haveVideoMaintenance1 && (vm1F.videoMaintenance1 == VK_TRUE)) {
        vm1F.pNext = featureChain;
        featureChain = &vm1F;
    }
    if (haveSync2 && (sync2F.synchronization2 == VK_TRUE)) {
        sync2F.pNext = featureChain;
        featureChain = &sync2F;
    }
    if (haveTimeline && (timelineF.timelineSemaphore == VK_TRUE)) {
        timelineF.pNext = featureChain;
        featureChain = &timelineF;
    }
    if (ycbcrF.samplerYcbcrConversion == VK_TRUE) {
        ycbcrF.pNext = featureChain;
        featureChain = &ycbcrF;
    }

    // VIDEO_PROFILE_INDEPENDENT is only legal with videoMaintenance1 enabled;
    // if the device cannot give us that, drop video usage rather than create
    // images that are spec-invalid on B but not on A (which would make the two
    // arms incomparable - the exact confound this test exists to avoid).
    if (m_config.videoUsage &&
        (!haveVideoMaintenance1 || (vm1F.videoMaintenance1 != VK_TRUE))) {
        std::cout << "[WARN] videoMaintenance1 unavailable on this device: "
                     "dropping VIDEO_ENCODE_SRC / VIDEO_PROFILE_INDEPENDENT "
                     "from the tested image (reported, not silent)\n";
        m_config.videoUsage = false;
    }

    VkPhysicalDeviceFeatures2 enabledFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabledFeatures.pNext = featureChain;   // features{} left zeroed on purpose

    // Same queue family as A: the only difference between the two arms must be
    // the VkDevice, not the queue family.
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueCI{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueCI.queueFamilyIndex = m_devA.queueFamily;
    queueCI.queueCount       = 1;
    queueCI.pQueuePriorities = &priority;

    VkDeviceCreateInfo devCI{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    devCI.pNext                   = &enabledFeatures;
    devCI.queueCreateInfoCount    = 1;
    devCI.pQueueCreateInfos       = &queueCI;
    devCI.enabledExtensionCount   = static_cast<uint32_t>(enabled.size());
    devCI.ppEnabledExtensionNames = enabled.data();

    PFN_vkCreateDevice pfnCreateDevice = (PFN_vkCreateDevice)
        m_vkDevCtx.GetInstanceProcAddr(m_vkDevCtx.getInstance(), "vkCreateDevice");
    if (pfnCreateDevice == nullptr) {
        m_initFailureDetail = "vkCreateDevice entry point not found";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkDevice device = VK_NULL_HANDLE;
    VkResult result = pfnCreateDevice(physDev, &devCI, nullptr, &device);
    if (result != VK_SUCCESS) {
        m_initFailureDetail = std::string("vkCreateDevice for device B failed: ") +
                              vkResultName(result);
        return result;
    }

    m_devB.label       = "B(import)";
    m_devB.device      = device;
    m_devB.queueFamily = m_devA.queueFamily;
    m_devB.ownsDevice  = true;

    std::string missing;
    if (!m_devB.fn.loadAll(m_vkDevCtx.GetDeviceProcAddr, m_devB.device, missing)) {
        m_initFailedAt = Step::ImportDeviceDispatch;
        m_initFailureDetail = "device B is missing entry point " + missing;
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    m_devB.fn.GetDeviceQueue(m_devB.device, m_devB.queueFamily, 0, &m_devB.queue);
    if (m_devB.queue == VK_NULL_HANDLE) {
        m_initFailureDetail = "vkGetDeviceQueue returned NULL on device B";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    std::cout << "[INFO] Import device created: a second VkDevice on the same "
                 "VkPhysicalDevice (" << enabled.size() << " extensions)\n";
    return VK_SUCCESS;
}

VkResult LinuxDmaBufImportTest::setUpDevice(TestDevice& dev) {
    VkCommandPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolCI.queueFamilyIndex = dev.queueFamily;
    poolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    return dev.fn.CreateCommandPool(dev.device, &poolCI, nullptr, &dev.cmdPool);
}

//=============================================================================
// Image usage / flags under test
//=============================================================================

VkImageUsageFlags LinuxDmaBufImportTest::exportUsage() const {
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (m_config.videoUsage) {
        usage |= VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
    }
    return usage;
}

bool LinuxDmaBufImportTest::importVideoUsage() const {
    return (m_config.importVideoUsage < 0) ? m_config.videoUsage
                                           : (m_config.importVideoUsage != 0);
}

VkImageUsageFlags LinuxDmaBufImportTest::importUsage() const {
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (importVideoUsage()) {
        usage |= VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
    }
    if (m_config.importUsageRaw != UINT32_MAX) {
        usage = (VkImageUsageFlags)m_config.importUsageRaw;
    }
    return usage;
}

VkImageCreateFlags LinuxDmaBufImportTest::importFlags() const {
    if (m_config.importFlagsRaw != UINT32_MAX) {
        return (VkImageCreateFlags)m_config.importFlagsRaw;
    }
    if (!importVideoUsage()) {
        return 0;
    }
    return VK_IMAGE_CREATE_EXTENDED_USAGE_BIT |
           VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
           VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
}

VkImageCreateFlags LinuxDmaBufImportTest::exportFlags() const {
    if (!m_config.videoUsage) {
        return 0;
    }
    // The triple a video-usage exporter sets, matching the sibling
    // drm_format_mod test (DrmFormatModTest.cpp:828-830): EXTENDED_USAGE +
    // MUTABLE_FORMAT for per-plane views, VIDEO_PROFILE_INDEPENDENT so no
    // VkVideoProfileListInfoKHR is needed at image-creation time. The
    // library does not invent these - it copies whatever the exporter
    // declared (imageCI.flags = desc.imageFlags,
    // vulkan_video_encoder_ext.cpp:3274) - so the exporter is where they
    // have to be right.
    return VK_IMAGE_CREATE_EXTENDED_USAGE_BIT |
           VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
           VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
}

//=============================================================================
// Modifier enumeration + PHYSICAL-device-scoped capability query.
//
// Section 3.2 of the plan rests on this query being physical-device scoped
// ("The modifier-import capability query is physical-device-scoped"), which is
// the spec reason a second LOGICAL device is expected to be able to import.
// Asking it here means the run knows, before it starts, which modifiers the
// driver claims are both EXPORTABLE and IMPORTABLE - so an import failure on a
// modifier the driver advertised is unambiguously a defect and not a
// misconfiguration by the test.
//=============================================================================

VkResult LinuxDmaBufImportTest::enumerateModifiers() {
    VkPhysicalDevice physDev = m_vkDevCtx.getPhysicalDevice();

    VkDrmFormatModifierPropertiesListEXT list{
        VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 fmtProps{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    fmtProps.pNext = &list;

    m_vkDevCtx.GetPhysicalDeviceFormatProperties2(physDev, m_config.format, &fmtProps);
    const uint32_t count = list.drmFormatModifierCount;
    if (count == 0) {
        m_initFailureDetail =
            std::string("the driver reports no DRM format modifiers for ") +
            formatName(m_config.format);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    std::vector<VkDrmFormatModifierPropertiesEXT> props(count);
    list.pDrmFormatModifierProperties = props.data();
    m_vkDevCtx.GetPhysicalDeviceFormatProperties2(physDev, m_config.format, &fmtProps);

    VkFormat viewFormat = m_config.format;
    const VkImageUsageFlags  usage = exportUsage();
    const VkImageCreateFlags flags = exportFlags();

    for (uint32_t i = 0; i < count; ++i) {
        ModifierCandidate c;
        c.modifier         = props[i].drmFormatModifier;
        c.memoryPlaneCount = props[i].drmFormatModifierPlaneCount;
        c.tilingFeatures   = props[i].drmFormatModifierTilingFeatures;

        if (m_config.linearOnly && (c.modifier != DRM_FORMAT_MOD_LINEAR)) {
            c.note = "skipped (--linear-only)";
            m_modifiers.push_back(c);
            continue;
        }
        if ((m_config.onlyModifier != DRM_FORMAT_MOD_INVALID) &&
            (c.modifier != m_config.onlyModifier)) {
            c.note = "skipped (--modifier)";
            m_modifiers.push_back(c);
            continue;
        }

        VkPhysicalDeviceExternalImageFormatInfo extInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        extInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        modInfo.pNext            = &extInfo;
        modInfo.drmFormatModifier = c.modifier;
        modInfo.sharingMode      = VK_SHARING_MODE_EXCLUSIVE;

        VkImageFormatListCreateInfo formatListCI{
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        formatListCI.pNext           = &modInfo;
        formatListCI.viewFormatCount = 1;
        formatListCI.pViewFormats    = &viewFormat;

        VkPhysicalDeviceImageFormatInfo2 info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.pNext  = (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)
                        ? (void*)&formatListCI : (void*)&modInfo;
        info.format = m_config.format;
        info.type   = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.usage  = usage;
        info.flags  = flags;

        VkExternalImageFormatProperties extProps{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 out{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        out.pNext = &extProps;

        c.queried     = true;
        c.queryResult = m_vkDevCtx.GetPhysicalDeviceImageFormatProperties2(
            physDev, &info, &out);

        if (c.queryResult == VK_SUCCESS) {
            const VkExternalMemoryFeatureFlags f =
                extProps.externalMemoryProperties.externalMemoryFeatures;
            c.exportable = (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
            c.importable = (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
            c.usable     = c.exportable && c.importable;
            if (!c.usable) {
                c.note = "driver does not advertise EXPORTABLE+IMPORTABLE for dma-buf";
            }
        } else {
            c.note = std::string("image format unsupported: ") + vkResultName(c.queryResult);
        }

        if (c.usable && (c.memoryPlaneCount == 0)) {
            c.usable = false;
            c.note   = "drmFormatModifierPlaneCount is 0";
        }

        m_modifiers.push_back(c);
    }

    bool anyUsable = false;
    for (const auto& c : m_modifiers) {
        if (c.usable) { anyUsable = true; break; }
    }
    if (!anyUsable) {
        m_initFailedAt = Step::ModifierCapability;
        m_initFailureDetail =
            std::string("no DRM modifier for ") + formatName(m_config.format) +
            " is advertised as both EXPORTABLE and IMPORTABLE for dma-buf with "
            "the requested usage/flags - there is nothing to test on this host";
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    return VK_SUCCESS;
}

//=============================================================================
// Small helpers
//=============================================================================

uint32_t LinuxDmaBufImportTest::chooseMemoryType(uint32_t candidateMask,
                                                 VkMemoryPropertyFlags required) const
{
    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
        if ((candidateMask & (1u << i)) &&
            ((m_memProps.memoryTypes[i].propertyFlags & required) == required)) {
            return i;
        }
    }
    return UINT32_MAX;
}

VkResult LinuxDmaBufImportTest::createStaging(TestDevice& dev, VkDeviceSize size,
                                              VkBufferUsageFlags usage, Staging& out)
{
    out = Staging{};
    out.size = size;

    VkBufferCreateInfo bufCI{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufCI.size        = size;
    bufCI.usage       = usage;
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult r = dev.fn.CreateBuffer(dev.device, &bufCI, nullptr, &out.buffer);
    if (r != VK_SUCCESS) {
        return r;
    }

    VkMemoryRequirements req{};
    dev.fn.GetBufferMemoryRequirements(dev.device, out.buffer, &req);

    uint32_t typeIdx = chooseMemoryType(req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    out.coherent = (typeIdx != UINT32_MAX);
    if (typeIdx == UINT32_MAX) {
        typeIdx = chooseMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    if (typeIdx == UINT32_MAX) {
        dev.fn.DestroyBuffer(dev.device, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize  = req.size;
    allocInfo.memoryTypeIndex = typeIdx;

    r = dev.fn.AllocateMemory(dev.device, &allocInfo, nullptr, &out.memory);
    if (r != VK_SUCCESS) {
        dev.fn.DestroyBuffer(dev.device, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return r;
    }

    r = dev.fn.BindBufferMemory(dev.device, out.buffer, out.memory, 0);
    if (r == VK_SUCCESS) {
        r = dev.fn.MapMemory(dev.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
    }
    if (r != VK_SUCCESS) {
        destroyStaging(dev, out);
    }
    return r;
}

void LinuxDmaBufImportTest::destroyStaging(TestDevice& dev, Staging& s) {
    if (s.mapped != nullptr) {
        dev.fn.UnmapMemory(dev.device, s.memory);
        s.mapped = nullptr;
    }
    if (s.buffer != VK_NULL_HANDLE) {
        dev.fn.DestroyBuffer(dev.device, s.buffer, nullptr);
        s.buffer = VK_NULL_HANDLE;
    }
    if (s.memory != VK_NULL_HANDLE) {
        dev.fn.FreeMemory(dev.device, s.memory, nullptr);
        s.memory = VK_NULL_HANDLE;
    }
}

VkResult LinuxDmaBufImportTest::submitOneShot(TestDevice& dev, VkCommandBuffer cmd,
                                              const char*& failedCall)
{
    failedCall = nullptr;

    VkResult r = dev.fn.EndCommandBuffer(cmd);
    if (r != VK_SUCCESS) { failedCall = "vkEndCommandBuffer"; return r; }

    VkFenceCreateInfo fenceCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    r = dev.fn.CreateFence(dev.device, &fenceCI, nullptr, &fence);
    if (r != VK_SUCCESS) { failedCall = "vkCreateFence"; return r; }

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;

    r = dev.fn.QueueSubmit(dev.queue, 1, &submit, fence);
    if (r != VK_SUCCESS) {
        failedCall = "vkQueueSubmit";
        dev.fn.DestroyFence(dev.device, fence, nullptr);
        return r;
    }

    // 5 s: a cross-device copy that has not landed by then is hung, and a hang
    // must be reported as a failure rather than waited on for ever.
    r = dev.fn.WaitForFences(dev.device, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
    dev.fn.DestroyFence(dev.device, fence, nullptr);
    if (r != VK_SUCCESS) { failedCall = "vkWaitForFences"; return r; }

    r = dev.fn.QueueWaitIdle(dev.queue);
    if (r != VK_SUCCESS) { failedCall = "vkQueueWaitIdle"; }
    return r;
}

//=============================================================================
// One end-to-end cycle: export on A, import on `importDev`, read back.
//=============================================================================

ArmResult LinuxDmaBufImportTest::runCycle(const char* armLabel,
                                          TestDevice& importDev,
                                          const ModifierCandidate& mod)
{
    ArmResult res;
    res.arm                = armLabel;
    res.modifier           = mod.modifier;
    res.exportDeviceHandle = (uint64_t)(uintptr_t)m_devA.device;
    res.importDeviceHandle = (uint64_t)(uintptr_t)importDev.device;

    VkImage         expImage = VK_NULL_HANDLE;
    VkDeviceMemory  expMem   = VK_NULL_HANDLE;
    VkImage         impImage = VK_NULL_HANDLE;
    VkDeviceMemory  impMem   = VK_NULL_HANDLE;
    int             exportFd = -1;
    int             importFd = -1;
    Staging         upload{};
    Staging         download{};
    VkCommandBuffer cmdA = VK_NULL_HANDLE;
    VkCommandBuffer cmdB = VK_NULL_HANDLE;

    std::vector<VkSubresourceLayout> memPlaneLayouts;
    std::vector<VkBufferImageCopy>   regions;
    std::vector<std::pair<uint64_t, uint64_t>> planeRanges;  // offset, size
    VkDeviceSize totalStagingBytes = 0;

    auto fail = [&](Step s, VkResult r, const std::string& d) {
        res.failedAt = s;
        res.vkResult = r;
        res.detail   = d;
    };

    // The content round-trip needs the modifier's tiling to support transfers
    // in both directions. When it does not, that is REPORTED, and the arm can
    // no longer contribute a "claim verified" - it is not quietly downgraded.
    const bool tilingCanTransfer =
        ((mod.tilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0) &&
        ((mod.tilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0);
    const bool doContent =
        m_config.contentCheck && m_formatDesc.known && tilingCanTransfer;

    auto body = [&]() {
        //--------------------------------------------------------------------
        // Guard the premise of the whole test.
        //--------------------------------------------------------------------
        if ((strcmp(armLabel, "second-device") == 0) &&
            (importDev.device == m_devA.device)) {
            fail(Step::ImportDeviceCreate, VK_ERROR_INITIALIZATION_FAILED,
                 "the 'second-device' arm was handed device A - the test would "
                 "prove nothing; refusing to report a result");
            return;
        }

        //--------------------------------------------------------------------
        // 1. Exportable image on device A.
        //--------------------------------------------------------------------
        res.lastStepStarted = Step::ExportImageCreate;

        VkFormat viewFormat = m_config.format;
        uint64_t modifier   = mod.modifier;

        VkExternalMemoryImageCreateInfo extMemCI{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        extMemCI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkImageDrmFormatModifierListCreateInfoEXT drmList{
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
        drmList.pNext                  = &extMemCI;
        drmList.drmFormatModifierCount = 1;
        drmList.pDrmFormatModifiers    = &modifier;

        VkImageFormatListCreateInfo formatListCI{
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        formatListCI.pNext           = &drmList;
        formatListCI.viewFormatCount = 1;
        formatListCI.pViewFormats    = &viewFormat;

        VkImageCreateInfo imageCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        // MUTABLE_FORMAT on a DRM-modifier image obliges a view-format list
        // (VUID-VkImageCreateInfo-tiling-02353).
        imageCI.pNext = (exportFlags() & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)
                            ? (void*)&formatListCI : (void*)&drmList;
        imageCI.flags         = exportFlags();
        imageCI.imageType     = VK_IMAGE_TYPE_2D;
        imageCI.format        = m_config.format;
        imageCI.extent        = {m_config.width, m_config.height, 1};
        imageCI.mipLevels     = 1;
        imageCI.arrayLayers   = 1;
        imageCI.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageCI.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        imageCI.usage         = exportUsage();
        imageCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult r = m_devA.fn.CreateImage(m_devA.device, &imageCI, nullptr, &expImage);
        if (r != VK_SUCCESS) {
            fail(Step::ExportImageCreate, r,
                 "vkCreateImage on the export device rejected the modifier the "
                 "driver had just advertised");
            return;
        }

        //--------------------------------------------------------------------
        // 2. Which modifier did we actually get?
        //--------------------------------------------------------------------
        res.lastStepStarted = Step::ExportModifierReadback;
        VkImageDrmFormatModifierPropertiesEXT gotMod{
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
        r = m_devA.fn.GetImageDrmFormatModifierPropertiesEXT(m_devA.device, expImage, &gotMod);
        if (r != VK_SUCCESS) {
            fail(Step::ExportModifierReadback, r,
                 "vkGetImageDrmFormatModifierPropertiesEXT failed - the importer "
                 "cannot be told which layout to expect");
            return;
        }
        res.readbackModifier = gotMod.drmFormatModifier;
        if (res.readbackModifier != mod.modifier) {
            fail(Step::ExportModifierReadback, VK_ERROR_INITIALIZATION_FAILED,
                 "driver chose modifier " + modifierToString(res.readbackModifier) +
                 " from a single-entry list containing only " +
                 modifierToString(mod.modifier));
            return;
        }

        //--------------------------------------------------------------------
        // 3. Exportable, dedicated allocation on A.
        //--------------------------------------------------------------------
        res.lastStepStarted = Step::ExportMemoryAllocate;
        VkMemoryRequirements memReqs{};
        m_devA.fn.GetImageMemoryRequirements(m_devA.device, expImage, &memReqs);

        VkMemoryDedicatedAllocateInfo dedicated{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = expImage;

        VkExportMemoryAllocateInfo exportAI{
            VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportAI.pNext       = &dedicated;
        exportAI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.pNext           = &exportAI;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = chooseMemoryType(memReqs.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) {
            fail(Step::ExportMemoryAllocate, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                 "no DEVICE_LOCAL memory type in the export image's requirements mask");
            return;
        }
        res.exportAllocSize    = memReqs.size;
        res.exportMemTypeIndex = allocInfo.memoryTypeIndex;

        r = m_devA.fn.AllocateMemory(m_devA.device, &allocInfo, nullptr, &expMem);
        if (r != VK_SUCCESS) {
            fail(Step::ExportMemoryAllocate, r, "exportable allocation failed on device A");
            return;
        }

        res.lastStepStarted = Step::ExportBindMemory;
        r = m_devA.fn.BindImageMemory(m_devA.device, expImage, expMem, 0);
        if (r != VK_SUCCESS) {
            fail(Step::ExportBindMemory, r, "vkBindImageMemory failed on device A");
            return;
        }

        //--------------------------------------------------------------------
        // 4. Memory-plane layouts. For a DRM-modifier image these come from the
        //    MEMORY_PLANE aspects, not the colour PLANE aspects, and the count
        //    is the modifier's drmFormatModifierPlaneCount.
        //--------------------------------------------------------------------
        res.lastStepStarted = Step::ExportPlaneLayouts;
        memPlaneLayouts.resize(mod.memoryPlaneCount);
        for (uint32_t p = 0; p < mod.memoryPlaneCount; ++p) {
            VkImageSubresource sub{};
            sub.aspectMask = (VkImageAspectFlags)(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p);
            sub.mipLevel   = 0;
            sub.arrayLayer = 0;
            m_devA.fn.GetImageSubresourceLayout(m_devA.device, expImage, &sub,
                                                &memPlaneLayouts[p]);
        }

        //--------------------------------------------------------------------
        // 5. Export the dma-buf fd. From here on the fd is a resource with an
        //    owner, and every exit below accounts for it.
        //--------------------------------------------------------------------
        res.lastStepStarted = Step::ExportFd;
        VkMemoryGetFdInfoKHR getFd{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        getFd.memory     = expMem;
        getFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        r = m_devA.fn.GetMemoryFdKHR(m_devA.device, &getFd, &exportFd);
        if ((r != VK_SUCCESS) || (exportFd < 0)) {
            fail(Step::ExportFd, r, "vkGetMemoryFdKHR (DMA_BUF) failed on device A");
            return;
        }

        //--------------------------------------------------------------------
        // 6. Write a known pattern through device A and release the image to
        //    VK_QUEUE_FAMILY_FOREIGN_EXT - the same handshake the encoder does
        //    for an imported frame (VkVideoEncoder.cpp StageInputFrame).
        //--------------------------------------------------------------------
        if (doContent) {
            res.lastStepStarted = Step::UploadPattern;
            res.contentAttempted = true;

            uint64_t offset = 0;
            for (const auto& pd : m_formatDesc.planes) {
                const uint32_t pw = m_config.width  / pd.widthDiv;
                const uint32_t ph = m_config.height / pd.heightDiv;
                const uint64_t bytes = (uint64_t)pw * ph * pd.texelBytes;

                VkBufferImageCopy region{};
                region.bufferOffset      = offset;
                region.bufferRowLength   = 0;   // tightly packed
                region.bufferImageHeight = 0;
                region.imageSubresource.aspectMask     = pd.aspect;
                region.imageSubresource.mipLevel       = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount     = 1;
                region.imageOffset = {0, 0, 0};
                region.imageExtent = {pw, ph, 1};
                regions.push_back(region);
                planeRanges.emplace_back(offset, bytes);

                // 16-byte aligned plane starts keep bufferOffset legal for every
                // texel-block size in play (VUID-VkBufferImageCopy-bufferOffset-00193).
                offset += (bytes + 15u) & ~15ull;
            }
            totalStagingBytes = offset;

            r = createStaging(m_devA, totalStagingBytes,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload);
            if (r != VK_SUCCESS) {
                fail(Step::UploadPattern, r, "host-visible upload buffer failed on device A");
                return;
            }
            uint8_t* src = static_cast<uint8_t*>(upload.mapped);
            for (const auto& pr : planeRanges) {
                for (uint64_t i = 0; i < pr.second; ++i) {
                    src[pr.first + i] = patternByte(pr.first + i);
                }
            }
            if (!upload.coherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = upload.memory;
                range.offset = 0;
                range.size   = VK_WHOLE_SIZE;
                m_devA.fn.FlushMappedMemoryRanges(m_devA.device, 1, &range);
            }

            VkCommandBufferAllocateInfo cbAI{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cbAI.commandPool        = m_devA.cmdPool;
            cbAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbAI.commandBufferCount = 1;
            r = m_devA.fn.AllocateCommandBuffers(m_devA.device, &cbAI, &cmdA);
            if (r != VK_SUCCESS) {
                fail(Step::UploadPattern, r, "vkAllocateCommandBuffers failed on device A");
                return;
            }

            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            r = m_devA.fn.BeginCommandBuffer(cmdA, &begin);
            if (r != VK_SUCCESS) {
                fail(Step::UploadPattern, r, "vkBeginCommandBuffer failed on device A");
                return;
            }

            VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toDst.srcAccessMask       = 0;
            toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.image               = expImage;
            toDst.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            m_devA.fn.CmdPipelineBarrier(cmdA,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toDst);

            m_devA.fn.CmdCopyBufferToImage(cmdA, upload.buffer, expImage,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           (uint32_t)regions.size(), regions.data());

            // Release to FOREIGN. The acquire on the import device must repeat
            // these same oldLayout/newLayout values; the transition happens once.
            res.lastStepStarted = Step::ReleaseToForeign;
            VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            release.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            release.dstAccessMask       = 0;
            release.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            release.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            release.srcQueueFamilyIndex = m_devA.queueFamily;
            release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
            release.image               = expImage;
            release.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            m_devA.fn.CmdPipelineBarrier(cmdA,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, nullptr, 0, nullptr, 1, &release);

            const char* failedCall = nullptr;
            r = submitOneShot(m_devA, cmdA, failedCall);
            if (r != VK_SUCCESS) {
                fail(Step::ReleaseToForeign, r,
                     std::string(failedCall ? failedCall : "submit") +
                     " failed on device A while writing the pattern");
                return;
            }
        }

        //--------------------------------------------------------------------
        // 7. Import on the target device. Explicit modifier + the exporter's
        //    memory-plane layouts, exactly as the library does at
        //    vulkan_video_encoder_ext.cpp:3286-3303.
        //--------------------------------------------------------------------
        res.lastStepStarted = Step::ImportImageCreate;

        VkExternalMemoryImageCreateInfo impExtMemCI{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        impExtMemCI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        std::vector<VkSubresourceLayout> importLayouts(memPlaneLayouts.size());
        for (size_t p = 0; p < memPlaneLayouts.size(); ++p) {
            importLayouts[p].offset     = memPlaneLayouts[p].offset;
            importLayouts[p].size       = 0;    // VUID-...-size-02267
            importLayouts[p].rowPitch   = memPlaneLayouts[p].rowPitch;
            // arrayLayers is 1 and extent.depth is 1, so these MUST be 0
            // (VUID-...-arrayPitch-02268 / -depthPitch-02269), whatever the
            // exporter reported.
            importLayouts[p].arrayPitch = 0;
            importLayouts[p].depthPitch = 0;
        }

        VkImageDrmFormatModifierExplicitCreateInfoEXT drmExplicit{
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
        drmExplicit.pNext                       = &impExtMemCI;
        drmExplicit.drmFormatModifier           = res.readbackModifier;
        drmExplicit.drmFormatModifierPlaneCount = (uint32_t)importLayouts.size();
        drmExplicit.pPlaneLayouts               = importLayouts.data();

        VkImageFormatListCreateInfo impFormatListCI{
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        impFormatListCI.pNext           = &drmExplicit;
        impFormatListCI.viewFormatCount = 1;
        impFormatListCI.pViewFormats    = &viewFormat;

        VkImageCreateInfo impCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        impCI.pNext = (importFlags() & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)
                          ? (void*)&impFormatListCI : (void*)&drmExplicit;
        impCI.flags         = importFlags();
        impCI.imageType     = VK_IMAGE_TYPE_2D;
        impCI.format        = m_config.format;
        impCI.extent        = {m_config.width, m_config.height, 1};
        impCI.mipLevels     = 1;
        impCI.arrayLayers   = 1;
        impCI.samples       = VK_SAMPLE_COUNT_1_BIT;
        impCI.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        impCI.usage         = importUsage();
        impCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        impCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        r = importDev.fn.CreateImage(importDev.device, &impCI, nullptr, &impImage);
        if (r != VK_SUCCESS) {
            fail(Step::ImportImageCreate, r,
                 "vkCreateImage on the IMPORT device rejected the exporter's "
                 "modifier + plane layouts");
            return;
        }

        VkMemoryRequirements impReqs{};
        importDev.fn.GetImageMemoryRequirements(importDev.device, impImage, &impReqs);
        res.importMemReqSize  = impReqs.size;
        res.importMemTypeBits = impReqs.memoryTypeBits;

        //--------------------------------------------------------------------
        // 8. Which memory types can hold THIS fd on THIS device. Spec-mandated
        //    for dma-buf (VUID-VkMemoryAllocateInfo-memoryTypeIndex-00648); the
        //    library warns that a type outside this mask can import
        //    "successfully" on NVIDIA and then read garbage
        //    (vulkan_video_encoder_ext.cpp:3328-3343). A test must not guess.
        //--------------------------------------------------------------------
        res.lastStepStarted = Step::ImportFdMemoryTypes;
        VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        r = importDev.fn.GetMemoryFdPropertiesKHR(
            importDev.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            exportFd, &fdProps);
        if (r != VK_SUCCESS) {
            fail(Step::ImportFdMemoryTypes, r,
                 "vkGetMemoryFdPropertiesKHR failed on the import device - the "
                 "importable memory-type mask for this fd is unknown and the "
                 "test refuses to guess one");
            return;
        }
        res.fdMemTypeBits = fdProps.memoryTypeBits;

        res.lastStepStarted = Step::ImportMemoryTypeSelect;
        const uint32_t candidates = impReqs.memoryTypeBits & fdProps.memoryTypeBits;
        if (candidates == 0) {
            std::ostringstream os;
            os << "no memory type satisfies both the import image (0x" << std::hex
               << impReqs.memoryTypeBits << ") and this dma-buf (0x"
               << fdProps.memoryTypeBits << ")" << std::dec;
            fail(Step::ImportMemoryTypeSelect, VK_ERROR_OUT_OF_DEVICE_MEMORY, os.str());
            return;
        }
        uint32_t impTypeIdx = chooseMemoryType(candidates, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (impTypeIdx == UINT32_MAX) {
            impTypeIdx = chooseMemoryType(candidates, 0);
        }
        res.importMemTypeIndex = impTypeIdx;

        //--------------------------------------------------------------------
        // 9. Import. dup() first: the fd handed to vkAllocateMemory belongs to
        //    the driver from the moment that call SUCCEEDS and never before, so
        //    the master fd stays ours for the second arm and for cleanup.
        //--------------------------------------------------------------------
        res.lastStepStarted = Step::ImportMemoryAllocate;
        importFd = ::dup(exportFd);
        if (importFd < 0) {
            fail(Step::ImportMemoryAllocate, VK_ERROR_OUT_OF_HOST_MEMORY,
                 "dup() of the exported dma-buf fd failed");
            return;
        }

        VkMemoryDedicatedAllocateInfo impDedicated{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        impDedicated.image = impImage;

        VkImportMemoryFdInfoKHR importFdInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        importFdInfo.pNext      = &impDedicated;
        importFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        importFdInfo.fd         = importFd;

        VkMemoryAllocateInfo impAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        impAlloc.pNext = &importFdInfo;
        // The EXPORTER's size. Deriving it from the importer's
        // vkGetImageMemoryRequirements is wrong for dma-buf on NVIDIA - see
        // vulkan_video_encoder_ext.cpp:3485-3495.
        impAlloc.allocationSize  = res.exportAllocSize;
        impAlloc.memoryTypeIndex = impTypeIdx;

        r = importDev.fn.AllocateMemory(importDev.device, &impAlloc, nullptr, &impMem);
        if (r != VK_SUCCESS) {
            std::ostringstream os;
            os << "vkAllocateMemory(import) failed on device " << importDev.label
               << ": size=" << res.exportAllocSize
               << " (importer's own requirement was " << impReqs.size << ")"
               << " memTypeIdx=" << impTypeIdx;
            fail(Step::ImportMemoryAllocate, r, os.str());
            return;   // cleanup closes importFd: the driver never took it
        }
        importFd = -1;  // handed off; closing it now would be a double close

        res.lastStepStarted = Step::ImportBindMemory;
        r = importDev.fn.BindImageMemory(importDev.device, impImage, impMem, 0);
        if (r != VK_SUCCESS) {
            fail(Step::ImportBindMemory, r,
                 "vkBindImageMemory failed on the import device");
            return;
        }
        res.importSucceeded = true;

        //--------------------------------------------------------------------
        // 10. Use the memory. This is the part the Windows sibling stops short
        //     of: without it, "import succeeded" is a claim about an API return
        //     code and not about the memory.
        //--------------------------------------------------------------------
        if (!doContent) {
            res.detail = m_config.contentCheck
                ? (m_formatDesc.known
                     ? "content round-trip unavailable: this modifier's tiling "
                       "features lack TRANSFER_SRC/DST"
                     : "content round-trip unavailable: no plane description for "
                       "this format")
                : "content round-trip disabled (--no-content-check)";
            res.lastStepStarted = Step::Complete;
            return;
        }

        res.lastStepStarted = Step::AcquireFromForeign;

        r = createStaging(importDev, totalStagingBytes,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, download);
        if (r != VK_SUCCESS) {
            fail(Step::Readback, r, "host-visible readback buffer failed on the import device");
            return;
        }
        // Prefill with the complement of the pattern: if the copy never runs,
        // the comparison cannot pass by accident.
        {
            uint8_t* dst = static_cast<uint8_t*>(download.mapped);
            for (VkDeviceSize i = 0; i < totalStagingBytes; ++i) {
                dst[i] = static_cast<uint8_t>(~patternByte(i));
            }
            if (!download.coherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = download.memory;
                range.offset = 0;
                range.size   = VK_WHOLE_SIZE;
                importDev.fn.FlushMappedMemoryRanges(importDev.device, 1, &range);
            }
        }

        VkCommandBufferAllocateInfo cbAI{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAI.commandPool        = importDev.cmdPool;
        cbAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAI.commandBufferCount = 1;
        r = importDev.fn.AllocateCommandBuffers(importDev.device, &cbAI, &cmdB);
        if (r != VK_SUCCESS) {
            fail(Step::Readback, r, "vkAllocateCommandBuffers failed on the import device");
            return;
        }

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        r = importDev.fn.BeginCommandBuffer(cmdB, &begin);
        if (r != VK_SUCCESS) {
            fail(Step::Readback, r, "vkBeginCommandBuffer failed on the import device");
            return;
        }

        // Acquire from FOREIGN. oldLayout/newLayout mirror the release on A.
        VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acquire.srcAccessMask       = 0;
        acquire.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        acquire.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        acquire.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        acquire.dstQueueFamilyIndex = importDev.queueFamily;
        acquire.image               = impImage;
        acquire.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        importDev.fn.CmdPipelineBarrier(cmdB,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &acquire);

        res.lastStepStarted = Step::Readback;
        importDev.fn.CmdCopyImageToBuffer(cmdB, impImage, VK_IMAGE_LAYOUT_GENERAL,
                                          download.buffer,
                                          (uint32_t)regions.size(), regions.data());

        const char* failedCall = nullptr;
        r = submitOneShot(importDev, cmdB, failedCall);
        if (r != VK_SUCCESS) {
            fail(Step::Readback, r,
                 std::string(failedCall ? failedCall : "submit") +
                 " failed on the import device while reading the shared image back");
            return;
        }

        if (!download.coherent) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = download.memory;
            range.offset = 0;
            range.size   = VK_WHOLE_SIZE;
            importDev.fn.InvalidateMappedMemoryRanges(importDev.device, 1, &range);
        }

        //--------------------------------------------------------------------
        // 11. Compare only the plane byte ranges (the inter-plane alignment
        //     padding is never written by either copy, so comparing it would
        //     manufacture a failure).
        //--------------------------------------------------------------------
        res.lastStepStarted = Step::ContentCompare;
        const uint8_t* got = static_cast<const uint8_t*>(download.mapped);
        for (const auto& pr : planeRanges) {
            for (uint64_t i = 0; i < pr.second; ++i) {
                const uint64_t off = pr.first + i;
                const uint8_t expected = patternByte(off);
                if (got[off] != expected) {
                    res.firstMismatchOffset = off;
                    res.expectedByte = expected;
                    res.actualByte   = got[off];
                    fail(Step::ContentCompare, VK_SUCCESS,
                         "the import device read back different bytes than the "
                         "export device wrote - the import bound memory that is "
                         "not the exported allocation, or the layouts disagree");
                    return;
                }
                res.comparedBytes++;
            }
        }
        res.contentVerified = true;
        res.lastStepStarted = Step::Complete;
    };

    body();

    //-------------------------------------------------------------------------
    // Cleanup. Order matters: images before their memory, and the fds last.
    // A failed/timed-out submit can leave work in flight; freeing command
    // buffers or destroying images under it is undefined behaviour, so drain
    // both devices before touching anything.
    if (importDev.fn.DeviceWaitIdle != nullptr) {
        importDev.fn.DeviceWaitIdle(importDev.device);
    }
    if (m_devA.fn.DeviceWaitIdle != nullptr) {
        m_devA.fn.DeviceWaitIdle(m_devA.device);
    }

    if (cmdB != VK_NULL_HANDLE) {
        importDev.fn.FreeCommandBuffers(importDev.device, importDev.cmdPool, 1, &cmdB);
    }
    if (cmdA != VK_NULL_HANDLE) {
        m_devA.fn.FreeCommandBuffers(m_devA.device, m_devA.cmdPool, 1, &cmdA);
    }
    destroyStaging(importDev, download);
    destroyStaging(m_devA, upload);

    if (impImage != VK_NULL_HANDLE) {
        importDev.fn.DestroyImage(importDev.device, impImage, nullptr);
    }
    if (impMem != VK_NULL_HANDLE) {
        importDev.fn.FreeMemory(importDev.device, impMem, nullptr);
    }
    if (expImage != VK_NULL_HANDLE) {
        m_devA.fn.DestroyImage(m_devA.device, expImage, nullptr);
    }
    if (expMem != VK_NULL_HANDLE) {
        m_devA.fn.FreeMemory(m_devA.device, expMem, nullptr);
    }

    // importFd is >= 0 only when vkAllocateMemory did NOT take it.
    if (importFd >= 0) {
        ::close(importFd);
    }
    if (exportFd >= 0) {
        ::close(exportFd);
    }

    return res;
}

//=============================================================================
// Run every usable modifier through both arms
//=============================================================================

std::vector<ArmResult> LinuxDmaBufImportTest::run() {
    std::vector<ArmResult> results;

    std::cout << "\n=== Linux dma-buf second-device import ===\n"
              << "Format:   " << formatName(m_config.format) << "\n"
              << "Size:     " << m_config.width << "x" << m_config.height << "\n"
              << "Handle:   VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT\n"
              << "Export:   usage 0x" << std::hex << exportUsage()
              << "  flags 0x" << exportFlags() << std::dec
              << (m_config.videoUsage ? "  (VIDEO_ENCODE_SRC)" : "  (no video usage)")
              << "\n"
              << "Import:   usage 0x" << std::hex << importUsage()
              << "  flags 0x" << importFlags() << std::dec
              << (importVideoUsage() ? "  (VIDEO_ENCODE_SRC)" : "  (no video usage)")
              << "\n"
              << "Content:  " << (m_config.contentCheck
                                    ? "write on A, read back on B, byte compare"
                                    : "DISABLED (--no-content-check)") << "\n\n";

    for (const auto& mod : m_modifiers) {
        if (!mod.usable) {
            std::cout << "[SKIP] modifier " << modifierToString(mod.modifier)
                      << ": " << (mod.note.empty() ? "not usable" : mod.note) << "\n";
            continue;
        }

        std::cout << "[RUN ] modifier " << modifierToString(mod.modifier)
                  << "  memPlanes=" << mod.memoryPlaneCount << "\n";

        results.push_back(runCycle("second-device", m_devB, mod));
        if (m_config.sameDeviceControl) {
            results.push_back(runCycle("same-device", m_devA, mod));
        }
    }

    return results;
}

//=============================================================================
// Reporting
//=============================================================================

void LinuxDmaBufImportTest::printResults(const std::vector<ArmResult>& results) const {
    std::cout << "\n" << std::string(108, '=') << "\n"
              << "                 LINUX DMA-BUF SECOND-DEVICE IMPORT RESULTS\n"
              << std::string(108, '=') << "\n\n";

    std::cout << "Modifier capability (physical-device scoped query):\n";
    for (const auto& c : m_modifiers) {
        std::cout << "  " << std::left << std::setw(22) << modifierToString(c.modifier)
                  << " planes=" << c.memoryPlaneCount
                  << "  exportable=" << (c.exportable ? "yes" : "no ")
                  << "  importable=" << (c.importable ? "yes" : "no ")
                  << "  " << c.note << "\n";
    }
    std::cout << "\n";

    std::cout << std::left
              << std::setw(16) << "Arm"
              << std::setw(22) << "Modifier"
              << std::setw(10) << "Import"
              << std::setw(10) << "Content"
              << std::setw(24) << "Stopped at"
              << "Result\n"
              << std::string(108, '-') << "\n";

    for (const auto& r : results) {
        const char* importStr = r.importSucceeded ? "OK" : "FAIL";
        const char* contentStr = r.contentVerified
                                   ? "OK"
                                   : (r.contentAttempted ? "FAIL" : "n/a");
        std::cout << std::left
                  << std::setw(16) << r.arm
                  << std::setw(22) << modifierToString(r.modifier)
                  << std::setw(10) << importStr
                  << std::setw(10) << contentStr
                  << std::setw(24) << (r.ok() ? "-" : stepName(r.failedAt))
                  << (r.ok() ? std::string("PASS")
                             : (std::string("FAIL ") + vkResultName(r.vkResult)))
                  << "\n";
    }
    std::cout << std::string(108, '-') << "\n\n";

    for (const auto& r : results) {
        if (r.ok() && r.detail.empty()) {
            continue;
        }
        std::cout << (r.ok() ? "NOTE  " : "FAIL  ")
                  << r.arm << " / modifier " << modifierToString(r.modifier) << "\n";
        if (!r.ok()) {
            std::cout << "      stopped at : " << stepName(r.failedAt)
                      << "   (" << vkResultName(r.vkResult) << ")\n";
        }
        if (!r.detail.empty()) {
            std::cout << "      detail     : " << r.detail << "\n";
        }
        std::cout << "      devices    : export VkDevice 0x" << std::hex
                  << r.exportDeviceHandle << ", import VkDevice 0x"
                  << r.importDeviceHandle << std::dec << "\n";
        if (r.exportAllocSize != 0) {
            std::cout << "      export     : size=" << r.exportAllocSize
                      << " memTypeIdx=" << r.exportMemTypeIndex
                      << " modifier=" << modifierToString(r.readbackModifier) << "\n";
        }
        if (r.importMemReqSize != 0) {
            std::cout << "      import     : reqSize=" << r.importMemReqSize
                      << " reqBits=0x" << std::hex << r.importMemTypeBits
                      << " fdBits=0x" << r.fdMemTypeBits << std::dec
                      << " chosen=" << (int64_t)(int32_t)r.importMemTypeIndex << "\n";
        }
        if (r.firstMismatchOffset != UINT64_MAX) {
            std::cout << "      first bad  : byte " << r.firstMismatchOffset
                      << " expected 0x" << std::hex << r.expectedByte
                      << " got 0x" << r.actualByte << std::dec
                      << " (after " << r.comparedBytes << " matching bytes)\n";
        }
        std::cout << "\n";
    }
}

Verdict LinuxDmaBufImportTest::verdict(const std::vector<ArmResult>& results,
                                       std::string& reasonOut) const
{
    if (results.empty()) {
        reasonOut = "no arm ran: no usable modifier survived the capability query";
        return Verdict::CouldNotRun;
    }

    // Pair the arms per modifier so a failure can be attributed.
    for (const auto& r : results) {
        if (r.arm != "second-device" || r.ok()) {
            continue;
        }
        bool controlAlsoFailed = false;
        bool controlRan = false;
        for (const auto& c : results) {
            if ((c.arm == "same-device") && (c.modifier == r.modifier)) {
                controlRan = true;
                controlAlsoFailed = !c.ok();
            }
        }
        reasonOut = std::string("second-device arm failed at ") +
                    stepName(r.failedAt) + " for modifier " +
                    modifierToString(r.modifier);
        if (controlRan && controlAlsoFailed) {
            reasonOut += " -- the same-device control failed too, so this is an "
                         "export/environment defect and says NOTHING about "
                         "device independence";
        } else if (controlRan) {
            reasonOut += " -- the same-device control PASSED, so this is a "
                         "device-independence defect";
        }
        return Verdict::ClaimFailed;
    }

    for (const auto& r : results) {
        if ((r.arm == "second-device") && r.importSucceeded && r.contentVerified) {
            reasonOut = "an image exported by VkDevice A was imported by a "
                        "different VkDevice B on the same VkPhysicalDevice, and "
                        "B read back exactly the bytes A wrote";
            return Verdict::ClaimVerified;
        }
    }

    if (!m_config.contentCheck) {
        reasonOut = "--no-content-check was passed: import reachability was "
                    "exercised, memory USABILITY was not. This is not a pass.";
        return Verdict::CouldNotRun;
    }

    reasonOut = "every second-device import returned VK_SUCCESS but no content "
                "round-trip completed, so nothing proved the memory is usable";
    return Verdict::CouldNotRun;
}

} // namespace linux_dmabuf_import_test

#endif // __linux__
