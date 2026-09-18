/*
 * Copyright 2024-2025 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "json/EncoderConfigJsonLoader.h"
#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkVideoEncoder/VkVideoGopStructure.h"
#include "simdjson.h"
#include <cstdio>
#include <cstring>
#include <string>

static VkVideoCodecOperationFlagBitsKHR codecFromString(std::string_view s) {
    if (s == "h264" || s == "264") return VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    if (s == "h265" || s == "hevc" || s == "265") return VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    if (s == "av1") return VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    return VK_VIDEO_CODEC_OPERATION_NONE_KHR;
}

static VkVideoEncodeRateControlModeFlagBitsKHR rcModeFromString(std::string_view s) {
    if (s == "cqp") return VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
    if (s == "cbr") return VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    if (s == "vbr") return VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
    return VK_VIDEO_ENCODE_RATE_CONTROL_MODE_FLAG_BITS_MAX_ENUM_KHR;
}

int LoadEncoderConfigFromJson(const char* path, void* encoderConfig) {
    EncoderConfig* config = static_cast<EncoderConfig*>(encoderConfig);
    if (!config || !path) return -1;

    simdjson::padded_string padded;
    if (simdjson::padded_string::load(path).get(padded)) {
        fprintf(stderr, "EncoderConfig JSON: cannot load file: %s\n", path);
        return -1;
    }
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) {
        fprintf(stderr, "EncoderConfig JSON: parse error: %s\n", path);
        return -1;
    }
    simdjson::ondemand::object obj;
    if (doc.get_object().get(obj)) return -1;

    for (auto field : obj) {
        simdjson::ondemand::raw_json_string key;
        simdjson::error_code key_err = field.key().get(key);
        if (key_err) continue;
        simdjson::ondemand::value val;
        if (field.value().get(val)) continue;

        if (key.is_equal("codec")) {
            std::string_view s;
            if (val.get_string().get(s) == simdjson::SUCCESS) config->codec = codecFromString(s);
        } else if (key.is_equal("outputPath")) {
            std::string_view s;
            if (val.get_string().get(s) == simdjson::SUCCESS && !s.empty())
                config->outputFileHandler.SetFileName(std::string(s).c_str());
        } else if (key.is_equal("encodeWidth")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 1) config->encodeWidth = (uint32_t)v;
        } else if (key.is_equal("encodeHeight")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 1) config->encodeHeight = (uint32_t)v;
        } else if (key.is_equal("rcMode")) {
            std::string_view s;
            if (val.get_string().get(s) == simdjson::SUCCESS) config->rateControlMode = rcModeFromString(s);
        } else if (key.is_equal("averageBitrate")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 0) config->averageBitrate = (uint32_t)v;
        } else if (key.is_equal("maxBitrate")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 0) config->maxBitrate = (uint32_t)v;
        } else if (key.is_equal("vbvBufferSize")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 0) config->vbvBufferSize = (uint32_t)v;
        } else if (key.is_equal("constQpI")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= -1 && v <= 51) config->constQp.qpIntra = (uint32_t)v;
        } else if (key.is_equal("constQpP")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= -1 && v <= 51) config->constQp.qpInterP = (uint32_t)v;
        } else if (key.is_equal("constQpB")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= -1 && v <= 51) config->constQp.qpInterB = (uint32_t)v;
        } else if (key.is_equal("qp")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= -1 && v <= 51) {
                config->constQp.qpIntra = config->constQp.qpInterP = (uint32_t)v;
            }
        } else if (key.is_equal("minQp")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= -1 && v <= 51) config->minQp = (int32_t)v;
        } else if (key.is_equal("maxQp")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= -1 && v <= 51) config->maxQp = (int32_t)v;
        } else if (key.is_equal("gopLength")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 0) config->gopStructure.SetGopFrameCount((uint32_t)v);
        } else if (key.is_equal("bFrameCount")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 0 && v <= 255) config->gopStructure.SetConsecutiveBFrameCount((uint8_t)v);
        } else if (key.is_equal("idrPeriod")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 0) config->gopStructure.SetIdrPeriod((uint32_t)v);
        } else if (key.is_equal("closedGop")) {
            bool b;
            if (val.get_bool().get(b) == simdjson::SUCCESS && b) config->gopStructure.SetClosedGop();
        } else if (key.is_equal("frameRateNum")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 1) config->frameRateNumerator = (uint32_t)v;
        } else if (key.is_equal("frameRateDen")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 1) config->frameRateDenominator = (uint32_t)v;
        } else if (key.is_equal("qualityPreset")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 0 && v <= 7) config->qualityLevel = (uint32_t)v;
        } else if (key.is_equal("colourPrimaries")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 0 && v <= 255) config->colour_primaries = (uint8_t)v;
        } else if (key.is_equal("transferCharacteristics")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 0 && v <= 255) config->transfer_characteristics = (uint8_t)v;
        } else if (key.is_equal("matrixCoefficients")) {
            int64_t v;
            if (val.get_int64().get(v) == simdjson::SUCCESS && v >= 0 && v <= 255) config->matrix_coefficients = (uint8_t)v;
        } else if (key.is_equal("videoFullRange")) {
            bool b;
            if (val.get_bool().get(b) == simdjson::SUCCESS) config->video_full_range_flag = b ? 1 : 0;
        } else if (key.is_equal("verbose")) {
            bool b;
            if (val.get_bool().get(b) == simdjson::SUCCESS) config->verbose = b;
        } else if (key.is_equal("validate")) {
            bool b;
            if (val.get_bool().get(b) == simdjson::SUCCESS) config->validate = b;
        } else if (key.is_equal("psnr")) {
            bool b;
            if (val.get_bool().get(b) == simdjson::SUCCESS) {
                config->enablePsnrMetrics = b ? 1u : 0u;
            }
        } else if (key.is_equal("crcInit")) {
            simdjson::ondemand::array arr;
            if (val.get_array().get(arr) == simdjson::SUCCESS) {
                config->crcInitValue.clear();
                for (auto elem : arr) {
                    int64_t v = 0;
                    if (elem.get_int64().get(v) == simdjson::SUCCESS) {
                        config->crcInitValue.push_back(static_cast<uint32_t>(v));
                    }
                }
            }
        }
    }

    // ---- Colour description: RAISE THE PRESENCE FLAGS -------------------
    //
    // WHAT THIS FIXES, and it is an A1-doctrine violation rather than a
    // missing feature: the four keys above wrote colour_primaries,
    // transfer_characteristics, matrix_coefficients and video_full_range_flag
    // and NOTHING ELSE. Every VUI writer in this library gates on the
    // PRESENCE flags -- VkEncoderConfigH264.cpp and VkEncoderConfigH265.cpp
    // both open their colour block with `if (!!color_description_present_flag)`
    // and their range block with video_signal_type_present_flag, and
    // EncoderConfigAV1 keys its own description on the same flag -- so a JSON
    // config that declared BT.2020 was ACCEPTED AND SILENTLY IGNORED. The CLI
    // reported success and the bitstream carried no colour description at all.
    //
    // It is also the only route by which matrix_coefficients could ever have
    // been non-zero WITHOUT the presence flag, which is the one state
    // EncoderConfig::ResolveRgbToYcbcrMatrix's `case 0` arm is written to
    // defend against. Closing it here is what makes that arm's "unreachable
    // through every producer in this tree" comment true.
    //
    // THE RULE IS THE EXT BINDER'S, DELIBERATELY, so the two entry points
    // cannot disagree about what a JSON file and a chained struct mean by the
    // same numbers (vulkan_video_encoder_ext.cpp, grep
    // `0 IS "NOT SUPPLIED" ON THIS SURFACE`):
    //   * 0 in any of the three idc fields means NOT SUPPLIED, not
    //     Reserved/Identity. It costs the ability to REQUEST code point 0
    //     through JSON, which is deliberate: this encoder has no Identity
    //     path to offer.
    //   * an unsupplied field becomes 2 (Unspecified) -- the code point that
    //     MEANS "not stated" in H.264, H.265 and AV1 alike -- so a partially
    //     supplied declaration stays truthful in every field instead of
    //     asserting Reserved by omission.
    //   * video_signal_type travels ALONE when only the range was declared,
    //     because on H.26x the range lives under a different presence flag.
    //   * video_format 5 = "unspecified" (Rec. ITU-T H.264 Table E-2), the
    //     correct value when a caller communicates colorimetry only.
    //
    // AFTER THE KEY LOOP, not inside it, because "was anything supplied" is a
    // question about the whole object and the keys may arrive in any order.
    {
        const bool anyColourIdcSupplied = (config->colour_primaries != 0) ||
                                          (config->transfer_characteristics != 0) ||
                                          (config->matrix_coefficients != 0);
        const uint8_t kColourIdcUnspecified = 2;
        if (anyColourIdcSupplied || (config->video_full_range_flag != 0)) {
            config->video_format                   = 5;
            config->video_signal_type_present_flag = 1;
        }
        if (anyColourIdcSupplied) {
            if (config->colour_primaries == 0) {
                config->colour_primaries = kColourIdcUnspecified;
            }
            if (config->transfer_characteristics == 0) {
                config->transfer_characteristics = kColourIdcUnspecified;
            }
            if (config->matrix_coefficients == 0) {
                config->matrix_coefficients = kColourIdcUnspecified;
            }
            config->color_description_present_flag = 1;
        }
    }

    return 0;
}
