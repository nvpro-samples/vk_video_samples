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

#ifndef LIBS_VKCODECUTILS_VULKANSHADERCOMPILER_H_
#define LIBS_VKCODECUTILS_VULKANSHADERCOMPILER_H_

#include <vulkan_interfaces.h>

namespace vulkanVideoUtils {

class VulkanShaderCompiler {

public:

    VulkanShaderCompiler();
    ~VulkanShaderCompiler();

    VkResult BuildGlslShader(const char *shaderCode, size_t shaderSize, VkShaderStageFlagBits type,
                                 VkDevice vkDevice, VkShaderModule *shaderOut);

    // Create VK shader module from given glsl shader file
    VkResult BuildShaderFromFile(const char *filePath, VkShaderStageFlagBits type,
                                 VkDevice vkDevice, VkShaderModule *shaderOut);

private:
    void* compilerHandle;
};

} // namespace vulkanVideoUtils


#endif /* LIBS_VKCODECUTILS_VULKANSHADERCOMPILER_H_ */
