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
#include "VkCodecUtils/VulkanDeviceContext.h"
#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
#include "VkShell/Shell.h"
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT

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

    if (verbose) std::cout << "Enumerating instance layers:" << std::endl;
    std::set<std::string> layer_names;
    for (const auto &layer : layers) {
        layer_names.insert(layer.layerName);
        if (verbose ) std::cout << '\t' << layer.layerName << std::endl;
    }

    // all listed instance layers are required
    if (verbose) std::cout << "Looking for instance layers:" << std::endl;
    for (uint32_t i = 0; i < m_reqInstanceLayers.size(); i++) {
        const char* name = m_reqInstanceLayers[i];
        if (name == nullptr) {
            break;
        }
        std::cout << '\t' << name << std::endl;
        if (layer_names.find(name) == layer_names.end()) {
            std::cerr << "AssertAllInstanceLayers() ERROR: requested instance layer"
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

    if (verbose) std::cout << "Enumerating instance extensions:" << std::endl;
    std::set<std::string> ext_names;
    for (const auto &ext : exts) {
        ext_names.insert(ext.extensionName);
        if (verbose) std::cout << '\t' <<  ext.extensionName << std::endl;
    }

    // all listed instance extensions are required
    if (verbose) std::cout << "Looking for instance extensions:" << std::endl;
    for (uint32_t i = 0; i < m_reqInstanceExtensions.size(); i++) {
        const char* name = m_reqInstanceExtensions[i];
        if (name == nullptr) {
            break;
        }
        if (verbose) std::cout << '\t' <<  name << std::endl;
        if (ext_names.find(name) == ext_names.end()) {
            std::cerr << "AssertAllInstanceExtensions() ERROR: requested instance extension "
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
            std::cout << "Added required device extension: " << name << std::endl;
        }
    }

    return VK_SUCCESS;
}

VkResult VulkanDeviceContext::AddReqDeviceExtension(const char* requiredDeviceExtension, bool verbose)
{
    if (requiredDeviceExtension) {
        m_requestedDeviceExtensions.push_back(requiredDeviceExtension);
        if (verbose) {
            std::cout << "Added required device extension: " << requiredDeviceExtension << std::endl;
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
            std::cout << "Added optional device extension: " << name << std::endl;
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
                std::cerr << __FUNCTION__
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
                std::cout << __FUNCTION__
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
      std::cout << map->l_name << std::endl;
      // do something with |map| like with handle, returned by |dlopen()|.
      map = map->l_next;
    }

    return 0;
}
#endif

VkResult VulkanDeviceContext::InitVkInstance(const char * pAppName, VkInstance vkInstance, bool verbose)
{
    VkResult result = CheckAllInstanceLayers(verbose);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = CheckAllInstanceExtensions(verbose);
    if (result != VK_SUCCESS) {
        return result;
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

bool VulkanDeviceContext::DebugReportCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT,
                                              uint64_t, size_t,
                                              int32_t, const char *layer_prefix, const char *msg)
{
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

    std::ostream &st = (prio >= LOG_ERR) ? std::cerr : std::cout;
    st << msg << "\n";

    return false;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugReportCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT obj_type,
                                                          uint64_t object, size_t location, int32_t msg_code,
                                                          const char *layer_prefix, const char *msg, void *user_data) {
    VulkanDeviceContext *ctx = reinterpret_cast<VulkanDeviceContext *>(user_data);
    return ctx->DebugReportCallback(flags, obj_type, object, location, msg_code, layer_prefix, msg);
}

VkResult VulkanDeviceContext::InitDebugReport(bool validate, bool validateVerbose)
{
    if (!validate) {
        return VK_SUCCESS;
    }
    VkDebugReportCallbackCreateInfoEXT debug_report_info = {};
    debug_report_info.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CREATE_INFO_EXT;

    debug_report_info.flags =
        VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT;

    if (validateVerbose) {
        debug_report_info.flags = VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;
    }

    debug_report_info.pfnCallback = debugReportCallback;
    debug_report_info.pUserData = reinterpret_cast<void *>(this);

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
    for (auto physicalDevice : availablePhysicalDevices) {

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
                std::cout << "*** Skipping vulkan physical device with NOT matching UUID: "
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
            std::cerr << "ERROR: Found physical device with name: " << devProp2.properties.deviceName << std::hex
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

                if (dumpQueues) std::cout << "\t Found video decode only queue family " <<  i <<
                        " with " << queue.queueFamilyProperties.queueCount <<
                        " max num of queues." << std::endl;

                // Does the video decode queue also support transfer operations?
                if (queueFamilyFlags & VK_QUEUE_TRANSFER_BIT) {
                    if (dumpQueues) std::cout << "\t\t Video decode queue " <<  i <<
                            " supports transfer operations" << std::endl;
                }

                // Does the video decode queue also support compute operations?
                if (queueFamilyFlags & VK_QUEUE_COMPUTE_BIT) {
                    if (dumpQueues) std::cout << "\t\t Video decode queue " <<  i <<
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

                if (dumpQueues) std::cout << "\t Found video encode only queue family " <<  i <<
                        " with " << queue.queueFamilyProperties.queueCount <<
                        " max num of queues." << std::endl;

                // Does the video encode queue also support transfer operations?
                if (queueFamilyFlags & VK_QUEUE_TRANSFER_BIT) {
                    if (dumpQueues) std::cout << "\t\t Video encode queue " <<  i <<
                            " supports transfer operations" << std::endl;
                }

                // Does the video encode queue also support compute operations?
                if (queueFamilyFlags & VK_QUEUE_COMPUTE_BIT) {
                    if (dumpQueues) std::cout << "\t\t Video encode queue " <<  i <<
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
                foundQueueTypes |= queueFamilyFlags;
                if (dumpQueues) std::cout << "\t Found graphics queue family " <<  i << " with " << queue.queueFamilyProperties.queueCount << " max num of queues." << std::endl;
            } else if ((requestQueueTypes & VK_QUEUE_COMPUTE_BIT) && (computeQueueFamilyOnly < 0) &&
                       ((VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT) == (queueFamilyFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)))) {
                computeQueueFamilyOnly = i;
                foundQueueTypes |= queueFamilyFlags;
                if (dumpQueues) std::cout << "\t Found compute only queue family " <<  i << " with " << queue.queueFamilyProperties.queueCount << " max num of queues." << std::endl;
            } else if ((requestQueueTypes & VK_QUEUE_TRANSFER_BIT) && (transferQueueFamilyOnly < 0) &&
                    (VK_QUEUE_TRANSFER_BIT == (queueFamilyFlags & VK_QUEUE_TRANSFER_BIT))) {
                transferQueueFamilyOnly = i;
                foundQueueTypes |= queueFamilyFlags;
                if (dumpQueues) std::cout << "\t Found transfer only queue family " <<  i << " with " << queue.queueFamilyProperties.queueCount << " max num of queues." << std::endl;
            }

            // requires only COMPUTE for frameProcessor queues
            if ((requestQueueTypes & VK_QUEUE_COMPUTE_BIT) && (computeQueueFamily < 0) &&
                    (queueFamilyFlags & VK_QUEUE_COMPUTE_BIT)) {
                computeQueueFamily = i;
                foundQueueTypes |= queueFamilyFlags;
                if (dumpQueues) std::cout << "\t Found compute queue family " <<  i << " with " << queue.queueFamilyProperties.queueCount << " max num of queues." << std::endl;
            }

            // present queue must support the surface
            if ((pWsiDisplay != nullptr) &&
                    (presentQueueFamily < 0) && pWsiDisplay->PhysDeviceCanPresent(physicalDevice, i)) {
                if (dumpQueues) std::cout << "\t Found present queue family " <<  i << "." << std::endl;
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
                    std::cout << "*** Selected Vulkan physical device with name: " << devProp2.properties.deviceName << std::hex
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
        std::cerr << "ERROR: Found physical device with name: " << devProp2.properties.deviceName << std::hex
                  << ", vendor ID: " << devProp2.properties.vendorID << ", and device ID: " << devProp2.properties.deviceID
                  << std::dec
                  << " NOT having the required queue families!" << std::endl << std::flush;
    }

    return (m_physDevice != VK_NULL_HANDLE) ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
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
        std::unordered_set<int32_t> uniqueQueueFamilies;
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
        const std::vector<float> queuePriorities(maxQueueInstances, 0.0f);
        std::array<VkDeviceQueueCreateInfo, MAX_QUEUE_FAMILIES> queueInfo = {};
        const bool isUnique = uniqueQueueFamilies.insert(m_gfxQueueFamily).second;
        assert(isUnique);
        if (!isUnique) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (createGraphicsQueue) {
            queueInfo[devInfo.queueCreateInfoCount].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo[devInfo.queueCreateInfoCount].queueFamilyIndex = m_gfxQueueFamily;
            queueInfo[devInfo.queueCreateInfoCount].queueCount = 1;
            queueInfo[devInfo.queueCreateInfoCount].pQueuePriorities = queuePriorities.data();
            devInfo.queueCreateInfoCount++;
        }

        if (createPresentQueue &&
                !(m_presentQueueFamily != -1) &&
                uniqueQueueFamilies.insert(m_presentQueueFamily).second) {

            queueInfo[devInfo.queueCreateInfoCount].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo[devInfo.queueCreateInfoCount].queueFamilyIndex = m_presentQueueFamily;
            queueInfo[devInfo.queueCreateInfoCount].queueCount = 1;
            queueInfo[devInfo.queueCreateInfoCount].pQueuePriorities = queuePriorities.data();
            devInfo.queueCreateInfoCount++;
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

        VkPhysicalDeviceFeatures2 deviceFeatures { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &synchronization2Features};
        GetPhysicalDeviceFeatures2(m_physDevice, &deviceFeatures);

        assert(timelineSemaphoreFeatures.timelineSemaphore);
        assert(videoMaintenance1Features.videoMaintenance1);
        assert(synchronization2Features.synchronization2);
        assert(((videoCodecs & VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) != 0) ==
                (videoEncodeAV1Feature.videoEncodeAV1 != VK_FALSE));
        assert(((videoCodecs & VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) != 0) ==
                (videoDecodeVP9Feature.videoDecodeVP9 != VK_FALSE));

        devInfo.pNext = &deviceFeatures;

        if ((numDecodeQueues > 0) &&
                (m_videoDecodeQueueFamily != -1) &&
                uniqueQueueFamilies.insert(m_videoDecodeQueueFamily).second) {
            queueInfo[devInfo.queueCreateInfoCount].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo[devInfo.queueCreateInfoCount].queueFamilyIndex = m_videoDecodeQueueFamily;
            queueInfo[devInfo.queueCreateInfoCount].queueCount = numDecodeQueues;
            queueInfo[devInfo.queueCreateInfoCount].pQueuePriorities = queuePriorities.data();
            devInfo.queueCreateInfoCount++;
        }

        if ((numEncodeQueues > 0) &&
                (m_videoEncodeQueueFamily != -1) &&
                uniqueQueueFamilies.insert(m_videoEncodeQueueFamily).second) {
            queueInfo[devInfo.queueCreateInfoCount].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo[devInfo.queueCreateInfoCount].queueFamilyIndex = m_videoEncodeQueueFamily;
            queueInfo[devInfo.queueCreateInfoCount].queueCount = numEncodeQueues;
            queueInfo[devInfo.queueCreateInfoCount].pQueuePriorities = queuePriorities.data();
            devInfo.queueCreateInfoCount++;
        }

        if (createComputeQueue &&
                (m_computeQueueFamily != -1) &&
                uniqueQueueFamilies.insert(m_computeQueueFamily).second) {
            queueInfo[devInfo.queueCreateInfoCount].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo[devInfo.queueCreateInfoCount].queueFamilyIndex = m_computeQueueFamily;
            queueInfo[devInfo.queueCreateInfoCount].queueCount = 1;
            queueInfo[devInfo.queueCreateInfoCount].pQueuePriorities = queuePriorities.data();
            devInfo.queueCreateInfoCount++;
        }

        if (createTransferQueue &&
                (m_transferQueueFamily != -1) &&
                uniqueQueueFamilies.insert(m_transferQueueFamily).second) {
            queueInfo[devInfo.queueCreateInfoCount].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo[devInfo.queueCreateInfoCount].queueFamilyIndex = m_transferQueueFamily;
            queueInfo[devInfo.queueCreateInfoCount].queueCount = 1;
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
    }

    vk::InitDispatchTableBottom(m_instance, m_device, this);

    if (createGraphicsQueue) {
        GetDeviceQueue(m_device, GetGfxQueueFamilyIdx()    , 0, &m_gfxQueue);
    }
    if (createComputeQueue) {
        GetDeviceQueue(m_device, GetComputeQueueFamilyIdx(), 0, &m_computeQueue);
    }
    if (createPresentQueue) {
        GetDeviceQueue(m_device, GetPresentQueueFamilyIdx(), 0, &m_presentQueue);
    }
    if (createTransferQueue) {
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
    : m_libHandle()
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

void VulkanDeviceContext::DeviceWaitIdle() const
{
    vk::VkInterfaceFunctions::DeviceWaitIdle(m_device);
}

VulkanDeviceContext::~VulkanDeviceContext() {

    if (m_device) {
        if (!m_importedDeviceHandle) {
            DestroyDevice(m_device, nullptr);
        }
        m_device = VkDevice();
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

#if !defined(VK_USE_PLATFORM_WIN32_KHR)
    dlclose(m_libHandle);
#else // defined(VK_USE_PLATFORM_WIN32_KHR)
    FreeLibrary(m_libHandle);
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
    std::cout << "###### List of " <<  (deviceExt ? "Device" : "Instance") << " Extensions: ######" << std::endl;
    for (const auto& e : extensions) {
        std::cout << "\t " << e.extensionName << "(v." << e.specVersion << ")\n";
    }
}

VkResult VulkanDeviceContext::PopulateInstanceExtensions()
{
    uint32_t extensionsCount = 0;
    VkResult result = EnumerateInstanceExtensionProperties( nullptr, &extensionsCount, nullptr );
    if ((result != VK_SUCCESS) || (extensionsCount == 0)) {
        std::cout << "Could not get the number of instance extensions." << std::endl;
        return result;
    }
    m_instanceExtensions.resize( extensionsCount );
    result = EnumerateInstanceExtensionProperties( nullptr, &extensionsCount, m_instanceExtensions.data() );
    if ((result != VK_SUCCESS) || (extensionsCount == 0)) {
        std::cout << "Could not enumerate instance extensions." << std::endl;
        return result;
    }
    return result;
}

VkResult VulkanDeviceContext::PopulateDeviceExtensions()
{
    uint32_t extensions_count = 0;
    VkResult result = EnumerateDeviceExtensionProperties( m_physDevice, nullptr, &extensions_count, nullptr );
    if ((result != VK_SUCCESS) || (extensions_count == 0)) {
        std::cout << "Could not get the number of device extensions." << std::endl;
        return result;
    }
    m_deviceExtensions.resize( extensions_count );
    result = EnumerateDeviceExtensionProperties( m_physDevice, nullptr, &extensions_count, m_deviceExtensions.data() );
    if ((result != VK_SUCCESS) || (extensions_count == 0)) {
        std::cout << "Could not enumerate device extensions." << std::endl;
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
                                                      const char * pCustomLoader)
{
    static const char* const requiredInstanceLayers[] = {
        "VK_LAYER_KHRONOS_validation",
        nullptr
    };

    static const char* const requiredInstanceExtensions[] = {
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
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
    }
    if (videoCodecs == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
        AddReqDeviceExtension(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME);
    }
    if (videoCodecs == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
        AddReqDeviceExtension(VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME);
    }
    if (videoCodecs == VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) {
        AddReqDeviceExtension(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME);
    }

    VkResult result = InitVulkanDevice(pAppName, vkInstance, enbaleVerboseDump);
    if (result != VK_SUCCESS) {
        printf("Could not initialize the Vulkan device!\n");
        return result;
    }

    result = InitDebugReport(enableValidation, enableVerboseValidation);
    if (result != VK_SUCCESS) {
        return result;
    }

    return result;
}
