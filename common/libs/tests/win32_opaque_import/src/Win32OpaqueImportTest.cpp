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

#ifdef _WIN32

#include "Win32OpaqueImportTest.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>

namespace win32_opaque_import_test {

//=============================================================================
// String helpers
//=============================================================================

std::string Win32OpaqueImportTest::usageToString(VkImageUsageFlags u) {
    std::string s;
    if (u & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)            s += "XFER_SRC|";
    if (u & VK_IMAGE_USAGE_TRANSFER_DST_BIT)            s += "XFER_DST|";
    if (u & VK_IMAGE_USAGE_SAMPLED_BIT)                 s += "SAMPLED|";
    if (u & VK_IMAGE_USAGE_STORAGE_BIT)                 s += "STORAGE|";
    if (u & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR)    s += "ENCODE_SRC|";
    if (u & VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR)    s += "DECODE_DST|";
    if (!s.empty()) s.pop_back(); // trailing '|'
    return s.empty() ? "(none)" : s;
}

std::string Win32OpaqueImportTest::flagsToString(VkImageCreateFlags f) {
    if (f == 0) return "-";
    std::string s;
    if (f & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)                 s += "EXTENDED|";
    if (f & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)                 s += "MUTABLE|";
    if (f & VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR)  s += "VPI|";
    if (!s.empty()) s.pop_back();
    return s;
}

//=============================================================================
// Lifecycle
//=============================================================================

Win32OpaqueImportTest::~Win32OpaqueImportTest() {
    if (m_importDevice != VK_NULL_HANDLE) {
        auto pfnDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
            m_vkDevCtx.GetDeviceProcAddr(m_importDevice, "vkDestroyDevice"));
        if (pfnDestroyDevice)
            pfnDestroyDevice(m_importDevice, nullptr);
    }
}

VkResult Win32OpaqueImportTest::init(const TestConfig& config) {
    m_config = config;

    static const char* const requiredInstanceExtensions[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        nullptr
    };
    m_vkDevCtx.AddReqInstanceExtensions(requiredInstanceExtensions);

    if (config.validation) {
        static const char* const layers[] = {
            "VK_LAYER_KHRONOS_validation", nullptr
        };
        static const char* const debugExt[] = {
            VK_EXT_DEBUG_REPORT_EXTENSION_NAME, nullptr
        };
        m_vkDevCtx.AddReqInstanceLayers(layers);
        m_vkDevCtx.AddReqInstanceExtensions(debugExt);
        std::cout << "[INFO] Validation layers enabled\n";
    }

    static const char* const requiredDevExts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        nullptr
    };
    static const char* const videoCommonExts[] = {
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        nullptr
    };
    static const char* const videoEncExts[] = {
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        nullptr
    };

    m_vkDevCtx.AddReqDeviceExtensions(requiredDevExts, config.verbose);
    m_vkDevCtx.AddOptDeviceExtensions(videoCommonExts, config.verbose);
    m_vkDevCtx.AddOptDeviceExtensions(videoEncExts, config.verbose);

    VkResult result = m_vkDevCtx.InitVulkanDevice(
        "Win32OpaqueImportTest", VK_NULL_HANDLE, config.verbose);
    if (result != VK_SUCCESS) {
        std::cerr << "[ERROR] InitVulkanDevice failed: " << result << "\n";
        return result;
    }

    if (config.validation)
        m_vkDevCtx.InitDebugReport(true, true);

    vk::DeviceUuidUtils deviceUuid;
    result = m_vkDevCtx.InitPhysicalDevice(
        -1, deviceUuid,
        VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT,
        nullptr,
        0, VK_VIDEO_CODEC_OPERATION_NONE_KHR,
        0, VK_VIDEO_CODEC_OPERATION_NONE_KHR);
    if (result != VK_SUCCESS) {
        std::cerr << "[ERROR] InitPhysicalDevice failed: " << result << "\n";
        return result;
    }

    VkPhysicalDeviceProperties props;
    m_vkDevCtx.GetPhysicalDeviceProperties(m_vkDevCtx.getPhysicalDevice(), &props);
    std::cout << "[INFO] Device: " << props.deviceName
              << "  driver=" << VK_VERSION_MAJOR(props.driverVersion) << "."
              << VK_VERSION_MINOR(props.driverVersion) << "."
              << VK_VERSION_PATCH(props.driverVersion) << "\n";

    result = m_vkDevCtx.CreateVulkanDevice(
        0, 0, VK_VIDEO_CODEC_OPERATION_NONE_KHR,
        true, false, false, true);
    if (result != VK_SUCCESS) {
        std::cerr << "[ERROR] CreateVulkanDevice failed: " << result << "\n";
        return result;
    }

    m_pfnGetMemWin32Handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        m_vkDevCtx.GetDeviceProcAddr(m_vkDevCtx.getDevice(),
                                     "vkGetMemoryWin32HandleKHR"));
    if (!m_pfnGetMemWin32Handle) {
        std::cerr << "[ERROR] vkGetMemoryWin32HandleKHR not available\n";
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    m_vkDevCtx.GetPhysicalDeviceMemoryProperties(
        m_vkDevCtx.getPhysicalDevice(), &m_memProps);

    VkResult impResult = createImportDevice();
    if (impResult != VK_SUCCESS) {
        std::cerr << "[ERROR] createImportDevice failed: " << impResult << "\n";
        return impResult;
    }

    return VK_SUCCESS;
}

