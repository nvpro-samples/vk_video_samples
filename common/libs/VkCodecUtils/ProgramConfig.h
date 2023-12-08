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

#ifndef _PROGRAMSETTINGS_H_
#define _PROGRAMSETTINGS_H_

#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include "vulkan_interfaces.h"

struct ProgramConfig {

    ProgramConfig(const char* programName) {
        appName = programName;
        initialWidth = 1920;
        initialHeight = 1080;
        initialBitdepth = 8;
        videoWidth = 0;
        videoHeight = 0;
        queueCount = 1;
        numDecodeImagesInFlight = 8;
        numDecodeImagesToPreallocate = -1; // pre-allocate the maximum num of images
        numBitstreamBuffersToPreallocate = 8;
        backBufferCount = 8;
        ticksPerSecond = 30;
        vsync = true;

        verbose = false;
        validate = false;
        validateVerbose = false;

        noTick = false;
        noPresent = false;

        maxFrameCount = -1;
        videoFileName = "";
        loopCount = 1;
        queueId = 0;
        gpuIndex = -1;
        forceParserType = VK_VIDEO_CODEC_OPERATION_NONE_KHR;
        decoderQueueSize = 10;
        enablePostProcessFilter = -1,
        enableStreamDemuxing = true;
        deviceId = (uint32_t)-1;
        directMode = false;
        enableHwLoadBalancing = false;
        selectVideoWithComputeQueue = false;
        enableVideoEncoder = false;
    }

    void ParseArgs(int argc, const char* argv[]) {
        for (int i = 1; i < argc; i++) {
            if (nullptr != strstr(argv[i], "--help")) {
                i++;
                break;
            } else if (nullptr != strstr(argv[i], "--enableStrDemux")) {
                enableStreamDemuxing = true;
            } else if (nullptr != strstr(argv[i], "--disableStrDemux")) {
                enableStreamDemuxing = false;
            } else if (nullptr != strstr(argv[i], "--codec")) {
                i++;
                if ((nullptr != strstr(argv[i], "5")) || (nullptr != strstr(argv[i], "hevc"))) {
                    forceParserType = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
                } else if ((nullptr != strstr(argv[i], "4")) || (nullptr != strstr(argv[i], "h264"))) {
                    forceParserType = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
                }
            } else if (nullptr != strstr(argv[i], "-b")) {
                vsync = false;
            } else if (nullptr != strstr(argv[i], "-w")) {
                i++;
                if (argv[i])
                    initialWidth = std::atoi(argv[i]);
            } else if (nullptr != strstr(argv[i], "-h")) {
                i++;
                if (argv[i])
                    initialHeight = std::atoi(argv[i]);
            } else if (nullptr != strstr(argv[i], "-v")) {
                validate = true;
            } else if (nullptr != strstr(argv[i], "--validate")) {
                validate = true;
            } else if (nullptr != strstr(argv[i], "-vv")) {
                validate = true;
                validateVerbose = true;
            } else if (nullptr != strstr(argv[i], "--verbose")) {
                verbose = true;
            } else if (nullptr != strstr(argv[i], "--noTick")) {
                noTick = true;
            } else if (nullptr != strstr(argv[i], "--noPresent")) {
                noPresent = true;
            } else if (nullptr != strstr(argv[i], "--enableHwLoadBalancing")) {
                enableHwLoadBalancing = true;
            } else if (nullptr != strstr(argv[i], "--selectVideoWithComputeQueue")) {
                selectVideoWithComputeQueue = true;
            } else if (nullptr != strstr(argv[i], "-o")) {
                i++;
                outputFileName = argv[i];
            } else if (nullptr != strstr(argv[i], "-i")) {
                i++;
                videoFileName = argv[i];
                std::ifstream validVideoFileStream(videoFileName, std::ifstream::in);
                if (!validVideoFileStream) {
                    std::cerr << "Invalid input video file: " << videoFileName << std::endl;
                    std::cerr << "Please provide a valid name for the input video file to be decoded with the \"-i\" command line option." << std::endl;
                    std::cerr << "   vk-video-dec-test -i <absolute file path location>" << std::endl;
                }
            } else if (nullptr != strstr(argv[i], "--gpu")) {
                i++;
                gpuIndex = std::atoi(argv[i]);
            } else if (nullptr != strstr(argv[i], "--queueSize")) {
                i++;
                decoderQueueSize = std::atoi(argv[i]);
            } else if (nullptr != strstr(argv[i], "--enablePostProcessFilter")) {
                i++;
                enablePostProcessFilter = std::atoi(argv[i]);
            } else if (nullptr != strstr(argv[i], "-c")) {
                i++;
                if (argv[i])
                    maxFrameCount = std::atoi(argv[i]);
            } else if (nullptr != strstr(argv[i], "--loop")) {
                i++;
                if (argv[i])
                    loopCount = std::atoi(argv[i]);
            } else if (nullptr != strstr(argv[i], "--queueid")) {
                i++;
                 if (argv[i])
                    queueId = std::atoi(argv[i]);
            } else  if (!strcmp(argv[i], "-deviceID")) {
                ++i;
                sscanf(argv[i], "%x", &deviceId);
            } else if (!strcmp(argv[i], "--direct")) {
                directMode = true;
            }
        }
    }

    std::string appName;
    int initialWidth;
    int initialHeight;
    int initialBitdepth;
    int videoWidth;
    int videoHeight;
    int queueCount;
    int32_t numDecodeImagesInFlight;
    int32_t numDecodeImagesToPreallocate;
    int32_t numBitstreamBuffersToPreallocate;
    int backBufferCount;
    int ticksPerSecond;
    int maxFrameCount;

    std::string videoFileName;
    std::string outputFileName;
    int gpuIndex;
    int loopCount;
    int queueId;
    VkVideoCodecOperationFlagBitsKHR forceParserType;
    uint32_t deviceId;
    uint32_t decoderQueueSize;
    int32_t enablePostProcessFilter;
    uint32_t enableStreamDemuxing : 1;
    uint32_t directMode : 1;
    uint32_t vsync : 1;
    uint32_t validate : 1;
    uint32_t validateVerbose : 1;
    uint32_t verbose : 1;
    uint32_t noTick : 1;
    uint32_t noPresent : 1;
    uint32_t enableHwLoadBalancing : 1;
    uint32_t selectVideoWithComputeQueue : 1;
    uint32_t enableVideoEncoder : 1;
};

#endif /* _PROGRAMSETTINGS_H_ */
