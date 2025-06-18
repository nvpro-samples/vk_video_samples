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

#include "VkCodecUtils/DecoderConfig.h"
#include "vulkan_video_decoder.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkCodecUtils/VulkanFrame.h"
#include "VkCodecUtils/VulkanDecoderFrameProcessor.h"
#include "VkDecoderUtils/VideoStreamDemuxer.h"
#include "VkShell/Shell.h"

static void DumpDecoderStreamInfo(VkSharedBaseObj<VulkanVideoDecoder>& vulkanVideoDecoder)
{
    const VkVideoProfileInfoKHR videoProfileInfo = vulkanVideoDecoder->GetVkProfile();

    const VkExtent3D extent = vulkanVideoDecoder->GetVideoExtent();

    std::cout << "Test Video Input Information" << std::endl
               << "\tCodec        : " << VkVideoCoreProfile::CodecToName(videoProfileInfo.videoCodecOperation) << std::endl
               << "\tCoded size   : [" << extent.width << ", " << extent.height << "]" << std::endl
               << "\tChroma Subsampling:";

    VkVideoCoreProfile::DumpFormatProfiles(&videoProfileInfo);
    std::cout << std::endl;
}

int main(int argc, const char** argv)
{
    std::cout << "Enter decoder test" << std::endl;

    DecoderConfig decoderConfig(argv[0]);
    bool configResult = decoderConfig.ParseArgs(argc, argv);
    if (!configResult && (decoderConfig.help == true)) {
        return 0;
    } else if (!configResult) {
        return -1;
    }

    VulkanDeviceContext vkDevCtxt;
    VkResult result = vkDevCtxt.InitVulkanDecoderDevice(decoderConfig.appName.c_str(),
                                                        VK_NULL_HANDLE,
                                                        !decoderConfig.noPresent,
                                                        decoderConfig.directMode,
                                                        decoderConfig.validate,
                                                        decoderConfig.validateVerbose,
                                                        decoderConfig.verbose);

    if (result != VK_SUCCESS) {
        printf("Could not initialize the Vulkan decoder device!\n");
        return -1;
    }


    VkSharedBaseObj<VideoStreamDemuxer> videoStreamDemuxer;
    result = VideoStreamDemuxer::Create(decoderConfig.videoFileName.c_str(),
                                        decoderConfig.forceParserType,
                                        decoderConfig.enableStreamDemuxing,
                                        decoderConfig.initialWidth,
                                        decoderConfig.initialHeight,
                                        decoderConfig.initialBitdepth,
                                        videoStreamDemuxer);
    if (result != VK_SUCCESS) {
        assert(!"Can't initialize the VideoStreamDemuxer!");
        return result;
    }

    const int32_t numDecodeQueues = ((decoderConfig.queueId != 0) ||
                                     (decoderConfig.enableHwLoadBalancing != 0)) ?
                                     -1 : // all available HW decoders
                                      1;  // only one HW decoder instance

    VkQueueFlags requestVideoDecodeQueueMask = VK_QUEUE_VIDEO_DECODE_BIT_KHR;

    if (decoderConfig.selectVideoWithComputeQueue) {
        requestVideoDecodeQueueMask |= VK_QUEUE_COMPUTE_BIT;
    }

    VkQueueFlags requestVideoComputeQueueMask = 0;
    if (decoderConfig.enablePostProcessFilter != -1) {
        requestVideoComputeQueueMask = VK_QUEUE_COMPUTE_BIT;
    }

    VkVideoCodecOperationFlagsKHR videoCodec = decoderConfig.forceParserType != VK_VIDEO_CODEC_OPERATION_NONE_KHR ? 
                                                        decoderConfig.forceParserType :
                                                        videoStreamDemuxer->GetVideoCodec();

    if (!decoderConfig.noPresent) {

        VkSharedBaseObj<Shell> displayShell;
        const Shell::Configuration configuration(decoderConfig.appName.c_str(),
                                                 decoderConfig.backBufferCount,
                                                 decoderConfig.directMode);

        result = Shell::Create(&vkDevCtxt, configuration, displayShell);
        if (result != VK_SUCCESS) {
            assert(!"Can't allocate display shell! Out of memory!");
            return -1;
        }

        result = vkDevCtxt.InitPhysicalDevice(decoderConfig.deviceId, decoderConfig.deviceUUID,
                                              (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT |
                                              requestVideoComputeQueueMask |
                                              requestVideoDecodeQueueMask),
                                              displayShell,
                                              requestVideoDecodeQueueMask,
                                              videoCodec,
                                              0,
                                              VK_VIDEO_CODEC_OPERATION_NONE_KHR);
        if (result != VK_SUCCESS) {
            assert(!"Can't initialize the Vulkan physical device!");
            return -1;
        }
        assert(displayShell->PhysDeviceCanPresent(vkDevCtxt.getPhysicalDevice(),
                                                  vkDevCtxt.GetPresentQueueFamilyIdx()));

        vkDevCtxt.CreateVulkanDevice(numDecodeQueues,
                                     0, // num encode queues
                                     videoCodec,
                                     false, //  createTransferQueue
                                     true,  // createGraphicsQueue
                                     true,  // createDisplayQueue
                                     requestVideoComputeQueueMask != 0  // createComputeQueue
                                     );


        VkSharedBaseObj<VkVideoFrameOutput> frameToFile;
        if (!decoderConfig.outputFileName.empty()) {
            const char* crcOutputFile = decoderConfig.outputcrcPerFrame ? decoderConfig.crcOutputFileName.c_str() : nullptr;
            result = VkVideoFrameOutput::Create(decoderConfig.outputFileName.c_str(),
                                              decoderConfig.outputy4m,
                                              decoderConfig.outputcrcPerFrame,
                                              crcOutputFile,
                                              decoderConfig.crcInitValue,
                                              frameToFile);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "Error creating output file %s\n", decoderConfig.outputFileName.c_str());
                return -1;
            }
        }

        VkSharedBaseObj<VulkanVideoDecoder> vulkanVideoDecoder;
        result = CreateVulkanVideoDecoder(vkDevCtxt.getInstance(),
                                        vkDevCtxt.getPhysicalDevice(),
                                        vkDevCtxt.getDevice(),
                                        videoStreamDemuxer,
                                        frameToFile,
                                        displayShell,
                                        argc, argv,
                                        vulkanVideoDecoder);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "Error creating video decoder\n");
            return -1;
        }

        DumpDecoderStreamInfo(vulkanVideoDecoder);

        VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>> videoQueue(vulkanVideoDecoder);
        DecoderFrameProcessorState frameProcessor(&vkDevCtxt, videoQueue, 0);

        displayShell->AttachFrameProcessor(frameProcessor);
        displayShell->RunLoop();

    } else {

        result = vkDevCtxt.InitPhysicalDevice(decoderConfig.deviceId, decoderConfig.deviceUUID,
                                              (VK_QUEUE_TRANSFER_BIT        |
                                               requestVideoDecodeQueueMask  |
                                               requestVideoComputeQueueMask),
                                              nullptr,
                                              requestVideoDecodeQueueMask);
        if (result != VK_SUCCESS) {
            assert(!"Can't initialize the Vulkan physical device!");
            return -1;
        }

        result = vkDevCtxt.CreateVulkanDevice(numDecodeQueues,
                                              0,     // num encode queues
                                              videoCodec,
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

        VkSharedBaseObj<VkVideoFrameOutput> frameToFile;
        if (!decoderConfig.outputFileName.empty()) {
            const char* crcOutputFile = decoderConfig.outputcrcPerFrame ? decoderConfig.crcOutputFileName.c_str() : nullptr;
            result = VkVideoFrameOutput::Create(decoderConfig.outputFileName.c_str(),
                                              decoderConfig.outputy4m,
                                              decoderConfig.outputcrcPerFrame,
                                              crcOutputFile,
                                              decoderConfig.crcInitValue,
                                              frameToFile);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "Error creating output file %s\n", decoderConfig.outputFileName.c_str());
                return -1;
            }
        }

        VkSharedBaseObj<VulkanVideoDecoder> vulkanVideoDecoder;
        result = CreateVulkanVideoDecoder(vkDevCtxt.getInstance(),
                                        vkDevCtxt.getPhysicalDevice(),
                                        vkDevCtxt.getDevice(),
                                        videoStreamDemuxer,
                                        frameToFile,
                                        nullptr,
                                        argc, argv,
                                        vulkanVideoDecoder);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "Error creating video decoder\n");
            return -1;
        }

        DumpDecoderStreamInfo(vulkanVideoDecoder);

        VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>> videoQueue(vulkanVideoDecoder);
        DecoderFrameProcessorState frameProcessor(&vkDevCtxt, videoQueue, decoderConfig.decoderQueueSize);

        bool continueLoop = true;
        do {
            continueLoop = frameProcessor->OnFrame(0);
        } while (continueLoop);
    }

    /*******************************************************************************************/

    std::cout << "Exit decoder test" << std::endl;
}


