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

// glslang GLSL-to-SPIR-V backend. See VulkanShaderCompilerBackend.h.
//
// glslang is the compiler shaderc itself wraps; going straight to it drops a
// dependency without changing what compiles the shaders.

#include <iostream>
#include <mutex>

// The SPIRV headers are included WITHOUT a glslang/ prefix on purpose. An
// installed glslang puts them at <prefix>/include/glslang/SPIRV/, but in the
// upstream source tree - which is what the from-source fallback in
// cmake/VulkanShaderCompilerBackend.cmake builds - SPIRV/ sits at the top
// level, not under glslang/. Spelling them <SPIRV/...> and adding both the
// include root and its glslang/ subdirectory to the search path is the one
// form that resolves in a distro install, the Vulkan SDK and a fetched
// source tree alike.
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <SPIRV/Logger.h>

#include "VkCodecUtils/VulkanShaderCompilerBackend.h"

namespace {

// Translate a Vulkan shader stage to a glslang stage.
// Returns EShLangCount for stages glslang cannot express.
EShLanguage getGlslangShaderStage(VkShaderStageFlagBits type)
{
    switch (type) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        return EShLangVertex;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        return EShLangFragment;
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
        return EShLangTessControl;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
        return EShLangTessEvaluation;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        return EShLangGeometry;
    case VK_SHADER_STAGE_COMPUTE_BIT:
        return EShLangCompute;
    default:
        std::cerr << "VulkanShaderCompiler: invalid VkShaderStageFlagBits type = "
                  << type << std::endl;
    }
    return EShLangCount;
}

class GlslangCompilerBackend : public VulkanShaderCompilerBackend {

public:

    const char* GetBackendName() const override { return "glslang"; }

    bool CompileGlslToSpirv(const char* shaderCode, size_t shaderSize,
                            VkShaderStageFlagBits type,
                            std::vector<uint32_t>& spirv) override
    {
        const EShLanguage stage = getGlslangShaderStage(type);
        if (stage == EShLangCount) {
            return false;
        }

        // glslang's parser works against process-global state, so compiles are
        // serialized - the shaderc path serialized them under a mutex too.
        std::lock_guard<std::mutex> lock(m_compileMutex);

        glslang::TShader shader(stage);

        const char* sources[1] = { shaderCode };
        const int   lengths[1] = { static_cast<int>(shaderSize) };
        shader.setStringsWithLengths(sources, lengths, 1);

        shader.setEntryPoint("main");
        shader.setSourceEntryPoint("main");

        // Vulkan 1.2 / SPIR-V 1.5, matching what shaderc was asked for. The
        // trailing 100 is the KHR_vulkan_glsl extension version, not a GLSL
        // version.
        shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

        // Without both of these glslang applies OpenGL semantics and rejects
        // layout(set = ...) and push constants.
        const EShMessages messages =
            static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

        // GetDefaultResources() is at global scope (libglslang-default-resource-limits),
        // not in namespace glslang. glslang has no implicit limits: omitting this
        // rejects valid shaders with spurious "too many ..." diagnostics.
        //
        // 450 applies only to source without a #version directive; the generated
        // shaders all declare one, which takes precedence.
        if (!shader.parse(GetDefaultResources(), 450, ENoProfile,
                          false /* forceDefaultVersionAndProfile */,
                          false /* forwardCompatible */, messages)) {
            std::cerr << "Compilation error: \n"
                      << shader.getInfoLog() << "\n"
                      << shader.getInfoDebugLog() << std::endl;
            return false;
        }

        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(messages)) {
            std::cerr << "Link error: \n" << program.getInfoLog() << std::endl;
            return false;
        }

        glslang::SpvOptions spvOptions;
        spvOptions.disableOptimizer = true;   // shaderc was not optimizing either
        spvOptions.generateDebugInfo = false;
        spvOptions.stripDebugInfo = false;
        spv::SpvBuildLogger logger;

        // shader and program must outlive this call: getIntermediate() hands
        // back a pointer the program owns.
        glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger, &spvOptions);

        const std::string spvMessages = logger.getAllMessages();
        if (!spvMessages.empty()) {
            std::cerr << "SPIR-V generation: " << spvMessages << std::endl;
        }

        if (spirv.empty()) {
            std::cerr << "SPIR-V generation produced no code" << std::endl;
            return false;
        }

        return true;
    }

private:

    std::mutex m_compileMutex;
};

}  // anonymous namespace

VulkanShaderCompilerBackend* VulkanShaderCompilerBackend::Create()
{
    // Process-global; must be balanced by FinalizeProcess() in Destroy().
    if (!glslang::InitializeProcess()) {
        std::cerr << "VulkanShaderCompiler: glslang::InitializeProcess() failed!" << std::endl;
        return nullptr;
    }

    return new GlslangCompilerBackend();
}

void VulkanShaderCompilerBackend::Destroy(VulkanShaderCompilerBackend* backend)
{
    delete backend;
    glslang::FinalizeProcess();
}
