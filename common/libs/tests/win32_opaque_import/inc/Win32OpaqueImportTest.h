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

#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <vector>
#include <string>

#include "VkCodecUtils/VulkanDeviceContext.h"

namespace win32_opaque_import_test {

struct TestConfig {
    bool        verbose{false};
    bool        validation{false};
    uint32_t    width{1920};
    uint32_t    height{1080};
    VkFormat    format{VK_FORMAT_G8_B8R8_2PLANE_420_UNORM};  // NV12
};

struct ImportAttempt {
    std::string         label;
    VkImageUsageFlags   usage{0};
    VkImageCreateFlags  flags{0};
    VkResult            createResult{VK_SUCCESS};
    VkResult            allocResult{VK_SUCCESS};
    VkResult            bindResult{VK_SUCCESS};
    VkDeviceSize        memReqSize{0};
    uint32_t            memTypeIdx{0};
};

struct TestCaseResult {
    uint32_t            id{0};
    VkImageUsageFlags   exportUsage{0};
    VkImageCreateFlags  exportFlags{0};
    VkResult            exportCreateResult{VK_SUCCESS};
    VkResult            exportAllocResult{VK_SUCCESS};
    VkResult            exportHandleResult{VK_SUCCESS};
    VkDeviceSize        exportAllocSize{0};
    uint32_t            exportMemTypeIdx{0};

    ImportAttempt       graphicsImport;
    ImportAttempt       videoImport;
};

class Win32OpaqueImportTest {
public:
    Win32OpaqueImportTest() = default;
    ~Win32OpaqueImportTest();

    VkResult init(const TestConfig& config);
    std::vector<TestCaseResult> runAllCombinations();
    void printResults(const std::vector<TestCaseResult>& results) const;

private:
    TestConfig              m_config;
    VulkanDeviceContext     m_vkDevCtx;          // export device
    VkDevice                m_importDevice{VK_NULL_HANDLE};  // separate import device

    PFN_vkGetMemoryWin32HandleKHR m_pfnGetMemWin32Handle{nullptr};

    // Per-device dispatch for the import device
    PFN_vkCreateImage               m_imp_CreateImage{nullptr};
    PFN_vkDestroyImage              m_imp_DestroyImage{nullptr};
    PFN_vkGetImageMemoryRequirements m_imp_GetImageMemoryRequirements{nullptr};
    PFN_vkAllocateMemory            m_imp_AllocateMemory{nullptr};
    PFN_vkFreeMemory                m_imp_FreeMemory{nullptr};
    PFN_vkBindImageMemory           m_imp_BindImageMemory{nullptr};

    VkPhysicalDeviceMemoryProperties m_memProps{};

    VkResult createImportDevice();

    TestCaseResult runSingleTest(uint32_t id,
                                VkImageUsageFlags exportUsage,
                                VkImageCreateFlags exportFlags);

    ImportAttempt tryImport(const char* label,
                            VkImageUsageFlags usage,
                            VkImageCreateFlags flags,
                            HANDLE memoryHandle,
                            VkDeviceSize exportAllocSize);

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    static std::string usageToString(VkImageUsageFlags usage);
    static std::string flagsToString(VkImageCreateFlags flags);
};

} // namespace win32_opaque_import_test

#endif // _WIN32
