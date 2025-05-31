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
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#else
#include <Windows.h>
#include <psapi.h>
#endif

#include "VkCodecUtils/DecoderConfig.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VulkanVideoProcessor.h"
#include "VkCodecUtils/VulkanDecoderFrameProcessor.h"
#include "VkShell/Shell.h"
#include "VkCodecUtils/VkVideoFrameOutput.h"

#include "VkCodecUtils/poll_manager.h"

int main(int argc, const char **argv) {

    int spawn = 0;
#ifdef _WIN32
    PROCESS_INFORMATION pi {};
    STARTUPINFO si {};
    if (strncmp(argv[argc-1],"spawn",5)==0) {
        spawn = 1;
        argc--;
    }
#endif

    DecoderConfig decoderConfig(argv[0]);
    decoderConfig.ParseArgs(argc, argv);

    int pid = getpid();
    if (spawn == 0) {
        for (int n = 0; n < (int)decoderConfig.numberOfDecodeWorkers; n++) {
#ifdef _WIN32
            cloneTheProcess(argc, argv, pi, si);
#else
            if(fork() == 0) {
                break;
            }
#endif
        }
    }
#ifdef _WIN32
    if (spawn == 0)
#else
    if (pid == getpid())
#endif
    {
        if (decoderConfig.enableWorkerProcessesPoll == 1) {
            int result = 1;
            if (decoderConfig.ipcType == IPC_TYPE::UNIX_DOMAIN_SOCKETS) {
                result = usoc_manager(decoderConfig.noPresent, decoderConfig.fileListIpc);
            }
            return result;
        }
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
    DecoderFrameProcessorState frameProcessor;
    std::vector<std::string> messageBuffer(128);
    auto processInputFile = [&] (const int i) -> bool {
        int res = 0;
        if (decoderConfig.enableWorkerProcessesPoll) {
            std::string receivedMessage;
            receivedMessage.resize(DEFAULT_BUFLEN);
            res = receiveNewBitstream(static_cast<IPC_TYPE>(decoderConfig.ipcType), decoderConfig.enableWorkerProcessesPoll, receivedMessage);
            if (res) {
                receivedMessage.resize(DEFAULT_BUFLEN);
                res = parseCharArray(messageBuffer, receivedMessage.c_str(), argc, argv);
                if (0) { // if(parseAllCmdline) { // we can pass full cmdline as well in case we need to change some decoding parameters
                    decoderConfig.ParseArgs(argc, argv);
                } else if (res) {
                    decoderConfig.videoFileName = argv[0];
                    std::cout << argv[0] << std::endl;
                } else {
                    return 0;
                }
            }
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

        result = VulkanVideoProcessor::Create(decoderConfig, &vkDevCtxt, vulkanVideoProcessor);
        if (result != VK_SUCCESS) {
            std::cerr << "Error creating the decoder instance: " << result << std::endl;
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

        vulkanVideoProcessor->Initialize(&vkDevCtxt, videoStreamDemuxer, frameToFile, decoderConfig);

        VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>> videoQueue(vulkanVideoProcessor);
        frameProcessor = { &vkDevCtxt, videoQueue, static_cast<int32_t>(decoderConfig.decoderQueueSize) };
        if (!decoderConfig.enableWorkerProcessesPoll) {
            return i == 0;
        }
        return res;
    };

    VkVideoCodecOperationFlagsKHR videoDecodeCodecs = (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR  |
                                                       VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR  |
                                                       VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR);

    VkVideoCodecOperationFlagsKHR videoEncodeCodecs = ( VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR  |
                                                        VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR  |
                                                        VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR);

    VkVideoCodecOperationFlagsKHR videoCodecs = videoDecodeCodecs |
                                        (decoderConfig.enableVideoEncoder ? videoEncodeCodecs : (VkVideoCodecOperationFlagsKHR) VK_VIDEO_CODEC_OPERATION_NONE_KHR);

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

        int i0 = 0;
        while (processInputFile(i0++)) {
            displayShell->AttachFrameProcessor(frameProcessor);
            displayShell->RunLoop();
        }

    } else {

        result = vkDevCtxt.InitPhysicalDevice(decoderConfig.deviceId, decoderConfig.deviceUUID,
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

        int i1 = 0;
        while (processInputFile(i1++))
        {
            const int numberOfFrames = decoderConfig.decoderQueueSize;
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
    }

#ifdef _WIN32
    WaitForSingleObject( pi.hProcess, INFINITE );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
#else
    int status = 1;
    wait(&status);
    exit(status);
#endif

    return 0;
}
