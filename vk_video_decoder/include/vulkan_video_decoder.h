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
    // Access the decoder's Vulkan handles (for external memory export, etc.)
    virtual VkDevice         GetDevice()         const = 0;
    virtual VkPhysicalDevice GetPhysicalDevice() const = 0;
    virtual VkInstance       GetInstance()        const = 0;

    // External consumer management (cross-process semaphore sync)
    // Register an imported release semaphore from an external consumer.
    // The frame buffer will CPU-wait on this before reusing a decoded frame slot.
    // Returns consumer index (>=0) on success, -1 on failure.
    virtual int32_t AddExternalConsumer(VkSemaphore importedReleaseSemaphore,
                                        uint64_t consumerType) = 0;

    // Export the frame-complete timeline semaphore as an opaque FD.
    // External consumers wait on this before reading decoded frames.
    virtual int ExportFrameCompleteSemaphoreFd() = 0;

    virtual ~VulkanVideoDecoder() {};
};

class VkWsiDisplay;
class VulkanDeviceContext;
/**
 * @brief Creates an instance of the Vulkan video decoder, returning a reference-counted
 *        VulkanVideoDecoder interface.
 *
 * This function instantiates a video decoder and hands back a reference-counted interface
 * (`VulkanVideoDecoder`) via the `vulkanVideoDecoder` parameter. The video decoder uses Vulkan
 * for video processing.
 *
 * The caller may optionally provide a fully initialized VulkanDeviceContext via `pVkDevCtxt`.
 * When provided (non-null), the decoder library uses the caller's device context directly —
 * sharing the Vulkan loader dispatch table, device, queues, and all state. This avoids creating
 * a second Vulkan loader instance and the dispatch table mismatch that results from passing raw
 * handles created through a different loader.
 *
 * When `pVkDevCtxt` is null, the library creates its own VulkanDeviceContext internally,
 * optionally seeded with the provided `vkInstance`, `vkPhysicalDevice`, and `vkDevice` handles.
 *
 * @param[in]  pVkDevCtxt         Optional pointer to a caller-owned VulkanDeviceContext. If
 *                                non-null, the library uses this context directly and the raw
 *                                handle parameters (vkInstance, vkPhysicalDevice, vkDevice) are
 *                                ignored. The caller must keep this context alive for the
 *                                lifetime of the decoder.
 * @param[in]  vkInstance         Optional Vulkan instance handle (ignored if pVkDevCtxt is set).
 *                                Pass `VK_NULL_HANDLE` if not required.
 * @param[in]  vkPhysicalDevice   Optional Vulkan physical device handle (ignored if pVkDevCtxt
 *                                is set). Pass `VK_NULL_HANDLE` if not required.
 * @param[in]  vkDevice           Optional Vulkan device handle (ignored if pVkDevCtxt is set).
 *                                Pass `VK_NULL_HANDLE` if not required.
 * @param[in]  videoStreamDemuxer A stream processor that abstracts elementary streams or container
 *                                formats (e.g., MPEG, Matroska). This object will be used to
 *                                feed data into the decoder.
 * @param[in]  frameToFile        Optional frame output handler for writing decoded frames to file.
 * @param[in]  pWsiDisplay        The display device context, if display is required, otherwise a nullptr.
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
 */
VK_VIDEO_DECODER_EXPORT
VkResult CreateVulkanVideoDecoder(VulkanDeviceContext* pVkDevCtxt,
                                  VkInstance vkInstance, VkPhysicalDevice vkPhysicalDevice, VkDevice vkDevice,
                                  VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer,
                                  VkSharedBaseObj<VkVideoFrameOutput>& frameToFile,
                                  const VkWsiDisplay* pWsiDisplay,
                                  int argc, const char** argv,
                                  VkSharedBaseObj<VulkanVideoDecoder>& vulkanVideoDecoder);

#endif /* _VULKAN_VIDEO_DECODER_H_ */
