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

#ifndef _VULKAN_VIDEO_DECODER_H_
#define _VULKAN_VIDEO_DECODER_H_

// VK_VIDEO_DECODER_EXPORT tags symbol that will be exposed by the shared library.
#if defined(VK_VIDEO_DECODER_SHAREDLIB)
#if defined(_WIN32)
#if defined(VK_VIDEO_DECODER_IMPLEMENTATION)
#define VK_VIDEO_DECODER_EXPORT __declspec(dllexport)
#else
#define VK_VIDEO_DECODER_EXPORT __declspec(dllimport)
#endif
#else
#if defined(VK_VIDEO_DECODER_IMPLEMENTATION)
#define VK_VIDEO_DECODER_EXPORT __attribute__((visibility("default")))
#else
#define VK_VIDEO_DECODER_EXPORT
#endif
#endif
#else
#define VK_VIDEO_DECODER_EXPORT
#endif

#include "vulkan_interfaces.h"
#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkCodecUtils/VkVideoQueue.h"
#include "VkCodecUtils/VulkanDecodedFrame.h"

// High-level interface of the video encoder
class VulkanVideoDecoder : public VkVideoQueue<VulkanDecodedFrame> {
public:
    virtual VkResult Initialize(VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
                                int argc, const char** argv) = 0;
    virtual int64_t  GetMaxNumberOfFrames() const = 0;
    virtual VkVideoProfileInfoKHR GetVkProfile() const  = 0;
    virtual uint32_t GetProfileIdc() const = 0;
    virtual VkExtent3D GetVideoExtent() const = 0;
    virtual int32_t  ParserProcessNextDataChunk() = 0;
    virtual uint32_t RestartStream(int64_t& bitstreamOffset) = 0;
    virtual size_t   OutputFrameToFile(VulkanDecodedFrame* pNewDecodedFrame) = 0;
};

extern "C" VK_VIDEO_DECODER_EXPORT
VkResult CreateVulkanVideoDecoder(VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
                                  int argc, const char** argv,
                                  VkSharedBaseObj<VulkanVideoDecoder>& vulkanVideoDecoder);

#endif /* _VULKAN_VIDEO_DECODER_H_ */
