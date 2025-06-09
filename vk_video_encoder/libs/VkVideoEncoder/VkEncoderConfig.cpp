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
#include "VkVideoEncoder/VkEncoderConfigAV1.h"

static void printHelp(VkVideoCodecOperationFlagBitsKHR codec)
{
    fprintf(stderr,
    "Usage : EncodeApp \n\
    -h, --help                      provides help\n\
    -i, --input                     .yuv Input YUV File Name (YUV420p 8bpp only) \n\
    -o, --output                    .264/5,ivf Output H264/5/AV1 File Name \n\
    -c, --codec                     <string> select codec type: avc (h264) or hevc (h265) or av1\n\
    --dpbMode                       <string>  : select DPB mode: layered, separate\n\
    --inputWidth                    <integer> : Input Width \n\
    --inputHeight                   <integer> : Input Height \n\
    --inputNumPlanes                <integer> : Number of planes \n\
    --inputChromaSubsampling        <string>  : Chromat subsapling to use, default 420 \n\
    --inputLumaPlanePitch           <integer> : Pitch for Luma plane \n\
    --inputBpp                      <integer> : Bits per pixel, default 8 \n\
    --msbShift                      <integer> : Shift the input plane pixels to the left when bpp > 8, default: 16 - inputBpp  \n\
    --startFrame                    <integer> : Start Frame Number to be Encoded \n\
    --numFrames                     <integer> : End Frame Number to be Encoded \n\
    --encodeOffsetX                 <integer> : Encoded offset X \n\
    --encodeOffsetY                 <integer> : Encoded offset Y \n\
    --encodeWidth                   <integer> : Encoded width \n\
    --encodeHeight                  <integer> : Encoded height \n\
    --encodeMaxWidth                <integer> : Encoded max width - the maximum content width supported. Used with content resize.\n\
    --encodeMaxHeight               <integer> : Encoded max height - the maximum content height supported. Used with content resize. \n\
    --minQp                         <integer> : Minimum QP value in the range [0, 51] \n\
    --maxQp                         <integer> : Maximum QP value in the range [0, 51] \n\
    --qpMap                         <string>  : selct quantization map type : deltaQpMap or emaphasisMap \n\
    --qpMapFileName                 <string>  : quantization map file name \n\
    --gopFrameCount                 <integer> : Number of frame in the GOP, default 16\n\
    --idrPeriod                     <integer> : Number of frame between 2 IDR frame, default 60\n\
    --consecutiveBFrameCount        <integer> : Number of consecutive B frame count in a GOP \n\
    --temporalLayerCount            <integer> : Count of temporal layer \n\
    --lastFrameType                 <integer> : Last frame type \n\
    --closedGop                     Close the Gop, default open\n\
    --qualityLevel                  <integer> : Select quality level \n\
    --tuningMode                    <integer> or <string> : Select tuning mode \n\
                                        default(0), hq(1), lowlatency(2), ultralowlatency(3), lossless(4) \n\
    --rateControlMode               <integer> or <string>: select different rate control modes: \n\
                                        default(0), disabled(1), cbr(2), vbr(4)\n\
    --averageBitrate                <integer> : Target bitrate for cbr/vbr RC modes\n\
    --maxBitrate                    <integer> : Peak bitrate for cbr/vbr RC modes\n\
    --qpI                           <integer> : QP or QIndex (for AV1) used for I-frames when RC disabled\n\
    --qpP                           <integer> : QP or QIndex (for AV1) used for P-frames when RC disabled\n\
    --qpB                           <integer> : QP or QIndex (for AV1) used for B-frames when RC disabled\n\
    --deviceID                      <hexadec> : deviceID to be used, \n\
    --deviceUuid                    <string>  : deviceUuid to be used \n\
    --enableHwLoadBalancing                   : enables HW load balancing using multiple encoder devices when available \n\
    --testOutOfOrderRecording                 : Testing only - enable testing for out-of-order-recording\n");

    if ((codec == VK_VIDEO_CODEC_OPERATION_NONE_KHR) || (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR)) {
        fprintf(stderr, "\nH264 specific arguments: None\n");
    }

    if ((codec == VK_VIDEO_CODEC_OPERATION_NONE_KHR) || (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR)) {
        fprintf(stderr, "\nH265 specific arguments: None\n");
    }

    if ((codec == VK_VIDEO_CODEC_OPERATION_NONE_KHR) || (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR)) {
        fprintf(stderr,
                "\nAV1 specific arguments:\n\
        --tiles                         Enable tile configuration\n\
        --params                        Enable custom tile configuration when followed by --tiles option\n\
                                        Otherwise default tile configuration will be used\n\
                                        Following tile paramers must be followed in order with this option\n\
                                        <uniform_tile_spacing_flag> <TileCols> [tileWidthInSbsMinus1[0] ...]\n\
                                        <TileRows> [tileHeightInSbsMinus1[0] ...] <context_update_tile_id>\n\
                                        Eg: 1. \"--tiles --params 0   5  3 3 3 3 3   3  2 4 3  2\"\n\
                                            2. \"--tiles --params 1   5              3         2\"\n\
                                            3. \"--tiles\"\n\n\
        --quant                         Enable quant configuration\n\
        --params                        Enable custom quant configuration when followed by --quant option\n\
                                        Otherwise default quant configuration will be used\n\
                                        Following quant parameters must be followed in order with this option\n\
                                        <base_q_idx> <DeltaQYDc> <DeltaQUDc> <DeltaQUAc> <diff_uv_delta [\n\
                                            <DeltaQVDc> <DeltaQVAc>]> <using_qmatrix [<qm_y> <qm_u> [qm_v]]>\n\
                                        Eg: 1. \"--quant --params 92   -1  2 -2   1  -1 0   1   2 4 3\"\n\
                                            2. \"--quant --params 92   -1  2 -2   0         0\"\n\
                                            3. \"--quant\"\n\n\
        --lf                            Enable loop filter configuration\n\
        --params                        Enable custom loop filter configuration when followed by --lf option\n\
                                        Otherwise default loop filter configuration will be used\n\
                                        Following loop filter parameters must be followed in order with this option\n\
                                        <level0> <level1> [<leve2> <level3>] <sharpness> <delta_enabled [<delta_update\n\
                                            [<update_ref_delta> <ref_deltas[0] ... ref_deltas[7]> <update_mode_delta>\n\
                                            <mode_deltas[0]< <mode_deltas[1]>] >]\n\
                                        Eg: 1. \"--lf --params 10 11 12 13   5  1  1  255  1 2 -1 2 1 2 -1 2   3  -1 1\"\n\
                                            2. \"--lf --params 10 11 12 13   5  1  0\"\n\
                                            3. \"--lf --params 10 11 12 13   5  0\"\n\
                                            4. \"--lf --params  0  0         5  0\"\n\
                                            5. \"--lf\"\n\n\
        --cdef                          Enable CDEF configuration\n\
        --params                        Enabel custom CDEF configuration when followed by --cdef option\n\
                                        Otherwise default CDEF configuration will be used\n\
                                        Following CDEF parameters must be followed in order with this option\n\
                                        <damping_minus_3> <bits> <y_pri[0]> <y_sec[0]> <uv_pri[0]> <uv_sec[0]> ...\n\
                                        Eg: 1. \"--cdef --params 3  2  1 2 9 1   2 3 7 3   3 1 4 2   4 0 3 2\"\n\
                                            2. \"--cdef --params 3  0  1 2 9 1\"\n\
                                            3. \"--cdef\"\n\
        --lr                            Enable loop restoration filter\n\
        --params                        Enabel custom loop restoration filter configuration when followed by --lr option\n\
                                        Otherwise default loop restoration filter configuration will be used\n\
                                        Following loop restoration parameters must be followed in order with this option\n\
                                        <type[0]> <type[1]> <type[2]> <size[0]> <size[1]> <size[2]>\n\
                                        Eg: 1. \"--lr --params 2 2 2   1 1 1\"\n\
                                            2. \"--lr\"\n\
        --profile                       <integer> or <string>: select different encoding profile: \n\
                                        main(0), high(1), professional(2)\n");
        }
}

