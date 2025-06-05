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

#include "VkVideoEncoder/VkEncoderConfigH264.h"

StdVideoH264LevelIdc EncoderConfigH264::DetermineLevel(uint8_t dpbSize,
                                                       uint32_t bitrate,
                                                       uint32_t _vbvBufferSize,
                                                       double frameRate)
{

    uint32_t frameSizeInMbs = pic_width_in_mbs * pic_height_in_map_units;
    for (uint32_t idx = 0; idx < levelLimitsSize; idx++) {
        if ((frameSizeInMbs * frameRate) > levelLimits[idx].maxMBPS) continue;

        if ((frameSizeInMbs) > ((uint32_t)levelLimits[idx].maxFS)) continue;
        if ((frameSizeInMbs * numRefFrames * 384) > levelLimits[idx].maxDPB * 1024) continue;

        if ((bitrate != 0) && (bitrate > ((uint32_t)levelLimits[idx].maxBR * 1200))) continue;
        if ((_vbvBufferSize != 0) && (_vbvBufferSize > ((uint32_t)levelLimits[idx].maxCPB * 1200))) continue;

        return levelLimits[idx].level;
    }

    assert(!"Invalid h264_level");
    return STD_VIDEO_H264_LEVEL_IDC_INVALID;
}

void EncoderConfigH264::SetAspectRatio(StdVideoH264SequenceParameterSetVui *vui,
                                       int32_t width, int32_t height,
                                       int32_t darWidth, int32_t darHeight) {

    static const struct {
        int32_t width;
        int32_t height;
        StdVideoH264AspectRatioIdc ratio;
    } sar_table[] = {{ 1, 1, STD_VIDEO_H264_ASPECT_RATIO_IDC_SQUARE},   {12, 11, STD_VIDEO_H264_ASPECT_RATIO_IDC_12_11},
                     { 10, 11, STD_VIDEO_H264_ASPECT_RATIO_IDC_10_11},  {16, 11, STD_VIDEO_H264_ASPECT_RATIO_IDC_16_11},
                     { 40, 33, STD_VIDEO_H264_ASPECT_RATIO_IDC_40_33},  {24, 11, STD_VIDEO_H264_ASPECT_RATIO_IDC_24_11},
                     { 20, 11, STD_VIDEO_H264_ASPECT_RATIO_IDC_20_11},  {32, 11, STD_VIDEO_H264_ASPECT_RATIO_IDC_32_11},
                     { 80, 33, STD_VIDEO_H264_ASPECT_RATIO_IDC_80_33},  {18, 11, STD_VIDEO_H264_ASPECT_RATIO_IDC_18_11},
                     { 15, 11, STD_VIDEO_H264_ASPECT_RATIO_IDC_15_11},  {64, 33, STD_VIDEO_H264_ASPECT_RATIO_IDC_64_33},
                     { 160, 99, STD_VIDEO_H264_ASPECT_RATIO_IDC_160_99}};

    if (darWidth <= 0 && darHeight <= 0) {
        return;
    }

    vui->flags.aspect_ratio_info_present_flag = true;

    // convert DAR to SAR
    uint32_t w = height * darWidth;
    uint32_t h = width  * darHeight;
    int32_t d = Gcd(w, h);
    w /= d;
    h /= d;

    vui->aspect_ratio_idc = STD_VIDEO_H264_ASPECT_RATIO_IDC_INVALID;
    for (uint32_t i = 0; i < ARRAYSIZE(sar_table); i++) {
        if (((unsigned int)(sar_table[i].width) == w) && ((unsigned int)(sar_table[i].height) == h)) {
            vui->aspect_ratio_idc = sar_table[i].ratio;
            break;
        }
    }

    if (vui->aspect_ratio_idc == STD_VIDEO_H264_ASPECT_RATIO_IDC_INVALID) {
        vui->aspect_ratio_idc = STD_VIDEO_H264_ASPECT_RATIO_IDC_EXTENDED_SAR;
        assert(w <= 0xFFFF);
        assert(h <= 0xFFFF);
        vui->sar_width = w & 0xFFFF;
        vui->sar_height = h & 0xFFFF;
    }
}

