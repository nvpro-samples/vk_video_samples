/*
* Copyright 2020 NVIDIA Corporation.
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

#include "assert.h"
#include <iostream>
#include <mutex>
#include <atomic>
#include <vector>

#include "VulkanShaderCompiler.h"
#include "VkCodecUtils/VulkanShaderCompilerBackend.h"
#include "Helpers.h"
#include "VkCodecUtils/VulkanDeviceContext.h"

// The compiler backend is shared: both implementations keep process-global
// state, so one instance is created on first use and torn down with the last
// VulkanShaderCompiler. Which backend this is - glslang by default, shaderc
// when opted in - is decided by which backend translation unit the build
// compiles; nothing here depends on the choice.
static std::mutex g_compilerMutex;
static std::atomic<int> g_compilerRefCount{0};
static VulkanShaderCompilerBackend* g_sharedBackend = nullptr;

void* VulkanShaderCompiler::GetSharedCompiler() {
    std::lock_guard<std::mutex> lock(g_compilerMutex);

    if (g_compilerRefCount == 0) {
        // First instance - create the shared backend
        g_sharedBackend = VulkanShaderCompilerBackend::Create();
        if (g_sharedBackend == nullptr) {
            std::cerr << "VulkanShaderCompiler: Failed to initialize the shader compiler backend!" << std::endl;
            return nullptr;
        }
    }

    g_compilerRefCount++;
    return g_sharedBackend;
}

void VulkanShaderCompiler::ReleaseSharedCompiler() {
    std::lock_guard<std::mutex> lock(g_compilerMutex);

    g_compilerRefCount--;

    if (g_compilerRefCount == 0 && g_sharedBackend != nullptr) {
        // Last instance - tear down the shared backend
        VulkanShaderCompilerBackend::Destroy(g_sharedBackend);
        g_sharedBackend = nullptr;
    }
}

VulkanShaderCompiler::VulkanShaderCompiler()
    : compilerHandle(nullptr)
{
    compilerHandle = GetSharedCompiler();
}

VulkanShaderCompiler::~VulkanShaderCompiler() {
    if (compilerHandle) {
        ReleaseSharedCompiler();
        compilerHandle = nullptr;
    }
}

VkShaderModule VulkanShaderCompiler::BuildGlslShader(const char *shaderCode, size_t shaderSize,
                                                     VkShaderStageFlagBits type,
                                                     const VulkanDeviceContext* vkDevCtx)
{
    if (compilerHandle == nullptr) {
        return VK_NULL_HANDLE;
    }

    VulkanShaderCompilerBackend* backend =
            reinterpret_cast<VulkanShaderCompilerBackend*>(compilerHandle);

    std::vector<uint32_t> spirv;
    if (!backend->CompileGlslToSpirv(shaderCode, shaderSize, type, spirv)) {
        return VK_NULL_HANDLE;
    }

    // build vulkan shader module. spirv.size() counts 32-bit words, while
    // codeSize is a byte count.
    VkShaderModuleCreateInfo shaderModuleCreateInfo = VkShaderModuleCreateInfo();
    shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleCreateInfo.pNext = nullptr;
    shaderModuleCreateInfo.codeSize = spirv.size() * sizeof(uint32_t);
    shaderModuleCreateInfo.pCode = spirv.data();
    shaderModuleCreateInfo.flags = 0;

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkResult result = vkDevCtx->CreateShaderModule(*vkDevCtx, &shaderModuleCreateInfo, nullptr, &shaderModule);
    assert(result == VK_SUCCESS);
    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create shader module" << std::endl;
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}
