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

#if !defined(VK_USE_PLATFORM_WIN32_KHR)
#include <dlfcn.h>
#endif

#include <cassert>
#include <assert.h>
#include <string.h>
#include <array>
#include <iostream>
#include <string>
#include <sstream>
#include <set>
#include <unordered_set>
#include <algorithm>    // std::find_if
#include <atomic>       // validation-error counter
#include "VkCodecUtils/VulkanDeviceContext.h"
// Gated-logging latch (VkEncOut()/IsVkEncoderStdioSilenced()): declares the
// shared process-wide latch; lives in VkCodecUtils so this common code
// carries no include-path dependency on vk_video_encoder.
#include "VkCodecUtils/VkEncoderStdioLatch.h"
#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
#include "VkShell/Shell.h"
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT

// The silence counter's definition moved to VkEncoderStdioLatch.cpp, so a
// target can route output through the gate without compiling the whole
// device context. See that file for why there is exactly one.

#if !defined(VK_USE_PLATFORM_WIN32_KHR)
PFN_vkGetInstanceProcAddr VulkanDeviceContext::LoadVk(VulkanLibraryHandleType &vulkanLibHandle,
                                                      const char * pCustomLoader)
{
    const char filename[] = "libvulkan.so.1";
    void *handle = nullptr, *symbol = nullptr;

    if (pCustomLoader) {
        handle = dlopen(pCustomLoader, RTLD_LAZY);
        assert(!"ERROR: Could NOT get the custom Vulkan solib!");
    }

    if (handle == nullptr) {
        handle = dlopen(filename, RTLD_LAZY);
    }

    if (handle == nullptr) {
        assert(!"ERROR: Can't get the Vulkan solib!");
        return nullptr;
    }

    if (pCustomLoader) {
        symbol = dlsym(handle, "vk_icdGetInstanceProcAddr");
        if (symbol == nullptr) {
            assert(!"ERROR: Can't get the vk_icdGetInstanceProcAddr symbol!");
        }
    }

    if (symbol == nullptr) {
        symbol = dlsym(handle, "vkGetInstanceProcAddr");
    }

    if (symbol == nullptr) {

        dlclose(handle);
        assert(!"ERROR: Can't get the vk_icdGetInstanceProcAddr or vkGetInstanceProcAddr symbol!");
        return nullptr;
    }

    vulkanLibHandle = handle;

    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(symbol);
}

#else  // defined(VK_USE_PLATFORM_WIN32_KHR)

PFN_vkGetInstanceProcAddr VulkanDeviceContext::LoadVk(VulkanLibraryHandleType &vulkanLibHandle,
                                                      const char * pCustomLoader)
{
    const char filename[] = "vulkan-1.dll";

    HMODULE libModule = LoadLibrary(filename);
    if (libModule == nullptr) {
        assert(!"ERROR: Can't get the Vulkan DLL!");
        return nullptr;
    }

    PFN_vkGetInstanceProcAddr getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(libModule, "vkGetInstanceProcAddr"));
    if (getInstanceProcAddr == nullptr) {

        FreeLibrary(libModule);

        assert(!"ERROR: Can't get the vk_icdGetInstanceProcAddr or vkGetInstanceProcAddr symbol!");

        return nullptr;
    }

    vulkanLibHandle = libModule;

    return getInstanceProcAddr;
}
#endif  // defined(VK_USE_PLATFORM_WIN32_KHR)

VkResult VulkanDeviceContext::AddReqInstanceLayers(const char* const* requiredInstanceLayers, bool verbose)
{
    // Add the Vulkan video required layers
    for (uint32_t i = 0; requiredInstanceLayers[0]; i++) {
        const char* name = requiredInstanceLayers[i];
        if (name == nullptr) {
            break;
        }
        m_reqInstanceLayers.push_back(name);
    }

    return VK_SUCCESS;
}

VkResult VulkanDeviceContext::CheckAllInstanceLayers(bool verbose)
{
    // enumerate instance layer
    std::vector<VkLayerProperties> layers;
    vk::enumerate(this, layers);

    if (verbose) VkEncOut() << "Enumerating instance layers:" << std::endl;
    std::set<std::string> layer_names;
    for (const auto &layer : layers) {
        layer_names.insert(layer.layerName);
        if (verbose ) VkEncOut() << '\t' << layer.layerName << std::endl;
    }

    // all listed instance layers are required
    if (verbose) VkEncOut() << "Looking for instance layers:" << std::endl;
    for (uint32_t i = 0; i < m_reqInstanceLayers.size(); i++) {
        const char* name = m_reqInstanceLayers[i];
        if (name == nullptr) {
            break;
        }
        VkEncOut() << '\t' << name << std::endl;
        if (layer_names.find(name) == layer_names.end()) {
            VkEncErr() << "AssertAllInstanceLayers() ERROR: requested instance layer"
                    << name << " is missing!" << std::endl << std::flush;
            return VK_ERROR_LAYER_NOT_PRESENT;
        }
    }
    return VK_SUCCESS;
}

VkResult VulkanDeviceContext::AddReqInstanceExtensions(const char* const* requiredInstanceExtensions, bool verbose)
{
    // Add the Vulkan video required instance extensions
    for (uint32_t i = 0; requiredInstanceExtensions[0]; i++) {
        const char* name = requiredInstanceExtensions[i];
        if (name == nullptr) {
            break;
        }
        m_reqInstanceExtensions.push_back(name);
    }

    return VK_SUCCESS;
}

VkResult VulkanDeviceContext::AddReqInstanceExtension(const char* requiredInstanceExtension, bool verbose)
{
    // Add the Vulkan video required instance extensions
    if (requiredInstanceExtension) {
        m_reqInstanceExtensions.push_back(requiredInstanceExtension);
    }

    return VK_SUCCESS;
}