StdVideoH264SequenceParameterSetVui*
EncoderConfigH264::InitVuiParameters(StdVideoH264SequenceParameterSetVui *vui,
                                     StdVideoH264HrdParameters *pHrdParameters) {

    SetAspectRatio(vui, encodeWidth, encodeHeight, darWidth, darHeight);

    if (!!overscan_info_present_flag) {
        vui->flags.overscan_info_present_flag = true;
        vui->flags.overscan_appropriate_flag = !!overscan_appropriate_flag;
    }

    if (!!video_signal_type_present_flag) {
        vui->flags.video_signal_type_present_flag = true;

        vui->video_format = video_format;

        vui->flags.video_full_range_flag = !!video_full_range_flag;

        if (!!color_description_present_flag) {
            vui->flags.color_description_present_flag = true;

            vui->colour_primaries = colour_primaries;
            vui->transfer_characteristics = transfer_characteristics;
            vui->matrix_coefficients = matrix_coefficients;
        }
    }

    vui->flags.chroma_loc_info_present_flag = chroma_loc_info_present_flag;

    if ((frameRateNumerator > 0) && (frameRateDenominator > 0)) {
        double frameRate = (double)frameRateNumerator / frameRateDenominator;

        const int ticks_1000 = (int)(frameRate * 1000.0 + 0.5);
        const int ticks_1001 = (int)(frameRate * 1001.0 + 0.5);

        if ((ticks_1001 % 500) == 0) {
            vui->time_scale = ticks_1001 * 2;
            vui->num_units_in_tick = 1001;
        } else {
            vui->time_scale = ticks_1000 * 2;
            vui->num_units_in_tick = 1000;
        }

        vui->flags.timing_info_present_flag = true;
        vui->flags.fixed_frame_rate_flag = true;
    }

    if (!!bitstream_restriction_flag) {
        vui->flags.bitstream_restriction_flag = true;
    }

    // TODO: set this to true when enabling buffering period SEI messages
    vui->flags.nal_hrd_parameters_present_flag = false;

    if (vui->flags.nal_hrd_parameters_present_flag) {
        assert(pHrdParameters);
        pHrdParameters->cpb_cnt_minus1 = 0;  // one CPB
        pHrdParameters->bit_rate_scale = 0;  // 64 bits units
        pHrdParameters->cpb_size_scale = 0;  // 16 bits units

        uint64_t bitrate = (hrdBitrate >> (6 + pHrdParameters->bit_rate_scale)) - 1;
        assert((bitrate & 0xFFFFFFFF00000000) == 0);

        uint64_t cbpsize = (vbvBufferSize >> (4 + pHrdParameters->cpb_size_scale)) - 1;
        assert((cbpsize & 0xFFFFFFFF00000000) == 0);

        pHrdParameters->bit_rate_value_minus1[0] = (uint32_t)bitrate;
        pHrdParameters->cpb_size_value_minus1[0] = (uint32_t)cbpsize;
        pHrdParameters->cbr_flag[0] = (rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR);

        pHrdParameters->initial_cpb_removal_delay_length_minus1 = 23;
        pHrdParameters->cpb_removal_delay_length_minus1 = 15;  // has to be >= ld(2*gop_length+1)-1
        pHrdParameters->dpb_output_delay_length_minus1 = 5;    // has to be >= ld(2*(num_b_frames+1)+1)-1
        pHrdParameters->time_offset_length = 24;

        vui->pHrdParameters = pHrdParameters;
    }

    // One or more B-frames
    vui->max_num_reorder_frames = gopStructure.GetConsecutiveBFrameCount();

    const bool vui_parameters_present_flag =
               (vui->flags.aspect_ratio_info_present_flag  ||
                vui->flags.overscan_info_present_flag      ||
                vui->flags.video_signal_type_present_flag  ||
                vui->flags.chroma_loc_info_present_flag    ||
                vui->flags.timing_info_present_flag        ||
                vui->flags.nal_hrd_parameters_present_flag ||
                vui->flags.vcl_hrd_parameters_present_flag ||
                vui->flags.bitstream_restriction_flag);

    if (vui_parameters_present_flag) {
        return vui;
    } else {
        return nullptr;
    }
}

