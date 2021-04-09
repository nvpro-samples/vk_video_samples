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

#include "VulkanShaderCompiler.h"
#include <shaderc/shaderc.hpp>
#include <NvCodecUtils/Logger.h>
#include "Helpers.h"

namespace vulkanVideoUtils {

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
        LOG(ERROR) << "VulkanShaderCompiler: " << "invalid VKShaderStageFlagBits" << "type = " <<  type;
    }
    return static_cast<shaderc_shader_kind>(-1);
}

VulkanShaderCompiler::VulkanShaderCompiler() {

    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    compilerHandle = compiler;
}

VulkanShaderCompiler::~VulkanShaderCompiler() {

    if (compilerHandle) {
        shaderc_compiler_t compiler = (shaderc_compiler_t)compilerHandle;
        shaderc_compiler_release(compiler);
        compilerHandle = nullptr;
    }
}

VkResult VulkanShaderCompiler::BuildGlslShader(const char *shaderCode, size_t shaderSize, VkShaderStageFlagBits type,
                             VkDevice vkDevice, VkShaderModule *shaderOut)
{
    VkResult result = VK_NOT_READY;
    if (compilerHandle) {
        shaderc_compiler_t compiler = (shaderc_compiler_t)compilerHandle;

        shaderc_compilation_result_t spvShader = shaderc_compile_into_spv(
                    compiler, shaderCode, shaderSize, getShadercShaderType(type),
                    "shaderc_error", "main", nullptr);
        if (shaderc_result_get_compilation_status(spvShader) !=
                shaderc_compilation_status_success) {
            return static_cast<VkResult>(-1);
        }

        // build vulkan shader module
        VkShaderModuleCreateInfo shaderModuleCreateInfo = VkShaderModuleCreateInfo();
        shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderModuleCreateInfo.pNext = nullptr;
        shaderModuleCreateInfo.codeSize = shaderc_result_get_length(spvShader);
        shaderModuleCreateInfo.pCode = (const uint32_t *)shaderc_result_get_bytes(spvShader);
        shaderModuleCreateInfo.flags = 0;
        result = vk::CreateShaderModule(vkDevice, &shaderModuleCreateInfo, nullptr, shaderOut);

        shaderc_result_release(spvShader);
    }
    return result;
}

// Create VK shader module from given glsl shader file
VkResult VulkanShaderCompiler::BuildShaderFromFile(const char *filePath, VkShaderStageFlagBits type,
                             VkDevice vkDevice, VkShaderModule *shaderOut)
{
    // read file from the path
    FILE *fp = fopen(filePath, "rb");

    int pos = ftell(fp);
    size_t size = 0;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, pos, SEEK_SET);

    size_t glslShaderLen = size;
    char* glslShader = new char[glslShaderLen];

    fread(static_cast<void *>(glslShader), glslShaderLen, 1, fp);
    fclose(fp);

    VkResult result = VK_NOT_READY;
    BuildGlslShader(glslShader, glslShaderLen, type, vkDevice, shaderOut);

    delete [] glslShader;
    glslShader = nullptr;

    return result;
}

} // namespace vulkanVideoUtils
