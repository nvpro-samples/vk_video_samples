/*
 * Copyright 2023 NVIDIA Corporation.
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

#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkVideoEncoder/VkEncoderConfigH264.h"
#include "VkVideoEncoder/VkEncoderConfigH265.h"

void printHelp()
{
    fprintf(stderr,
            "Usage : EncodeApp \n\
    -i                              .yuv Input YUV File Name (YUV420p 8bpp only) \n\
    -o                              .264/5 Output H264/5 File Name \n\
    --codec                         <sting> select codec type: avc (h264) or hevc (h265) or av1\n\
    --startFrame                    <integer> : Start Frame Number to be Encoded \n\
    --numFrames                     <integer> : End Frame Number to be Encoded \n\
    --inputWidth                         <integer> : Encode Width \n\
    --inputHeight                        <integer> : Encode Height \n\
    --minQp                         <integer> : Minimum QP value in the range [0, 51] \n\
    --logBatchEncoding              Enable verbose logging of batch recording and submission of commands \n"
    );
}

int EncoderConfig::ParseArguments(int argc, char *argv[])
{
    int argcount = 0;
    std::vector<char*> arglist;

    appName = argv[0];

    for (int32_t i = 1; i < argc; i++) {

        if (strcmp(argv[i], "-i") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
            size_t fileSize = inputFileHandler.SetFileName(argv[i]);
            if (fileSize <= 0) {
                return (int)fileSize;
            }
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
            size_t fileSize = outputFileHandler.SetFileName(argv[i]);
            if (fileSize <= 0) {
                return (int)fileSize;
            }
        } else if (strcmp(argv[i], "-h") == 0) {
            printHelp();
            return -1;
        } else if (strcmp(argv[i], "--codec") == 0) {
            const char *codec_ = argv[i + 1];
            if ((strcmp(codec_, "avc") == 0) || (strcmp(codec_, "h264") == 0)) {
                codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
            } else if ((strcmp(codec_, "hevc") == 0) || (strcmp(codec_, "h265") == 0)) {
                codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
            } else if (strcmp(codec_, "av1") == 0) {
                // codec = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
		assert(!"AV1 is not supported yet!");
		return -1;
            } else {
                // Invalid codec
                fprintf(stderr, "Invalid codec: %s\n", codec_);
                return -1;
            }
            printf("Selected codec: %s\n", codec_);
            i++; // Skip the next argument since it's the codec value
        } else if ((strcmp(argv[i], "--inputWidth") == 0)) {
            if ((++i >= argc) || (sscanf(argv[i], "%u", &input.width) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        }  else if ((strcmp(argv[i], "--inputHeight") == 0)) {
            if ((++i >= argc) || (sscanf(argv[i], "%u", &input.height) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        }  else if ((strcmp(argv[i], "--inputNumPlanes") == 0)) {
            if ((++i >= argc) || (sscanf(argv[i], "%u", &input.numPlanes) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
            if ((input.numPlanes < 2) || (input.numPlanes > 3)) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                fprintf(stderr, "Currently supported number of planes are 2 or 3\n");
            }
        } else if (strcmp(argv[i], "--inputChromaSubsampling") == 0) {
            const char *chromeSubsampling = argv[i + 1];
            if (strcmp(chromeSubsampling, "400") == 0) {
                input.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR;
            } else if (strcmp(chromeSubsampling, "420") == 0) {
                input.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
            } else if (strcmp(chromeSubsampling, "422") == 0) {
                input.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR;
            } else if (strcmp(chromeSubsampling, "444") == 0) {
                input.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
            } else {
                // Invalid chromeSubsampling
                fprintf(stderr, "Invalid chromeSubsampling: %s\n", chromeSubsampling);
                fprintf(stderr, "Valid string values are 400, 420, 422, 444 \n");
                return -1;
            }
            i++; // Skip the next argument since it's the chromeSubsampling value
        }  else if ((strcmp(argv[i], "--inputLumaPlanePitch") == 0)) {
            if ((++i >= argc) || (sscanf(argv[i], "%llu",
                    (long long unsigned int*)&input.planeLayouts[0].rowPitch) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        }  else if ((strcmp(argv[i], "--inputBpp") == 0)) {
            if ((++i >= argc) || (sscanf(argv[i], "%hhu", &input.bpp) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        } else if (strcmp(argv[i], "--startFrame") == 0) {
            if (++i >= argc || sscanf(argv[i], "%u", &startFrame) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        } else if (strcmp(argv[i], "--numFrames") == 0) {
            if (++i >= argc || sscanf(argv[i], "%u", &numFrames) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        } else if (strcmp(argv[i], "--minQp") == 0) {
            if (++i >= argc || sscanf(argv[i], "%u", &minQp) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        } else if (strcmp(argv[i], "--maxQp") == 0) {
            if (++i >= argc || sscanf(argv[i], "%u", &minQp) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        // GOP structure
        } else if (strcmp(argv[i], "--gopFrameCount") == 0) {
            uint8_t gopFrameCount = EncoderConfig::DEFAULT_GOP_FRAME_COUNT;
            if (++i >= argc || sscanf(argv[i], "%hhu", &gopFrameCount) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
            gopStructure.SetGopFrameCount(gopFrameCount);
            printf("Selected gopFrameCount: %d\n", gopFrameCount);
        } else if (strcmp(argv[i], "--idrPeriod") == 0) {
            int32_t idrPeriod = EncoderConfig::DEFAULT_GOP_IDR_PERIOD;
            if (++i >= argc || sscanf(argv[i], "%d", &idrPeriod) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
            gopStructure.SetIdrPeriod(idrPeriod);
            printf("Selected idrPeriod: %d\n", idrPeriod);
        } else if (strcmp(argv[i], "--consecutiveBFrameCount") == 0) {
            uint8_t consecutiveBFrameCount = EncoderConfig::DEFAULT_CONSECUTIVE_B_FRAME_COUNT;
            if (++i >= argc || sscanf(argv[i], "%hhu", &consecutiveBFrameCount) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
            gopStructure.SetConsecutiveBFrameCount(consecutiveBFrameCount);
            printf("Selected consecutiveBFrameCount: %d\n", consecutiveBFrameCount);
        } else if (strcmp(argv[i], "--temporalLayerCount") == 0) {
            uint8_t temporalLayerCount = EncoderConfig::DEFAULT_TEMPORAL_LAYER_COUNT;
            if (++i >= argc || sscanf(argv[i], "%hhu", &temporalLayerCount) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
            gopStructure.SetTemporalLayerCount(temporalLayerCount);
            printf("Selected temporalLayerCount: %d\n", temporalLayerCount);
        } else if (strcmp(argv[i], "--lastFrameType") == 0) {
            VkVideoGopStructure::FrameType lastFrameType = VkVideoGopStructure::FRAME_TYPE_P;
            const char *frameTypeName = argv[i + 1];
            if ((strcmp(frameTypeName, "p") == 0) || (strcmp(frameTypeName, "P") == 0)) {
                lastFrameType = VkVideoGopStructure::FRAME_TYPE_P;
            } else if ((strcmp(frameTypeName, "b") == 0) || (strcmp(frameTypeName, "B") == 0)) {
                lastFrameType = VkVideoGopStructure::FRAME_TYPE_B;
            } else if ((strcmp(frameTypeName, "i") == 0) || (strcmp(frameTypeName, "I") == 0)) {
                lastFrameType = VkVideoGopStructure::FRAME_TYPE_I;
            } else {
                // Invalid frameTypeName
                fprintf(stderr, "Invalid frameTypeName: %s\n", frameTypeName);
                return -1;
            }
            i++; // Skip the next argument since it's the frameTypeName value
            gopStructure.SetLastFrameType(lastFrameType);
            printf("Selected frameTypeName: %s\n", gopStructure.GetFrameTypeName(lastFrameType));
        } else if (strcmp(argv[i], "--closedGop") == 0) {
            gopStructure.SetClosedGop();
        } else if (strcmp(argv[i], "--deviceID") == 0) {
            if ((++i >= argc) || (sscanf(argv[i], "%x", &deviceId) != 1)) {
                 fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                 return -1;
             }
        } else if (strcmp(argv[i], "--deviceUuid") == 0) {
            if (++i >= argc) {
               fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
               return -1;
            }
            size_t size = SetHexDeviceUUID(argv[i]);
            if (size != VK_UUID_SIZE) {
                std::cerr << "Invalid deviceUuid format used: " << argv[i]
                          << " with size: " << strlen(argv[i])
                          << std::endl;
                std::cerr << "deviceUuid must be represented by 16 hex (32 bytes) values."
                          << std::endl;
                return -1;
            }
        } else {
            argcount++;
            arglist.push_back(argv[i]);
        }
    }

    if (!inputFileHandler.HasFileName()) {
        fprintf(stderr, "An input file was not specified\n");
        return -1;
    }

    if (input.width == 0) {
        fprintf(stderr, "The width was not specified\n");
        return -1;
    }
    encodeWidth = input.width;

    if (input.height == 0) {
        fprintf(stderr, "The height was not specified\n");
        return -1;
    }
    encodeHeight = input.height;

    if (!outputFileHandler.HasFileName()) {
        const char* defaultOutName = (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) ? "out.264" :
                                     (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR) ? "out.265" : "out.ivf";
        fprintf(stdout, "No output file name provided. Using %s.\n", defaultOutName);
        size_t fileSize = outputFileHandler.SetFileName(defaultOutName);
        if (fileSize <= 0) {
            return (int)fileSize;
        }
    }

    if (minQp == -1) {
        fprintf(stdout, "No QP was provided. Using default value: 20.\n");
        minQp = 20;
    }

    codecBlockAlignment = H264MbSizeAlignment; // H264

    return DoParseArguments(argcount, arglist.data());
}

VkResult EncoderConfig::CreateCodecConfig(int argc, char *argv[],
                                          VkSharedBaseObj<EncoderConfig>& encoderConfig)
{

    VkVideoCodecOperationFlagBitsKHR codec = VK_VIDEO_CODEC_OPERATION_NONE_KHR;

    for (int32_t i = 1; i < argc; i++) {

        if (strcmp(argv[i], "--codec") == 0) {
            const char *codecStr = argv[i + 1];
            if ((strcmp(codecStr, "avc") == 0) || (strcmp(codecStr, "h264") == 0)) {
                codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
            } else if ((strcmp(codecStr, "hevc") == 0) || (strcmp(codecStr, "h265") == 0)) {
                codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
            } else {
                // Invalid codec
                fprintf(stderr, "Invalid codec: %s\n", codecStr);
                fprintf(stderr, "Supported codecs are: avc and hevc\n");
                return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
            }
        }
    }

    if (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) {

        VkSharedBaseObj<EncoderConfigH264> vkEncoderConfigh264(new EncoderConfigH264());
        int ret = vkEncoderConfigh264->ParseArguments(argc, argv);
        if (ret != 0) {
            assert(!"Invalid arguments");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkResult result = vkEncoderConfigh264->InitializeParameters();
        if (result != VK_SUCCESS) {
            assert(!"InitializeParameters failed");
            return result;
        }

        encoderConfig = vkEncoderConfigh264;
        return VK_SUCCESS;

    } else if (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR) {

        VkSharedBaseObj<EncoderConfigH265> vkEncoderConfigh265(new EncoderConfigH265());
        int ret = vkEncoderConfigh265->ParseArguments(argc, argv);
        if (ret != 0) {
            assert(!"Invalid arguments");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkResult result = vkEncoderConfigh265->InitializeParameters();
        if (result != VK_SUCCESS) {
            assert(!"InitializeParameters failed");
            return result;
        }

        encoderConfig = vkEncoderConfigh265;
        return VK_SUCCESS;

    } else {
        fprintf(stderr, "Codec type is not selected\n. Please select it with --codec <avc or hevc> parameters\n");
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    return VK_ERROR_INITIALIZATION_FAILED;
}

void EncoderConfig::InitVideoProfile()
{
    // update the video profile
    videoCoreProfile = VkVideoCoreProfile(codec, encodeChromaSubsampling,
                                          GetComponentBitDepthFlagBits(encodeBitDepthLuma),
                                          GetComponentBitDepthFlagBits(encodeBitDepthChroma),
                                          (videoProfileIdc != (uint32_t)-1) ? videoProfileIdc :
                                                  GetDefaultVideoProfileIdc());
}

bool EncoderConfig::InitRateControl()
{
    uint32_t levelBitRate = ((rateControlMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) && hrdBitrate == 0)
                                ? averageBitrate
                                :            // constrained by avg bitrate
                                hrdBitrate;  // constrained by max bitrate

    // If no bitrate is specified, use the level limit
    if (averageBitrate == 0) {
        averageBitrate = hrdBitrate ? hrdBitrate : levelBitRate;
    }

    // If no HRD bitrate is specified, use 3x average for VBR (without going above level limit) or equal to average bitrate for
    // CBR
    if (hrdBitrate == 0) {
        if ((rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR) && (averageBitrate < levelBitRate)) {
            hrdBitrate = std::min(averageBitrate * 3, levelBitRate);
        } else {
            hrdBitrate = averageBitrate;
        }
    }

    // avg bitrate must not be higher than max bitrate,
    if (averageBitrate > hrdBitrate) {
        averageBitrate = hrdBitrate;
    }

    if (rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR) {
        hrdBitrate = averageBitrate;
    }

    return true;
}
