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
#include "json/EncoderConfigJsonLoader.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <cstdlib>
#include <cmath>
#include <cerrno>

namespace {
    // A PARSE THAT CANNOT SILENTLY CHANGE THE NUMBER IT WAS GIVEN.
    //
    // Three ways the value the caller typed can differ from the value the
    // encoder runs with, all of which report success:
    //
    //   * NARROWING. static_cast<T> of a value too large for T keeps the low
    //     bits. Into a uint8_t, 256 is 0 and 300 is 44 -- and 0 is a sentinel
    //     in more than one of these fields, so the truncation does not even
    //     land on an obviously wrong number.
    //   * A NEGATIVE INTO AN UNSIGNED. strtoull accepts a leading '-' and
    //     wraps, so "-1" arrives as the largest value T can hold rather than
    //     as an error.
    //   * OVERFLOW. Past the range of the accumulator, strtoull/strtoll
    //     saturate and set ERANGE, which nothing was reading.
    //
    // Each is refused here instead. The check that the value survives its own
    // narrowing is what lets a caller keep parsing straight into a uint8_t
    // field: out-of-range input is rejected rather than folded.
    template<typename T>
    inline bool parseUint(const std::string& str, T& value) {
        if (str.empty()) return false;
        // "-1" IS AN ACCEPTED SPELLING, and means all bits set.
        //
        // It is the Video Codec SDK convention these fields inherit: an
        // infinite GOP length is UINT32_MAX -- see the json_config README and
        // the idrPeriod derivation in EncoderConfigH264 -- and -1 is how a
        // caller writes it without counting the f's. It yields the maximum
        // value of T, which is what the unchecked wrap produced, so every
        // command line that already used it means the same thing.
        //
        // NO OTHER NEGATIVE IS. Those are typos, and wrapping one silently is
        // how a mistyped bound becomes a four-billion-frame GOP that the
        // encoder honours without comment.
        if (str == "-1") {
            value = static_cast<T>(~0ull);
            return true;
        }
        // A '-' anywhere else is malformed for an unsigned field, including
        // one behind leading whitespace that strtoull would skip past.
        if (str.find('-') != std::string::npos) return false;
        errno = 0;
        char* end = nullptr;
        unsigned long long result = strtoull(str.c_str(), &end, 0);
        if (end != str.c_str() + str.size()) return false;
        if (errno == ERANGE) return false;
        const T narrowed = static_cast<T>(result);
        if (static_cast<unsigned long long>(narrowed) != result) return false;
        value = narrowed;
        return true;
    }

    template<typename T>
    inline bool parseInt(const std::string& str, T& value) {
        if (str.empty()) return false;
        errno = 0;
        char* end = nullptr;
        long long result = strtoll(str.c_str(), &end, 10);
        if (end != str.c_str() + str.size()) return false;
        if (errno == ERANGE) return false;
        const T narrowed = static_cast<T>(result);
        if (static_cast<long long>(narrowed) != result) return false;
        value = narrowed;
        return true;
    }

    inline bool parseFloat(const std::string& str, float& value) {
        if (str.empty()) return false;
        char* end = nullptr;
        double result = strtod(str.c_str(), &end);
        if (end == str.c_str()) return false;
        value = static_cast<float>(result);
        return true;
    }

    inline bool parseHex(const std::string& str, uint32_t& value) {
        if (str.empty()) return false;
        char* end = nullptr;
        unsigned long result = strtoul(str.c_str(), &end, 16);
        if (end == str.c_str()) return false;
        value = static_cast<uint32_t>(result);
        return true;
    }

    /** Comma-separated uint32 values (decimal or 0x hex). Whitespace around commas is allowed. */
    bool parseCommaSeparatedUint32s(const std::string& str, std::vector<uint32_t>& out)
    {
        out.clear();
        std::string token;
        for (size_t i = 0; i <= str.size(); ++i) {
            if (i == str.size() || str[i] == ',') {
                size_t start = 0;
                size_t end = token.size();
                while (start < end && std::isspace(static_cast<unsigned char>(token[start]))) {
                    start++;
                }
                while (end > start && std::isspace(static_cast<unsigned char>(token[end - 1]))) {
                    end--;
                }
                if (start < end) {
                    uint32_t v = 0;
                    if (!parseHex(token.substr(start, end - start), v)) {
                        return false;
                    }
                    out.push_back(v);
                }
                token.clear();
            } else {
                token += str[i];
            }
        }
        return !out.empty();
    }
}

