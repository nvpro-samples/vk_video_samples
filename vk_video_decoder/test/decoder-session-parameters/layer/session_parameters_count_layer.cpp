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

// VK_LAYER_VVS_session_parameters_count: a test layer. It counts the video session parameters objects a
// device creates and destroys, and prints on stderr, when the device is destroyed, how many it created, the
// most that were alive at once, and how many are still alive then:
//   [VK_LAYER_VVS_session_parameters_count] created N, peak live P, live at vkDestroyDevice L
// Everything else passes through.
//
// It intercepts vkCreateVideoSessionParametersKHR, vkDestroyVideoSessionParametersKHR, vkDestroyDevice and
// the calls a layer must (vkCreateInstance, vkDestroyInstance, vkCreateDevice, vkGetInstanceProcAddr,
// vkGetDeviceProcAddr). An instance and its physical devices share the loader's dispatch table pointer,
// which keys the instance data; a device is keyed by its own.

#include <algorithm>
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
};

struct DeviceData {
    PFN_vkGetDeviceProcAddr nextGetDeviceProcAddr;
    PFN_vkDestroyDevice nextDestroyDevice;
    PFN_vkCreateVideoSessionParametersKHR nextCreate;
    PFN_vkDestroyVideoSessionParametersKHR nextDestroy;
    uint64_t created;
    int64_t live;
    int64_t peak;
};

std::mutex g_lock;
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, DeviceData> g_devices;

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

VKAPI_ATTR VkResult VKAPI_CALL CreateSessionParameters(VkDevice device, const VkVideoSessionParametersCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkVideoSessionParametersKHR* pVideoSessionParameters) {
    PFN_vkCreateVideoSessionParametersKHR next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_devices.find(DispatchKey(device));
        if (it != g_devices.end()) {
            next = it->second.nextCreate;
        }
    }
    if (next == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = next(device, pCreateInfo, pAllocator, pVideoSessionParameters);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_lock);
        DeviceData& data = g_devices[DispatchKey(device)];
        data.created++;
        data.live++;
        data.peak = std::max(data.peak, data.live);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySessionParameters(VkDevice device, VkVideoSessionParametersKHR videoSessionParameters,
                                                    const VkAllocationCallbacks* pAllocator) {
    PFN_vkDestroyVideoSessionParametersKHR next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_devices.find(DispatchKey(device));
        if (it != g_devices.end()) {
            next = it->second.nextDestroy;
            if (videoSessionParameters != VK_NULL_HANDLE) {
                it->second.live--;
            }
        }
    }
    if (next != nullptr) {
        next(device, videoSessionParameters, pAllocator);
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    DeviceData data{};
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_devices.find(DispatchKey(device));
        if (it == g_devices.end()) {
            return;
        }
        data = it->second;
        g_devices.erase(it);
    }
    std::fprintf(stderr, "[VK_LAYER_VVS_session_parameters_count] created %llu, peak live %lld, live at vkDestroyDevice %lld\n",
                 static_cast<unsigned long long>(data.created), static_cast<long long>(data.peak),
                 static_cast<long long>(data.live));
    data.nextDestroyDevice(device, pAllocator);
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
    InstanceData instanceData{};
    if ((chain == nullptr) || !FindInstance(physicalDevice, instanceData)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr nextGetDeviceProcAddr = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto nextCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(nextGetInstanceProcAddr(instanceData.instance, "vkCreateDevice"));
    const VkResult result = nextCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }
    DeviceData data{};
    data.nextGetDeviceProcAddr = nextGetDeviceProcAddr;
    data.nextDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(nextGetDeviceProcAddr(*pDevice, "vkDestroyDevice"));
    data.nextCreate = reinterpret_cast<PFN_vkCreateVideoSessionParametersKHR>(
        nextGetDeviceProcAddr(*pDevice, "vkCreateVideoSessionParametersKHR"));
    data.nextDestroy = reinterpret_cast<PFN_vkDestroyVideoSessionParametersKHR>(
        nextGetDeviceProcAddr(*pDevice, "vkDestroyVideoSessionParametersKHR"));
    std::lock_guard<std::mutex> lock(g_lock);
    g_devices[DispatchKey(*pDevice)] = data;
    return result;
}

}  // namespace

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CountGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(CountGetDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkDestroyDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    }
    if (std::strcmp(pName, "vkCreateVideoSessionParametersKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(CreateSessionParameters);
    }
    if (std::strcmp(pName, "vkDestroyVideoSessionParametersKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroySessionParameters);
    }
    PFN_vkGetDeviceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_devices.find(DispatchKey(device));
        if (it != g_devices.end()) {
            next = it->second.nextGetDeviceProcAddr;
        }
    }
    return (next != nullptr) ? next(device, pName) : nullptr;
}

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CountGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(CountGetInstanceProcAddr);
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
        return reinterpret_cast<PFN_vkVoidFunction>(CountGetDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkDestroyDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    }
    if (std::strcmp(pName, "vkCreateVideoSessionParametersKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(CreateSessionParameters);
    }
    if (std::strcmp(pName, "vkDestroyVideoSessionParametersKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroySessionParameters);
    }
    if (instance == VK_NULL_HANDLE) {
        return nullptr;
    }
    InstanceData data{};
    return FindInstance(instance, data) ? data.nextGetInstanceProcAddr(instance, pName) : nullptr;
}
