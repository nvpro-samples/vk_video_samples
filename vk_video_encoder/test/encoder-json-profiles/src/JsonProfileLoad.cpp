/*
 * Copyright 2026 NVIDIA Corporation.
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

// vk-video-encoder-json-load loads each JSON file it is given through the encoder library's
// loader (LoadEncoderConfigFromJson, which --encoderConfig calls) into a fresh configuration, with
// no Vulkan device, and writes one line per file: the loader's result and the settings the file
// can set, with the names its keys have. scripts/encoder_json_profiles.py compares them with the
// files. The configuration is EncoderConfigAV1's: the loader writes only the base configuration.
//
// vk-video-encoder-json-load --argv <encoder arguments> creates the configuration of an encoder
// command line, as the encoder does (EncoderConfig::CreateCodecConfig), and writes its line, with
// the result as rc and the codec configuration created (config=h264|h265|av1, none when none is).
// For an H.264 or H.265 configuration the line ends with the GOP length and IDR period its rate
// control states (rcGopFrameCount, rcIdrPeriod), from GetRateControlParameters.

#include <cstdio>
#include <vector>

#include "VkVideoEncoder/VkEncoderConfigAV1.h"
#include "VkVideoEncoder/VkEncoderConfigH264.h"
#include "VkVideoEncoder/VkEncoderConfigH265.h"
#include "json/EncoderConfigJsonLoader.h"

namespace {

const char* CodecName(VkVideoCodecOperationFlagBitsKHR codec) {
    switch (codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            return "h264";
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            return "h265";
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            return "av1";
        default:
            return "none";
    }
}

const char* RcModeName(VkVideoEncodeRateControlModeFlagBitsKHR mode) {
    switch (mode) {
        case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR:
            return "cqp";
        case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR:
            return "cbr";
        case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR:
            return "vbr";
        default:
            return "unset";
    }
}

const char* TuningModeName(VkVideoEncodeTuningModeKHR mode) {
    switch (mode) {
        case VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR:
            return "default";
        case VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR:
            return "highquality";
        case VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR:
            return "lowlatency";
        case VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR:
            return "ultralowlatency";
        case VK_VIDEO_ENCODE_TUNING_MODE_LOSSLESS_KHR:
            return "lossless";
        default:
            return "unknown";
    }
}

const char* ConfigName(EncoderConfig* config) {
    if (config == nullptr) {
        return "none";
    }
    if (config->GetEncoderConfigh264() != nullptr) {
        return "h264";
    }
    if (config->GetEncoderConfigh265() != nullptr) {
        return "h265";
    }
    if (config->GetEncoderConfigAV1() != nullptr) {
        return "av1";
    }
    return "none";
}

// Starts the line of one configuration; the caller ends it.
void PrintConfig(const char* file, int rc, EncoderConfig* config) {
    printf(
        "file=%s rc=%d config=%s codec=%s rcMode=%s averageBitrate=%u maxBitrate=%u vbvBufferSize=%u constQpI=%u "
        "constQpP=%u constQpB=%u gopLength=%u bFrameCount=%u idrPeriod=%u closedGop=%d frameRateNum=%u frameRateDen=%u "
        "qualityPreset=%u tuningMode=%s minQp=%d minQpSet=%u maxQp=%d maxQpSet=%u",
        file, rc, ConfigName(config), CodecName(config->codec), RcModeName(config->rateControlMode), config->averageBitrate,
        config->maxBitrate, config->vbvBufferSize, config->constQp.qpIntra, config->constQp.qpInterP, config->constQp.qpInterB,
        config->gopStructure.GetGopFrameCount(), static_cast<uint32_t>(config->gopStructure.GetConsecutiveBFrameCount()),
        config->gopStructure.GetIdrPeriod(), config->gopStructure.IsClosedGop() ? 1 : 0, config->frameRateNumerator,
        config->frameRateDenominator, config->qualityLevel, TuningModeName(config->tuningMode), config->minQp,
        static_cast<uint32_t>(config->minQpSet), config->maxQp, static_cast<uint32_t>(config->maxQpSet));
}

// The rate control's GOP length and IDR period, on the line PrintConfig starts.
void PrintRateControlGop(EncoderConfig* config) {
    VkVideoEncodeRateControlInfoKHR rcInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR};
    VkVideoEncodeRateControlLayerInfoKHR rcLayer{VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR};
    if (EncoderConfigH264* h264 = config->GetEncoderConfigh264()) {
        VkVideoEncodeH264RateControlInfoKHR rc{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR};
        VkVideoEncodeH264RateControlLayerInfoKHR layer{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR};
        h264->GetRateControlParameters(&rcInfo, &rcLayer, &rc, &layer);
        printf(" rcGopFrameCount=%u rcIdrPeriod=%u", rc.gopFrameCount, rc.idrPeriod);
    } else if (EncoderConfigH265* h265 = config->GetEncoderConfigh265()) {
        VkVideoEncodeH265RateControlInfoKHR rc{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR};
        VkVideoEncodeH265RateControlLayerInfoKHR layer{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR};
        h265->GetRateControlParameters(&rcInfo, &rcLayer, &rc, &layer);
        printf(" rcGopFrameCount=%u rcIdrPeriod=%u", rc.gopFrameCount, rc.idrPeriod);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <encoder config JSON>...\n       %s --argv <encoder arguments>...\n", argv[0], argv[0]);
        return 2;
    }
    if (std::string(argv[1]) == "--argv") {
        std::vector<const char*> args = {"vk-video-enc-test"};
        args.insert(args.end(), argv + 2, argv + argc);
        VkSharedBaseObj<EncoderConfig> config;
        const VkResult result = EncoderConfig::CreateCodecConfig(static_cast<int>(args.size()), args.data(), config);
        if (config) {
            PrintConfig("argv", result, config.get());
            PrintRateControlGop(config.get());
            printf("\n");
        } else {
            printf("file=argv rc=%d config=none\n", result);
        }
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        VkSharedBaseObj<EncoderConfig> config(new EncoderConfigAV1());
        const int rc = LoadEncoderConfigFromJson(argv[i], config.get());
        PrintConfig(argv[i], rc, config.get());
        printf("\n");
    }
    return 0;
}
