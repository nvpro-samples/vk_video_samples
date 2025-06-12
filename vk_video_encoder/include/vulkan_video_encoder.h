/*
 * Copyright 2024 NVIDIA Corporation.
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

#ifndef _VULKAN_VIDEO_ENCODER_H_
#define _VULKAN_VIDEO_ENCODER_H_

// VK_VIDEO_ENCODER_EXPORT tags symbol that will be exposed by the shared library.
#if defined(VK_VIDEO_ENCODER_SHAREDLIB)
#if defined(_WIN32)
#if defined(VK_VIDEO_ENCODER_IMPLEMENTATION)
#define VK_VIDEO_ENCODER_EXPORT __declspec(dllexport)
#else
#define VK_VIDEO_ENCODER_EXPORT __declspec(dllimport)
#endif
#else
#if defined(VK_VIDEO_ENCODER_IMPLEMENTATION)
#define VK_VIDEO_ENCODER_EXPORT __attribute__((visibility("default")))
#else
#define VK_VIDEO_ENCODER_EXPORT
#endif
#endif
#else
#define VK_VIDEO_ENCODER_EXPORT
#endif

#include "vulkan_interfaces.h"
#include "VkCodecUtils/VkVideoRefCountBase.h"

// High-level interface of the video encoder
class VulkanVideoEncoder : public virtual VkVideoRefCountBase {
public:
    virtual VkResult Initialize(VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
                                int argc, const char** argv) = 0;
    virtual int64_t  GetNumberOfFrames() = 0;
    virtual VkResult EncodeNextFrame(int64_t& frameNumEncoded) = 0;
    virtual VkResult GetBitstream() = 0;
};


extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult CreateVulkanVideoEncoder(VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
                                  int argc, const char** argv,
                                  VkSharedBaseObj<VulkanVideoEncoder>& vulkanVideoEncoder);

#endif /* _VULKAN_VIDEO_ENCODER_H_ */
