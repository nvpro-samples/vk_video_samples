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

#include <iostream>
#include "vulkan_video_decoder.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkCodecUtils/ProgramConfig.h"
#include "VkCodecUtils/VulkanFrame.h"
#include "VkCodecUtils/VulkanDecoderFrameProcessor.h"
#include "VkShell/Shell.h"

int main(int argc, const char** argv)
{
    std::cout << "Enter decoder test" << std::endl;

    ProgramConfig decoderConfig(argv[0]);
    decoderConfig.ParseArgs(argc, argv);

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

    if (decoderConfig.validate) {
        vkDevCtxt.AddReqInstanceLayers(requiredInstanceLayers);
        vkDevCtxt.AddReqInstanceExtensions(requiredInstanceExtensions);
    }

    // Add the Vulkan video required device extensions
    vkDevCtxt.AddReqDeviceExtensions(requiredDeviceExtension);
    vkDevCtxt.AddOptDeviceExtensions(optinalDeviceExtension);

    /********** Start WSI instance extensions support *******************************************/
    if (!decoderConfig.noPresent) {
        const std::vector<VkExtensionProperties>& wsiRequiredInstanceInstanceExtensions =
                Shell::GetRequiredInstanceExtensions(decoderConfig.directMode);

        for (size_t e = 0; e < wsiRequiredInstanceInstanceExtensions.size(); e++) {
            vkDevCtxt.AddReqInstanceExtension(wsiRequiredInstanceInstanceExtensions[e].extensionName);
        }

        // Add the WSI required instance extensions
        vkDevCtxt.AddReqInstanceExtensions(requiredWsiInstanceExtensions);

        // Add the WSI required device extensions
        vkDevCtxt.AddReqDeviceExtensions(requiredWsiDeviceExtension);
    }
    /********** End WSI instance extensions support *******************************************/

    VkResult result = vkDevCtxt.InitVulkanDevice(decoderConfig.appName.c_str(),
                                                 decoderConfig.verbose);
    if (result != VK_SUCCESS) {
        printf("Could not initialize the Vulkan device!\n");
        return result;
    }

    result = vkDevCtxt.InitDebugReport(decoderConfig.validate,
                                       decoderConfig.validateVerbose);
    if (result != VK_SUCCESS) {
        return result;
    }

    const int32_t numDecodeQueues = ((decoderConfig.queueId != 0) ||
                                     (decoderConfig.enableHwLoadBalancing != 0)) ?
                                     -1 : // all available HW decoders
                                      1;  // only one HW decoder instance

    VkQueueFlags requestVideoDecodeQueueMask = VK_QUEUE_VIDEO_DECODE_BIT_KHR;

    VkQueueFlags requestVideoEncodeQueueMask = 0;
    if (decoderConfig.enableVideoEncoder) {
        requestVideoEncodeQueueMask |= VK_QUEUE_VIDEO_ENCODE_BIT_KHR;
    }

    if (decoderConfig.selectVideoWithComputeQueue) {
        requestVideoDecodeQueueMask |= VK_QUEUE_COMPUTE_BIT;
        if (decoderConfig.enableVideoEncoder) {
            requestVideoEncodeQueueMask |= VK_QUEUE_COMPUTE_BIT;
        }
    }

    VkQueueFlags requestVideoComputeQueueMask = 0;
    if (decoderConfig.enablePostProcessFilter != -1) {
        requestVideoComputeQueueMask = VK_QUEUE_COMPUTE_BIT;
    }

    VkVideoCodecOperationFlagsKHR videoDecodeCodecs = (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR  |
                                                       VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR  |
                                                       VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR);

    VkVideoCodecOperationFlagsKHR videoEncodeCodecs = ( VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR  |
                                                        VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR  |
                                                        VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR);

    VkVideoCodecOperationFlagsKHR videoCodecs = videoDecodeCodecs |
                                        (decoderConfig.enableVideoEncoder ? videoEncodeCodecs : (VkVideoCodecOperationFlagsKHR) VK_VIDEO_CODEC_OPERATION_NONE_KHR);


    VkSharedBaseObj<VulkanVideoDecoder> vulkanVideoDecoder;
    result = CreateVulkanVideoDecoder(decoderConfig.forceParserType,
                                      argc, argv, vulkanVideoDecoder);

    if (result != VK_SUCCESS) {
        std::cerr << "Error creating the decoder instance: " << result << std::endl;
    }

    VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>> videoQueue(vulkanVideoDecoder);

    DecoderFrameProcessorState frameProcessor(&vkDevCtxt, videoQueue,
                                              decoderConfig.noPresent ? decoderConfig.decoderQueueSize : 0);

    if (!decoderConfig.noPresent) {

        const Shell::Configuration configuration(decoderConfig.appName.c_str(),
                                                 decoderConfig.backBufferCount,
                                                 decoderConfig.directMode);
        VkSharedBaseObj<Shell> displayShell;
        result = Shell::Create(&vkDevCtxt, configuration, frameProcessor, displayShell);
        if (result != VK_SUCCESS) {
            assert(!"Can't allocate display shell! Out of memory!");
            return result;
        }

        result = vkDevCtxt.InitPhysicalDevice(decoderConfig.deviceId, decoderConfig.GetDeviceUUID(),
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
            return result;
        }
        assert(displayShell->PhysDeviceCanPresent(vkDevCtxt.getPhysicalDevice(),
                                                  vkDevCtxt.GetPresentQueueFamilyIdx()));

        vkDevCtxt.CreateVulkanDevice(numDecodeQueues,
                                       decoderConfig.enableVideoEncoder ? 1 : 0, // num encode queues
                                       videoCodecs,
                                       false, //  createTransferQueue
                                       true,  // createGraphicsQueue
                                       true,  // createDisplayQueue
                                       requestVideoComputeQueueMask != 0  // createComputeQueue
                                       );

        displayShell->RunLoop();

    } else {

        result = vkDevCtxt.InitPhysicalDevice(decoderConfig.deviceId, decoderConfig.GetDeviceUUID(),
                                              (VK_QUEUE_TRANSFER_BIT        |
                                               requestVideoDecodeQueueMask  |
                                               requestVideoComputeQueueMask |
                                               requestVideoEncodeQueueMask),
                                              nullptr,
                                              requestVideoDecodeQueueMask);
        if (result != VK_SUCCESS) {

            assert(!"Can't initialize the Vulkan physical device!");
            return result;
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
            return result;
        }

        VkSharedBaseObj<FrameProcessor> decodeFrameProcessor(frameProcessor);
        bool continueLoop = true;
        do {
            continueLoop = decodeFrameProcessor->OnFrame(0);
        } while (continueLoop);
    }

    /*******************************************************************************************/

    int64_t numFrames = vulkanVideoDecoder->GetMaxNumberOfFrames();
    if (numFrames > 0) {
        std::cout << "Max number of frames to decode: " << numFrames << std::endl;
    }

    const VkVideoProfileInfoKHR videoProfileInfo = vulkanVideoDecoder->GetVkProfile();

    const VkExtent3D extent = vulkanVideoDecoder->GetVideoExtent();

    std::cout << "Test Video Input Information" << std::endl
               << "\tCodec        : " << VkVideoCoreProfile::CodecToName(videoProfileInfo.videoCodecOperation) << std::endl
               << "\tCoded size   : [" << extent.width << ", " << extent.height << "]" << std::endl
               << "\tChroma Subsampling:";
    VkVideoCoreProfile::DumpFormatProfiles(&videoProfileInfo);
    std::cout << std::endl;

    for (int64_t frameNum = 0; frameNum < numFrames; frameNum++) {
        int64_t frameNumDecoded = -1;
        result = VK_SUCCESS; // vulkanVideoDecoder->GetNextFrame(frameNumDecoded);
        if (result != VK_SUCCESS) {
            std::cerr << "Error encoding frame: "  << frameNum  << ", error: " << result << std::endl;
        }
    }

    std::cout << "Exit decoder test" << std::endl;
}


