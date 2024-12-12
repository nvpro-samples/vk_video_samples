/*
 * Copyright (C) 2016 Google, Inc.
 * Copyright 2020 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/ProgramConfig.h"
#include "VkCodecUtils/VulkanVideoProcessor.h"
#include "VkCodecUtils/VulkanDecoderFrameProcessor.h"
#include "VkShell/Shell.h"

int main(int argc, const char **argv) {

    ProgramConfig programConfig(argv[0]);
    programConfig.ParseArgs(argc, argv);

    // In the regular application usecase the CRC output variables are allocated here and also output as part of main.
    // In the library case it is up to the caller of the library to allocate the values and initialize them.
    std::vector<uint32_t> crcAllocation;
    crcAllocation.resize(programConfig.crcInitValue.size());
    if (crcAllocation.empty() == false) {
        programConfig.crcOutput = &crcAllocation[0];
        for (size_t i = 0; i < programConfig.crcInitValue.size(); i += 1) {
            crcAllocation[i] = programConfig.crcInitValue[i];
        }
    }

    static const char* const requiredInstanceLayers[] = {
        "VK_LAYER_KHRONOS_validation",
        nullptr
    };

    static const char* const requiredInstanceExtensions[] = {
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
        nullptr
    };

    static const char* const requiredWsiInstanceExtensions[] = {
        // Required generic WSI extensions
        VK_KHR_SURFACE_EXTENSION_NAME,
        nullptr
    };

    static const char* const requiredDeviceExtension[] = {
#if defined(__linux) || defined(__linux__) || defined(linux)
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
#endif
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        nullptr
    };

    static const char* const requiredWsiDeviceExtension[] = {
        // Add the WSI required device extensions
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
        // VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME,
#endif
        nullptr
    };

    static const char* const optinalDeviceExtension[] = {
        VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        nullptr
    };

    VulkanDeviceContext vkDevCtxt;

    if (programConfig.validate) {
        vkDevCtxt.AddReqInstanceLayers(requiredInstanceLayers);
        vkDevCtxt.AddReqInstanceExtensions(requiredInstanceExtensions);
    }

    // Add the Vulkan video required device extensions
    vkDevCtxt.AddReqDeviceExtensions(requiredDeviceExtension);
    vkDevCtxt.AddOptDeviceExtensions(optinalDeviceExtension);

    /********** Start WSI instance extensions support *******************************************/
    if (!programConfig.noPresent) {
        const std::vector<VkExtensionProperties>& wsiRequiredInstanceInstanceExtensions =
                Shell::GetRequiredInstanceExtensions(programConfig.directMode);

        for (size_t e = 0; e < wsiRequiredInstanceInstanceExtensions.size(); e++) {
            vkDevCtxt.AddReqInstanceExtension(wsiRequiredInstanceInstanceExtensions[e].extensionName);
        }

        // Add the WSI required instance extensions
        vkDevCtxt.AddReqInstanceExtensions(requiredWsiInstanceExtensions);

        // Add the WSI required device extensions
        vkDevCtxt.AddReqDeviceExtensions(requiredWsiDeviceExtension);
    }
    /********** End WSI instance extensions support *******************************************/

    VkResult result = vkDevCtxt.InitVulkanDevice(programConfig.appName.c_str(),
                                                 programConfig.verbose);
    if (result != VK_SUCCESS) {
        printf("Could not initialize the Vulkan device!\n");
        return -1;
    }

    result = vkDevCtxt.InitDebugReport(programConfig.validate,
                                       programConfig.validateVerbose);
    if (result != VK_SUCCESS) {
        return -1;
    }

    const bool supportsDisplay = true;
    const int32_t numDecodeQueues = ((programConfig.queueId != 0) ||
                                     (programConfig.enableHwLoadBalancing != 0)) ?
                                     -1 : // all available HW decoders
                                      1;  // only one HW decoder instance

    VkQueueFlags requestVideoDecodeQueueMask = VK_QUEUE_VIDEO_DECODE_BIT_KHR;

    VkQueueFlags requestVideoEncodeQueueMask = 0;
    if (programConfig.enableVideoEncoder) {
        requestVideoEncodeQueueMask |= VK_QUEUE_VIDEO_ENCODE_BIT_KHR;
    }

    if (programConfig.selectVideoWithComputeQueue) {
        requestVideoDecodeQueueMask |= VK_QUEUE_COMPUTE_BIT;
        if (programConfig.enableVideoEncoder) {
            requestVideoEncodeQueueMask |= VK_QUEUE_COMPUTE_BIT;
        }
    }

    VkQueueFlags requestVideoComputeQueueMask = 0;
    if (programConfig.enablePostProcessFilter != -1) {
        requestVideoComputeQueueMask = VK_QUEUE_COMPUTE_BIT;
    }

    VkSharedBaseObj<VulkanVideoProcessor> vulkanVideoProcessor;
    result = VulkanVideoProcessor::Create(programConfig, &vkDevCtxt, vulkanVideoProcessor);
    if (result != VK_SUCCESS) {
        return -1;
    }

    VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>> videoQueue(vulkanVideoProcessor);
    VkSharedBaseObj<FrameProcessor> frameProcessor;
    result = CreateDecoderFrameProcessor(&vkDevCtxt, videoQueue, frameProcessor);
    if (result != VK_SUCCESS) {
        return -1;
    }

    VkVideoCodecOperationFlagsKHR videoDecodeCodecs = (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR  |
                                                       VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR  |
                                                       VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR);

    VkVideoCodecOperationFlagsKHR videoEncodeCodecs = ( VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR  |
                                                        VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR  |
                                                        VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR);

    VkVideoCodecOperationFlagsKHR videoCodecs = videoDecodeCodecs |
                                        (programConfig.enableVideoEncoder ? videoEncodeCodecs : (VkVideoCodecOperationFlagsKHR) VK_VIDEO_CODEC_OPERATION_NONE_KHR);


    if (supportsDisplay && !programConfig.noPresent) {

        const Shell::Configuration configuration(programConfig.appName.c_str(),
                                                 programConfig.backBufferCount,
                                                 programConfig.directMode);
        VkSharedBaseObj<Shell> displayShell;
        result = Shell::Create(&vkDevCtxt, configuration, frameProcessor, displayShell);
        if (result != VK_SUCCESS) {
            assert(!"Can't allocate display shell! Out of memory!");
            return -1;
        }

        result = vkDevCtxt.InitPhysicalDevice(programConfig.deviceId, programConfig.GetDeviceUUID(),
                                              (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT |
                                              requestVideoComputeQueueMask |
                                              requestVideoDecodeQueueMask |
                                              requestVideoEncodeQueueMask),
                                              displayShell,
                                              requestVideoDecodeQueueMask,
                                              (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
                                               VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR |
                                               VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR),
                                              requestVideoEncodeQueueMask,
                                              (VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR |
                                               VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR |
                                               VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR));
        if (result != VK_SUCCESS) {

            assert(!"Can't initialize the Vulkan physical device!");
            return -1;
        }
        assert(displayShell->PhysDeviceCanPresent(vkDevCtxt.getPhysicalDevice(),
                                                   vkDevCtxt.GetPresentQueueFamilyIdx()));

        vkDevCtxt.CreateVulkanDevice(numDecodeQueues,
                                     programConfig.enableVideoEncoder ? 1 : 0, // num encode queues
                                     videoCodecs,
                                     false, //  createTransferQueue
                                     true,  // createGraphicsQueue
                                     true,  // createDisplayQueue
                                     requestVideoComputeQueueMask != 0  // createComputeQueue
                                     );
        vulkanVideoProcessor->Initialize(&vkDevCtxt, programConfig);


        displayShell->RunLoop();

    } else {

        result = vkDevCtxt.InitPhysicalDevice(programConfig.deviceId, programConfig.GetDeviceUUID(),
                                              (VK_QUEUE_TRANSFER_BIT        |
                                               requestVideoDecodeQueueMask  |
                                               requestVideoComputeQueueMask |
                                               requestVideoEncodeQueueMask),
                                              nullptr,
                                              requestVideoDecodeQueueMask);
        if (result != VK_SUCCESS) {

            assert(!"Can't initialize the Vulkan physical device!");
            return -1;
        }


        result = vkDevCtxt.CreateVulkanDevice(numDecodeQueues,
                                              0,     // num encode queues
                                              videoCodecs,
                                              // If no graphics or compute queue is requested, only video queues
                                              // will be created. Not all implementations support transfer on video queues,
                                              // so request a separate transfer queue for such implementations.
                                              ((vkDevCtxt.GetVideoDecodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) == 0), //  createTransferQueue
                                              false, // createGraphicsQueue
                                              false, // createDisplayQueue
                                              requestVideoComputeQueueMask != 0   // createComputeQueue
                                              );
        if (result != VK_SUCCESS) {

            assert(!"Failed to create Vulkan device!");
            return -1;
        }

        vulkanVideoProcessor->Initialize(&vkDevCtxt, programConfig);

        const int numberOfFrames = programConfig.decoderQueueSize;
        int ret = frameProcessor->CreateFrameData(numberOfFrames);
        assert(ret == numberOfFrames);
        if (ret != numberOfFrames) {
            return -1;
        }
        bool continueLoop = true;
        do {
            continueLoop = frameProcessor->OnFrame(0);
        } while (continueLoop);
        frameProcessor->DestroyFrameData();
    }

    if (programConfig.outputcrc != 0) {
        fprintf(programConfig.crcOutputFile, "CRC: ");
        for (size_t i = 0; i < programConfig.crcInitValue.size(); i += 1) {
            fprintf(programConfig.crcOutputFile, "0x%08X ", crcAllocation[i]);
        }

        fprintf(programConfig.crcOutputFile, "\n");
        if (programConfig.crcOutputFile != stdout) {
            fclose(programConfig.crcOutputFile);
            programConfig.crcOutputFile = stdout;
        }
    }

    return 0;
}
