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

// VK_LAYER_VVS_gop_no_preference: a test layer. Every encode quality level it passes through states
// preferredGopFrameCount 0 for H.264, H.265 and AV1, "no preference", which a device may state and
// which the GPUs the tests run on do not; everything else passes through unchanged. For each value it
// rewrites it prints one line on stderr, which tells a test the layer was in effect.
//
// It intercepts vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR and the calls a layer must
// (vkCreateInstance, vkDestroyInstance, vkCreateDevice, vkGetInstanceProcAddr, vkGetDeviceProcAddr).
// An instance and its physical devices share the loader's dispatch table pointer, which keys the
// instance data; a device is keyed by its own.

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#define LAYER_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

struct InstanceData {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr;
    PFN_vkDestroyInstance nextDestroyInstance;
    PFN_vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR nextQualityLevelProperties;
};

std::mutex g_lock;
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, PFN_vkGetDeviceProcAddr> g_devices;

void* DispatchKey(const void* handle) { return *static_cast<void* const*>(handle); }

bool FindInstance(const void* handle, InstanceData& data) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_instances.find(DispatchKey(handle));
    if (it == g_instances.end()) {
        return false;
    }
    data = it->second;
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL QualityLevelProperties(VkPhysicalDevice physicalDevice,
                                                      const VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR* pInfo,
                                                      VkVideoEncodeQualityLevelPropertiesKHR* pProperties) {
    InstanceData data{};
    if (!FindInstance(physicalDevice, data) || (data.nextQualityLevelProperties == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = data.nextQualityLevelProperties(physicalDevice, pInfo, pProperties);
    if (result != VK_SUCCESS) {
        return result;
    }
    for (auto* p = static_cast<VkBaseOutStructure*>(pProperties->pNext); p != nullptr; p = p->pNext) {
        uint32_t* preferred = nullptr;
        const char* codec = "";
        switch (p->sType) {
            case VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUALITY_LEVEL_PROPERTIES_KHR:
                preferred = &reinterpret_cast<VkVideoEncodeH264QualityLevelPropertiesKHR*>(p)->preferredGopFrameCount;
                codec = "H.264";
                break;
            case VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUALITY_LEVEL_PROPERTIES_KHR:
                preferred = &reinterpret_cast<VkVideoEncodeH265QualityLevelPropertiesKHR*>(p)->preferredGopFrameCount;
                codec = "H.265";
                break;
            case VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_QUALITY_LEVEL_PROPERTIES_KHR:
                preferred = &reinterpret_cast<VkVideoEncodeAV1QualityLevelPropertiesKHR*>(p)->preferredGopFrameCount;
                codec = "AV1";
                break;
            default:
                break;
        }
        if (preferred != nullptr) {
            std::fprintf(stderr, "[VK_LAYER_VVS_gop_no_preference] %s preferredGopFrameCount %u -> 0\n", codec, *preferred);
            *preferred = 0;
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                              VkInstance* pInstance) {
    auto* chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(pCreateInfo->pNext));
    while ((chain != nullptr) &&
           !((chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) && (chain->function == VK_LAYER_LINK_INFO))) {
        chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(chain->pNext));
    }
    if (chain == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto nextCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    const VkResult result = nextCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        return result;
    }
    InstanceData data{};
    data.instance = *pInstance;
    data.nextGetInstanceProcAddr = nextGetInstanceProcAddr;
    data.nextDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(nextGetInstanceProcAddr(*pInstance, "vkDestroyInstance"));
    data.nextQualityLevelProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR>(
        nextGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR"));
    std::lock_guard<std::mutex> lock(g_lock);
    g_instances[DispatchKey(*pInstance)] = data;
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    InstanceData data{};
    if (!FindInstance(instance, data)) {
        return;
    }
    data.nextDestroyInstance(instance, pAllocator);
    std::lock_guard<std::mutex> lock(g_lock);
    g_instances.erase(DispatchKey(instance));
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    auto* chain = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(pCreateInfo->pNext));
    while ((chain != nullptr) &&
           !((chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) && (chain->function == VK_LAYER_LINK_INFO))) {
        chain = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(chain->pNext));
    }
    InstanceData data{};
    if ((chain == nullptr) || !FindInstance(physicalDevice, data)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr nextGetDeviceProcAddr = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto nextCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(nextGetInstanceProcAddr(data.instance, "vkCreateDevice"));
    const VkResult result = nextCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }
    std::lock_guard<std::mutex> lock(g_lock);
    g_devices[DispatchKey(*pDevice)] = nextGetDeviceProcAddr;
    return result;
}

}  // namespace

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GopNoPreferenceGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(GopNoPreferenceGetDeviceProcAddr);
    }
    PFN_vkGetDeviceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_devices.find(DispatchKey(device));
        if (it != g_devices.end()) {
            next = it->second;
        }
    }
    return (next != nullptr) ? next(device, pName) : nullptr;
}

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GopNoPreferenceGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(GopNoPreferenceGetInstanceProcAddr);
    }
    if (std::strcmp(pName, "vkCreateInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
    }
    if (std::strcmp(pName, "vkDestroyInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance);
    }
    if (std::strcmp(pName, "vkCreateDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(CreateDevice);
    }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(GopNoPreferenceGetDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(QualityLevelProperties);
    }
    if (instance == VK_NULL_HANDLE) {
        return nullptr;
    }
    InstanceData data{};
    return FindInstance(instance, data) ? data.nextGetInstanceProcAddr(instance, pName) : nullptr;
}