bool EncoderConfigH264::InitSpsPpsParameters(StdVideoH264SequenceParameterSet *sps,
                                             StdVideoH264PictureParameterSet *pps,
                                             StdVideoH264SequenceParameterSetVui* vui)
{
    sps->pic_width_in_mbs_minus1 = pic_width_in_mbs - 1;
    sps->pic_height_in_map_units_minus1 = pic_height_in_map_units - 1;

    sps->chroma_format_idc = (StdVideoH264ChromaFormatIdc)(FastIntLog2<uint32_t>(encodeChromaSubsampling) - 1);

    sps->flags.frame_mbs_only_flag = true;

    if (16 * (pic_width_in_mbs) > (uint32_t)encodeWidth ||
        16 * (sps->pic_height_in_map_units_minus1 + 1) > (uint32_t)encodeHeight) {
        sps->flags.frame_cropping_flag = true;

        sps->frame_crop_right_offset = (16 * (pic_width_in_mbs) - encodeWidth);
        sps->frame_crop_bottom_offset = (16 * (sps->pic_height_in_map_units_minus1 + 1) - encodeHeight);

        if (sps->chroma_format_idc == STD_VIDEO_H264_CHROMA_FORMAT_IDC_420) {
            sps->frame_crop_right_offset >>= 1;
            sps->frame_crop_bottom_offset >>= 1;
        }
    }

    if (!!qpprime_y_zero_transform_bypass_flag &&
        (tuningMode == VK_VIDEO_ENCODE_TUNING_MODE_LOSSLESS_KHR)) {
        sps->flags.qpprime_y_zero_transform_bypass_flag = true;
    }

    // Set this unconditionally because we always seem to signal this bit in
    // the stream (see InitializeSeqParSet()).
    sps->flags.direct_8x8_inference_flag = true;

    sps->seq_parameter_set_id = spsId;

    // TODO: Update the values based on settings
    sps->log2_max_frame_num_minus4 = 4;
    sps->log2_max_pic_order_cnt_lsb_minus4 = 4;

    sps->max_num_ref_frames = numRefL0 + numRefL1;

    // Initialize PPS values
    pps->seq_parameter_set_id = sps->seq_parameter_set_id;
    pps->pic_parameter_set_id = ppsId;
    pps->weighted_bipred_idc = STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_DEFAULT;
    pps->num_ref_idx_l0_default_active_minus1 = numRefL0 > 0 ? numRefL0 - 1 : 0;
    pps->num_ref_idx_l1_default_active_minus1 = numRefL1 > 0 ? numRefL1 - 1 : 0;

    if ((sps->chroma_format_idc == 3) && !sps->flags.qpprime_y_zero_transform_bypass_flag) {
        pps->chroma_qp_index_offset = pps->second_chroma_qp_index_offset = 6;
    }

    // We need to set max_num_ref_frames to a sane value, for writing to the
    // bitstream later. The check for 0 is to handle cases where the client
    // did not provide a DPB size, and the other check is to recompute
    // max_num_ref_frames if required (in case of multi-ref).
    if ((sps->max_num_ref_frames == 0) ||
           (sps->max_num_ref_frames <= pps->num_ref_idx_l0_default_active_minus1)) {
        sps->max_num_ref_frames = (uint8_t)(pps->num_ref_idx_l0_default_active_minus1 + 1);
        if (gopStructure.GetConsecutiveBFrameCount() > 0) {
            sps->max_num_ref_frames = (uint8_t)(sps->max_num_ref_frames + pps->num_ref_idx_l1_default_active_minus1 + 1U);
        }

        // max_num_ref_frames must not exceed the largest DPB size allowed by
        // the selected level.
        sps->max_num_ref_frames = std::min(sps->max_num_ref_frames, (uint8_t)dpbCount);
    }

    if (gopStructure.GetConsecutiveBFrameCount() > 0) {
        sps->pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
    } else {
        sps->pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2;
    }

    // FIXME: Check if the HW supports transform_8x8_mode_is_supported
    // based on capabilities or profiles supported
    const bool transform_8x8_mode_is_supported = true;
    const bool bIsFastestPreset = false;

    if (adaptiveTransformMode == ADAPTIVE_TRANSFORM_ENABLE) {
        pps->flags.transform_8x8_mode_flag = true;
    } else if (adaptiveTransformMode == ADAPTIVE_TRANSFORM_DISABLE) {
        pps->flags.transform_8x8_mode_flag = false;
    } else {
        // Autoselect
        if (!bIsFastestPreset || transform_8x8_mode_is_supported) {
            if ((profileIdc == STD_VIDEO_H264_PROFILE_IDC_INVALID) ||
                (profileIdc >= STD_VIDEO_H264_PROFILE_IDC_HIGH)) {
                // Unconditionally enable 8x8 transform
                pps->flags.transform_8x8_mode_flag = true;
            }
        }
    }

    if (entropyCodingMode == ENTROPY_CODING_MODE_CABAC) {
        pps->flags.entropy_coding_mode_flag = true;
    } else {
        pps->flags.entropy_coding_mode_flag = false;
    }

    // Always write out deblocking_filter_control_present_flag
    pps->flags.deblocking_filter_control_present_flag = true;

    if (constrained_intra_pred_flag) {
        pps->flags.constrained_intra_pred_flag = true;
    }

    // If the profileIdc hasn't been specified, force set it now.
    if (profileIdc == STD_VIDEO_H264_PROFILE_IDC_INVALID) {
        profileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;

        if (entropyCodingMode == ENTROPY_CODING_MODE_CABAC) {
            profileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
        }

        if ((gopStructure.GetConsecutiveBFrameCount() > 0) || pps->flags.entropy_coding_mode_flag || !sps->flags.frame_mbs_only_flag)
            profileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;

        if (pps->flags.transform_8x8_mode_flag) {
            profileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
        }

        if ((sps->flags.qpprime_y_zero_transform_bypass_flag &&
             (rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR)) ||
                (sps->chroma_format_idc == STD_VIDEO_H264_CHROMA_FORMAT_IDC_444)) {
            profileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE;
        }
    }

    sps->profile_idc = profileIdc;
    sps->level_idc = levelIdc;

    // constraint_setX_flag values
    sps->flags.constraint_set0_flag = profileIdc == STD_VIDEO_H264_PROFILE_IDC_BASELINE;
    sps->flags.constraint_set1_flag = ((profileIdc == STD_VIDEO_H264_PROFILE_IDC_BASELINE) ||
                                       (profileIdc == STD_VIDEO_H264_PROFILE_IDC_MAIN));

    if ((profileIdc == STD_VIDEO_H264_PROFILE_IDC_MAIN) ||
        (profileIdc == STD_VIDEO_H264_PROFILE_IDC_HIGH)) {
        // FIXME: set values based on constraint profiles
        sps->flags.constraint_set4_flag = 0;
        sps->flags.constraint_set5_flag = 0;
    }

    sps->pSequenceParameterSetVui = vui;
    sps->flags.vui_parameters_present_flag = (vui != nullptr);
    return true;
}

