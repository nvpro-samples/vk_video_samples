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
#include "VkCodecUtils/DecoderConfig.h"
#include "VkCodecUtils/VulkanVideoProcessor.h"
#include "VkCodecUtils/VulkanDecoderFrameProcessor.h"
#include "VkShell/Shell.h"

#if (_TRANSCODING)
#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkCodecUtils/VulkanVideoDisplayQueue.h"
#include "VkCodecUtils/VulkanVideoEncodeDisplayQueue.h"
#include "VkCodecUtils/VulkanEncoderFrameProcessor.h"
#endif

int main(int argc, const char **argv) {

    DecoderConfig decoderConfig(argv[0]);
    decoderConfig.ParseArgs(argc, argv);

    static const char* const requiredInstanceLayers[] = {
        "VK_LAYER_KHRONOS_validation",
        nullptr
    };

#if _TRANSCODING
    decoderConfig.enableVideoEncoder = 1;
    decoderConfig.noPresent = 1;
    VkSharedBaseObj<EncoderConfig> encoderConfig;
    if (VK_SUCCESS != EncoderConfig::CreateCodecConfig(argc, (char**)argv, encoderConfig)) {
        return -1;
    }
    if (encoderConfig->numEncoderResizedOutputs > 0) {
        decoderConfig.enablePostProcessFilter = 4;
    }
    const int32_t numEncodeQueues = 1;  // only one HW encoder instance
#endif //_TRANSCODING

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
#if _TRANSCODING
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
#endif
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

    VkResult result = vkDevCtxt.InitVulkanDevice(decoderConfig.appName.c_str(),  VK_NULL_HANDLE,
                                                 decoderConfig.verbose);
    if (result != VK_SUCCESS) {
        printf("Could not initialize the Vulkan device!\n");
        return -1;
    }

    result = vkDevCtxt.InitDebugReport(decoderConfig.validate,
                                       decoderConfig.validateVerbose);
    if (result != VK_SUCCESS) {
        return -1;
    }

    const bool supportsDisplay = true;
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

    VkSharedBaseObj<VulkanVideoProcessor> vulkanVideoProcessor;
    result = VulkanVideoProcessor::Create(decoderConfig, &vkDevCtxt, vulkanVideoProcessor);
    if (result != VK_SUCCESS) {
        return -1;
    }

    VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>> videoQueue(vulkanVideoProcessor);
    VkSharedBaseObj<FrameProcessor> frameProcessor;
    result = CreateDecoderFrameProcessor(&vkDevCtxt, frameProcessor);
    if (result != VK_SUCCESS) {
        return -1;
    }

    VkVideoCodecOperationFlagsKHR videoDecodeCodecs = (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR  |
                                                       VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR  |
                                                       VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR);

    VkVideoCodecOperationFlagsKHR videoCodecs = videoDecodeCodecs;

#if (_TRANSCODING)
    VkVideoCodecOperationFlagsKHR videoEncodeCodecs = ( VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR  |
                                                        VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR  |
                                                        VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR);
    videoCodecs |= videoEncodeCodecs;
#endif

    if (supportsDisplay && !decoderConfig.noPresent) {
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
                                     decoderConfig.enableVideoEncoder ? 1 : 0, // num encode queues
                                     videoCodecs,
                                     false, //  createTransferQueue
                                     true,  // createGraphicsQueue
                                     true,  // createDisplayQueue
                                     requestVideoComputeQueueMask != 0  // createComputeQueue
                                     );

        VkSharedBaseObj<VideoStreamDemuxer> videoStreamDemuxer;
        result = VideoStreamDemuxer::Create(decoderConfig.videoFileName.c_str(),
                                            decoderConfig.forceParserType,
                                            (decoderConfig.enableStreamDemuxing == 1),
                                            decoderConfig.initialWidth,
                                            decoderConfig.initialHeight,
                                            decoderConfig.initialBitdepth,
                                            videoStreamDemuxer);

        if (result != VK_SUCCESS) {

            assert(!"Can't initialize the VideoStreamDemuxer!");
            return result;
        }

        VkSharedBaseObj<VulkanVideoProcessor> vulkanVideoProcessor;
        result = VulkanVideoProcessor::Create(decoderConfig, &vkDevCtxt, vulkanVideoProcessor);
        if (result != VK_SUCCESS) {
            return -1;
        }

        VkSharedBaseObj<VkVideoFrameOutput> frameToFile;
#if (!_TRANSCODING)
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
#endif

        vulkanVideoProcessor->Initialize(&vkDevCtxt, videoStreamDemuxer, frameToFile, decoderConfig);

        displayShell->RunLoop();

    } else {

        result = vkDevCtxt.InitPhysicalDevice(decoderConfig.deviceId, decoderConfig.deviceUUID,
                                              (VK_QUEUE_TRANSFER_BIT | requestVideoDecodeQueueMask  |
                                              requestVideoComputeQueueMask |
                                              requestVideoEncodeQueueMask),
                                              nullptr,
                                              requestVideoDecodeQueueMask);
        if (result != VK_SUCCESS) {

            assert(!"Can't initialize the Vulkan physical device!");
            return -1;
        }


        result = vkDevCtxt.CreateVulkanDevice(numDecodeQueues,
#ifdef _TRANSCODING
                                              numEncodeQueues,
#else
                                              0,     // num encode queues
#endif //_TRANSCODING
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

        VkSharedBaseObj<VideoStreamDemuxer> videoStreamDemuxer;
        result = VideoStreamDemuxer::Create(decoderConfig.videoFileName.c_str(),
                                            decoderConfig.forceParserType,
                                            (decoderConfig.enableStreamDemuxing == 1),
                                            decoderConfig.initialWidth,
                                            decoderConfig.initialHeight,
                                            decoderConfig.initialBitdepth,
                                            videoStreamDemuxer);

        if (result != VK_SUCCESS) {

            assert(!"Can't initialize the VideoStreamDemuxer!");
            return result;
        }

        VkSharedBaseObj<VkVideoFrameOutput> frameToFile;
#if (!_TRANSCODING)
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
#endif

        vulkanVideoProcessor->Initialize(&vkDevCtxt, videoStreamDemuxer, frameToFile, decoderConfig);

        VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>> videoQueue(vulkanVideoProcessor);
        DecoderFrameProcessorState frameProcessor(&vkDevCtxt, videoQueue, decoderConfig.decoderQueueSize);

        const int numberOfFrames = decoderConfig.decoderQueueSize;
        int ret = frameProcessor->CreateFrameData(numberOfFrames);
        assert(ret == numberOfFrames);
        if (ret != numberOfFrames) {
            return -1;
        }
#if (_TRANSCODING)
    int curFrameIndex = 0;
#endif // _TRANSCODING
        bool continueLoop = true;
        do {
#if (_TRANSCODING)
            continueLoop = frameProcessor->OnFrameTranscoding(0, &decoderConfig, encoderConfig);
            curFrameIndex++;
#else
            continueLoop = frameProcessor->OnFrame(0);
#endif //_TRANSCODING
        } while (continueLoop);
#if (_TRANSCODING)
        int i = 0;
        do {
            vulkanVideoProcessor->getEncoder(i)->WaitForThreadsToComplete();
            std::string outputFilename = encoderConfig->numEncoderResizedOutputs == 0 ?
                                        encoderConfig->outputFileHandler.GetFileName() :
                                        encoderConfig->resizedOutputFileHandler[i].GetFileName();
            std::cout << "Done processing " << curFrameIndex << " input frames!" << std::endl
                    << "Encoded file's location is at " << outputFilename
                    << std::endl;
            i++;
        }  while (i < encoderConfig->numEncoderResizedOutputs);
#endif // _TRANSCODING
        frameProcessor->DestroyFrameData();
    }

    return 0;
}