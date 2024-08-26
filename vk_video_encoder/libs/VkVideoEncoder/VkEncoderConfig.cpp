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

void printHelp(VkVideoCodecOperationFlagBitsKHR codec)
{
    fprintf(stderr,
    "Usage : EncodeApp \n\
    -h, --help                      provides help\n\
    -i, --input                     .yuv Input YUV File Name (YUV420p 8bpp only) \n\
    -o, --output                    .264/5,ivf Output H264/5/AV1 File Name \n\
    -c, --codec                     <string> select codec type: avc (h264) or hevc (h265) or av1\n\
    --dpbMode                       <string>  : select DPB mode: layered, separate\n\
    --inputWidth                    <integer> : Encode Width \n\
    --inputHeight                   <integer> : Encode Height \n\
    --inputNumPlanes                <integer> : Number of planes \n\
    --inputChromaSubsampling        <string>  : Chromat subsapling to use, default 420 \n\
    --inputLumaPlanePitch           <integer> : Pitch for Luma plane \n\
    --inputBpp                      <integer> : Bits per pixel, default 8 \n\
    --startFrame                    <integer> : Start Frame Number to be Encoded \n\
    --numFrames                     <integer> : End Frame Number to be Encoded \n\
    --minQp                         <integer> : Minimum QP value in the range [0, 51] \n\
    --maxQp                         <integer> : Maximum QP value in the range [0, 51] \n\
    --gopFrameCount                 <integer> : Number of frame in the GOP, default 16\n\
    --idrPeriod                     <integer> : Number of frame between 2 IDR frame, default 60\n\
    --consecutiveBFrameCount        <integer> : Number of consecutive B frame count in a GOP \n\
    --temporalLayerCount            <integer> : Count of temporal layer \n\
    --lastFrameType                 <integer> : Last frame type \n\
    --closedGop                     Close the Gop, default open\n\
    --qualityLevel                  <integer> : Select quality level \n\
    --tuningMode                    <integer> or <string> : Select tuning mode \n\
                                        default(0), hq(1), lowlatency(2), lossless(3) \n\
    --rateControlMode               <integer> or <string>: select different rate control modes: \n\
                                        default(0), disabled(1), cbr(2), vbr(4)\n\
    --deviceID                      <string>  : DeviceID to be used, \n\
    --deviceUuid                    <string>  : deviceUuid to be used \n");

}

