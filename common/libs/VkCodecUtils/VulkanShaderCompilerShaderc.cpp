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

// shaderc GLSL-to-SPIR-V backend. See VulkanShaderCompilerBackend.h.
//
// This is the original implementation, kept as an opt-in alternative for
// embedders that already carry shaderc. glslang is the default; shaderc is a
// wrapper over glslang adding a C API, #include resolution and a SPIRV-Tools
// optimization pass, none of which this library uses.

#include <iostream>
#include <mutex>

#include <shaderc/shaderc.h>

#include "VkCodecUtils/VulkanShaderCompilerBackend.h"

namespace {

// Translate a Vulkan shader stage to a shaderc shader kind.
// Sets valid to false for stages shaderc cannot express.
shaderc_shader_kind getShadercShaderType(VkShaderStageFlagBits type, bool& valid)
{
    valid = true;
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
        std::cerr << "VulkanShaderCompiler: invalid VkShaderStageFlagBits type = "
                  << type << std::endl;
    }
    valid = false;
    return shaderc_glsl_infer_from_source;
}

class ShadercCompilerBackend : public VulkanShaderCompilerBackend {

public:

    explicit ShadercCompilerBackend(shaderc_compiler_t compiler)
        : m_compiler(compiler) { }

    ~ShadercCompilerBackend() override
    {
        if (m_compiler != nullptr) {
            shaderc_compiler_release(m_compiler);
            m_compiler = nullptr;
        }
    }

    const char* GetBackendName() const override { return "shaderc"; }

    bool CompileGlslToSpirv(const char* shaderCode, size_t shaderSize,
                            VkShaderStageFlagBits type,
                            std::vector<uint32_t>& spirv) override
    {
        bool stageIsValid = false;
        const shaderc_shader_kind shaderKind = getShadercShaderType(type, stageIsValid);
        if (!stageIsValid) {
            return false;
        }

        shaderc_compile_options_t options = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
                                               shaderc_env_version_vulkan_1_2);
        shaderc_compile_options_set_target_spirv(options, shaderc_spirv_version_1_5);

        std::lock_guard<std::mutex> lock(m_compileMutex);
        shaderc_compilation_result_t spvShader = shaderc_compile_into_spv(
                    m_compiler, shaderCode, shaderSize, shaderKind,
                    "shaderc_error", "main", options);

        shaderc_compile_options_release(options);

        if (shaderc_result_get_compilation_status(spvShader) !=
                shaderc_compilation_status_success) {
            std::cerr << "Compilation error: \n"
                      << shaderc_result_get_error_message(spvShader) << std::endl;
            shaderc_result_release(spvShader);
            return false;
        }

        // shaderc reports a byte count; spirv holds 32-bit words.
        const size_t spirvSizeInBytes = shaderc_result_get_length(spvShader);
        const uint32_t* spirvWords =
                reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(spvShader));
        spirv.assign(spirvWords, spirvWords + (spirvSizeInBytes / sizeof(uint32_t)));

        shaderc_result_release(spvShader);
        return !spirv.empty();
    }

private:

    shaderc_compiler_t m_compiler;
    std::mutex         m_compileMutex;
};

}  // anonymous namespace

VulkanShaderCompilerBackend* VulkanShaderCompilerBackend::Create()
{
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    if (compiler == nullptr) {
        std::cerr << "VulkanShaderCompiler: Failed to initialize shared shaderc compiler!" << std::endl;
        return nullptr;
    }

    return new ShadercCompilerBackend(compiler);
}

void VulkanShaderCompilerBackend::Destroy(VulkanShaderCompilerBackend* backend)
{
    // ~ShadercCompilerBackend releases the shaderc compiler.
    delete backend;
}
