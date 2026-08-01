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

#ifndef LIBS_VKCODECUTILS_VULKANSHADERCOMPILERBACKEND_H_
#define LIBS_VKCODECUTILS_VULKANSHADERCOMPILERBACKEND_H_

#include <vector>
#include <vulkan_interfaces.h>

// GLSL-to-SPIR-V compiler backend.
//
// VulkanShaderCompiler owns one of these and knows nothing about which
// compiler library is underneath. Exactly one backend implementation is
// compiled into a build - it is the translation unit that defines Create()
// and Destroy() - so the choice is made by the build system, not at runtime,
// and only the selected compiler's library needs to be linked.
//
// Implementations:
//   VulkanShaderCompilerGlslang.cpp  glslang (default)
//   VulkanShaderCompilerShaderc.cpp  shaderc (opt-in; a wrapper over glslang)
class VulkanShaderCompilerBackend {

public:

    virtual ~VulkanShaderCompilerBackend() { }

    // Name of the underlying compiler, for diagnostics.
    virtual const char* GetBackendName() const = 0;

    // Compile GLSL to SPIR-V. Returns false and reports the compiler's
    // diagnostics on failure; on success spirv holds the module as 32-bit
    // words (not bytes).
    virtual bool CompileGlslToSpirv(const char* shaderCode, size_t shaderSize,
                                    VkShaderStageFlagBits type,
                                    std::vector<uint32_t>& spirv) = 0;

    // Backend lifetime. Both compilers keep process-global state, so these are
    // refcounted by VulkanShaderCompiler and the instance is shared.
    // Create() returns nullptr if the compiler could not be initialized.
    static VulkanShaderCompilerBackend* Create();
    static void Destroy(VulkanShaderCompilerBackend* backend);
};

#endif /* LIBS_VKCODECUTILS_VULKANSHADERCOMPILERBACKEND_H_ */