int EncoderConfig::ParseArguments(int argc, char *argv[])
{
    int argcount = 0;
    std::vector<char*> arglist;
    std::vector<std::string> args(argv, argv + argc);

    appName = args[0];

    for (int32_t i = 1; i < argc; i++) {

        if (args[i] == "-i" || args[i] == "--input") {
            if (++i >= argc) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            size_t fileSize = inputFileHandler.SetFileName(args[i].c_str());
            if (fileSize <= 0) {
                return (int)fileSize;
            }
        } else if (args[i] == "-o" || args[i] == "--output") {
            if (++i >= argc) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            size_t fileSize = outputFileHandler.SetFileName(args[i].c_str());
            if (fileSize <= 0) {
                return (int)fileSize;
            }
        } else if (args[i] == "-h" || args[i] == "--help") {
            printHelp(codec);
            return -1;
        } else if (args[i] == "-c" || args[i] == "--codec") {
            std::string codec_ = args[i + 1];
            if (codec_ == "avc" || codec_== "h264") {
                codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
            } else if (codec_ == "hevc" || codec_== "h265") {
                codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
            } else if (codec_ == "av1") {
                codec = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
                assert(!"AV1 is not supported yet!");
                return -1;
            } else {
                // Invalid codec
                fprintf(stderr, "Invalid codec: %s\n", codec_.c_str());
                return -1;
            }
            printf("Selected codec: %s\n", codec_.c_str());
            i++; // Skip the next argument since it's the codec value
        } else if (args[i] == "--dpbMode") {
            std::string dpbMode = args[i + 1];
            if (dpbMode == "separate") {
                useDpbArray = false;
            } else if (dpbMode == "layered") {
                useDpbArray = true;
            } else {
                // Invalid codec
                fprintf(stderr, "Invalid DPB mode: %s\n", dpbMode.c_str());
                return -1;
            }
            printf("Selected DPB mode: %s\n", dpbMode.c_str());
            i++; // Skip the next argument since it's the dpbMode value
        } else if (args[i] == "--inputWidth") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%u", &input.width) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--inputHeight") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%u", &input.height) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--inputNumPlanes") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%u", &input.numPlanes) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            if ((input.numPlanes < 2) || (input.numPlanes > 3)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                fprintf(stderr, "Currently supported number of planes are 2 or 3\n");
            }
        } else if (args[i] == "--inputChromaSubsampling") {
            std::string chromeSubsampling = args[i + 1];
            if (chromeSubsampling== "400") {
                input.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR;
            } else if (chromeSubsampling== "420") {
                input.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
            } else if (chromeSubsampling== "422") {
                input.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR;
            } else if (chromeSubsampling == "444") {
                input.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
            } else {
                // Invalid chromeSubsampling
                fprintf(stderr, "Invalid chromeSubsampling: %s\nValid string values are 400, 420, 422, 444 \n", chromeSubsampling.c_str());
                return -1;
            }
            i++; // Skip the next argument since it's the chromeSubsampling value
        }  else if (args[i] == "--inputLumaPlanePitch") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%llu",
                    (long long unsigned int*)&input.planeLayouts[0].rowPitch) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        }  else if (args[i] == "--inputBpp") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%hhu", &input.bpp) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--startFrame") {
            if (++i >= argc || sscanf(args[i].c_str(), "%u", &startFrame) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--numFrames") {
            if (++i >= argc || sscanf(args[i].c_str(), "%u", &numFrames) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--minQp") {
            if (++i >= argc || sscanf(args[i].c_str(), "%u", &minQp) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--maxQp") {
            if (++i >= argc || sscanf(args[i].c_str(), "%u", &maxQp) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        // GOP structure
        } else if (args[i] == "--gopFrameCount") {
            uint8_t gopFrameCount = EncoderConfig::DEFAULT_GOP_FRAME_COUNT;
            if (++i >= argc || sscanf(args[i].c_str(), "%hhu", &gopFrameCount) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetGopFrameCount(gopFrameCount);
            printf("Selected gopFrameCount: %d\n", gopFrameCount);
        } else if (args[i] == "--idrPeriod") {
            int32_t idrPeriod = EncoderConfig::DEFAULT_GOP_IDR_PERIOD;
            if (++i >= argc || sscanf(args[i].c_str(), "%d", &idrPeriod) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetIdrPeriod(idrPeriod);
            printf("Selected idrPeriod: %d\n", idrPeriod);
        } else if (args[i] == "--consecutiveBFrameCount") {
            uint8_t consecutiveBFrameCount = EncoderConfig::DEFAULT_CONSECUTIVE_B_FRAME_COUNT;
            if (++i >= argc || sscanf(args[i].c_str(), "%hhu", &consecutiveBFrameCount) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetConsecutiveBFrameCount(consecutiveBFrameCount);
            printf("Selected consecutiveBFrameCount: %d\n", consecutiveBFrameCount);
        } else if (args[i] == "--temporalLayerCount") {
            uint8_t temporalLayerCount = EncoderConfig::DEFAULT_TEMPORAL_LAYER_COUNT;
            if (++i >= argc || sscanf(args[i].c_str(), "%hhu", &temporalLayerCount) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetTemporalLayerCount(temporalLayerCount);
            printf("Selected temporalLayerCount: %d\n", temporalLayerCount);
        } else if (args[i] == "--lastFrameType") {
            VkVideoGopStructure::FrameType lastFrameType = VkVideoGopStructure::FRAME_TYPE_P;
            std::string frameTypeName = args[i + 1];
            if (frameTypeName == "p" || frameTypeName == "P") {
                lastFrameType = VkVideoGopStructure::FRAME_TYPE_P;
            } else if (frameTypeName == "b" || frameTypeName == "B") {
                lastFrameType = VkVideoGopStructure::FRAME_TYPE_B;
            } else if (frameTypeName == "i" || frameTypeName == "I") {
                lastFrameType = VkVideoGopStructure::FRAME_TYPE_I;
            } else {
                // Invalid frameTypeName
                fprintf(stderr, "Invalid frameTypeName: %s\n", frameTypeName.c_str());
                return -1;
            }
            i++; // Skip the next argument since it's the frameTypeName value
            gopStructure.SetLastFrameType(lastFrameType);
            printf("Selected frameTypeName: %s\n", gopStructure.GetFrameTypeName(lastFrameType));
        } else if (args[i] == "--closedGop") {
            gopStructure.SetClosedGop();
        } else if (args[i] == "--qualityLevel") {
            if (++i >= argc || sscanf(args[i].c_str(), "%u", &qualityLevel) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--tuningMode") {
            if (++i >= argc) {
                fprintf(stderr, "Invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            std::string tuningModeStr = argv[i];
            if (tuningModeStr == "0" || tuningModeStr == "default") {
                tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR;
            } else if (tuningModeStr == "1" || tuningModeStr == "hq") {
                tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR;
            } else if (tuningModeStr == "2" || tuningModeStr == "lowlatency") {
                tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR;
            } else if (tuningModeStr == "3" || tuningModeStr == "lossless") {
                tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_LOSSLESS_KHR;
            } else {
                fprintf(stderr, "Invalid tuningMode: %s\n", tuningModeStr.c_str());
                return -1;
            }
        } else if (args[i] == "--rateControlMode") {
            if (++i >= argc) {
                fprintf(stderr, "invalid parameter for %s\n", args[i-1].c_str());
                return -1;
            }
            std::string rc = args[i];
            if ( rc == "0" || rc == "default") {
                rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
            } else if (rc == "1" || rc == "disabled") {
                rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
            } else if (rc == "2" || rc == "cbr") {
                rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
            } else if (rc == "4" || rc == "vbr") {
                rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
            }else {
                // Invalid rateControlMode
                fprintf(stderr, "Invalid rateControlMode: %s\n", rc.c_str());
                return -1;
            }
        } else if (args[i] == "--deviceID") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%x", &deviceId) != 1)) {
                 fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                 return -1;
             }
        } else if (args[i] == "--deviceUuid") {
            if (++i >= argc) {
               fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
               return -1;
            }
            size_t size = SetHexDeviceUUID(args[i].c_str());
            if (size != VK_UUID_SIZE) {
                fprintf(stderr,"Invalid deviceUuid format used: %s with size: %zu."
                               "deviceUuid must be represented by 16 hex (32 bytes) values.", args[i].c_str(), args[i].length());
                return -1;
            }
        } else {
            argcount++;
            arglist.push_back((char*)args[i].c_str());
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
    std::vector<std::string> args(argv, argv + argc);

    for (int32_t i = 1; i < argc; i++) {

        if (args[i] == "--codec" || args[i] == "-c") {
            std::string codecStr = args[i + 1];
            if (codecStr == "avc" || codecStr == "h264") {
                codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
            } else if (codecStr == "hevc" || codecStr == "h265") {
                codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
            } else if (codecStr == "av1") {
                codec = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
                assert(!"AV1 is not supported yet!");
                return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
            } else {
                // Invalid codec
                fprintf(stderr, "Invalid codec: %s\n", codecStr.c_str());
                fprintf(stderr, "Supported codecs are: avc, hevc and av1\n");
                return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
            }
        } else if (args[i] == "--help" || args[i] == "-h") {
            printHelp(codec);
            return VK_ERROR_INITIALIZATION_FAILED;
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
        fprintf(stderr, "Codec type is not selected\n. Please select it with --codec <avc or hevc or av1> parameters\n");
        printHelp(codec);
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
                                                  GetDefaultVideoProfileIdc(),
                                          tuningMode);
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