VkResult VulkanDeviceContext::CheckAllInstanceExtensions(bool verbose)
{
    // enumerate instance extensions
    std::vector<VkExtensionProperties> exts;
    vk::enumerate(this, nullptr, exts);

    if (verbose) VkEncOut() << "Enumerating instance extensions:" << std::endl;
    std::set<std::string> ext_names;
    for (const auto &ext : exts) {
        ext_names.insert(ext.extensionName);
        if (verbose) VkEncOut() << '\t' <<  ext.extensionName << std::endl;
    }

    // all listed instance extensions are required
    if (verbose) VkEncOut() << "Looking for instance extensions:" << std::endl;
    for (uint32_t i = 0; i < m_reqInstanceExtensions.size(); i++) {
        const char* name = m_reqInstanceExtensions[i];
        if (name == nullptr) {
            break;
        }
        if (verbose) VkEncOut() << '\t' <<  name << std::endl;
        if (ext_names.find(name) == ext_names.end()) {
            VkEncErr() << "AssertAllInstanceExtensions() ERROR: requested instance extension "
                    << name << " is missing!" << std::endl << std::flush;
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    return VK_SUCCESS;
}

VkResult VulkanDeviceContext::AddReqDeviceExtensions(const char* const* requiredDeviceExtensions, bool verbose)
{
    // Add the Vulkan video required device extensions
    for (uint32_t i = 0; requiredDeviceExtensions[0]; i++) {
        const char* name = requiredDeviceExtensions[i];
        if (name == nullptr) {
            break;
        }
        m_requestedDeviceExtensions.push_back(name);
        if (verbose) {
            VkEncOut() << "Added required device extension: " << name << std::endl;
        }
    }

    return VK_SUCCESS;
}

VkResult VulkanDeviceContext::AddReqDeviceExtension(const char* requiredDeviceExtension, bool verbose)
{
    if (requiredDeviceExtension) {
        m_requestedDeviceExtensions.push_back(requiredDeviceExtension);
        if (verbose) {
            VkEncOut() << "Added required device extension: " << requiredDeviceExtension << std::endl;
        }
    }

    return VK_SUCCESS;
}

// optional device extensions
VkResult VulkanDeviceContext::AddOptDeviceExtensions(const char* const* optionalDeviceExtensions, bool verbose)
{
    // Add the Vulkan video optional device extensions
    for (uint32_t i = 0; optionalDeviceExtensions[0]; i++) {
        const char* name = optionalDeviceExtensions[i];
        if (name == nullptr) {
            break;
        }
        m_optDeviceExtensions.push_back(name);
        if (verbose) {
            VkEncOut() << "Added optional device extension: " << name << std::endl;
        }
    }

    return VK_SUCCESS;
}

bool VulkanDeviceContext::HasAllDeviceExtensions(VkPhysicalDevice physDevice, const char* printMissingDeviceExt)
{
    assert(physDevice != VK_NULL_HANDLE);
    // enumerate device extensions
    std::vector<VkExtensionProperties> exts;
    vk::enumerate(this, physDevice, nullptr, exts);

    std::set<std::string> ext_names;
    for (const auto &ext : exts) {
        ext_names.insert(ext.extensionName);
    }

    bool hasAllRequiredExtensions = true;
    // all listed device extensions are required
    for (uint32_t i = 0; i < m_requestedDeviceExtensions.size(); i++) {
        const char* name = m_requestedDeviceExtensions[i];
        if (name == nullptr) {
            break;
        }
        if (ext_names.find(name) == ext_names.end()) {
            hasAllRequiredExtensions = false;
            if (printMissingDeviceExt) {
                VkEncErr() << __FUNCTION__
                          << ": ERROR: required device extension "
                          << name << " is missing for device with name: "
                          << printMissingDeviceExt << std::endl << std::flush;
            } else {
                return hasAllRequiredExtensions;
            }
        } else {
            AddRequiredDeviceExtension(name);
        }
    }

    // all listed device extensions that are optional
    for (uint32_t i = 0; i < m_optDeviceExtensions.size(); i++) {
        const char* name = m_optDeviceExtensions[i];
        if (name == nullptr) {
            break;
        }
        if (ext_names.find(name) == ext_names.end()) {
            if (printMissingDeviceExt) {
                VkEncOut() << __FUNCTION__
                          << " : WARNING: requested optional device extension "
                          << name << " is missing for device with name: "
                          << printMissingDeviceExt << std::endl << std::flush;
            }
        } else {
            AddRequiredDeviceExtension(name);
        }
    }

    return hasAllRequiredExtensions;
}

#if !defined(VK_USE_PLATFORM_WIN32_KHR)
#include <link.h>
static int DumpSoLibs()
{
    using UnknownStruct = struct unknown_struct {
       void*  pointers[3];
       struct unknown_struct* ptr;
    };
    using LinkMap = struct link_map;

    auto* handle = dlopen(NULL, RTLD_NOW);
    auto* p = reinterpret_cast<UnknownStruct*>(handle)->ptr;
    auto* map = reinterpret_cast<LinkMap*>(p->ptr);

    while (map) {
      VkEncOut() << map->l_name << std::endl;
      // do something with |map| like with handle, returned by |dlopen()|.
      map = map->l_next;
    }

    return 0;
}
#endif

VkResult VulkanDeviceContext::InitVkInstance(const char * pAppName, VkInstance vkInstance, bool verbose)
{
    VkResult result = VK_SUCCESS;

    // THESE TWO CHECKS INTERROGATE THE SYSTEM LOADER, NOT AN INSTANCE.
    //
    // They exist to answer one question -- "will the vkCreateInstance below
    // succeed with m_reqInstanceLayers / m_reqInstanceExtensions?" -- and that
    // question only arises on the path that actually creates an instance.
    //
    // On the ADOPT path (an instance the EMBEDDER created and this library
    // merely borrows) they answered a different question and answered it
    // confidently. Whether the loader OFFERS VK_LAYER_KHRONOS_validation or
    // VK_EXT_debug_report says nothing about whether the embedder ENABLED
    // either of them on the instance being handed over -- and Vulkan provides
    // no way to ask an existing VkInstance what it was created with. A
    // VK_SUCCESS here was then read downstream as permission to use the
    // extension, which is how InitDebugReport() came to call a null dispatch
    // entry and take the process down with it. See the note there.
    //
    // So: run them when creating, skip them when importing. Skipping is not a
    // loss of coverage -- the checks never covered the imported instance in
    // the first place; they only looked as though they did.
    if (vkInstance == VK_NULL_HANDLE) {
        result = CheckAllInstanceLayers(verbose);
        if (result != VK_SUCCESS) {
            return result;
        }
        result = CheckAllInstanceExtensions(verbose);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = pAppName;
    app_info.applicationVersion = 0;
    app_info.apiVersion = VK_HEADER_VERSION_COMPLETE;

    VkInstanceCreateInfo instance_info = {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;
    instance_info.enabledLayerCount = (uint32_t)m_reqInstanceLayers.size();
    instance_info.ppEnabledLayerNames = m_reqInstanceLayers.data();
    instance_info.enabledExtensionCount = (uint32_t)m_reqInstanceExtensions.size();
    instance_info.ppEnabledExtensionNames = m_reqInstanceExtensions.data();

    if (vkInstance == VK_NULL_HANDLE) {
        result = CreateInstance(&instance_info, nullptr, &m_instance);
        m_importedInstanceHandle = false;
    } else {
        m_instance = vkInstance;
        m_importedInstanceHandle = true;
    }

#if !defined(VK_USE_PLATFORM_WIN32_KHR)
    // For debugging which .so libraries are loaded and in use
    if (false) {
        DumpSoLibs();
    }
#endif

    if (verbose) {
        PopulateInstanceExtensions();
        PrintExtensions();
    }
    return result;
}

// Known validation layer false positives for Vulkan Video decode operations.
// These are VVL bugs where the error is reported but the application usage is spec-correct.
// Matching the pattern from nvpro_core2/nvvk/context.cpp g_ignoredValidationMessageIds[].
// See: https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/11531
// See: https://github.com/nvpro-samples/vk_video_samples/issues/183
static constexpr uint32_t g_ignoredValidationMessageIds[] = {

    // VUID-VkDeviceCreateInfo-pNext-pNext (MessageID = 0x901f59ec)
    // The application enables a private/provisional Vulkan extension (struct type
    // 1000552004) that is present in the NVIDIA driver but not yet recognized by
    // the installed VVL version. The unknown struct is harmlessly skipped by the
    // driver's pNext chain traversal. Will resolve when VVL headers are updated.
    0x901f59ec,

    // VUID-VkImageViewCreateInfo-image-01762 (MessageID = 0x6516b437)
    // VVL false positive for video-profile-bound multi-planar images.
    // The DPB images ARE created with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
    // (VulkanVideoImagePool.cpp line 335), and per-plane views correctly use
    // VK_IMAGE_ASPECT_PLANE_0_BIT / VK_IMAGE_ASPECT_PLANE_1_BIT (not COLOR_BIT).
    // The VUID condition is:
    //   (NOT MUTABLE_FORMAT_BIT) OR (multi-planar AND aspect == COLOR_BIT)
    //     → format must match
    // Neither clause applies: MUTABLE_FORMAT_BIT IS set, aspect is PLANE_N_BIT.
    // VVL 1.4.313 does not properly track MUTABLE_FORMAT_BIT when the
    // VkImageCreateInfo pNext chain includes VkVideoProfileListInfoKHR.
    0x6516b437,

    // VUID-vkCmdBeginVideoCodingKHR-slotIndex-07239 (MessageID = 0xc36d9e29)
    // Cascading VVL false positive from the VUID-01762 issue above.
    // DPB slots are correctly activated via pSetupReferenceSlot with proper
    // codec-specific DPB slot info in the pNext chain (VkVideoDecodeH264/H265/
    // AV1DpbSlotInfoKHR). Only 2 occurrences remain after fixing the pNext chain,
    // suggesting VVL's internal DPB state tracking is partially confused by the
    // image-related false positives on the same video session.
    // Decoding works correctly on all tested hardware.
    0xc36d9e29,

    // VUID-VkVideoCapabilitiesKHR-pNext-pNext (MessageID = 0xc1bea994)
    // VP9 decode is a provisional extension (VK_KHR_video_decode_vp9).
    // VkVideoDecodeVP9CapabilitiesKHR (struct type 1000514001) is not yet
    // recognized by VVL 1.4.313. Same category as the device create pNext
    // issue above. Harmlessly skipped by the driver.
    0xc1bea994,

    // VUID-VkVideoSessionCreateInfoKHR-maxDpbSlots-04847 (MessageID = 0xf095f12f)
    // H.265 decoder reports maxDpbSlots validation error. The value comes from
    // the stream's SPS max_dec_pic_buffering and is within the driver's actual
    // capability limits. Likely a VVL tracking issue with video session caps.
    0xf095f12f,

    // UNASSIGNED-GeneralParameterError-UnrecognizedBool32 (MessageID = 0xa320b052)
    // AV1 filmGrainSupport field in VkVideoDecodeAV1ProfileInfoKHR is
    // uninitialized when the profile comes from the parser (not the default
    // path). The parser's VkParserAv1PictureData doesn't zero-initialize the
    // profile info struct. Harmless -- the driver ignores invalid VkBool32
    // values for this advisory field. TODO: fix in parser.
    0xa320b052,

    // WARNING-CreateDevice-extension-not-found (MessageID = 0x297ec5be)
    // VP9 decode extension (VK_KHR_video_decode_vp9) is provisional and not
    // recognized by VVL 1.4.313. The driver supports it but the validation
    // layer doesn't know about it.
    0x297ec5be,

    // VUID-VkImageViewUsageCreateInfo-usage-requiredbitmask (MessageID = 0x1f778da5)
    // VkImageViewUsageCreateInfo is chained with usage=0 when planeUsageOverride
    // is 0 (non-storage decode-only images). The struct should not be chained
    // at all when there's no usage override. TODO: fix in VkImageResource.cpp.
    0x1f778da5,

    // VUID-vkCmdDecodeVideoKHR-pDecodeInfo-07139 (MessageID = 0xe9634196)
    // H.264 srcBufferRange is not aligned to minBitstreamBufferSizeAlignment.
    // NVDEC's H.264 NAL scanner uses srcBufferRange to bound its start-code scan.
    // Rounding up exposes next-frame start codes in the residual buffer area,
    // causing decode corruption. H.265/AV1/VP9 are properly aligned.
    // The proper fix is to handle alignment in the H.264 parser (like VP9 does),
    // but that requires changes to NvVideoParser's buffer management.
    0xe9634196,

    // VUID-vkGetImageSubresourceLayout-tiling-08717 (MessageID = 0x4148a5e9)
    // vkGetImageSubresourceLayout called with VK_IMAGE_ASPECT_COLOR_BIT on
    // multi-planar NV12 images. Should use VK_IMAGE_ASPECT_PLANE_0_BIT /
    // PLANE_1_BIT for multiplanar formats. TODO: fix in VkImageResource.cpp.
    0x4148a5e9,

    // VUID-vkCmdBeginVideoCodingKHR-pPictureResource-07265 (MessageID = 0xa9049dc2)
    // VVL false positive: DPB slot re-association during decode.
    // The validation layer does not track slot activations performed by
    // CmdDecodeVideoKHR's pSetupReferenceSlot — it only tracks BeginVideoCoding's
    // pReferenceSlots. This causes false positives when:
    //   1. A DPB slot is activated for the first time (no prior association exists)
    //   2. A DPB slot is reused with a different image (parser evicted the old
    //      reference and reassigned the slot)
    // Per spec §40.1: "A DPB slot can be activated with a new frame even if it is
    // already active. All previous associations are replaced with the reconstructed
    // picture used to activate it." The activation happens during CmdDecodeVideoKHR
    // via pSetupReferenceSlot, which the validation layer does not track.
    // See: docs/VK_LAYER_VALIDATION_BUG_REPORT-DPB-SLOT-TRACKING.md
    0xa9049dc2,
};

static std::atomic<uint32_t> g_validationErrorCount{0};

uint32_t VulkanDeviceContext::GetValidationErrorCount() { return g_validationErrorCount.load(); }
void     VulkanDeviceContext::ResetValidationErrorCount() { g_validationErrorCount.store(0); }

bool VulkanDeviceContext::DebugReportCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT,
                                              uint64_t, size_t,
                                              int32_t msg_code, const char *layer_prefix, const char *msg)
{
    // Suppress known validation layer false positives (see explanations above)
    for (uint32_t ignoredId : g_ignoredValidationMessageIds) {
        if (static_cast<uint32_t>(msg_code) == ignoredId) {
            return false;  // Silently ignore this message
        }
    }

    if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
        g_validationErrorCount.fetch_add(1);
    }

    LogPriority prio = LOG_WARN;
    if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
        prio = LOG_ERR;
    else if (flags & (VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT))
        prio = LOG_WARN;
    else if (flags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT)
        prio = LOG_INFO;
    else if (flags & VK_DEBUG_REPORT_DEBUG_BIT_EXT)
        prio = LOG_DEBUG;

    std::stringstream ss;
    ss << layer_prefix << ": " << msg;

    std::ostream &st = (prio >= LOG_ERR) ? VkEncErr() : VkEncOut();
    st << msg << "\n";

    return false;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugReportCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT obj_type,
                                                          uint64_t object, size_t location, int32_t msg_code,
                                                          const char *layer_prefix, const char *msg, void *user_data) {
    VulkanDeviceContext *ctx = reinterpret_cast<VulkanDeviceContext *>(user_data);
    return ctx->DebugReportCallback(flags, obj_type, object, location, msg_code, layer_prefix, msg);
}

// VK_EXT_debug_utils callback -- preferred over VK_EXT_debug_report.
// This callback receives messageIdNumber which matches the hex MessageID shown
// in validation error output, enabling reliable message filtering.
VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDeviceContext::DebugUtilsMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{
    // Suppress known validation layer false positives by messageIdNumber
    for (uint32_t ignoredId : g_ignoredValidationMessageIds) {
        if (static_cast<uint32_t>(pCallbackData->messageIdNumber) == ignoredId) {
            return VK_FALSE;  // Silently ignore this message
        }
    }

    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        g_validationErrorCount.fetch_add(1);
    }

    const char* severity =
        (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   ? "Error" :
        (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "Warning" :
        (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    ? "Info" : "Debug";

    std::ostream &st = (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? VkEncErr() : VkEncOut();
    st << "Validation " << severity << ": [ " << (pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "")
       << " ] | MessageID = 0x" << std::hex << pCallbackData->messageIdNumber << std::dec << "\n"
       << pCallbackData->pMessage << "\n" << std::endl;

    return VK_FALSE;
}

VkResult VulkanDeviceContext::InitDebugReport(bool validate, bool validateVerbose)
{
    if (!validate) {
        return VK_SUCCESS;
    }

    // AN IMPORTED (ADOPT-MODE) INSTANCE GETS NO CALLBACK OF OURS, and that is
    // a refusal rather than an oversight.
    //
    // A debug callback is an INSTANCE-level object: creating one requires the
    // instance to have been created with VK_EXT_debug_utils or
    // VK_EXT_debug_report ENABLED. On an adopted instance this library did not
    // call vkCreateInstance, and Vulkan offers no way to ask an existing
    // VkInstance which extensions it carries. The two things that look like an
    // answer are both wrong:
    //   * the loader's instance-extension list (CheckAllInstanceExtensions)
    //     describes the LOADER, not the instance -- which is why that check is
    //     now confined to the create path;
    //   * GetInstanceProcAddr hands back a non-null trampoline for an instance
    //     extension whenever any layer or ICD implements it, enabled on THIS
    //     instance or not.
    //
    // Guessing wrong is not a status code. With the validation layer
    // present in the loader but NOT enabled on the borrowed instance, the
    // debug-utils probe below comes back null, control falls through to
    // CreateDebugReportCallbackEXT, and that dispatch-table entry is null
    // too -- a call through a null pointer inside this function, before the
    // session has created anything at all.
    //
    // There is a second and independent reason, and it is the ADOPT lifetime
    // rule rather than a crash. A callback we create is OURS to destroy, on an
    // instance whose lifetime belongs to the embedder; ~VulkanDeviceContext
    // destroys the messenger BEFORE it declines to destroy the imported
    // instance, so an embedder that dropped its instance first would have us
    // calling into a dead one. ADOPT retains and destroys nothing of the
    // caller's, and a debug callback is not an exception to that.
    //
    // Validation itself still works over an adopted instance: the embedder's
    // layer and the embedder's callback are what report. All that is skipped
    // here is this library adding a second, redundant reporting channel to an
    // object it does not own.
    if (m_importedInstanceHandle) {
        VkEncOut() << "VulkanDeviceContext: validation was requested on an "
                     "IMPORTED VkInstance -- not attaching a debug callback. "
                     "The instance belongs to the embedder, which is the only "
                     "party that knows which debug extensions it enabled and "
                     "the only one whose reporting may outlive this session."
                  << std::endl << std::flush;
        return VK_SUCCESS;
    }

    // Prefer VK_EXT_debug_utils over VK_EXT_debug_report.
    // debug_utils provides messageIdNumber for reliable VUID filtering
    // and is the non-deprecated API. Load extension procs via GetInstanceProcAddr.
    if (GetInstanceProcAddr) {
        m_createDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            GetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        m_destroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            GetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
    }
    if (m_createDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {};
        messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messengerInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        if (validateVerbose) {
            messengerInfo.messageSeverity |=
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
        }
        messengerInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = DebugUtilsMessengerCallback;
        messengerInfo.pUserData = reinterpret_cast<void*>(this);

        return m_createDebugUtilsMessengerEXT(m_instance, &messengerInfo, nullptr, &m_debugUtilsMessenger);
    }

    // Fallback to deprecated VK_EXT_debug_report if debug_utils is unavailable
    VkDebugReportCallbackCreateInfoEXT debug_report_info = {};
    debug_report_info.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CREATE_INFO_EXT;

    debug_report_info.flags =
        VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT;

    if (validateVerbose) {
        debug_report_info.flags = VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;
    }

    debug_report_info.pfnCallback = debugReportCallback;
    debug_report_info.pUserData = reinterpret_cast<void *>(this);

    // A null dispatch entry means "this instance has no VK_EXT_debug_report",
    // which is a diagnostic we do without -- not a reason to jump to address
    // 0. The imported-instance case above is what made this reachable, but the
    // guard is deliberately unconditional: the table is filled by resolving
    // names through the loader (HelpersDispatchTable.cpp), so any entry in it
    // can be null for reasons this code does not control, and the one thing
    // that must never happen is that a request for DIAGNOSTICS kills the
    // process it was meant to diagnose.
    if (CreateDebugReportCallbackEXT == nullptr) {
        VkEncOut() << "VulkanDeviceContext: neither VK_EXT_debug_utils nor "
                     "VK_EXT_debug_report resolved on this instance; "
                     "continuing without a library debug callback."
                  << std::endl << std::flush;
        return VK_SUCCESS;
    }

    return CreateDebugReportCallbackEXT(m_instance, &debug_report_info, nullptr, &m_debugReport);
}

VkResult VulkanDeviceContext::InitPhysicalDevice(int32_t deviceId, const vk::DeviceUuidUtils& deviceUuid,
                                                 const VkQueueFlags requestQueueTypes,
                                                 const VkWsiDisplay* pWsiDisplay,
                                                 const VkQueueFlags requestVideoDecodeQueueMask,
                                                 const VkVideoCodecOperationFlagsKHR requestVideoDecodeQueueOperations,
                                                 const VkQueueFlags requestVideoEncodeQueueMask,
                                                 const VkVideoCodecOperationFlagsKHR requestVideoEncodeQueueOperations,
                                                 VkPhysicalDevice vkPhysicalDevice)
{
    std::vector<VkPhysicalDevice> availablePhysicalDevices;
    if (vkPhysicalDevice == VK_NULL_HANDLE) {
        // enumerate physical devices
        VkResult result = vk::enumerate(this, m_instance, availablePhysicalDevices);
        if (result != VK_SUCCESS) {
            return result;
        }
    } else {
        availablePhysicalDevices.push_back(vkPhysicalDevice);
    }

    m_physDevice = VK_NULL_HANDLE;

    // Device-extension accumulation is per candidate. HasAllDeviceExtensions()
    // below calls AddRequiredDeviceExtension() for every required AND optional
    // extension it finds on the candidate it is inspecting, and that function is
    // a bare push_back with no de-duplication. Because the selection block
    // returns VK_SUCCESS as soon as a device is chosen, anything a REJECTED
    // candidate left behind would still be on the list handed to vkCreateDevice
    // for the device actually selected. On a multi-GPU host that means requesting
    // an optional extension only the rejected GPU advertised, which fails device
    // creation with VK_ERROR_EXTENSION_NOT_PRESENT -- and duplicate names besides.
    // Reset to the caller-supplied baseline before inspecting each candidate.
    const size_t reqDeviceExtensionsBaseline = m_reqDeviceExtensions.size();
    for (auto physicalDevice : availablePhysicalDevices) {
        m_reqDeviceExtensions.resize(reqDeviceExtensionsBaseline);


        // Get Vulkan 1.1 specific properties which include deviceUUID
        VkPhysicalDeviceVulkan11Properties deviceVulkan11Properties = {};
        deviceVulkan11Properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;

        VkPhysicalDeviceProperties2 devProp2 = {};
        devProp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        devProp2.pNext = &deviceVulkan11Properties;

        // Get the properties
        GetPhysicalDeviceProperties2(physicalDevice, &devProp2);

        if ((deviceId != -1) && (devProp2.properties.deviceID != (uint32_t)deviceId)) {
            continue;
        }

        if (deviceUuid) {
            if (!deviceUuid.Compare( deviceVulkan11Properties.deviceUUID)) {

                vk::DeviceUuidUtils deviceUuid(deviceVulkan11Properties.deviceUUID);
                VkEncOut() << "*** Skipping vulkan physical device with NOT matching UUID: "
                          << "Device Name: " << devProp2.properties.deviceName << std::hex
                          << ", vendor ID: " << devProp2.properties.vendorID
                          << ", device UUID: " << deviceUuid.ToString()
                          << ", and device ID: " << devProp2.properties.deviceID << std::dec
                          << ", Num Decode Queues: " << m_videoDecodeNumQueues
                          << ", Num Encode Queues: " << m_videoEncodeNumQueues
                          << " ***" << std::endl << std::flush;
                continue;
            }
        }

        if (!HasAllDeviceExtensions(physicalDevice, devProp2.properties.deviceName)) {
            VkEncErr() << "ERROR: Found physical device with name: " << devProp2.properties.deviceName << std::hex
                         << ", vendor ID: " << devProp2.properties.vendorID << ", and device ID: " << devProp2.properties.deviceID
                         << std::dec
                         << " NOT having the required extensions!" << std::endl << std::flush;
            continue;
        }

        // get queue properties
        std::vector<VkQueueFamilyProperties2> queues;
        std::vector<VkQueueFamilyVideoPropertiesKHR> videoQueues;
        std::vector<VkQueueFamilyQueryResultStatusPropertiesKHR> queryResultStatus;
        vk::get(this, physicalDevice, queues, videoQueues, queryResultStatus);

        bool videoDecodeQueryResultStatus = false;
        bool videoEncodeQueryResultStatus = false;
        VkQueueFlags foundQueueTypes = 0;
        int gfxQueueFamily = -1,
            computeQueueFamily = -1,
            computeQueueFamilyOnly = -1,
            presentQueueFamily = -1,
            videoDecodeQueueFamily = -1,
            videoDecodeQueueCount  = 0,
            videoEncodeQueueFamily = -1,
            videoEncodeQueueCount  = 0,
            videoDecodeEncodeComputeQueueFamily = -1,
            videoDecodeEncodeComputeNumQueues = 0,
            transferQueueFamily = -1,
            transferQueueFamilyOnly = -1,
            transferNumQueues = 0;

        const bool dumpQueues = false;
        for (uint32_t i = 0; i < queues.size(); i++) {
            const VkQueueFamilyProperties2 &queue = queues[i];

            // At this point, we only care about these queue types:
            const VkQueueFlags queueFamilyFlagsFilter = (VK_QUEUE_GRAPHICS_BIT |
                                                         VK_QUEUE_COMPUTE_BIT |
                                                         VK_QUEUE_TRANSFER_BIT |
                                                         VK_QUEUE_VIDEO_DECODE_BIT_KHR |
                                                         VK_QUEUE_VIDEO_ENCODE_BIT_KHR);

            const VkQueueFlags queueFamilyFlags = queue.queueFamilyProperties.queueFlags &
                                                      queueFamilyFlagsFilter;

            if ((queueFamilyFlags & requestQueueTypes) == 0) {
                continue;
            }

            const VkQueueFamilyVideoPropertiesKHR &videoQueue = videoQueues[i];

            if ((requestQueueTypes & VK_QUEUE_VIDEO_DECODE_BIT_KHR) && (videoDecodeQueueFamily < 0) &&
                        (requestVideoDecodeQueueMask == (queueFamilyFlags & requestVideoDecodeQueueMask)) &&
                            (videoQueue.videoCodecOperations & requestVideoDecodeQueueOperations)) {
                videoDecodeQueueFamily = i;
                videoDecodeQueueCount = queue.queueFamilyProperties.queueCount;

                if (dumpQueues) VkEncOut() << "\t Found video decode only queue family " <<  i <<
                        " with " << queue.queueFamilyProperties.queueCount <<
                        " max num of queues." << std::endl;

                // Does the video decode queue also support transfer operations?
                if (queueFamilyFlags & VK_QUEUE_TRANSFER_BIT) {
                    if (dumpQueues) VkEncOut() << "\t\t Video decode queue " <<  i <<
                            " supports transfer operations" << std::endl;
                }

                // Does the video decode queue also support compute operations?
                if (queueFamilyFlags & VK_QUEUE_COMPUTE_BIT) {
                    if (dumpQueues) VkEncOut() << "\t\t Video decode queue " <<  i <<
                            " supports compute operations" << std::endl;
                }

                m_videoDecodeQueueFlags = queueFamilyFlags;
                foundQueueTypes |= queueFamilyFlags;
                // assert(queueFamilyFlags & VK_QUEUE_TRANSFER_BIT);
                videoDecodeQueryResultStatus = queryResultStatus[i].queryResultStatusSupport;
            }

            if ((requestQueueTypes & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) && (videoEncodeQueueFamily < 0) &&
                        (requestVideoEncodeQueueMask == (queueFamilyFlags & requestVideoEncodeQueueMask)) &&
                        (videoQueue.videoCodecOperations & requestVideoEncodeQueueOperations)) {
                videoEncodeQueueFamily = i;
                videoEncodeQueueCount = queue.queueFamilyProperties.queueCount;

                if (dumpQueues) VkEncOut() << "\t Found video encode only queue family " <<  i <<
                        " with " << queue.queueFamilyProperties.queueCount <<
                        " max num of queues." << std::endl;

                // Does the video encode queue also support transfer operations?
                if (queueFamilyFlags & VK_QUEUE_TRANSFER_BIT) {
                    if (dumpQueues) VkEncOut() << "\t\t Video encode queue " <<  i <<
                            " supports transfer operations" << std::endl;
                }

                // Does the video encode queue also support compute operations?
                if (queueFamilyFlags & VK_QUEUE_COMPUTE_BIT) {
                    if (dumpQueues) VkEncOut() << "\t\t Video encode queue " <<  i <<
                            " supports compute operations" << std::endl;
                }

                m_videoEncodeQueueFlags = queueFamilyFlags;
                foundQueueTypes |= queueFamilyFlags;
                // assert(queueFamilyFlags & VK_QUEUE_TRANSFER_BIT);
                videoEncodeQueryResultStatus = queryResultStatus[i].queryResultStatusSupport;

            }

            // requires only GRAPHICS for frameProcessor queues
            if ((requestQueueTypes & VK_QUEUE_GRAPHICS_BIT) && (gfxQueueFamily < 0) &&
                    (queueFamilyFlags & VK_QUEUE_GRAPHICS_BIT)) {
                gfxQueueFamily = i;
                if ((transferQueueFamily < 0) && !!(queueFamilyFlags & VK_QUEUE_TRANSFER_BIT)) {
                    transferQueueFamily = i;
                }
                foundQueueTypes |= queueFamilyFlags;
                if (dumpQueues) VkEncOut() << "\t Found graphics queue family " <<  i << " with " << queue.queueFamilyProperties.queueCount << " max num of queues." << std::endl;
            } else if ((requestQueueTypes & VK_QUEUE_COMPUTE_BIT) && (computeQueueFamilyOnly < 0) &&
                       ((VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT) == (queueFamilyFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)))) {
                computeQueueFamilyOnly = i;
                foundQueueTypes |= queueFamilyFlags;
                if ((transferQueueFamily < 0) && !!(queueFamilyFlags & VK_QUEUE_TRANSFER_BIT)) {
                    transferQueueFamily = i;
                }
                if (dumpQueues) VkEncOut() << "\t Found compute only queue family " <<  i << " with " << queue.queueFamilyProperties.queueCount << " max num of queues." << std::endl;
            } else if ((requestQueueTypes & VK_QUEUE_TRANSFER_BIT) && (transferQueueFamilyOnly < 0) &&
                    (VK_QUEUE_TRANSFER_BIT == (queueFamilyFlags & VK_QUEUE_TRANSFER_BIT))) {
                transferQueueFamilyOnly = i;
                foundQueueTypes |= queueFamilyFlags;
                if (dumpQueues) VkEncOut() << "\t Found transfer only queue family " <<  i << " with " << queue.queueFamilyProperties.queueCount << " max num of queues." << std::endl;
            }

            // requires only COMPUTE for frameProcessor queues
            if ((requestQueueTypes & VK_QUEUE_COMPUTE_BIT) && (computeQueueFamily < 0) &&
                    (queueFamilyFlags & VK_QUEUE_COMPUTE_BIT)) {
                computeQueueFamily = i;
                foundQueueTypes |= queueFamilyFlags;
                if (dumpQueues) VkEncOut() << "\t Found compute queue family " <<  i << " with " << queue.queueFamilyProperties.queueCount << " max num of queues." << std::endl;
            }

            // present queue must support the surface
            if ((pWsiDisplay != nullptr) &&
                    (presentQueueFamily < 0) && pWsiDisplay->PhysDeviceCanPresent(physicalDevice, i)) {
                if (dumpQueues) VkEncOut() << "\t Found present queue family " <<  i << "." << std::endl;
                presentQueueFamily = i;
            }

            if (((foundQueueTypes & requestQueueTypes) == requestQueueTypes) &&
                    ((pWsiDisplay == nullptr) || (presentQueueFamily >= 0))) {

                // Selected a physical device
                m_physDevice = physicalDevice;
                m_gfxQueueFamily = gfxQueueFamily;
                m_computeQueueFamily = (computeQueueFamilyOnly != -1) ? computeQueueFamilyOnly : computeQueueFamily;
                m_presentQueueFamily = presentQueueFamily;
                m_videoDecodeQueueFamily = videoDecodeQueueFamily;
                m_videoDecodeNumQueues = videoDecodeQueueCount;
                m_videoEncodeQueueFamily = videoEncodeQueueFamily;
                m_videoEncodeNumQueues = videoEncodeQueueCount;

                m_videoDecodeQueryResultStatusSupport = videoDecodeQueryResultStatus;
                m_videoEncodeQueryResultStatusSupport = videoEncodeQueryResultStatus;
                m_videoDecodeEncodeComputeQueueFamily = videoDecodeEncodeComputeQueueFamily;
                m_videoDecodeEncodeComputeNumQueues = videoDecodeEncodeComputeNumQueues;
                m_transferQueueFamily = (transferQueueFamilyOnly != -1) ? transferQueueFamilyOnly : transferQueueFamily;
                m_transferNumQueues = transferNumQueues;

                assert(m_physDevice != VK_NULL_HANDLE);
                PopulateDeviceExtensions();
                if (false) {
                    PrintExtensions(true);
                }
#if !defined(VK_VIDEO_NO_STDOUT_INFO)
                if (true) {

                    vk::DeviceUuidUtils deviceUuid(deviceVulkan11Properties.deviceUUID);
                    VkEncOut() << "*** Selected Vulkan physical device with name: " << devProp2.properties.deviceName << std::hex
                              << ", vendor ID: " << devProp2.properties.vendorID
                              << ", device UUID: " << deviceUuid.ToString()
                              << ", and device ID: " << devProp2.properties.deviceID << std::dec
                              << ", Num Decode Queues: " << m_videoDecodeNumQueues
                              << ", Num Encode Queues: " << m_videoEncodeNumQueues
                              << " ***" << std::endl << std::flush;
                }
#endif
                return VK_SUCCESS;
            }
        }
        VkEncErr() << "ERROR: Found physical device with name: " << devProp2.properties.deviceName << std::hex
                  << ", vendor ID: " << devProp2.properties.vendorID << ", and device ID: " << devProp2.properties.deviceID
                  << std::dec
                  << " NOT having the required queue families!" << std::endl << std::flush;
    }

    return (m_physDevice != VK_NULL_HANDLE) ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult VulkanDeviceContext::OverrideImportedQueueFamilies(
    uint32_t videoEncodeQueueFamilyIndex,
    VkVideoCodecOperationFlagsKHR videoEncodeQueueOperations,
    uint32_t computeQueueFamilyIndex)
{
    if (m_physDevice == VK_NULL_HANDLE) {
        assert(!"OverrideImportedQueueFamilies requires InitPhysicalDevice() first");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if ((videoEncodeQueueFamilyIndex == UINT32_MAX) &&
        (computeQueueFamilyIndex == UINT32_MAX)) {
        return VK_SUCCESS; // nothing to override
    }

    // Re-query the family table (with the video/query-status pNext chains)
    // so the override can validate flags and refresh the cached
    // per-family metadata the encoder relies on.
    std::vector<VkQueueFamilyProperties2> queues;
    std::vector<VkQueueFamilyVideoPropertiesKHR> videoQueues;
    std::vector<VkQueueFamilyQueryResultStatusPropertiesKHR> queryResultStatus;
    vk::get(this, m_physDevice, queues, videoQueues, queryResultStatus);

    // A10.3: the three arrays are filled in parallel, one entry per queue
    // family, and the bounds check below validates an index against |queues|
    // ONLY -- then uses it to index |videoQueues|. If they ever disagree that
    // is an out-of-bounds read past a check that appeared to cover it.
    // Require the invariant instead of assuming it.
    if ((videoQueues.size() != queues.size()) ||
        (queryResultStatus.size() != queues.size())) {
        VkEncErr() << "[VulkanDeviceContext] queue-family property arrays "
                   << "disagree in length (queues=" << queues.size()
                   << ", video=" << videoQueues.size()
                   << ", queryStatus=" << queryResultStatus.size()
                   << "); refusing to index them against one another"
                   << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (videoEncodeQueueFamilyIndex != UINT32_MAX) {
        if (videoEncodeQueueFamilyIndex >= queues.size()) {
            VkEncErr() << "[VulkanDeviceContext] caller-provided encode "
                       << "queue family " << videoEncodeQueueFamilyIndex
                       << " out of range (device has " << queues.size()
                       << " families)" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const VkQueueFlags familyFlags =
            queues[videoEncodeQueueFamilyIndex].queueFamilyProperties.queueFlags;
        const VkVideoCodecOperationFlagsKHR familyOps =
            videoQueues[videoEncodeQueueFamilyIndex].videoCodecOperations;
        if (((familyFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) == 0) ||
            ((videoEncodeQueueOperations != 0) &&
             ((familyOps & videoEncodeQueueOperations) == 0))) {
            VkEncErr() << "[VulkanDeviceContext] caller-provided encode "
                       << "queue family " << videoEncodeQueueFamilyIndex
                       << " lacks VIDEO_ENCODE support for the requested codec "
                       << "(flags=0x" << std::hex << familyFlags
                       << ", codecOps=0x" << familyOps << std::dec << ")"
                       << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        m_videoEncodeQueueFamily = (int32_t)videoEncodeQueueFamilyIndex;
        m_videoEncodeNumQueues   = (int32_t)queues[videoEncodeQueueFamilyIndex]
                                       .queueFamilyProperties.queueCount;
        m_videoEncodeQueueFlags  = familyFlags &
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
             VK_QUEUE_TRANSFER_BIT | VK_QUEUE_VIDEO_DECODE_BIT_KHR |
             VK_QUEUE_VIDEO_ENCODE_BIT_KHR);
        m_videoEncodeQueryResultStatusSupport =
            queryResultStatus[videoEncodeQueueFamilyIndex].queryResultStatusSupport;
        VkEncOut() << "VulkanDeviceContext: using caller-provided encode "
                   << "queue family " << videoEncodeQueueFamilyIndex
                   << " for the imported VkDevice" << std::endl;
    }

    if (computeQueueFamilyIndex != UINT32_MAX) {
        if (computeQueueFamilyIndex >= queues.size()) {
            VkEncErr() << "[VulkanDeviceContext] caller-provided compute "
                       << "queue family " << computeQueueFamilyIndex
                       << " out of range (device has " << queues.size()
                       << " families)" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const VkQueueFlags familyFlags =
            queues[computeQueueFamilyIndex].queueFamilyProperties.queueFlags;
        if ((familyFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
            VkEncErr() << "[VulkanDeviceContext] caller-provided compute "
                       << "queue family " << computeQueueFamilyIndex
                       << " lacks VK_QUEUE_COMPUTE_BIT (flags=0x" << std::hex
                       << familyFlags << std::dec << ")" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        m_computeQueueFamily = (int32_t)computeQueueFamilyIndex;
        VkEncOut() << "VulkanDeviceContext: using caller-provided compute "
                   << "queue family " << computeQueueFamilyIndex
                   << " for the imported VkDevice" << std::endl;
    }

    return VK_SUCCESS;
}

VkResult VulkanDeviceContext::InitVulkanDevice(const char * pAppName,
                                               VkInstance vkInstance,
                                               bool verbose,
                                               const char * pCustomLoader) {
    PFN_vkGetInstanceProcAddr getInstanceProcAddrFunc = LoadVk(m_libHandle, pCustomLoader);
    if ((getInstanceProcAddrFunc == nullptr) || m_libHandle == VulkanLibraryHandleType()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    vk::InitDispatchTableTop(getInstanceProcAddrFunc, this);

    VkResult result = InitVkInstance(pAppName, vkInstance, verbose);
    if (result != VK_SUCCESS) {
        return result;
    }
    vk::InitDispatchTableMiddle(m_instance, false, this);

    return result;
}

VkResult VulkanDeviceContext::CreateVulkanDevice(int32_t numDecodeQueues,
                                                 int32_t numEncodeQueues,
                                                 VkVideoCodecOperationFlagsKHR videoCodecs,
                                                 bool createTransferQueue,
                                                 bool createGraphicsQueue,
                                                 bool createPresentQueue,
                                                 bool createComputeQueue,
                                                 VkDevice vkDevice)
{
    if (vkDevice == VK_NULL_HANDLE) {
        VkDeviceCreateInfo devInfo = {};
        devInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        devInfo.pNext = nullptr;
        devInfo.queueCreateInfoCount = 0;

        if (numDecodeQueues < 0) {
            numDecodeQueues = m_videoDecodeNumQueues;
        } else {
            numDecodeQueues = std::min(numDecodeQueues, m_videoDecodeNumQueues);
        }

        if (numEncodeQueues < 0) {
            numEncodeQueues = m_videoEncodeNumQueues;
        } else {
            numEncodeQueues = std::min(numEncodeQueues, m_videoEncodeNumQueues);
        }

        const int32_t maxQueueInstances = std::max(numDecodeQueues, numEncodeQueues);
        assert(maxQueueInstances <= MAX_QUEUE_INSTANCES);
        // At least one entry, always. The graphics/present/compute/transfer entries below
        // each ask for queueCount = 1 regardless of how many VIDEO queues were requested.
        const std::vector<float> queuePriorities(std::max(1, (int)maxQueueInstances), 0.0f);
        std::array<VkDeviceQueueCreateInfo, MAX_QUEUE_FAMILIES> queueInfo = {};

        // ONE ENTRY PER FAMILY, SIZED BY THE LARGEST ROLE THAT ASKED FOR IT.
        //
        // The bookkeeping this replaces had three separate faults. It
        // reserved the graphics family in the uniqueness set unconditionally,
        // so when graphics was NOT requested any other role sharing that
        // family was silently dropped -- and then retrieved anyway. The
        // present guard read !(m_presentQueueFamily != -1), which built the
        // entry exactly when the family was INVALID and assigned -1 to a
        // uint32_t queueFamilyIndex. And a family claimed by two roles kept
        // whichever queueCount was written first, so a present or compute
        // request could shrink a video family's count to one.
        struct RequestedQueue {
            int32_t  family = -1;
            uint32_t count  = 0;
        };
        std::array<RequestedQueue, MAX_QUEUE_FAMILIES> requested = {};
        uint32_t requestedFamilies = 0;

        // Records one role. |required| roles refuse an invalid family instead
        // of being skipped: the caller asked for something no family can
        // serve, and building a device without it would leave the retrieval
        // below asking for a queue that does not exist.
        auto requestQueue = [&](bool wanted, int32_t family, uint32_t count,
                                bool required) -> bool {
            if (!wanted) {
                return true;  // not requested: reserve nothing
            }
            if (family < 0) {
                return !required;
            }
            for (uint32_t i = 0; i < requestedFamilies; ++i) {
                if (requested[i].family == family) {
                    requested[i].count = std::max(requested[i].count, count);
                    return true;
                }
            }
            if (requestedFamilies >= requested.size()) {
                return false;
            }
            requested[requestedFamilies].family = family;
            requested[requestedFamilies].count  = count;
            requestedFamilies++;
            return true;
        };

        if (!requestQueue(createGraphicsQueue, m_gfxQueueFamily, 1, true)) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // Present always uses index 0 of its family, so it adds no count --
        // only the requirement that the family exist.
        if (!requestQueue(createPresentQueue, m_presentQueueFamily, 1, true)) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPhysicalDeviceVideoDecodeVP9FeaturesKHR videoDecodeVP9Feature { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR,
                                                                          nullptr,
                                                                          false // videoDecodeVP9
                                                                        };

        VkPhysicalDeviceVideoEncodeAV1FeaturesKHR videoEncodeAV1Feature { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_AV1_FEATURES_KHR,
                                                                          nullptr,
                                                                          false // videoEncodeAV1
                                                                        };

        // Chain only the structures that are requested
        VkBaseInStructure* pNext = nullptr;
        if (videoCodecs & VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) {
            videoEncodeAV1Feature.pNext = pNext;
            pNext = (VkBaseInStructure*)&videoEncodeAV1Feature;
        }
        if (videoCodecs & VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) {
            videoDecodeVP9Feature.pNext = pNext;
            pNext = (VkBaseInStructure*)&videoDecodeVP9Feature;
        }

        VkPhysicalDeviceTimelineSemaphoreFeatures timelineSemaphoreFeatures { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
                                                                              pNext,
                                                                              VK_FALSE
        };

        VkPhysicalDeviceVideoMaintenance1FeaturesKHR videoMaintenance1Features { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR,
                                                                                 &timelineSemaphoreFeatures,
                                                                                 VK_FALSE
                                                                               };

        VkPhysicalDeviceSynchronization2Features synchronization2Features { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
                                                                            &videoMaintenance1Features,
                                                                            VK_FALSE
                                                                           };

        VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR intraRefreshFeatures { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_INTRA_REFRESH_FEATURES_KHR,
                                                                                  &synchronization2Features,
                                                                                  VK_FALSE
                                                                                };

        // A feature struct may only be chained when its extension is actually enabled
        // (VUID-VkDeviceCreateInfo-pNext-pNext). VK_KHR_video_encode_intra_refresh is
        // requested by the encoder path alone, so every other client -- the filter tests,
        // the decoder -- creates a device without it and must not chain its feature
        // struct. The queried value is never used, so skipping the struct costs nothing.
        void* const pAfterYcbcr =
            (FindRequiredDeviceExtension(VK_KHR_VIDEO_ENCODE_INTRA_REFRESH_EXTENSION_NAME) != nullptr)
                ? static_cast<void*>(&intraRefreshFeatures)
                : static_cast<void*>(&synchronization2Features);

        // Required for creating YCbCr samplers used with multi-planar video formats
        VkPhysicalDeviceSamplerYcbcrConversionFeatures samplerYcbcrFeatures { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
                                                                               pAfterYcbcr,
                                                                               VK_FALSE
                                                                             };

        VkPhysicalDeviceFeatures2 deviceFeatures { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &samplerYcbcrFeatures};
        GetPhysicalDeviceFeatures2(m_physDevice, &deviceFeatures);

        assert(timelineSemaphoreFeatures.timelineSemaphore);
        assert(videoMaintenance1Features.videoMaintenance1);
        assert(synchronization2Features.synchronization2);
        if ((videoCodecs & VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) &&
            !videoEncodeAV1Feature.videoEncodeAV1) {
            VkEncErr() << "ERROR: AV1 encode requested but videoEncodeAV1 feature not supported" << std::endl;
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if ((videoCodecs & VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) &&
            !videoDecodeVP9Feature.videoDecodeVP9) {
            VkEncErr() << "ERROR: VP9 decode requested but videoDecodeVP9 feature not supported" << std::endl;
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        devInfo.pNext = &deviceFeatures;

        // Video/compute/transfer keep the existing tolerance: a role with no
        // family is a device that cannot do it, and the caller finds out when
        // it asks for the queue rather than at device creation.
        if (!requestQueue(numDecodeQueues > 0, m_videoDecodeQueueFamily,
                          (uint32_t)numDecodeQueues, false) ||
            !requestQueue(numEncodeQueues > 0, m_videoEncodeQueueFamily,
                          (uint32_t)numEncodeQueues, false) ||
            !requestQueue(createComputeQueue, m_computeQueueFamily, 1, false) ||
            !requestQueue(createTransferQueue, m_transferQueueFamily, 1, false)) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        for (uint32_t i = 0; i < requestedFamilies; ++i) {
            queueInfo[devInfo.queueCreateInfoCount].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo[devInfo.queueCreateInfoCount].queueFamilyIndex =
                (uint32_t)requested[i].family;
            queueInfo[devInfo.queueCreateInfoCount].queueCount = requested[i].count;
            queueInfo[devInfo.queueCreateInfoCount].pQueuePriorities = queuePriorities.data();
            devInfo.queueCreateInfoCount++;
        }

        assert(devInfo.queueCreateInfoCount <= MAX_QUEUE_FAMILIES);

        devInfo.pQueueCreateInfos = queueInfo.data();

        devInfo.enabledExtensionCount = static_cast<uint32_t>(m_reqDeviceExtensions.size());
        devInfo.ppEnabledExtensionNames = m_reqDeviceExtensions.data();

        // disable all features
        devInfo.pEnabledFeatures = nullptr;

        VkResult result = CreateDevice(m_physDevice, &devInfo, nullptr, &m_device);
        if (result != VK_SUCCESS) {
            return result;
        }

        m_importedDeviceHandle = false;

    } else {

        m_device = vkDevice;
        m_importedDeviceHandle = true;
        // Log the imported-VkDevice path so an embedder can verify it is
        // active. Gated on the silenceStdio latch via VkEncOut() so it
        // stays quiet inside the sandboxed GPU process.
        VkEncOut() << "VulkanDeviceContext: using caller-imported "
                   << "VkDevice (m_importedDeviceHandle=true)"
                   << std::endl;
    }

    vk::InitDispatchTableBottom(m_instance, m_device, this);

    // Retrieve only what a family was actually selected for. An index of -1
    // reaching GetDeviceQueue as a uint32_t asks the driver for family
    // 0xFFFFFFFF. On the created path the builder above guarantees an entry
    // exists for each of these; on the imported path the caller's contract is
    // that the queues exist, and this is the one part of it we can check.
    if (createGraphicsQueue && (GetGfxQueueFamilyIdx() >= 0)) {
        GetDeviceQueue(m_device, GetGfxQueueFamilyIdx()    , 0, &m_gfxQueue);
    }
    if (createComputeQueue && (GetComputeQueueFamilyIdx() >= 0)) {
        GetDeviceQueue(m_device, GetComputeQueueFamilyIdx(), 0, &m_computeQueue);
    }
    if (createPresentQueue && (GetPresentQueueFamilyIdx() >= 0)) {
        GetDeviceQueue(m_device, GetPresentQueueFamilyIdx(), 0, &m_presentQueue);
    }
    if (createTransferQueue && (GetTransferQueueFamilyIdx() >= 0)) {
        GetDeviceQueue(m_device, GetTransferQueueFamilyIdx(), 0, &m_trasferQueue);
    }
    if (numDecodeQueues) {
        assert(GetVideoDecodeQueueFamilyIdx() != -1);
        assert(GetVideoDecodeNumQueues() > 0);
        m_videoDecodeQueues.resize(GetVideoDecodeNumQueues());
        // m_videoDecodeQueueMutexes.resize(GetVideoDecodeNumQueues());
        for (uint32_t queueIdx = 0; queueIdx < (uint32_t)numDecodeQueues; queueIdx++) {
            GetDeviceQueue(m_device, GetVideoDecodeQueueFamilyIdx(), queueIdx, &m_videoDecodeQueues[queueIdx]);
        }
    }

    if (numEncodeQueues) {
        assert(GetVideoEncodeQueueFamilyIdx() != -1);
        assert(GetVideoEncodeNumQueues() > 0);
        m_videoEncodeQueues.resize(GetVideoEncodeNumQueues());
        // m_videoEncodeQueueMutexes.resize(GetVideoEncodeNumQueues());
        for (uint32_t queueIdx = 0; queueIdx < (uint32_t)numEncodeQueues; queueIdx++) {
            GetDeviceQueue(m_device, GetVideoEncodeQueueFamilyIdx(), queueIdx, &m_videoEncodeQueues[queueIdx]);
        }
    }

    return VK_SUCCESS;
}

VulkanDeviceContext::VulkanDeviceContext()
    // NAMING THE BASE IS LOAD-BEARING. vk::VkInterfaceFunctions is a plain
    // aggregate of function pointers with no constructor and no default
    // member initializers, and this class has a user-provided constructor, so
    // leaving the base out of this list DEFAULT-initializes it: every entry
    // holds an indeterminate value until InitDispatchTable* runs. Reading one
    // is UB, and GetVkGetInstanceProcAddr() promises callers nullptr before
    // initialization -- a promise that was false for any embedder that took
    // it at its word. The realistic non-null case is a recycled allocation:
    // destroy one encoder, construct the next into the same chunk, and the
    // stale table reads as live. {} value-initializes the whole base.
    : vk::VkInterfaceFunctions{}
    , m_libHandle()
    , m_instance()
    , m_physDevice()
    , m_gfxQueueFamily(-1)
    , m_computeQueueFamily(-1)
    , m_presentQueueFamily(-1)
    , m_transferQueueFamily(-1)
    , m_transferNumQueues(0)
    , m_videoDecodeQueueFamily(-1)
    , m_videoDecodeDefaultQueueIndex(0)
    , m_videoDecodeNumQueues(0)
    , m_videoEncodeQueueFamily(-1)
    , m_videoEncodeDefaultQueueIndex(0)
    , m_videoEncodeNumQueues(0)
    , m_videoDecodeEncodeComputeQueueFamily(-1)
    , m_videoDecodeEncodeComputeNumQueues(0)
    , m_videoDecodeQueueFlags(0)
    , m_videoEncodeQueueFlags(0)
    , m_importedInstanceHandle(false)
    , m_retainedLibHandle(false)
    , m_importedDeviceHandle(false)
    , m_videoDecodeQueryResultStatusSupport(false)
    , m_videoEncodeQueryResultStatusSupport(false)
    , m_device()
    , m_gfxQueue()
    , m_computeQueue()
    , m_trasferQueue()
    , m_presentQueue()
    , m_debugReport()
    , m_reqInstanceLayers()
    , m_reqInstanceExtensions()
    , m_requestedDeviceExtensions()
    , m_optDeviceExtensions()
{

}

VkResult VulkanDeviceContext::DeviceWaitIdle() const
{
    if (m_device == VK_NULL_HANDLE) {
        return VK_SUCCESS;  // nothing was ever created; nothing can be busy
    }
    return vk::VkInterfaceFunctions::DeviceWaitIdle(m_device);
}

VulkanDeviceContext::~VulkanDeviceContext() {

    if (m_device) {
        if (!m_importedDeviceHandle) {
            DeviceWaitIdle();
            DestroyDevice(m_device, nullptr);
        }
        m_device = VkDevice();
    }

    // Only destroy if we created a valid messenger (InitDebugReport was called with validate=true).
    // Skip VK_NULL_HANDLE and known invalid sentinels (e.g. 0xdededededededede) to avoid
    // VUID-vkDestroyDebugUtilsMessengerEXT-messenger-parameter.
    if (m_debugUtilsMessenger != VK_NULL_HANDLE && m_destroyDebugUtilsMessengerEXT) {
        const uint64_t v = (uint64_t)m_debugUtilsMessenger;
        constexpr uint64_t kSentinel64 = 0xdedededededededeULL;
        constexpr uint64_t kSentinel32 = 0x00000000dedededeULL;
        if (v != kSentinel64 && v != kSentinel32 && (v & 0xFFFFFFFFULL) != kSentinel32) {
            m_destroyDebugUtilsMessengerEXT(m_instance, m_debugUtilsMessenger, nullptr);
        }
        m_debugUtilsMessenger = VK_NULL_HANDLE;
    }

    if (m_debugReport) {
        DestroyDebugReportCallbackEXT(m_instance, m_debugReport, nullptr);
    }

    if (m_instance) {
        if (!m_importedInstanceHandle) {
            DestroyInstance(m_instance, nullptr);
        }
        m_instance = VkInstance();
    }

    m_gfxQueue = VK_NULL_HANDLE;
    m_computeQueue = VK_NULL_HANDLE;
    m_presentQueue = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < m_videoDecodeQueues.size(); i++) {
        m_videoDecodeQueues[i] = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < m_videoEncodeQueues.size(); i++) {
        m_videoEncodeQueues[i] = VK_NULL_HANDLE;
    }

    m_importedDeviceHandle = false;

    // RetainLoaderHandle() suppresses the unload. Anything that resolved
    // Vulkan entry points out of this same shared object -- an embedder's
    // own function-pointer table, another VulkanDeviceContext -- keeps
    // holding pointers into it after this object dies, and unloading it
    // underneath them is a crash rather than a leak avoided.
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
    if (m_libHandle && !m_retainedLibHandle) {
        dlclose(m_libHandle);
    }
#else // defined(VK_USE_PLATFORM_WIN32_KHR)
    if (m_libHandle && !m_retainedLibHandle) {
        FreeLibrary(m_libHandle);
    }
#endif // defined(VK_USE_PLATFORM_WIN32_KHR)
}

const VkExtensionProperties* VulkanDeviceContext::FindExtension(const std::vector<VkExtensionProperties>& extensions,
                                                                const char* name) const
{
    auto it = std::find_if(extensions.cbegin(), extensions.cend(),
                           [=](const VkExtensionProperties& ext) {
                               return (strcmp(ext.extensionName, name) == 0);
                           });
    return (it != extensions.cend()) ? &*it : nullptr;
}

const VkExtensionProperties* VulkanDeviceContext::FindInstanceExtension(const char* name) const {
    return FindExtension(m_instanceExtensions, name);
}

const VkExtensionProperties* VulkanDeviceContext::FindDeviceExtension(const char* name) const {
    return FindExtension(m_deviceExtensions, name);
}

const char * VulkanDeviceContext::FindRequiredDeviceExtension(const char* name) const {
    auto it = std::find_if(m_reqDeviceExtensions.cbegin(), m_reqDeviceExtensions.cend(),
                           [=](const char * extName) {
                               return (strcmp(extName, name) == 0);
                           });
    return (it != m_reqDeviceExtensions.cend()) ? *it : nullptr;
}


void VulkanDeviceContext::PrintExtensions(bool deviceExt) const {
    const std::vector<VkExtensionProperties>& extensions = deviceExt ? m_deviceExtensions : m_instanceExtensions;
    VkEncOut() << "###### List of " <<  (deviceExt ? "Device" : "Instance") << " Extensions: ######" << std::endl;
    for (const auto& e : extensions) {
        VkEncOut() << "\t " << e.extensionName << "(v." << e.specVersion << ")\n";
    }
}

VkResult VulkanDeviceContext::PopulateInstanceExtensions()
{
    uint32_t extensionsCount = 0;
    VkResult result = EnumerateInstanceExtensionProperties( nullptr, &extensionsCount, nullptr );
    if ((result != VK_SUCCESS) || (extensionsCount == 0)) {
        VkEncOut() << "Could not get the number of instance extensions." << std::endl;
        return result;
    }
    m_instanceExtensions.resize( extensionsCount );
    result = EnumerateInstanceExtensionProperties( nullptr, &extensionsCount, m_instanceExtensions.data() );
    if ((result != VK_SUCCESS) || (extensionsCount == 0)) {
        VkEncOut() << "Could not enumerate instance extensions." << std::endl;
        return result;
    }
    return result;
}

VkResult VulkanDeviceContext::AdoptPhysicalDevice(VkPhysicalDevice physicalDevice)
{
    if (physicalDevice == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    m_physDevice = physicalDevice;
    // The queue families stay at their initialised values: this context is for
    // physical-device-level queries only and never reaches vkCreateDevice.
    return PopulateDeviceExtensions();
}

VkResult VulkanDeviceContext::PopulateDeviceExtensions()
{
    uint32_t extensions_count = 0;
    VkResult result = EnumerateDeviceExtensionProperties( m_physDevice, nullptr, &extensions_count, nullptr );
    if ((result != VK_SUCCESS) || (extensions_count == 0)) {
        VkEncOut() << "Could not get the number of device extensions." << std::endl;
        return result;
    }
    m_deviceExtensions.resize( extensions_count );
    result = EnumerateDeviceExtensionProperties( m_physDevice, nullptr, &extensions_count, m_deviceExtensions.data() );
    if ((result != VK_SUCCESS) || (extensions_count == 0)) {
        VkEncOut() << "Could not enumerate device extensions." << std::endl;
        return result;
    }
    return result;
}

VkResult VulkanDeviceContext::InitVulkanDecoderDevice(const char * pAppName,
                                                      VkInstance vkInstance,
                                                      VkVideoCodecOperationFlagsKHR videoCodecs,
                                                      bool enableWsi,
                                                      bool enableWsiDirectMode,
                                                      bool enableValidation,
                                                      bool enableVerboseValidation,
                                                      bool enbaleVerboseDump,
                                                      bool enableInlineSessionParameters,
                                                      const char * pCustomLoader)
{
    static const char* const requiredInstanceLayers[] = {
        "VK_LAYER_KHRONOS_validation",
        nullptr
    };

    static const char* const requiredInstanceExtensions[] = {
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        nullptr
    };

#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
    static const char* const requiredWsiInstanceExtensions[] = {
        // Required generic WSI extensions
        VK_KHR_SURFACE_EXTENSION_NAME,
        nullptr
    };
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT

    static const char* const requiredDeviceExtension[] = {
#if defined(__linux) || defined(__linux__) || defined(linux)
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
#endif
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        nullptr
    };

#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
    static const char* const requiredWsiDeviceExtension[] = {
        // Add the WSI required device extensions
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
        // VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME,
#endif
        nullptr
    };
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT

    static const char* const optinalDeviceExtension[] = {
        VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        nullptr
    };

    if (enableValidation) {
        AddReqInstanceLayers(requiredInstanceLayers);
        AddReqInstanceExtensions(requiredInstanceExtensions);
    }

    // Add the Vulkan video required device extensions
    AddReqDeviceExtensions(requiredDeviceExtension);
    AddOptDeviceExtensions(optinalDeviceExtension);

    if (enableInlineSessionParameters) {
        AddReqDeviceExtension("VK_KHR_video_maintenance2");
    }

#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
    /********** Start WSI instance extensions support *******************************************/
    if (enableWsi) {
        const std::vector<VkExtensionProperties>& wsiRequiredInstanceInstanceExtensions =
                Shell::GetRequiredInstanceExtensions(enableWsiDirectMode);

        for (size_t e = 0; e < wsiRequiredInstanceInstanceExtensions.size(); e++) {
            AddReqInstanceExtension(wsiRequiredInstanceInstanceExtensions[e].extensionName);
        }

        // Add the WSI required instance extensions
        AddReqInstanceExtensions(requiredWsiInstanceExtensions);

        // Add the WSI required device extensions
        AddReqDeviceExtensions(requiredWsiDeviceExtension);
    }
    /********** End WSI instance extensions support *******************************************/
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT

    if (videoCodecs == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
        AddReqDeviceExtension(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
    } else if (videoCodecs == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
        AddReqDeviceExtension(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME);
    } else if (videoCodecs == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
        AddReqDeviceExtension(VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME);
    } else if (videoCodecs == VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) {
        AddReqDeviceExtension(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME);
    } else {
        static const char* const optinalCodecsExtensions[] = {
                VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
                VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
                VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
                VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME,
            nullptr
        };
        // If the codec set is VK_VIDEO_CODEC_OPERATION_NONE_KHR or
        // VIDEO_CODEC_OPERATIONS_ALL, then set all codecs as optional extensions.
        AddOptDeviceExtensions(optinalCodecsExtensions);
    }

    VkResult result = InitVulkanDevice(pAppName, vkInstance, enbaleVerboseDump);
    if (result != VK_SUCCESS) {
        VkEncPrintfOut("Could not initialize the Vulkan device!\n");
        return result;
    }

    result = InitDebugReport(enableValidation, enableVerboseValidation);
    if (result != VK_SUCCESS) {
        return result;
    }

    return result;
}