VkResult EncoderConfigH264::InitDeviceCapabilities(const VulkanDeviceContext* vkDevCtx)
{
    VkResult result = VulkanVideoCapabilities::GetVideoEncodeCapabilities<VkVideoEncodeH264CapabilitiesKHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR,
                                                                          VkVideoEncodeH264QuantizationMapCapabilitiesKHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUANTIZATION_MAP_CAPABILITIES_KHR>
                                                                (vkDevCtx, videoCoreProfile,
                                                                 videoCapabilities,
                                                                 videoEncodeCapabilities,
                                                                 h264EncodeCapabilities,
                                                                 quantizationMapCapabilities,
                                                                 h264QuantizationMapCapabilities);
    if (result != VK_SUCCESS) {
        std::cout << "*** Could not get Video Capabilities :" << result << " ***" << std::endl;
        assert(!"Could not get Video Capabilities!");
        return result;
    }

    if (verboseMsg) {
        std::cout << "\t\t\t" << VkVideoCoreProfile::CodecToName(codec) << "encode capabilities: " << std::endl;
        std::cout << "\t\t\t" << "minBitstreamBufferOffsetAlignment: " << videoCapabilities.minBitstreamBufferOffsetAlignment << std::endl;
        std::cout << "\t\t\t" << "minBitstreamBufferSizeAlignment: " << videoCapabilities.minBitstreamBufferSizeAlignment << std::endl;
        std::cout << "\t\t\t" << "pictureAccessGranularity: " << videoCapabilities.pictureAccessGranularity.width << " x " << videoCapabilities.pictureAccessGranularity.height << std::endl;
        std::cout << "\t\t\t" << "minExtent: " << videoCapabilities.minCodedExtent.width << " x " << videoCapabilities.minCodedExtent.height << std::endl;
        std::cout << "\t\t\t" << "maxExtent: " << videoCapabilities.maxCodedExtent.width  << " x " << videoCapabilities.maxCodedExtent.height << std::endl;
        std::cout << "\t\t\t" << "maxDpbSlots: " << videoCapabilities.maxDpbSlots << std::endl;
        std::cout << "\t\t\t" << "maxActiveReferencePictures: " << videoCapabilities.maxActiveReferencePictures << std::endl;
        std::cout << "\t\t\t" << "maxBPictureL0ReferenceCount: " << h264EncodeCapabilities.maxBPictureL0ReferenceCount << std::endl;
    }

    result = VulkanVideoCapabilities::GetPhysicalDeviceVideoEncodeQualityLevelProperties<VkVideoEncodeH264QualityLevelPropertiesKHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUALITY_LEVEL_PROPERTIES_KHR>
                                                                                (vkDevCtx, videoCoreProfile, qualityLevel,
                                                                                 qualityLevelProperties,
                                                                                 h264QualityLevelProperties);
    if (result != VK_SUCCESS) {
        std::cout << "*** Could not get Video Encode QualityLevel Properties :" << result << " ***" << std::endl;
        assert(!"Could not get Video Encode QualityLevel Properties");
        return result;
    }

    if (verboseMsg) {
        std::cout << "\t\t" << VkVideoCoreProfile::CodecToName(codec) << "encode quality level properties: " << std::endl;
        std::cout << "\t\t\t" << "preferredRateControlMode : " << qualityLevelProperties.preferredRateControlMode << std::endl;
        std::cout << "\t\t\t" << "preferredRateControlLayerCount : " << qualityLevelProperties.preferredRateControlLayerCount << std::endl;
        std::cout << "\t\t\t" << "preferredRateControlFlags : " << h264QualityLevelProperties.preferredRateControlFlags << std::endl;
        std::cout << "\t\t\t" << "preferredGopFrameCount : " << h264QualityLevelProperties.preferredGopFrameCount << std::endl;
        std::cout << "\t\t\t" << "preferredIdrPeriod : " << h264QualityLevelProperties.preferredIdrPeriod << std::endl;
        std::cout << "\t\t\t" << "preferredConsecutiveBFrameCount : " << h264QualityLevelProperties.preferredConsecutiveBFrameCount << std::endl;
        std::cout << "\t\t\t" << "preferredTemporalLayerCount : " << h264QualityLevelProperties.preferredTemporalLayerCount << std::endl;
        std::cout << "\t\t\t" << "preferredConstantQp.qpI : " << h264QualityLevelProperties.preferredConstantQp.qpI << std::endl;
        std::cout << "\t\t\t" << "preferredConstantQp.qpP : " << h264QualityLevelProperties.preferredConstantQp.qpP << std::endl;
        std::cout << "\t\t\t" << "preferredConstantQp.qpB : " << h264QualityLevelProperties.preferredConstantQp.qpB << std::endl;
        std::cout << "\t\t\t" << "preferredMaxL0ReferenceCount : " << h264QualityLevelProperties.preferredMaxL0ReferenceCount << std::endl;
        std::cout << "\t\t\t" << "preferredMaxL1ReferenceCount : " << h264QualityLevelProperties.preferredMaxL1ReferenceCount << std::endl;
        std::cout << "\t\t\t" << "preferredStdEntropyCodingModeFlag : " << h264QualityLevelProperties.preferredStdEntropyCodingModeFlag << std::endl;
    }

    if (rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_FLAG_BITS_MAX_ENUM_KHR) {
        rateControlMode = qualityLevelProperties.preferredRateControlMode;
    }
    if (gopStructure.GetGopFrameCount() == ZERO_GOP_FRAME_COUNT) {
        gopStructure.SetGopFrameCount(h264QualityLevelProperties.preferredGopFrameCount);
    }
    if (gopStructure.GetIdrPeriod() == ZERO_GOP_IDR_PERIOD) {
        gopStructure.SetIdrPeriod(h264QualityLevelProperties.preferredIdrPeriod);
    }
    if (gopStructure.GetConsecutiveBFrameCount() == CONSECUTIVE_B_FRAME_COUNT_MAX_VALUE) {
        gopStructure.SetConsecutiveBFrameCount(h264QualityLevelProperties.preferredConsecutiveBFrameCount);
    }
    if (constQp.qpIntra == 0) {
        constQp.qpIntra = h264QualityLevelProperties.preferredConstantQp.qpI;
    }
    if (constQp.qpInterP == 0) {
        constQp.qpInterP = h264QualityLevelProperties.preferredConstantQp.qpP;
    }
    if (constQp.qpInterB == 0) {
        constQp.qpInterB = h264QualityLevelProperties.preferredConstantQp.qpB;
    }
    if (rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
        minQp = h264QualityLevelProperties.preferredConstantQp;
        maxQp = h264QualityLevelProperties.preferredConstantQp;
    }
    numRefL0 = h264QualityLevelProperties.preferredMaxL0ReferenceCount;
    numRefL1 = h264QualityLevelProperties.preferredMaxL1ReferenceCount;
    numRefFrames = numRefL0 + numRefL1;
    entropyCodingMode = h264QualityLevelProperties.preferredStdEntropyCodingModeFlag == VK_TRUE ? ENTROPY_CODING_MODE_CABAC : ENTROPY_CODING_MODE_CAVLC;

    return VK_SUCCESS;
}