//=============================================================================
// Create a SECOND VkDevice on the same physical device for imports.
// Simulates cross-process import (different device, same GPU).
//=============================================================================

VkResult Win32OpaqueImportTest::createImportDevice() {
    VkPhysicalDevice physDev = m_vkDevCtx.getPhysicalDevice();

    const char* extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
    };
    constexpr uint32_t extCount = sizeof(extensions) / sizeof(extensions[0]);

    uint32_t availCount = 0;
    m_vkDevCtx.EnumerateDeviceExtensionProperties(physDev, nullptr, &availCount, nullptr);
    std::vector<VkExtensionProperties> avail(availCount);
    m_vkDevCtx.EnumerateDeviceExtensionProperties(physDev, nullptr, &availCount, avail.data());

    std::vector<const char*> enabled;
    for (uint32_t i = 0; i < extCount; ++i) {
        for (const auto& a : avail) {
            if (strcmp(extensions[i], a.extensionName) == 0) {
                enabled.push_back(extensions[i]);
                break;
            }
        }
    }

    float priority = 1.0f;
    uint32_t gfxFamily = 0;
    uint32_t qfCount = 0;
    m_vkDevCtx.GetPhysicalDeviceQueueFamilyProperties(physDev, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfProps(qfCount);
    m_vkDevCtx.GetPhysicalDeviceQueueFamilyProperties(physDev, &qfCount, qfProps.data());
    for (uint32_t i = 0; i < qfCount; ++i) {
        if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfxFamily = i; break; }
    }

    VkDeviceQueueCreateInfo queueCI{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueCI.queueFamilyIndex = gfxFamily;
    queueCI.queueCount = 1;
    queueCI.pQueuePriorities = &priority;

    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    ycbcrFeat.samplerYcbcrConversion = VK_TRUE;

    VkDeviceCreateInfo devCI{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    devCI.pNext = &ycbcrFeat;
    devCI.queueCreateInfoCount = 1;
    devCI.pQueueCreateInfos = &queueCI;
    devCI.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    devCI.ppEnabledExtensionNames = enabled.data();

    auto pfnCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
        m_vkDevCtx.GetInstanceProcAddr(m_vkDevCtx.getInstance(), "vkCreateDevice"));
    if (!pfnCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = pfnCreateDevice(physDev, &devCI, nullptr, &m_importDevice);
    if (result != VK_SUCCESS) return result;

    auto load = [&](const char* name) {
        return m_vkDevCtx.GetDeviceProcAddr(m_importDevice, name);
    };
    m_imp_CreateImage               = reinterpret_cast<PFN_vkCreateImage>(load("vkCreateImage"));
    m_imp_DestroyImage              = reinterpret_cast<PFN_vkDestroyImage>(load("vkDestroyImage"));
    m_imp_GetImageMemoryRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(load("vkGetImageMemoryRequirements"));
    m_imp_AllocateMemory            = reinterpret_cast<PFN_vkAllocateMemory>(load("vkAllocateMemory"));
    m_imp_FreeMemory                = reinterpret_cast<PFN_vkFreeMemory>(load("vkFreeMemory"));
    m_imp_BindImageMemory           = reinterpret_cast<PFN_vkBindImageMemory>(load("vkBindImageMemory"));

    std::cout << "[INFO] Import device created (separate VkDevice on same GPU)\n";
    return VK_SUCCESS;
}

//=============================================================================
// Memory type selection
//=============================================================================

uint32_t Win32OpaqueImportTest::findMemoryType(
    uint32_t typeBits, VkMemoryPropertyFlags reqProps) const
{
    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (m_memProps.memoryTypes[i].propertyFlags & reqProps) == reqProps)
            return i;
    }
    return UINT32_MAX;
}

