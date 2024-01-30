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

#ifndef LIBS_VKCODECUTILS_FRAMEPROCESSORFACTORY_H_
#define LIBS_VKCODECUTILS_FRAMEPROCESSORFACTORY_H_

#include <string>
#include <vector>
#include "VkCodecUtils/ProgramConfig.h"
#include "VkCodecUtils/VkVideoRefCountBase.h"

class FrameProcessor;
class VulkanDeviceContext;
class VulkanVideoProcessor;

VkResult CreateFrameProcessor(const ProgramConfig& programConfig,
                              const VulkanDeviceContext* vkDevCtx,
                              VkSharedBaseObj<VulkanVideoProcessor>& videoProcessor,
                              VkSharedBaseObj<FrameProcessor>& frameProcessor);

#endif /* LIBS_VKCODECUTILS_FRAMEPROCESSORFACTORY_H_ */