static void printHelp(VkVideoCodecOperationFlagBitsKHR codec)
{
    fprintf(stderr,
    "Usage : EncodeApp \n\
    -h, --help                      provides help\n\
    -i, --input                     .yuv Input YUV File Name (YUV420p 8bpp only) \n\
    -o, --output                    .264/5,ivf Output H264/5/AV1 File Name \n\
    -c, --codec                     <string> select codec type: avc (h264) or hevc (h265) or av1\n\
    --verbose                       verbose output\n\
    --psnr                          enable PSNR metrics (input vs reconstructed)\n\
    --crcInit                       <list> comma-separated CRC32 seed uint32s (decimal or 0x hex), e.g. 0xFFFFFFFF,0\n\
    --noDeviceFallback                        : don't try other GPUs if first device doesn't meet requirements \n\
    --encoderConfig                 <path>    : load base config from JSON (CLI overrides); see json_config/encoder_config.schema.json\n\
    --dpbMode                       <string>  : select DPB mode: layered, separate\n\
    --inputWidth                    <integer> : Input Width \n\
    --inputHeight                   <integer> : Input Height \n\
    --inputNumPlanes                <integer> : Number of planes \n\
    --inputChromaSubsampling        <string>  : Chromat subsapling to use, default 420 \n\
    --inputLumaPlanePitch           <integer> : Pitch for Luma plane \n\
    --inputBpp                      <integer> : Bits per pixel, default 8 \n\
    --msbShift                      <integer> : Shift the input plane pixels to the left when bpp > 8. Default is detected \n\
                                                from the data: right-aligned samples (yuv420p10le) get 16 - inputBpp, \n\
                                                already-left-aligned samples (P010/P210/P410) get 0. \n\
    --preferPackedYcbcr                none :   Prefer the packed 4:4:4 encode-source format (AYUV / Y410) when the driver \n\
                                                advertises both it and the 2-plane form for the profile. The driver lists \n\
                                                the 2-plane form first, so without this packed 4:4:4 is only reachable by \n\
                                                feeding an already-packed input file. The compute filter converts from any \n\
                                                input format and plane layout, so this works with any source. Ignored on \n\
                                                profiles with no packed form (4:2:0, 4:2:2). \n\
    --startFrame                    <integer> : Start Frame Number to be Encoded \n\
    --numFrames                     <integer> : End Frame Number to be Encoded \n\
    --repeatInputFrames                none :   Repeat the input file frame'ss sequence by reseting the the stream to the beginning \n\
                                                when the file ends. The numFrames parameter in this case can be higher than \n\
                                                the max frames contained in the file.\n\
    --encodeOffsetX                 <integer> : Encoded offset X \n\
    --encodeOffsetY                 <integer> : Encoded offset Y \n\
    --encodeWidth                   <integer> : Encoded width \n\
    --encodeHeight                  <integer> : Encoded height \n\
    --encodeMaxWidth                <integer> : Encoded max width - the maximum content width supported. Used with content resize.\n\
    --encodeMaxHeight               <integer> : Encoded max height - the maximum content height supported. Used with content resize. \n\
    --minQp                         <integer> : Minimum QP value in the range [0, 51] \n\
    --maxQp                         <integer> : Maximum QP value in the range [0, 51] \n\
    --qpMap                         <string>  : select quantization map type : deltaQpMap or emaphasisMap \n\
    --qpMapFileName                 <string>  : quantization map file name \n\
    --gopFrameCount                 <integer> : Number of frame in the GOP, default 16.\n\
                                                -1 requests an INFINITE GOP: the count is set to\n\
                                                UINT32_MAX, so the sequence opens with an IDR and\n\
                                                no second one is emitted. 0 leaves the length to\n\
                                                the device's preferred value.\n\
    --idrPeriod                     <integer> : Number of frame between 2 IDR frame, default 60.\n\
                                                -1 requests an INFINITE IDR period (UINT32_MAX):\n\
                                                no periodic IDR is emitted after the first. 0 leaves\n\
                                                the period to the device's preferred value.\n\
    --consecutiveBFrameCount        <integer> : Number of consecutive B frame count in a GOP \n\
    --temporalLayerCount            <integer> : Count of temporal layer \n\
    --lastFrameType                 <integer> : Last frame type \n\
    --closedGop                       none    : Close the Gop, default open\n\
    --qualityLevel                  <integer> : Select quality level \n\
    --usageHints                    <string> : Select encode usage hints \n\
                                        default, transcoding, streaming, recording, conferencing \n\
    --contentHints                  <string> : Select encode content hints \n\
                                        default, camera, desktop, rendered \n\
    --tuningMode                    <string> : Select tuning mode \n\
                                        default, highquality, lowlatency, ultralowlatency, lossless \n\
    --rateControlMode               <integer> or <string>: select different rate control modes: \n\
                                        default(0), disabled(1), cbr(2), vbr(4)\n\
    --averageBitrate                <integer> : Target bitrate in bits/sec for cbr/vbr RC modes\n\
                                        (e.g., 5000000 for 5 Mbps, 15000000 for 15 Mbps)\n\
    --maxBitrate                    <integer> : Peak bitrate in bits/sec for cbr/vbr RC modes\n\
    --vbvBufferSize                 <integer> : Size in bits of the VBV / HRD buffer for cbr/vbr RC modes\n\
    --qpI                           <integer> : QP or QIndex (for AV1) used for I-frames when RC disabled\n\
    --qpP                           <integer> : QP or QIndex (for AV1) used for P-frames when RC disabled\n\
    --qpB                           <integer> : QP or QIndex (for AV1) used for B-frames when RC disabled\n\
    --disableEncodeParameterOptimizations     : Disables encode parameter optimization flag bit in VkVideoSessionCreateFlagsKHR \n\
                                                when creating a video session\n\
    --deviceID                      <hexadec> : deviceID to be used, \n\
    --deviceUuid                    <string>  : deviceUuid to be used \n\
    --enableHwLoadBalancing                   : enables HW load balancing using multiple encoder devices when available \n\
    --drmFormatModifierIndex          <integer> : Use DRM format modifier at given index from non-linear modifier list.\n\
                                        Queries modifiers with VIDEO_ENCODE_SRC usage, skips LINEAR (mod=0x0).\n\
                                        -1 = disabled (default OPTIMAL), 0..N = pick by index.\n\
    --enableDebugEncoderInputDisplay  none    : Testing only - enable presenting to the display the frames input to the encoder\n\
    --testOutOfOrderRecording                 : Testing only - enable testing for out-of-order-recording\n\
    --intraRefreshCycleDuration     <integer> : Duration of (number of frames in) an intra-refresh cycle\n\
    --intraRefreshMode              <string>  : Intra-refresh mode to be used\n\
                                        picpartition, blockrows, blockcolumns, blocks\n\
    --testIntraRefreshMidway        <integer> : Index at which an intra-refresh cycle is to be interrupted.\n\
                                        This is for testing purposes only. Allowed values for this option\n\
                                        are such that 0 <= index < intraRefreshCycleDuration .\n\
                                        A value of 0 is a no-op; for other values, a new intra-refresh\n\
                                        cycle will start `index` frames into an existing intra-refresh\n\
                                        cycle and a complete cycle will be finished. This results in\n\
                                        a fully intra-refreshed frame being available after every\n\
                                        `intraRefreshCycleDuration + index` frames.\n\
    --testSkipIntraRefreshStart     <integer> : Index at which an intra-refresh cycle should start.\n\
                                        This is for testing purposes only. Allowed values for this option\n\
                                        are such that 0 <= index < intraRefreshCycleDuration .\n\
                                        A value of 0 is a no-op; for other values, a new intra-refresh\n\
                                        cycle will start with an intra-refresh index of `index` and\n\
                                        will run to completion. This will be followed by a full\n\
                                        intra-refresh cycle. This results in a fully intra-refreshed frame\n\
                                        being available after every `intraRefreshCycleDuration + index` frames.\n");

#ifdef NV_AQ_GPU_LIB_SUPPORTED
    fprintf(stderr, "\
    --spatialAQStrength             <float>   : Spatial AQ strength in range [-1.0, 1.0]\n\
                                        < -1.0 = disabled (default: -2.0)\n\
                                        0.0 = default/neutral strength\n\
                                        -1.0 = minimum, 1.0 = maximum\n\
                                        If >= -1.0, spatial AQ is enabled\n\
                                        In combined mode, ratio determines mix\n\
    --temporalAQStrength            <float>   : Temporal AQ strength in range [-1.0, 1.0]\n\
                                        < -1.0 = disabled (default: -2.0)\n\
                                        0.0 = default/neutral strength\n\
                                        -1.0 = minimum, 1.0 = maximum\n\
                                        If >= -1.0, temporal AQ is enabled\n\
                                        In combined mode, ratio determines mix\n\
    --aqDumpDir                      <string>  : Directory for AQ dump files\n\
                                        Default: ./aqDump\n");
#endif // NV_AQ_GPU_LIB_SUPPORTED

    if ((codec == VK_VIDEO_CODEC_OPERATION_NONE_KHR) || (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR)) {
        fprintf(stderr, "\nH264 specific arguments:\n\
        --slices                        <integer> : Number of slices to divide the picture into\n");
    }

    if ((codec == VK_VIDEO_CODEC_OPERATION_NONE_KHR) || (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR)) {
        fprintf(stderr, "\nH265 specific arguments:\n\
        --slices                        <integer> : Number of slices to divide the picture into\n");
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

int EncoderConfig::LoadFromJsonFile(const char* path)
{
#if defined(VK_VIDEO_ENCODER_SKIP_JSON_CONFIG)
    // JSON-config support is compiled out by this build (it
    // avoids pulling in simdjson + EncoderConfigJsonLoader.cpp
    // for a feature only the command line uses). Callers go
    // through ParseArguments, which checks the return value, so
    // -1 surfaces as a clean parse failure.
    (void)path;
    return -1;
#else
    return LoadEncoderConfigFromJson(path, this);
#endif
}

int EncoderConfig::ParseArguments(int argc, const char *argv[])
{
    int argcount = 0;
    std::vector<const char*> arglist;
    std::vector<std::string> args(argv, argv + argc);

    appName = args[0];

    const auto lambdaToLower = [](unsigned char c) { return std::tolower(c); };

    for (int32_t i = 1; i < argc; i++) {
        // --encoderConfig: load JSON first (base config); CLI args below override. Precedence: JSON then CLI.
        if (args[i] == "--encoderConfig") {
            if (i + 1 >= argc) {
                fprintf(stderr, "--encoderConfig requires a path\n");
                return -1;
            }
            if (LoadFromJsonFile(args[i + 1].c_str()) != 0) return -1;
            i++; // skip path argument (for loop will increment i again)
            continue;
        }
        if (args[i] == "-i" || args[i] == "--input") {
            if (++i >= argc) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            size_t fileSize = inputFileHandler.SetFileName(args[i].c_str());
            if (fileSize <= 0) {
                return (int)fileSize;
            }
            if (inputFileHandler.ParseY4mHeader(&input.width, &input.height, &frameRateNumerator, &frameRateDenominator)) {
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
        } else if (args[i] == "--disableFileOutput") {
            // In-memory bitstream capture: the encoded bytes are returned to
            // the caller and NO bitstream file is written, including the
            // default out.264 / out.265 / out.ivf. This is the command-line
            // spelling of EncoderConfig::disableFileOutput; an embedding host
            // sets the field instead. Either way FinalizeConfig() reads it
            // before deciding whether to open the default output, so the two
            // routes reach the same answer.
            //
            // Not a flush and not a completion signal: which frames are ready
            // is reported the same way in both modes. This decides only where
            // the bytes go -- to the caller, or additionally to a file.
            //
            // Combining it with -o still writes no file: capture wins. A
            // process that cannot touch the filesystem, such as the Chromium
            // GPU process under sandbox, needs a mode in which no path is
            // opened at all rather than one it must remember not to name.
            disableFileOutput = true;
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
            if ((++i >= argc) || !parseUint(args[i], input.width)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--inputHeight") {
            if ((++i >= argc) || !parseUint(args[i], input.height)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--inputNumPlanes") {
            if ((++i >= argc) || !parseUint(args[i], input.numPlanes)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            // 1 = packed/interleaved single plane (AYUV, Y410 -- 4:4:4 only),
            // 2 = semi-planar, 3 = planar.
            if ((input.numPlanes < 1) || (input.numPlanes > 3)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                fprintf(stderr, "Supported number of planes are 1 (packed 4:4:4), 2 or 3\n");
                // Reject rather than merely diagnose: an out-of-range plane count that
                // reaches VerifyInputs indexes planeLayouts[] past its end.
                return -1;
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
            uint64_t rowPitch = 0;
            if ((++i >= argc) || !parseUint(args[i], rowPitch)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            input.planeLayouts[0].rowPitch = rowPitch;
        }  else if (args[i] == "--inputBpp") {
            if ((++i >= argc) || !parseUint(args[i], input.bpp)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        }  else if (args[i] == "--msbShift") {
            uint8_t msbShiftVal = 0;
            if ((++i >= argc) || !parseUint(args[i], msbShiftVal)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            input.msbShift = static_cast<int8_t>(msbShiftVal);
        } else if (args[i] == "--preferPackedYcbcr") {
            preferPackedYcbcr = true;
        } else if (args[i] == "--startFrame") {
            if (++i >= argc || !parseUint(args[i], startFrame)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--numFrames") {
            if (++i >= argc || !parseUint(args[i], numFrames)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--repeatInputFrames") {
            repeatInputFrames = true;
        } else if (args[i] == "--encodeOffsetX") {
            if ((++i >= argc) || !parseUint(args[i], encodeOffsetX)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--encodeOffsetY") {
            if ((++i >= argc) || !parseUint(args[i], encodeOffsetY)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--encodeWidth") {
            if ((++i >= argc) || !parseUint(args[i], encodeWidth)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--encodeHeight") {
            if ((++i >= argc) || !parseUint(args[i], encodeHeight)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--encodeMaxWidth") {
            if ((++i >= argc) || !parseUint(args[i], encodeMaxWidth)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--encodeMaxHeight") {
            if ((++i >= argc) || !parseUint(args[i], encodeMaxHeight)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--minQp") {
            if (++i >= argc || !parseInt(args[i], minQp)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            minQpSet = 1;
        } else if (args[i] == "--maxQp") {
            if (++i >= argc || !parseInt(args[i], maxQp)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            maxQpSet = 1;
        // GOP structure
        } else if (args[i] == "--gopFrameCount") {
            // Parsed at the width the GOP structure stores it. parseUint
            // casts without a range check, so a narrower local truncates
            // silently while still reporting success: 300 arrives as 44, and
            // 256 as 0 -- which the codec configs read as
            // ZERO_GOP_FRAME_COUNT and replace with the device's preferred
            // count. Both give the caller a GOP it did not ask for and is
            // never told about.
            //
            // Zero stays legal here: it IS that sentinel, and asking for the
            // device's preference is a request like any other.
            uint32_t gopFrameCount = EncoderConfig::DEFAULT_GOP_FRAME_COUNT;
            if (++i >= argc || !parseUint(args[i], gopFrameCount)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetGopFrameCount(gopFrameCount);
            if (verbose) {
                VkEncPrintfOut("Selected gopFrameCount: %u\n", gopFrameCount);
            }
        } else if (args[i] == "--idrPeriod") {
            // Unsigned, because SetIdrPeriod stores it unsigned. Read as a
            // signed value it took every negative, and the conversion at the
            // setter turned each one into a period so large that no periodic
            // IDR is ever emitted -- so a mistyped -5 asked for an infinite
            // IDR period and got it. Parsed here at the width and signedness
            // the field actually has, -1 keeps its meaning (all bits set,
            // infinite) and other negatives are refused.
            uint32_t idrPeriod = EncoderConfig::DEFAULT_GOP_IDR_PERIOD;
            if (++i >= argc || !parseUint(args[i], idrPeriod)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetIdrPeriod(idrPeriod);
            if (verbose) {
                VkEncPrintfOut("Selected idrPeriod: %u\n", idrPeriod);
            }
        } else if (args[i] == "--consecutiveBFrameCount") {
            uint8_t consecutiveBFrameCount = EncoderConfig::DEFAULT_CONSECUTIVE_B_FRAME_COUNT;
            if (++i >= argc || !parseUint(args[i], consecutiveBFrameCount)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetConsecutiveBFrameCount(consecutiveBFrameCount);
            if (verbose) {
                VkEncPrintfOut("Selected consecutiveBFrameCount: %u\n",
                       (unsigned)consecutiveBFrameCount);
            }
        } else if (args[i] == "--temporalLayerCount") {
            uint8_t temporalLayerCount = EncoderConfig::DEFAULT_TEMPORAL_LAYER_COUNT;
            if (++i >= argc || !parseUint(args[i], temporalLayerCount)) {
                VkEncPrintfErr("invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetTemporalLayerCount(temporalLayerCount);
            if (verbose) {
                VkEncPrintfOut("Selected temporalLayerCount: %u\n",
                       (unsigned)temporalLayerCount);
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
            if (++i >= argc || !parseUint(args[i], qualityLevel)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--usageHints") {
            if (++i >= argc) {
                fprintf(stderr, "Invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            std::string encodeUsageStr = argv[i];
            std::transform(encodeUsageStr.begin(), encodeUsageStr.end(), encodeUsageStr.begin(), lambdaToLower);
            if (encodeUsageStr == "default") {
                encodeUsageHints = VK_VIDEO_ENCODE_USAGE_DEFAULT_KHR;
            } else if (encodeUsageStr == "transcoding") {
                encodeUsageHints = VK_VIDEO_ENCODE_USAGE_TRANSCODING_BIT_KHR;
            } else if (encodeUsageStr == "streaming") {
                encodeUsageHints = VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR;
            } else if (encodeUsageStr == "recording") {
                encodeUsageHints = VK_VIDEO_ENCODE_USAGE_RECORDING_BIT_KHR;
            } else if (encodeUsageStr == "conferencing") {
                encodeUsageHints = VK_VIDEO_ENCODE_USAGE_CONFERENCING_BIT_KHR;
            } else {
                fprintf(stderr, "Invalid encodeUsage: %s\n", encodeUsageStr.c_str());
                return -1;
            }
        } else if (args[i] == "--contentHints") {
            if (++i >= argc) {
                fprintf(stderr, "Invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            std::string encodeContentStr = argv[i];
            std::transform(encodeContentStr.begin(), encodeContentStr.end(), encodeContentStr.begin(), lambdaToLower);
            if (encodeContentStr == "default") {
                encodeContentHints = VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR;
            } else if (encodeContentStr == "camera") {
                encodeContentHints = VK_VIDEO_ENCODE_CONTENT_CAMERA_BIT_KHR;
            } else if (encodeContentStr == "desktop") {
                encodeContentHints = VK_VIDEO_ENCODE_CONTENT_DESKTOP_BIT_KHR;
            } else if (encodeContentStr == "rendered") {
                encodeContentHints = VK_VIDEO_ENCODE_CONTENT_RENDERED_BIT_KHR;
            } else {
                fprintf(stderr, "Invalid encodeContent: %s\n", encodeContentStr.c_str());
                return -1;
            }
        } else if (args[i] == "--tuningMode") {
            if (++i >= argc) {
                fprintf(stderr, "Invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            std::string tuningModeStr = argv[i];
            std::transform(tuningModeStr.begin(), tuningModeStr.end(), tuningModeStr.begin(), lambdaToLower);
            if (tuningModeStr == "default") {
                tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR;
            } else if (tuningModeStr == "highquality") {
                tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR;
            } else if (tuningModeStr == "lowlatency") {
                tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR;
            } else if (tuningModeStr == "ultralowlatency") {
                tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR;
            } else if (tuningModeStr == "lossless") {
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
            } else {
                // Invalid rateControlMode
                fprintf(stderr, "Invalid rateControlMode: %s\n", rc.c_str());
                return -1;
            }
        }
        else if (args[i] == "--averageBitrate") {
            if (++i >= argc || !parseUint(args[i], averageBitrate)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--maxBitrate") {
                if (++i >= argc || !parseUint(args[i], maxBitrate)) {
                    fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                    return -1;
                }
        } else if (args[i] == "--vbvBufferSize") {
            if (++i >= argc || !parseUint(args[i], vbvBufferSize)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
        } else if (args[i] == "--qpI") {
                if (++i >= argc || !parseUint(args[i], constQp.qpIntra)) {
                    fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                    return -1;
                }
        } else if (args[i] == "--qpP") {
                if (++i >= argc || !parseUint(args[i], constQp.qpInterP)) {
                    fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                    return -1;
                }
        } else if (args[i] == "--qpB") {
                if (++i >= argc || !parseUint(args[i], constQp.qpInterB)) {
                    fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                    return -1;
                }
        } else if (args[i] == "--disableEncodeParameterOptimizations") {
            disableEncodeParameterOptimizations = true;
        } else if (args[i] == "--drmFormatModifierIndex") {
            int32_t idx = -1;
            if ((++i >= argc) || !parseUint(args[i], idx)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            drmFormatModifierIndex = idx;
        } else if (args[i] == "--deviceID") {
            uint32_t deviceIdVal = 0;
            if ((++i >= argc) || !parseHex(args[i], deviceIdVal)) {
                 fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                 return -1;
             }
            deviceId = static_cast<int32_t>(deviceIdVal);
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
        } else if (args[i] == "--syncAssembly") {
            asyncAssembly = false;
        } else if (args[i] == "--assemblyThreads") {
            if (++i >= argc) {
                fprintf(stderr, "Invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            uint32_t val = 0;
            if (!parseUint(args[i], val) || val == 0 || val > 16) {
                fprintf(stderr, "Invalid value for --assemblyThreads (1..16)\n");
                return -1;
            }
            assemblyThreadCount = val;
        } else if (args[i] == "--testOutOfOrderRecording") {
            // Testing only - don't use this feature for production!
            fprintf(stdout, "Warning: %s should only be used for testing!\n", args[i].c_str());
            enableOutOfOrderRecording = true;
        } else if (args[i] == "--enableDebugEncoderInputDisplay") {
            fprintf(stdout, "Warning: %s Enabling the display for testing encoder input frames!\n", args[i].c_str());
            enableFramePresent = true;
        } else if (args[i] == "--intraRefreshCycleDuration") {
            if (++i >= argc || !parseUint(args[i], intraRefreshCycleDuration)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetIntraRefreshCycleDuration(intraRefreshCycleDuration);
            if (verbose) {
                printf("Selected intraRefreshCycleDuration: %d\n", intraRefreshCycleDuration);
            }
        } else if (args[i] == "--intraRefreshMode") {
            if (++i >= argc) {
                fprintf(stderr, "Invalid paramter for %s\n", args[i - 1].c_str());
                return -1;
            }

            if (args[i] == "picpartition") {
                intraRefreshMode = REFRESH_PER_PARTITION;
            } else if (args[i] == "blockrows") {
                intraRefreshMode = REFRESH_BLOCK_ROWS;
            } else if (args[i] == "blockcolumns") {
                intraRefreshMode = REFRESH_BLOCK_COLUMNS;
            } else if (args[i] == "blocks") {
                intraRefreshMode = REFRESH_BLOCKS;
            } else {
                fprintf(stderr, "Invalid intra-refresh mode %s\n", args[i].c_str());
                return -1;
            }
        } else if (args[i] == "--testIntraRefreshMidway") {
            // Testing only - don't use this feature for production!
            fprintf(stdout, "Warning: %s should only be used for testing!\n", args[i].c_str());
            if (++i >= argc || !parseUint(args[i], intraRefreshCycleRestartIndex)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetIntraRefreshCycleRestartIndex(intraRefreshCycleRestartIndex);
        } else if (args[i] == "--testSkipIntraRefreshStart") {
            // Testing only - don't use this feature for production!
            fprintf(stdout, "Warning: %s should only be used for testing!\n", args[i].c_str());
            if (++i >= argc || !parseUint(args[i], intraRefreshSkippedStartIndex)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            gopStructure.SetIntraRefreshSkippedStartIndex(intraRefreshSkippedStartIndex);
#ifdef NV_AQ_GPU_LIB_SUPPORTED
        } else if (args[i] == "--spatialAQStrength") {
            if (++i >= argc || !parseFloat(args[i], spatialAQStrength)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            // Valid range is [-1.0, 1.0], but values < -1.0 mean disabled
            if (spatialAQStrength > 1.0f) {
                fprintf(stderr, "spatialAQStrength must be <= 1.0 (use < -1.0 to disable)\n");
                return -1;
            }
            // Only enable if value is in valid range [-1.0, 1.0]
            if (spatialAQStrength >= -1.0f) {
                enableAQ = VK_TRUE;
                enableQpMap = VK_TRUE;
                qpMapMode = DELTA_QP_MAP;
            }
        } else if (args[i] == "--temporalAQStrength") {
            if (++i >= argc || !parseFloat(args[i], temporalAQStrength)) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            // Valid range is [-1.0, 1.0], but values < -1.0 mean disabled
            if (temporalAQStrength > 1.0f) {
                fprintf(stderr, "temporalAQStrength must be <= 1.0 (use < -1.0 to disable)\n");
                return -1;
            }
            // Only enable if value is in valid range [-1.0, 1.0]
            if (temporalAQStrength >= -1.0f) {
                enableAQ = VK_TRUE;
                enableQpMap = VK_TRUE;
                qpMapMode = DELTA_QP_MAP;
            }
        } else if (args[i] == "--aqDumpDir") {
            if (++i >= argc) {
                fprintf(stderr, "invalid parameter for %s\n", args[i - 1].c_str());
                return -1;
            }
            aqDumpDir = args[i];
#endif // NV_AQ_GPU_LIB_SUPPORTED
        } else if (args[i] == "--psnr") {
            enablePsnrMetrics = 1;
        } else if (args[i] == "--crcInit") {
            if (++i >= argc) {
                fprintf(stderr, "--crcInit requires a comma-separated list of uint32 values\n");
                return -1;
            }
            if (!parseCommaSeparatedUint32s(args[i], crcInitValue)) {
                fprintf(stderr, "Invalid --crcInit value (use comma-separated decimal or 0x hex uint32s): %s\n",
                        args[i].c_str());
                return -1;
            }
        } else if (args[i] == "--verbose") {
            verbose = true;
        } else if (args[i] == "--noDeviceFallback") {
            noDeviceFallback = true;
        } else {
            argcount++;
            arglist.push_back(args[i].c_str());
        }
    }

    {
        // Derived defaults + validation, shared with the direct-binding path.
        const int finalizeResult = FinalizeConfig();
        if (finalizeResult != 0) {
            return finalizeResult;
        }
    }

    return DoParseArguments(argcount, arglist.data());
}

int EncoderConfig::FinalizeConfig(const EncoderConfig::DeviceCapabilities* deviceCaps)
{
    // Extracted ParseArguments tail: runs identically after argv parsing and
    // after direct field binding (CreateCodecConfigDirect path).
    uint32_t frameCount = 0;

    // External frame input mode (IPC/service): no -i file, width/height come from caller.
    // The encoder library's InitializeExt path sets a large finite numFrames
    // (see the ext streaming config) and provides frames via
    // SetExternalInputFrame/SubmitExternalFrame -- there is no UINT32_MAX
    // sentinel on this path.
    if (!inputFileHandler.HasFileName()) {
        if (input.width == 0 || input.height == 0) {
            VkEncPrintfErr("An input file (-i) or --inputWidth/--inputHeight must be specified\n");
            return -1;
        }
        // External frame mode: skip file handler setup, use provided dimensions
        inputFileHandler.SetFrameGeometry(input.width, input.height, input.bpp, input.chromaSubsampling);
        // frameCount stays at the value from --numFrames (or default)
    } else {
        if (input.width == 0) {
            fprintf(stderr, "The input width must be specified\n");
            return -1;
        }

        if (input.height == 0) {
            fprintf(stderr, "The input height must specified\n");
            return -1;
        }

        inputFileHandler.SetFrameGeometry(input.width, input.height, input.bpp, input.chromaSubsampling);

        frameCount = inputFileHandler.GetMaxFrameCount();
    }

    if (startFrame > 0) {
        if (startFrame >= frameCount) {
            std::cout << "startFrame " << startFrame
                      <<  " must be inferior to input file max frame count of "
                      << frameCount << ". Reseting startFrame to 0." << std::endl;
            startFrame = 0;
        } else {
            inputFileHandler.ResetFrameOffset(startFrame);
        }
    }

    if (inputFileHandler.HasFileName()) {
        // File-based input: clamp numFrames to actual file frame count
        if ((repeatInputFrames == false) &&
                ((numFrames == 0) || (numFrames > (frameCount - startFrame)))) {
            std::cout << "numFrames " << numFrames
                      <<  " should be different from zero and inferior to input file max frame count of "
                      << frameCount << ". Using input file frame count." << std::endl;
            numFrames = frameCount;
            if (numFrames == 0) {
                fprintf(stderr, "No frames found in the input file, frame count is zero. Exit.");
                return -1;
            }
        }
    }
    // External frame input: numFrames comes from the --numFrames arg; the ext
    // streaming path passes a large finite count rather than a sentinel.

    // No default output file in capture mode. SetFileName() fopen()s
    // immediately, so the guard belongs here, during parsing, rather than
    // after CreateCodecConfig returns: otherwise a host that writes no file
    // still gets a 0-byte out.264/.265/.ivf in its working directory, which
    // a process confined to a sandbox may not be permitted to create at all.
    if (!disableFileOutput && !outputFileHandler.HasFileName()) {
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
            VkEncPrintfOut("No QP was provided. Using default value: 20.\n");
        }
        minQp = 20;
        // NOT PUSHED INTO RATE CONTROL, and deliberately so. Every consumer of
        // this field reads it only under minQpSet -- which stays clear on this
        // path, because nobody asked for a QP -- so an unset minQp reaches the
        // driver as "no clamp" rather than as 20. Validating or clamping the
        // value here against the device would therefore decide nothing: the
        // number is a documented default, not a request. A caller that wants a
        // QP clamp sets one, and the codec configs check THAT against the
        // device window and refuse it rather than narrowing it silently.
    }

    // Carried, not consumed: nothing in the tree reads codecBlockAlignment. It
    // holds the H.264 macroblock size for every codec, which is wrong for H.265
    // CTBs and AV1 superblocks, so a reader would have to derive it from the
    // device's VkVideoCapabilitiesKHR::pictureAccessGranularity rather than trust
    // this. Left as it stands rather than given a per-codec value that nothing
    // would check.
    codecBlockAlignment = H264MbSizeAlignment;

    if (enableQpMap && !qpMapFileHandler.HasFileName() && !enableAQ) {
        VkEncPrintfErr("No qpMap file was provided.");
        return -1;
    }

    if ((intraRefreshMode == REFRESH_NONE && intraRefreshCycleDuration > 0) ||
        (intraRefreshMode != REFRESH_NONE && intraRefreshCycleDuration == 0)) {

        VkEncPrintfErr("Both --intraRefreshMode and --intraRefreshCycleDuration must be "
                        "specified to enable intra-refresh.\n");
        return -1;
    }

    enableIntraRefresh = (intraRefreshMode != REFRESH_NONE) && (intraRefreshCycleDuration > 0);

    // Intra refresh is a device FEATURE, not a command-line one. Asking for it on
    // a device that does not expose it is refused here, where the request is
    // still attributable, rather than inside session creation. Without a device
    // to ask, the request stands and the session decides.
    if (enableIntraRefresh && (deviceCaps != nullptr) && !deviceCaps->intraRefreshSupported) {
        VkEncPrintfErr("Intra refresh was requested, but this device does not expose "
                "VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR::videoEncodeIntraRefresh.\n");
        return -1;
    }

    if (!enableIntraRefresh && intraRefreshCycleRestartIndex > 0) {
        VkEncPrintfErr("Intra-refresh must be enabled when using --testIntraRefreshMidway\n");
        return -1;
    }

    if (enableIntraRefresh && intraRefreshCycleRestartIndex >= intraRefreshCycleDuration) {
        VkEncPrintfErr("The value specified for --testIntraRefreshMidway must be in "
                        "the range [0, intraRefreshCycleDuration-1]\n");
        return -1;
    }

    if (!enableIntraRefresh && intraRefreshSkippedStartIndex > 0) {
        VkEncPrintfErr("Intra-refresh must be enabled when using --testSkipIntraRefreshStart\n");
        return -1;
    }

    if (enableIntraRefresh && intraRefreshSkippedStartIndex >= intraRefreshCycleDuration) {
        VkEncPrintfErr("The value specified for --testSkipIntraRefreshStart must be in "
                        "the range [0, intraRefreshCycleDuration-1]\n");
        return -1;
    }

    if (intraRefreshCycleRestartIndex > 0 && intraRefreshSkippedStartIndex > 0) {
        VkEncPrintfErr("Combining --testIntraRefreshMidway with --testSkipIntraRefreshStart "
                        "is not supported\n");
        return -1;
    }

    return 0;
}

VkResult EncoderConfig::CreateCodecConfigDirect(
    VkVideoCodecOperationFlagBitsKHR codecOperation,
    VkSharedBaseObj<EncoderConfig>& encoderConfig)
{
    switch ((uint32_t)codecOperation) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR: {
            VkSharedBaseObj<EncoderConfigH264> config(new EncoderConfigH264());
            encoderConfig = config;
        } break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR: {
            VkSharedBaseObj<EncoderConfigH265> config(new EncoderConfigH265());
            encoderConfig = config;
        } break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR: {
            VkSharedBaseObj<EncoderConfigAV1> config(new EncoderConfigAV1());
            encoderConfig = config;
        } break;
        default:
            VkEncPrintfErr("[EncoderConfig] CreateCodecConfigDirect: unsupported codec 0x%x\n",
                    (unsigned)codecOperation);
            return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }
    encoderConfig->codec = codecOperation;
    return VK_SUCCESS;
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
            std::string argDump;
            for (int a = 0; a < argc; a++) {
                argDump += " ";
                argDump += argv[a];
            }
            fprintf(stderr, "[EncoderConfig] H264 ParseArguments failed (ret=%d). argc=%d. Args:%s\n", ret, argc, argDump.c_str());
            fflush(stderr);
            // Don't assert — return error so caller can handle gracefully
            return VK_ERROR_INITIALIZATION_FAILED;
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

        if (getenv("VKENC_DEBUG_PSNR")) {
            vkEncoderConfigh265->enablePsnrMetrics = 1;
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

    // Get the codec-specific profile (already set by InitProfileLevel)
    uint32_t codecProfile = GetCodecProfile();

    VkVideoEncodeUsageInfoKHR encodeUsageInfo = {};
    encodeUsageInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR;
    encodeUsageInfo.pNext = NULL;
    encodeUsageInfo.videoUsageHints = encodeUsageHints;
    encodeUsageInfo.videoContentHints = encodeContentHints;
    encodeUsageInfo.tuningMode = tuningMode;

    if (verbose) fprintf(stderr, "[EncoderConfig] VkVideoEncodeUsageInfoKHR:\n"
            "  videoUsageHints   = 0x%x%s\n"
            "  videoContentHints = 0x%x\n"
            "  tuningMode        = %d (%s)\n",
            encodeUsageHints,
            (encodeUsageHints & VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR) ? " (STREAMING)" : "",
            encodeContentHints,
            tuningMode,
            (tuningMode == VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR)            ? "DEFAULT" :
            (tuningMode == VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR)       ? "HIGH_QUALITY" :
            (tuningMode == VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR)        ? "LOW_LATENCY" :
            (tuningMode == VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR)  ? "ULTRA_LOW_LATENCY" :
            (tuningMode == VK_VIDEO_ENCODE_TUNING_MODE_LOSSLESS_KHR)           ? "LOSSLESS" : "UNKNOWN");

    // Create video profile with the codec-specific profile
    videoCoreProfile = VkVideoCoreProfile(codec, encodeChromaSubsampling,
                                          GetComponentBitDepthFlagBits(encodeBitDepthLuma),
                                          GetComponentBitDepthFlagBits(encodeBitDepthChroma),
                                          codecProfile,
                                          VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR, // interlaced video is not supported with encode
                                          encodeUsageInfo);
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