//=============================================================================
// Single export → import test
//=============================================================================

TestCaseResult Win32OpaqueImportTest::runSingleTest(
    uint32_t id,
    VkImageUsageFlags exportUsage,
    VkImageCreateFlags exportFlags)
{
    TestCaseResult tc;
    tc.id           = id;
    tc.exportUsage  = exportUsage;
    tc.exportFlags  = exportFlags;

    VkDevice device     = m_vkDevCtx.getDevice();
    VkFormat format     = m_config.format;
    uint32_t w          = m_config.width;
    uint32_t h          = m_config.height;

    // ── 1. Create exportable image ────────────────────────────────────
    VkExternalMemoryImageCreateInfo extMemCI{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extMemCI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkImageCreateInfo imageCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCI.pNext        = &extMemCI;
    imageCI.flags        = exportFlags;
    imageCI.imageType    = VK_IMAGE_TYPE_2D;
    imageCI.format       = format;
    imageCI.extent       = {w, h, 1};
    imageCI.mipLevels    = 1;
    imageCI.arrayLayers  = 1;
    imageCI.samples      = VK_SAMPLE_COUNT_1_BIT;
    imageCI.tiling       = VK_IMAGE_TILING_OPTIMAL;
    imageCI.usage        = exportUsage;
    imageCI.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage exportImage = VK_NULL_HANDLE;
    tc.exportCreateResult = m_vkDevCtx.CreateImage(
        device, &imageCI, nullptr, &exportImage);
    if (tc.exportCreateResult != VK_SUCCESS) {
        if (m_config.verbose)
            std::cout << "  [" << id << "] CreateImage FAIL " << tc.exportCreateResult
                      << "  usage=0x" << std::hex << exportUsage
                      << " flags=0x" << exportFlags << std::dec << "\n";
        return tc;
    }

    // ── 2. Allocate with export + dedicated ───────────────────────────
    VkMemoryRequirements memReqs{};
    m_vkDevCtx.GetImageMemoryRequirements(device, exportImage, &memReqs);
    tc.exportAllocSize = memReqs.size;

    VkMemoryDedicatedAllocateInfo dedicatedAI{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicatedAI.image = exportImage;

    VkExportMemoryAllocateInfo exportAI{
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exportAI.pNext       = &dedicatedAI;
    exportAI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.pNext           = &exportAI;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    tc.exportMemTypeIdx = allocInfo.memoryTypeIndex;

    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        tc.exportAllocResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        m_vkDevCtx.DestroyImage(device, exportImage, nullptr);
        return tc;
    }

    VkDeviceMemory exportMem = VK_NULL_HANDLE;
    tc.exportAllocResult = m_vkDevCtx.AllocateMemory(
        device, &allocInfo, nullptr, &exportMem);
    if (tc.exportAllocResult != VK_SUCCESS) {
        if (m_config.verbose)
            std::cout << "  [" << id << "] Export AllocateMemory FAIL "
                      << tc.exportAllocResult << "\n";
        m_vkDevCtx.DestroyImage(device, exportImage, nullptr);
        return tc;
    }

    m_vkDevCtx.BindImageMemory(device, exportImage, exportMem, 0);

    // ── 3. Export Win32 handle ────────────────────────────────────────
    VkMemoryGetWin32HandleInfoKHR getHandleInfo{
        VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    getHandleInfo.memory     = exportMem;
    getHandleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    HANDLE exportHandle = nullptr;
    tc.exportHandleResult = m_pfnGetMemWin32Handle(
        device, &getHandleInfo, &exportHandle);

    if (tc.exportHandleResult != VK_SUCCESS || !exportHandle) {
        if (m_config.verbose)
            std::cout << "  [" << id << "] vkGetMemoryWin32HandleKHR FAIL "
                      << tc.exportHandleResult << "\n";
        m_vkDevCtx.FreeMemory(device, exportMem, nullptr);
        m_vkDevCtx.DestroyImage(device, exportImage, nullptr);
        return tc;
    }

    if (m_config.verbose) {
        std::cout << "  [" << id << "] Export OK  usage=0x" << std::hex << exportUsage
                  << " flags=0x" << exportFlags << std::dec
                  << " size=" << memReqs.size
                  << " memIdx=" << tc.exportMemTypeIdx << "\n";
    }

    // ── 4. Graphics import (strip VIDEO_ENCODE_SRC, VIDEO_PROFILE_INDEPENDENT) ──
    {
        VkImageUsageFlags gfxUsage = exportUsage
            & ~(VkImageUsageFlags)VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR
            & ~(VkImageUsageFlags)VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
        VkImageCreateFlags gfxFlags = exportFlags
            & ~(VkImageCreateFlags)VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;

        tc.graphicsImport = tryImport("gfx", gfxUsage, gfxFlags,
                                      exportHandle, memReqs.size);
    }

    // ── 5. Video import (same usage/flags as export) ─────────────────
    {
        tc.videoImport = tryImport("video", exportUsage, exportFlags,
                                   exportHandle, memReqs.size);
    }

    // ── Cleanup ──────────────────────────────────────────────────────
    CloseHandle(exportHandle);
    m_vkDevCtx.FreeMemory(device, exportMem, nullptr);
    m_vkDevCtx.DestroyImage(device, exportImage, nullptr);

    return tc;
}

//=============================================================================
// Try a single import
//=============================================================================

ImportAttempt Win32OpaqueImportTest::tryImport(
    const char* label,
    VkImageUsageFlags usage,
    VkImageCreateFlags flags,
    HANDLE memoryHandle,
    VkDeviceSize exportAllocSize)
{
    ImportAttempt att;
    att.label = label;
    att.usage = usage;
    att.flags = flags;

    // Import on the SECOND device (simulates cross-process import)
    VkDevice device = m_importDevice;

    VkExternalMemoryImageCreateInfo extMemCI{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extMemCI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkImageCreateInfo imageCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCI.pNext        = &extMemCI;
    imageCI.flags        = flags;
    imageCI.imageType    = VK_IMAGE_TYPE_2D;
    imageCI.format       = m_config.format;
    imageCI.extent       = {m_config.width, m_config.height, 1};
    imageCI.mipLevels    = 1;
    imageCI.arrayLayers  = 1;
    imageCI.samples      = VK_SAMPLE_COUNT_1_BIT;
    imageCI.tiling       = VK_IMAGE_TILING_OPTIMAL;
    imageCI.usage        = usage;
    imageCI.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    att.createResult = m_imp_CreateImage(device, &imageCI, nullptr, &image);
    if (att.createResult != VK_SUCCESS) {
        if (m_config.verbose)
            std::cout << "      [" << label << "] CreateImage FAIL " << att.createResult
                      << "  usage=0x" << std::hex << usage
                      << " flags=0x" << flags << std::dec << "\n";
        return att;
    }

    VkMemoryRequirements memReqs{};
    m_imp_GetImageMemoryRequirements(device, image, &memReqs);
    att.memReqSize = memReqs.size;

    VkMemoryDedicatedAllocateInfo dedicatedAI{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicatedAI.image = image;

    VkImportMemoryWin32HandleInfoKHR importMI{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    importMI.pNext      = &dedicatedAI;
    importMI.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    importMI.handle     = memoryHandle;

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.pNext           = &importMI;
    allocInfo.allocationSize  = exportAllocSize;
    allocInfo.memoryTypeIndex = findMemoryType(
        memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    att.memTypeIdx = allocInfo.memoryTypeIndex;

    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        att.allocResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        m_imp_DestroyImage(device, image, nullptr);
        return att;
    }

    VkDeviceMemory memory = VK_NULL_HANDLE;
    att.allocResult = m_imp_AllocateMemory(device, &allocInfo, nullptr, &memory);

    if (att.allocResult == VK_SUCCESS) {
        att.bindResult = m_imp_BindImageMemory(device, image, memory, 0);
        m_imp_FreeMemory(device, memory, nullptr);
    }

    m_imp_DestroyImage(device, image, nullptr);

    if (m_config.verbose) {
        std::cout << "      [" << label << "] alloc="
                  << att.allocResult
                  << " (size=" << att.memReqSize
                  << " exportSize=" << exportAllocSize
                  << " memIdx=" << att.memTypeIdx << ")\n";
    }

    return att;
}

//=============================================================================
// Run full matrix
//=============================================================================

std::vector<TestCaseResult> Win32OpaqueImportTest::runAllCombinations() {
    std::vector<TestCaseResult> results;

    const VkImageUsageFlags baseUsage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    const VkImageUsageFlags optionalUsages[] = {
        VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
    };

    const VkImageCreateFlags optionalFlags[] = {
        VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR,
    };

    std::cout << "\n=== Win32 Opaque Handle Import Test Matrix ===\n"
              << "Format:  NV12 (VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)\n"
              << "Size:    " << m_config.width << "x" << m_config.height << "\n"
              << "Handle:  VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT\n"
              << "Import:  graphics (strip VIDEO_ENCODE_SRC) + video (same as export)\n\n";

    uint32_t id = 0;
    for (uint32_t uMask = 0; uMask < 8; ++uMask) {
        VkImageUsageFlags usage = baseUsage;
        for (int i = 0; i < 3; ++i)
            if (uMask & (1u << i)) usage |= optionalUsages[i];

        for (uint32_t fMask = 0; fMask < 8; ++fMask) {
            VkImageCreateFlags flags = 0;
            for (int i = 0; i < 3; ++i)
                if (fMask & (1u << i)) flags |= optionalFlags[i];

            results.push_back(runSingleTest(++id, usage, flags));
        }
    }

    printResults(results);
    return results;
}

//=============================================================================
// Print results
//=============================================================================

void Win32OpaqueImportTest::printResults(
    const std::vector<TestCaseResult>& results) const
{
    auto statusStr = [](VkResult r) -> const char* {
        switch (r) {
            case VK_SUCCESS: return "OK";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "FAIL(-2)";
            case VK_ERROR_OUT_OF_HOST_MEMORY: return "FAIL(-1)";
            case VK_ERROR_FORMAT_NOT_SUPPORTED: return "UNSUPPORTED";
            case VK_ERROR_FEATURE_NOT_PRESENT: return "NO_FEAT";
            default: {
                static thread_local char buf[32];
                snprintf(buf, sizeof(buf), "ERR(%d)", (int)r);
                return buf;
            }
        }
    };

    auto importStatus = [&](const ImportAttempt& a) -> std::string {
        if (a.createResult != VK_SUCCESS)
            return std::string("IMG_") + statusStr(a.createResult);
        if (a.allocResult != VK_SUCCESS)
            return statusStr(a.allocResult);
        if (a.bindResult != VK_SUCCESS)
            return std::string("BIND_") + statusStr(a.bindResult);
        return "OK";
    };

    // Header
    std::cout << "\n"
        << std::string(112, '=') << "\n"
        << "                     WIN32 OPAQUE IMPORT TEST RESULTS\n"
        << std::string(112, '=') << "\n\n";

    std::cout << std::left
              << std::setw(4)  << "#"
              << std::setw(38) << "Export Usage"
              << std::setw(22) << "Export Flags"
              << std::setw(8)  << "Export"
              << std::setw(10) << "AllocSz"
              << std::setw(12) << "GfxImport"
              << std::setw(12) << "VidImport"
              << "\n"
              << std::string(112, '-') << "\n";

    int totalExportOK = 0, totalGfxOK = 0, totalVidOK = 0;
    int totalGfxFail = 0, totalVidFail = 0;

    for (const auto& tc : results) {
        std::string expStr;
        if (tc.exportCreateResult != VK_SUCCESS)
            expStr = std::string("IMG_") + statusStr(tc.exportCreateResult);
        else if (tc.exportAllocResult != VK_SUCCESS)
            expStr = std::string("MEM_") + statusStr(tc.exportAllocResult);
        else if (tc.exportHandleResult != VK_SUCCESS)
            expStr = std::string("HDL_") + statusStr(tc.exportHandleResult);
        else
            expStr = "OK";

        std::string gfxStr = (tc.exportHandleResult != VK_SUCCESS &&
                              tc.exportCreateResult  == VK_SUCCESS)
                             ? "-" : importStatus(tc.graphicsImport);
        std::string vidStr = (tc.exportHandleResult != VK_SUCCESS &&
                              tc.exportCreateResult  == VK_SUCCESS)
                             ? "-" : importStatus(tc.videoImport);

        if (tc.exportCreateResult != VK_SUCCESS) {
            gfxStr = "-";
            vidStr = "-";
        }

        std::cout << std::setw(4)  << tc.id
                  << std::setw(38) << usageToString(tc.exportUsage)
                  << std::setw(22) << flagsToString(tc.exportFlags)
                  << std::setw(8)  << expStr
                  << std::setw(10) << (tc.exportCreateResult == VK_SUCCESS ? std::to_string(tc.exportAllocSize) : "-")
                  << std::setw(12) << gfxStr
                  << std::setw(12) << vidStr
                  << "\n";

        if (tc.exportCreateResult == VK_SUCCESS &&
            tc.exportAllocResult == VK_SUCCESS &&
            tc.exportHandleResult == VK_SUCCESS) {
            totalExportOK++;
            if (tc.graphicsImport.allocResult == VK_SUCCESS &&
                tc.graphicsImport.createResult == VK_SUCCESS)
                totalGfxOK++;
            else if (tc.graphicsImport.createResult == VK_SUCCESS)
                totalGfxFail++;

            if (tc.videoImport.allocResult == VK_SUCCESS &&
                tc.videoImport.createResult == VK_SUCCESS)
                totalVidOK++;
            else if (tc.videoImport.createResult == VK_SUCCESS)
                totalVidFail++;
        }
    }

    std::cout << std::string(112, '-') << "\n\n";

    // Summary
    std::cout << "SUMMARY:\n"
              << "  Total combinations:      " << results.size() << "\n"
              << "  Export succeeded:         " << totalExportOK << "\n"
              << "  Graphics import OK:       " << totalGfxOK << "\n"
              << "  Graphics import FAIL:     " << totalGfxFail << "\n"
              << "  Video import OK:          " << totalVidOK << "\n"
              << "  Video import FAIL:        " << totalVidFail << "\n\n";

    // Highlight failures
    bool anyFail = false;
    for (const auto& tc : results) {
        if (tc.exportCreateResult != VK_SUCCESS) continue;
        if (tc.exportAllocResult  != VK_SUCCESS) continue;
        if (tc.exportHandleResult != VK_SUCCESS) continue;

        bool gfxFail = tc.graphicsImport.createResult == VK_SUCCESS &&
                       tc.graphicsImport.allocResult != VK_SUCCESS;
        bool vidFail = tc.videoImport.createResult == VK_SUCCESS &&
                       tc.videoImport.allocResult != VK_SUCCESS;

        if (gfxFail || vidFail) {
            if (!anyFail) {
                std::cout << "IMPORT FAILURES (export OK but import alloc failed):\n";
                anyFail = true;
            }
            std::cout << "  #" << tc.id << "  export usage=0x" << std::hex << tc.exportUsage
                      << " flags=0x" << tc.exportFlags << std::dec
                      << "  export_size=" << tc.exportAllocSize;
            if (gfxFail)
                std::cout << "  GFX_IMPORT=" << tc.graphicsImport.allocResult
                          << " (size=" << tc.graphicsImport.memReqSize << ")";
            if (vidFail)
                std::cout << "  VID_IMPORT=" << tc.videoImport.allocResult
                          << " (size=" << tc.videoImport.memReqSize << ")";
            std::cout << "\n";
        }
    }
    if (!anyFail)
        std::cout << "No import failures detected!\n";

    std::cout << "\n";
}

} // namespace win32_opaque_import_test

#endif // _WIN32
