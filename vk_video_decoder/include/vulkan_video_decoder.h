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
#include "VkCodecUtils/VkVideoFrameOutput.h"
#include "VkDecoderUtils/VideoStreamDemuxer.h"

// High-level interface of the video decoder
class VulkanVideoDecoder : public VkVideoQueue<VulkanDecodedFrame> {
public:
    virtual ~VulkanVideoDecoder() {};
};

class VkWsiDisplay;
/**
 * @brief Creates an instance of the Vulkan video decoder, returning a reference-counted
 *        VulkanVideoDecoder interface.
 *
 * This function instantiates a video decoder and hands back a reference-counted interface
 * (`VulkanVideoDecoder`) via the `vulkanVideoDecoder` parameter. The video decoder uses Vulkan
 * for video processing. The client may optionally provide existing Vulkan handles for
 * `VkInstance`, `VkPhysicalDevice`, and `VkDevice` to share resources with other parts of the
 * application. If the client does not require sharing, any of these parameters can be passed as
 * `VK_NULL_HANDLE`.
 *
 * @param[in]  vkInstance         Optional Vulkan instance handle. If not required, pass
 *                                `VK_NULL_HANDLE`.
 * @param[in]  vkPhysicalDevice   Optional Vulkan physical device handle. If not required, pass
 *                                `VK_NULL_HANDLE`. If vkPhysicalDevice is not `VK_NULL_HANDLE` then
 *                                vkInstance must be a valid Vulkan instance handle.
 * @param[in]  vkDevice           Optional Vulkan device handle. If not required, pass
 *                                `VK_NULL_HANDLE`. If vkDevice is not `VK_NULL_HANDLE` then
 *                                vkPhysicalDevice must be a valid Vulkan physical device handle.
 * @param[in]  videoStreamDemuxer A stream processor that abstracts elementary streams or container
 *                                formats (e.g., MPEG, Matroska). This object will be used to
 *                                feed data into the decoder.
 * @param[in]  pWsiDisplay        The display device context, if display is required, otherwise a nullptr.
 *
 * @param[in]  argc               The number of configuration arguments passed for decoder setup.
 * @param[in]  argv               An array of null-terminated strings, containing the decoder
 *                                configuration options. All possible arguments are documented
 *                                in the @c DecoderConfig structure.
 * @param[out] vulkanVideoDecoder A smart pointer (reference-counted) that will receive the newly
 *                                created decoder interface on success.
 *
 * @return
 * - `VK_SUCCESS` on success.
 * - Appropriate Vulkan error codes if creation or initialization fails.
 *
 * @note If `vkInstance`, `vkPhysicalDevice`, or `vkDevice` are provided, they must remain valid
 *       for the duration of the decoder's lifetime if resources are shared with them.
 */
extern "C" VK_VIDEO_DECODER_EXPORT
VkResult CreateVulkanVideoDecoder(VkInstance vkInstance, VkPhysicalDevice vkPhysicalDevice, VkDevice vkDevice,
                                  VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer,
                                  VkSharedBaseObj<VkVideoFrameOutput>& frameToFile,
                                  const VkWsiDisplay* pWsiDisplay,
                                  int argc, const char** argv,
                                  VkSharedBaseObj<VulkanVideoDecoder>& vulkanVideoDecoder);

#endif /* _VULKAN_VIDEO_DECODER_H_ */
