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
#include <fstream>
#include <mutex>
#include <atomic>

#include "VulkanShaderCompiler.h"
#include <shaderc/shaderc.h>
#include "Helpers.h"
#include "VkCodecUtils/VulkanDeviceContext.h"

// Shared compiler singleton with thread safety
static std::mutex g_compilerMutex;
static shaderc_compiler_t g_sharedCompiler = nullptr;
static std::atomic<int> g_compilerRefCount{0};

// Translate Vulkan Shader Type to shaderc shader type
static shaderc_shader_kind getShadercShaderType(VkShaderStageFlagBits type)
{
    switch (type) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        return shaderc_glsl_vertex_shader;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        return shaderc_glsl_fragment_shader;
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
        return shaderc_glsl_tess_control_shader;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
        return shaderc_glsl_tess_evaluation_shader;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        return shaderc_glsl_geometry_shader;
    case VK_SHADER_STAGE_COMPUTE_BIT:
        return shaderc_glsl_compute_shader;
    default:
        std::cerr << "VulkanShaderCompiler: " << "invalid VKShaderStageFlagBits" << "type = " <<  type;
    }
    return static_cast<shaderc_shader_kind>(-1);
}

void* VulkanShaderCompiler::GetSharedCompiler() {
    std::lock_guard<std::mutex> lock(g_compilerMutex);

    if (g_compilerRefCount == 0) {
        // First instance - create the shared compiler
        g_sharedCompiler = shaderc_compiler_initialize();
        if (!g_sharedCompiler) {
            std::cerr << "VulkanShaderCompiler: Failed to initialize shared shaderc compiler!" << std::endl;
            return nullptr;
        }
        std::cout << "VulkanShaderCompiler: Initialized shared shaderc compiler" << std::endl;
    }

    g_compilerRefCount++;
    return g_sharedCompiler;
}

void VulkanShaderCompiler::ReleaseSharedCompiler() {
    std::lock_guard<std::mutex> lock(g_compilerMutex);

    g_compilerRefCount--;

    if (g_compilerRefCount == 0 && g_sharedCompiler) {
        // Last instance - release the shared compiler
        shaderc_compiler_release(g_sharedCompiler);
        g_sharedCompiler = nullptr;
        std::cout << "VulkanShaderCompiler: Released shared shaderc compiler" << std::endl;
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
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (compilerHandle) {
        shaderc_compiler_t compiler = (shaderc_compiler_t)compilerHandle;

        // Create compile options with explicit Vulkan target
        shaderc_compile_options_t options = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
                                               shaderc_env_version_vulkan_1_2);
        shaderc_compile_options_set_target_spirv(options, shaderc_spirv_version_1_5);

        // Thread-safe compilation using shared compiler
        std::lock_guard<std::mutex> lock(g_compilerMutex);
        shaderc_compilation_result_t spvShader = shaderc_compile_into_spv(
                    compiler, shaderCode, shaderSize, getShadercShaderType(type),
                    "shaderc_error", "main", options);

        shaderc_compile_options_release(options);

        if (shaderc_result_get_compilation_status(spvShader) !=
                shaderc_compilation_status_success) {

            std::cerr << "Compilation error: \n" << shaderc_result_get_error_message(spvShader) << std::endl;
            shaderc_result_release(spvShader);
            return VK_NULL_HANDLE;
        }

        // build vulkan shader module
        VkShaderModuleCreateInfo shaderModuleCreateInfo = VkShaderModuleCreateInfo();
        shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderModuleCreateInfo.pNext = nullptr;
        shaderModuleCreateInfo.codeSize = shaderc_result_get_length(spvShader);
        shaderModuleCreateInfo.pCode = (const uint32_t *)shaderc_result_get_bytes(spvShader);
        shaderModuleCreateInfo.flags = 0;
        VkResult result = vkDevCtx->CreateShaderModule(*vkDevCtx, &shaderModuleCreateInfo, nullptr, &shaderModule);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            std::cerr << "Failed to create shader module" << std::endl;
            shaderc_result_release(spvShader);
            return VK_NULL_HANDLE;
        }
        shaderc_result_release(spvShader);
    }
    return shaderModule;
}

// Create VK shader module from given glsl shader file
VkShaderModule VulkanShaderCompiler::BuildShaderFromFile(const char *fileName,
                                                         VkShaderStageFlagBits type,
                                                         const VulkanDeviceContext* vkDevCtx)
{
#ifdef seekg
    // read file from the path
    std::ifstream is(fileName, std::ios::binary | std::ios::in | std::ios::ate);

    if (is.is_open()) {
        is.seekg (0, is.end);
        std::streamoff fileSize = is.tellg();
        if (fileSize < 0 || static_cast<size_t>(fileSize) > std::numeric_limits<size_t>::max()) {
            std::cerr << "File size is too large or invalid" << std::endl;
            return VK_NULL_HANDLE;
        }
        size_t size = static_cast<size_t>(fileSize);
        is.seekg(0, is.beg);
        char* shaderCode = new char[size];
        is.read(shaderCode, size);
        is.close();

        assert(size > 0);

        VkShaderModule shaderModule = BuildGlslShader(shaderCode, size, type, vkDevCtx);

        delete [] shaderCode;

        return shaderModule;
    }
#endif

    return VK_NULL_HANDLE;
}