int8_t EncoderConfigH264::InitDpbCount()
{
    dpbCount = 0; // TODO: What is the need for this?

    // spsInfo->level represents the smallest level that we require for the
    // given stream. This level constrains the maximum size (in terms of
    // number of frames) that the DPB can have. levelDpbSize is this maximum
    // value.
    uint32_t levelBitRate = ((rateControlMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) && hrdBitrate == 0)
                                ? averageBitrate // constrained by avg bitrate
                                : hrdBitrate;  // constrained by max bitrate

    assert(pic_width_in_mbs > 0);
    assert(pic_height_in_map_units > 0);
    uint32_t frameSizeInMbs = pic_width_in_mbs * pic_height_in_map_units;

    double frameRate = ((frameRateNumerator > 0) && (frameRateDenominator > 0))
                           ? (double)frameRateNumerator / frameRateDenominator
                           : (double)FRAME_RATE_NUM_DEFAULT / (double)FRAME_RATE_DEN_DEFAULT;

    // WAR for super HD resolution (bypass H264 level check for frame mode and use level 5.2)
    if ((frameSizeInMbs > ((uint32_t)levelLimits[levelLimitsSize - 1].maxFS) ||
        ((frameSizeInMbs * frameRate) > ((uint32_t)levelLimits[levelLimitsSize - 1].maxMBPS)))) {
        levelIdc = STD_VIDEO_H264_LEVEL_IDC_5_2;
    } else {
        // find lowest possible level
        levelIdc = DetermineLevel(dpbCount, levelBitRate, vbvBufferSize, frameRate);
    }

    uint8_t levelDpbSize = (uint8_t)(((1024 * levelLimits[levelIdc].maxDPB)) /
                            ((pic_width_in_mbs * pic_height_in_map_units) * 384));

    // XXX: If the level is 5.2, it is highly likely that we forced it to that
    // value as a WAR for super HD. In that case, force the DPB size to
    // DEFAULT_MAX_NUM_REF_FRAMES. Otherwise, clamp the computed DPB size to DEFAULT_MAX_NUM_REF_FRAMES.
    levelDpbSize = (levelIdc == STD_VIDEO_H264_LEVEL_IDC_5_2) ? (uint8_t)DEFAULT_MAX_NUM_REF_FRAMES : std::min(uint8_t(DEFAULT_MAX_NUM_REF_FRAMES), levelDpbSize);

    uint8_t dpbSize = (uint8_t)((dpbCount < 1) ? levelDpbSize : (uint8_t)(std::min((uint8_t)dpbCount, levelDpbSize))) + 1;

    dpbCount = dpbSize;
    return dpbSize;
}

