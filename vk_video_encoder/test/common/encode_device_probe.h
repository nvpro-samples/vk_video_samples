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

#ifndef _ENCODE_DEVICE_PROBE_H_
#define _ENCODE_DEVICE_PROBE_H_

// WHO DECIDES THAT A FAILURE IS A SKIP.
//
// A gpu test that cannot get a working encoder out of the library has two
// possible causes: the machine has no device that encodes the codec, which is a
// SKIP (exit 77), or the library failed on a machine that has one, which is a
// FAILURE. The library cannot be asked which: it is the thing that failed. This
// probe asks the Vulkan loader alone -- an instance, its physical devices, their
// queue families' video properties -- and nothing of the encoder library or of
// VkCodecUtils, so a defect there cannot turn into a skip.
//
// Once the library has a device, no later failure is the machine's: it exits 1
// with what failed, through Fail().
//
// VVS_REQUIRE_GPU (a compile definition from the CMake option of that name, or
// the environment variable set to 1) turns the no-device skip into a failure as
// well, for the rigs, where a missing device is itself the fault. A skip for
// another reason -- a format the device refuses, a platform without dma-buf --
// is not affected.

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace enctest {

constexpr int kExitSkip = 77;

inline const char* CodecOperationName(VkVideoCodecOperationFlagsKHR op)
{
    switch (op) {
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR: return "H.264 encode";
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR: return "H.265 encode";
    case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:  return "AV1 encode";
    default:                                           return "video encode";
    }
}

inline std::string Hex(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%x", value);
    return text;
}

// Whether some physical device -- of |vendorID|, when it is not 0 -- has a
// queue family that encodes every operation in |codecOps| (any encode queue,
// when it is 0). |detail| says what was found, or where the search stopped.
inline bool ProbeEncodeDevice(VkVideoCodecOperationFlagsKHR codecOps, std::string* detail, uint32_t vendorID = 0)
{
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr gipa =
        lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr")) : nullptr;
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    PFN_vkGetInstanceProcAddr gipa =
        lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")) : nullptr;
#endif
    if (gipa == nullptr) {
        *detail = "no Vulkan loader";
        return false;
    }
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "encode-device-probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = createInstance ? createInstance(&ici, nullptr, &instance) : VK_ERROR_INITIALIZATION_FAILED;
    if (created != VK_SUCCESS) {
        *detail = "vkCreateInstance failed (" + std::to_string(created) + "): no Vulkan driver";
        return false;
    }
    auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
        gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties2"));

    bool found = false;
    *detail = "no physical device";
    uint32_t count = 0;
    if (enumerate && properties && queues && (enumerate(instance, &count, nullptr) == VK_SUCCESS) && (count != 0)) {
        std::vector<VkPhysicalDevice> devices(count);
        enumerate(instance, &count, devices.data());
        *detail = std::to_string(count) + " physical device(s), none with a queue family for " +
                  CodecOperationName(codecOps) + ((vendorID != 0) ? " from vendor " + Hex(vendorID) : "");
        for (uint32_t d = 0; (d < count) && !found; d++) {
            VkPhysicalDeviceProperties p = {};
            properties(devices[d], &p);
            if ((vendorID != 0) && (p.vendorID != vendorID)) {
                continue;
            }
            uint32_t families = 0;
            queues(devices[d], &families, nullptr);
            std::vector<VkQueueFamilyVideoPropertiesKHR> video(families, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR});
            std::vector<VkQueueFamilyProperties2> props(families, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
            for (uint32_t f = 0; f < families; f++) {
                props[f].pNext = &video[f];
            }
            queues(devices[d], &families, props.data());
            for (uint32_t f = 0; f < families; f++) {
                if (((props[f].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) != 0) &&
                    ((video[f].videoCodecOperations & codecOps) == codecOps)) {
                    *detail = std::string(p.deviceName) + ", queue family " + std::to_string(f) + ", encodes " +
                              CodecOperationName(codecOps);
                    found = true;
                    break;
                }
            }
        }
    }
    if (destroyInstance) {
        destroyInstance(instance, nullptr);
    }
    return found;
}

inline bool RequireGpu()
{
#ifdef VVS_REQUIRE_GPU
    return true;
#else
    const char* env = std::getenv("VVS_REQUIRE_GPU");
    return (env != nullptr) && (std::strcmp(env, "1") == 0);
#endif
}

// The exit status of a test whose encoder library failed before it had a
// device, |what| saying how: 77 when no device (of |vendorID|, when it is not
// 0) encodes |codecOps| and VVS_REQUIRE_GPU is off; 1 otherwise, since then
// the library is at fault.
inline int NoDeviceOrFail(const std::string& what, VkVideoCodecOperationFlagsKHR codecOps, uint32_t vendorID = 0)
{
    std::string detail;
    if (ProbeEncodeDevice(codecOps, &detail, vendorID)) {
        std::printf("FAIL: %s, on a host with a device that encodes: %s\n", what.c_str(), detail.c_str());
        return 1;
    }
    if (RequireGpu()) {
        std::printf("FAIL: %s, and VVS_REQUIRE_GPU is set: %s\n", what.c_str(), detail.c_str());
        return 1;
    }
    std::printf("SKIP: %s: %s\n", what.c_str(), detail.c_str());
    return kExitSkip;
}

inline int NoDeviceOrFail(const char* call, VkResult result, VkVideoCodecOperationFlagsKHR codecOps)
{
    if (result == VK_ERROR_LAYER_NOT_PRESENT) {
        // The test asked for a layer this machine does not have, the validation layer of a --validate run. A
        // validation gate with REQUIRE_LAYER turns this skip into a failure where the layer must be present.
        std::printf("SKIP: %s returned %d: a requested layer is not on the loader's search path\n", call,
                    static_cast<int>(result));
        return kExitSkip;
    }
    return NoDeviceOrFail(std::string(call) + " returned " + std::to_string(result), codecOps);
}

// The exit status of a failure no machine excuses: after the library has a
// device, or in a call that never needs one.
inline int Fail(const char* what, long long result)
{
    std::printf("FAIL: %s (%lld)\n", what, result);
    return 1;
}

inline int Fail(const char* what)
{
    std::printf("FAIL: %s\n", what);
    return 1;
}

}  // namespace enctest

#endif /* _ENCODE_DEVICE_PROBE_H_ */