int EncoderConfig::ParseArguments(int argc, const char *argv[])
{
    int argcount = 0;
    std::vector<const char*> arglist;
    std::vector<std::string> args(argv, argv + argc);
    uint32_t frameCount = 0;

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
            if (inputFileHandler.parseY4M(&input.width, &input.height, &frameRateNumerator, &frameRateDenominator)) {
                if (verbose) {
                    printf("Y4M file detected: width %d height %d\n", input.width, input.height);
                }
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
            } else {
                // Invalid codec
                fprintf(stderr, "Invalid codec: %s\n", codec_.c_str());
                return -1;
            }
            if (verbose) {
                printf("Selected codec: %s\n", codec_.c_str());
            }
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
            if (verbose) {
                printf("Selected DPB mode: %s\n", dpbMode.c_str());
            }
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
        }  else if (args[i] == "--msbShift") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%hhu", &input.msbShift) != 1)) {
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
        } else if (args[i] == "--encodeOffsetX") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%u", &encodeOffsetX) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--encodeOffsetY") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%u", &encodeOffsetY) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--encodeWidth") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%u", &encodeWidth) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--encodeHeight") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%u", &encodeHeight) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--encodeMaxWidth") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%u", &encodeMaxWidth) != 1)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--encodeMaxHeight") {
            if ((++i >= argc) || (sscanf(args[i].c_str(), "%u", &encodeMaxHeight) != 1)) {
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
            if (verbose) {
                printf("Selected gopFrameCount: %d\n", gopFrameCount);
            }
        } else if (args[i] == "--idrPeriod") {
            int32_t idrPeriod = EncoderConfig::DEFAULT_GOP_IDR_PERIOD;
            if (++i >= argc || sscanf(args[i].c_str(), "%d", &idrPeriod) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetIdrPeriod(idrPeriod);
            if (verbose) {
                printf("Selected idrPeriod: %d\n", idrPeriod);
            }
        } else if (args[i] == "--consecutiveBFrameCount") {
            uint8_t consecutiveBFrameCount = EncoderConfig::DEFAULT_CONSECUTIVE_B_FRAME_COUNT;
            if (++i >= argc || sscanf(args[i].c_str(), "%hhu", &consecutiveBFrameCount) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetConsecutiveBFrameCount(consecutiveBFrameCount);
            if (verbose) {
                printf("Selected consecutiveBFrameCount: %d\n", consecutiveBFrameCount);
            }
        } else if (args[i] == "--temporalLayerCount") {
            uint8_t temporalLayerCount = EncoderConfig::DEFAULT_TEMPORAL_LAYER_COUNT;
            if (++i >= argc || sscanf(args[i].c_str(), "%hhu", &temporalLayerCount) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetTemporalLayerCount(temporalLayerCount);
            if (verbose) {
                printf("Selected temporalLayerCount: %d\n", temporalLayerCount);
            }
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
            if (verbose) {
                printf("Selected frameTypeName: %s\n", gopStructure.GetFrameTypeName(lastFrameType));
            }
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
            } else if (tuningModeStr == "3" || tuningModeStr == "ultralowlatency") {
                tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR;
            } else if (tuningModeStr == "4" || tuningModeStr == "lossless") {
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
        }
        else if (args[i] == "--averageBitrate") {
            if (++i >= argc || sscanf(args[i].c_str(), "%u", &averageBitrate) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--maxBitrate") {
                if (++i >= argc || sscanf(args[i].c_str(), "%u", &maxBitrate) != 1) {
                    fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                    return -1;
                }
        } else if (args[i] == "--qpI") {
                if (++i >= argc || sscanf(args[i].c_str(), "%u", &constQp.qpIntra) != 1) {
                    fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                    return -1;
                }
        } else if (args[i] == "--qpP") {
                if (++i >= argc || sscanf(args[i].c_str(), "%u", &constQp.qpInterP) != 1) {
                    fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                    return -1;
                }
        } else if (args[i] == "--qpB") {
                if (++i >= argc || sscanf(args[i].c_str(), "%u", &constQp.qpInterB) != 1) {
                    fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
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
            size_t size = deviceUUID.StringToUUID(args[i].c_str());
            if (size != VK_UUID_SIZE) {
                fprintf(stderr,"Invalid deviceUuid format used: %s with size: %zu."
                               "deviceUuid must be represented by 16 hex (32 bytes) values.", args[i].c_str(), args[i].length());
                return -1;
            }
        } else if (args[i] == "--qpMap") {
            if (++i >= argc) {
                fprintf(stderr, "Invalid paramter for %s\n", args[i - 1].c_str());
                return -1;
            }
            if (args[i] == "deltaQpMap") {
                qpMapMode = DELTA_QP_MAP;
            } else if (args[i] == "emphasisMap") {
                qpMapMode = EMPHASIS_MAP;
            } else {
                fprintf(stderr, "Invalid quntization map mode %s\n", args[i].c_str());
                return -1;
            }
            enableQpMap = true;
        } else if (args[i] == "--qpMapFileName") {
            if (++i >= argc) {
                fprintf(stderr, "Invaid paramter for %s\n", args[i - 1].c_str());
                return -1;
            }
            size_t fileSize = qpMapFileHandler.SetFileName(args[i].c_str());
            if (fileSize <= 0) {
                return (int)fileSize;
            }
            enableQpMap = true;
        } else if (args[i] == "--enableHwLoadBalancing") {
            // Enables HW load balancing using multiple encoders devices when available
            enableHwLoadBalancing = true;
        } else if (args[i] == "--testOutOfOrderRecording") {
            // Testing only - don't use this feature for production!
            fprintf(stdout, "Warning: %s should only be used for testing!\n", args[i].c_str());
            enableOutOfOrderRecording = true;
        } else {
            argcount++;
            arglist.push_back(args[i].c_str());
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

    if (input.height == 0) {
        fprintf(stderr, "The height was not specified\n");
        return -1;
    }

    if (!outputFileHandler.HasFileName()) {
        const char* defaultOutName = (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) ? "out.264" :
                                     (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR) ? "out.265" : "out.ivf";
        if (verbose) {
            fprintf(stdout, "No output file name provided. Using %s.\n", defaultOutName);
        }
        size_t fileSize = outputFileHandler.SetFileName(defaultOutName);
        if (fileSize <= 0) {
            return (int)fileSize;
        }
    }

    if ((encodeWidth == 0) || (encodeWidth > input.width)) {
        encodeWidth = input.width;
    }

    if ((encodeHeight == 0) || (encodeHeight > input.height)) {
        encodeHeight = input.height;
    }

    if ((encodeMaxWidth != 0) && (encodeWidth > encodeMaxWidth)) {
        encodeWidth = encodeMaxWidth;
    }

    if ((encodeMaxHeight != 0) && (encodeHeight > encodeMaxHeight)) {
        encodeHeight = encodeMaxHeight;
    }

    if (encodeMaxWidth == 0) {
        encodeMaxWidth = encodeWidth;
    }

    if (encodeAlignedWidth == 0) {
        encodeAlignedWidth = encodeWidth;
    }

    if (encodeMaxHeight == 0) {
        encodeMaxHeight = encodeHeight;
    }

    if (encodeAlignedHeight == 0) {
        encodeAlignedHeight = encodeHeight;
    }

    if (minQp == -1) {
        if (verbose) {
            fprintf(stdout, "No QP was provided. Using default value: 20.\n");
        }
        minQp = 20;
    }

    codecBlockAlignment = H264MbSizeAlignment; // H264

    if (enableQpMap && !qpMapFileHandler.HasFileName()) {
        fprintf(stderr, "No qpMap file was provided.");
        return -1;
    }

    frameCount = inputFileHandler.GetFrameCount(input.width, input.height, input.bpp, input.chromaSubsampling);

    if (numFrames == 0 || numFrames > frameCount) {
        std::cout << "numFrames " << numFrames
                  <<  " should be different from zero and inferior to input file frame count: "
                  << frameCount << ". Use input file frame count." << std::endl;
        numFrames = frameCount;
        if (numFrames == 0) {
            fprintf(stderr, "No frames found in the input file, frame count is zero. Exit.");
            return -1;
        }
    }

    return DoParseArguments(argcount, arglist.data());
}

VkResult EncoderConfig::CreateCodecConfig(int argc, const char *argv[],
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

    } else if (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) {

        VkSharedBaseObj<EncoderConfigAV1> vkEncoderConfigAV1(new EncoderConfigAV1());
        int ret = vkEncoderConfigAV1->ParseArguments(argc, argv);
        if (ret != 0) {
            assert(!"Invalid arguments");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkResult result = vkEncoderConfigAV1->InitializeParameters();
        if (result != VK_SUCCESS) {
            assert(!"InitializeParameters failed");
            return result;
        }

        encoderConfig = vkEncoderConfigAV1;
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
    if (encodeBitDepthLuma == 0) {
        encodeBitDepthLuma = input.bpp;
    }

    if (encodeBitDepthChroma == 0) {
        encodeBitDepthChroma = encodeBitDepthLuma;
    }

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