bool EncoderConfigH264::InitRateControl()
{
    uint32_t levelBitRate = ((rateControlMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) && hrdBitrate == 0)
                                ? averageBitrate
                                :            // constrained by avg bitrate
                                hrdBitrate;  // constrained by max bitrate

    // Update levelBitrate to the maximum allowed (used to determine default average and HRD bitrates below)
    //  800 instead of 1000 is to remain BD-compliant for level 4.1 (40Mbps at level 4.x)
    // 120Mbps limit is mainly to prevent overflows in fullness computations (~(2^31)/16) -> FIXME: hunt down all these
    // overflows
    levelBitRate = std::min(std::max(levelBitRate, (uint32_t)levelLimits[levelIdc].maxBR * 800u), uint32_t(120000000));

    // If no bitrate is specified, use the level limit
    if (averageBitrate == 0) {
        averageBitrate = hrdBitrate ? hrdBitrate : levelBitRate;
    }

    // If no HRD bitrate is specified, use 3x average for VBR (without going above level limit) or equal to average bitrate for
    // CBR
    if (hrdBitrate == 0) {
        if ((rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR) && (averageBitrate < levelBitRate)) {
            hrdBitrate = std::min(averageBitrate * 3, levelBitRate);
            // At least 500ms at peak rate if the application specifies the buffersize but not the HRD bitrate
            if (vbvBufferSize != 0) {
                hrdBitrate = std::min(hrdBitrate, std::max(vbvBufferSize * 2, averageBitrate));
            }
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

    // Use the level limit for the max VBV buffer size, and no more than 8 seconds at peak rate
    if (vbvBufferSize == 0) {
        vbvBufferSize = std::min(levelLimits[levelIdc].maxCPB * 1000, 120000000U);
        if (rateControlMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR)
            if ((vbvBufferSize >> 3) > hrdBitrate) {
                vbvBufferSize = hrdBitrate << 3;
            }
    }

    if (vbvInitialDelay == 0) {
        // 90% occupancy or at least one second of fullness if possible
        vbvInitialDelay = std::max(vbvBufferSize - vbvBufferSize / 10, std::min(vbvBufferSize, hrdBitrate));
    }

    return true;
}

bool EncoderConfigH264::GetRateControlParameters(VkVideoEncodeRateControlInfoKHR *pRateControlInfo,
                                                 VkVideoEncodeRateControlLayerInfoKHR *pRateControlLayersInfo,
                                                 VkVideoEncodeH264RateControlInfoKHR *pRateControlInfoH264,
                                                 VkVideoEncodeH264RateControlLayerInfoKHR *pRateControlLayerInfoH264)
{
    pRateControlLayersInfo->frameRateNumerator = frameRateNumerator;
    pRateControlLayersInfo->frameRateDenominator = frameRateDenominator;

    pRateControlInfo->rateControlMode = rateControlMode;

    if (pRateControlInfo->rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
        pRateControlLayerInfoH264->minQp = pRateControlLayerInfoH264->maxQp = minQp;
    } else {
        pRateControlLayerInfoH264->minQp = minQp;
        pRateControlLayerInfoH264->maxQp = maxQp;
    }

    pRateControlLayersInfo->averageBitrate = averageBitrate;
    pRateControlLayersInfo->maxBitrate = hrdBitrate;

    if (averageBitrate > 0 || hrdBitrate > 0) {
       pRateControlInfo->virtualBufferSizeInMs = (uint32_t)(vbvBufferSize * 1000ull / (hrdBitrate ? hrdBitrate : averageBitrate));
       pRateControlInfo->initialVirtualBufferSizeInMs = (uint32_t)(vbvInitialDelay * 1000ull / (hrdBitrate ? hrdBitrate : averageBitrate));
    }

    pRateControlInfoH264->consecutiveBFrameCount = gopStructure.GetConsecutiveBFrameCount();

    pRateControlInfoH264->gopFrameCount = (gopStructure.GetGopFrameCount() > 0) ? gopStructure.GetGopFrameCount() : (uint32_t)GOP_LENGTH_DEFAULT;
    pRateControlInfoH264->idrPeriod = (gopStructure.GetIdrPeriod() > 0) ? gopStructure.GetIdrPeriod() : (uint32_t)IDR_PERIOD_DEFAULT;

    return true;
}
