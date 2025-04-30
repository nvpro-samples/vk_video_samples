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

#include <math.h>       /* sqrt */
#include "VkVideoEncoder/VkEncoderConfigH265.h"

static void SetupAspectRatio(StdVideoH265SequenceParameterSetVui *vui, uint32_t width, uint32_t height,
                             uint32_t darWidth, uint32_t darHeight)
{
    //Table E 1
    static const uint32_t stSampleAspectRatioTable[16][2] = {   {1,1}, {12,11}, {10,11}, {16,11}, {40,33}, {24,11},
                                                              {20,11}, {32,11}, {80,33}, {18,11}, {15,11}, {64,33},
                                                              {160,99},   {4,3},   {3,2}, {2,1} };

    if ((darWidth <= 0) || (darHeight <= 0)) {
        vui->flags.aspect_ratio_info_present_flag = 0;
        return;
    }

    vui->flags.aspect_ratio_info_present_flag = 1;

    // convert DAR to SAR
    uint32_t w = height * darWidth;
    uint32_t h = width * darHeight;
    uint32_t d = Gcd(w, h);
    w /= d;
    h /= d;

    int32_t indexFound = -1;
    for (uint32_t i = 0; i < 16; i++) {
        if ((stSampleAspectRatioTable[i][0] == w) && (stSampleAspectRatioTable[i][1] == h)) {
            indexFound = i;
            break;
        }
    }

    if (indexFound >= 0)
    {
        vui->aspect_ratio_idc = (StdVideoH265AspectRatioIdc)(indexFound + 1); // 1..16
    } else {
        vui->aspect_ratio_idc = STD_VIDEO_H265_ASPECT_RATIO_IDC_EXTENDED_SAR; // Extended_SAR
        vui->sar_width  = (uint16_t)w;
        vui->sar_height = (uint16_t)h;
    }
}

// From Table A.8
uint32_t EncoderConfigH265::GetCpbVclFactor()
{
    uint32_t chroma_format_idc = encodeChromaSubsampling;
    uint32_t bit_depth = std::max(encodeBitDepthLuma, encodeBitDepthChroma);
    uint32_t baseFactor = (chroma_format_idc == 3) ? (bit_depth >= 10) ? 2500 : 2000 : 1000; // NOTE: Assumes chroma_format_idc is either 1 or 3
    uint32_t depthFactor = (bit_depth >= 10) ? ((bit_depth - 10) >> 1) * 500 : 0;    // +500 for 12-bit, +1000 for 14-bit, +1500 for 16-bit
    return baseFactor + depthFactor;
}

VkResult EncoderConfigH265::InitDeviceCapabilities(const VulkanDeviceContext* vkDevCtx)
{
    VkResult result = VulkanVideoCapabilities::GetVideoEncodeCapabilities<VkVideoEncodeH265CapabilitiesKHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR,
                                                                          VkVideoEncodeH265QuantizationMapCapabilitiesKHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUANTIZATION_MAP_CAPABILITIES_KHR>
                                                                (vkDevCtx, videoCoreProfile,
                                                                 videoCapabilities,
                                                                 videoEncodeCapabilities,
                                                                 h265EncodeCapabilities,
                                                                 quantizationMapCapabilities,
                                                                 h265QuantizationMapCapabilities);
    if (result != VK_SUCCESS) {
        std::cout << "*** Could not get Video Capabilities :" << result << " ***" << std::endl;
        assert(!"Could not get Video Capabilities!");
        return result;
    }

    if (verboseMsg) {
        std::cout << "\t\t" << VkVideoCoreProfile::CodecToName(codec) << "encode capabilities: " << std::endl;
        std::cout << "\t\t\t" << "minBitstreamBufferOffsetAlignment: " << videoCapabilities.minBitstreamBufferOffsetAlignment << std::endl;
        std::cout << "\t\t\t" << "minBitstreamBufferSizeAlignment: " << videoCapabilities.minBitstreamBufferSizeAlignment << std::endl;
        std::cout << "\t\t\t" << "pictureAccessGranularity: " << videoCapabilities.pictureAccessGranularity.width << " x " << videoCapabilities.pictureAccessGranularity.height << std::endl;
        std::cout << "\t\t\t" << "minExtent: " << videoCapabilities.minCodedExtent.width << " x " << videoCapabilities.minCodedExtent.height << std::endl;
        std::cout << "\t\t\t" << "maxExtent: " << videoCapabilities.maxCodedExtent.width  << " x " << videoCapabilities.maxCodedExtent.height << std::endl;
        std::cout << "\t\t\t" << "maxDpbSlots: " << videoCapabilities.maxDpbSlots << std::endl;
        std::cout << "\t\t\t" << "maxActiveReferencePictures: " << videoCapabilities.maxActiveReferencePictures << std::endl;
        std::cout << "\t\t\t" << "maxBPictureL0ReferenceCount: " << h265EncodeCapabilities.maxBPictureL0ReferenceCount << std::endl;
    }

    result = VulkanVideoCapabilities::GetPhysicalDeviceVideoEncodeQualityLevelProperties<VkVideoEncodeH265QualityLevelPropertiesKHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUALITY_LEVEL_PROPERTIES_KHR>
                                                                                (vkDevCtx, videoCoreProfile, qualityLevel,
                                                                                 qualityLevelProperties,
                                                                                 h265QualityLevelProperties);
    if (result != VK_SUCCESS) {
        std::cout << "*** Could not get Video Encode QualityLevel Properties :" << result << " ***" << std::endl;
        assert(!"Could not get Video Encode QualityLevel Properties");
        return result;
    }

    if (verboseMsg) {
        std::cout << "\t\t" << VkVideoCoreProfile::CodecToName(codec) << "encode quality level properties: " << std::endl;
        std::cout << "\t\t\t" << "preferredRateControlMode : " << qualityLevelProperties.preferredRateControlMode << std::endl;
        std::cout << "\t\t\t" << "preferredRateControlLayerCount : " << qualityLevelProperties.preferredRateControlLayerCount << std::endl;
        std::cout << "\t\t\t" << "preferredRateControlFlags : " << h265QualityLevelProperties.preferredRateControlFlags << std::endl;
        std::cout << "\t\t\t" << "preferredGopFrameCount : " << h265QualityLevelProperties.preferredGopFrameCount << std::endl;
        std::cout << "\t\t\t" << "preferredIdrPeriod : " << h265QualityLevelProperties.preferredIdrPeriod << std::endl;
        std::cout << "\t\t\t" << "preferredConsecutiveBFrameCount : " << h265QualityLevelProperties.preferredConsecutiveBFrameCount << std::endl;
        std::cout << "\t\t\t" << "preferredSubLayerCount : " << h265QualityLevelProperties.preferredSubLayerCount << std::endl;
        std::cout << "\t\t\t" << "preferredConstantQp.qpI : " << h265QualityLevelProperties.preferredConstantQp.qpI << std::endl;
        std::cout << "\t\t\t" << "preferredConstantQp.qpP : " << h265QualityLevelProperties.preferredConstantQp.qpP << std::endl;
        std::cout << "\t\t\t" << "preferredConstantQp.qpB : " << h265QualityLevelProperties.preferredConstantQp.qpB << std::endl;
        std::cout << "\t\t\t" << "preferredMaxL0ReferenceCount : " << h265QualityLevelProperties.preferredMaxL0ReferenceCount << std::endl;
        std::cout << "\t\t\t" << "preferredMaxL1ReferenceCount : " << h265QualityLevelProperties.preferredMaxL1ReferenceCount << std::endl;
    }

    if (rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_FLAG_BITS_MAX_ENUM_KHR) {
        rateControlMode = qualityLevelProperties.preferredRateControlMode;
    }
    if (gopStructure.GetGopFrameCount() == ZERO_GOP_FRAME_COUNT) {
        gopStructure.SetGopFrameCount(h265QualityLevelProperties.preferredGopFrameCount);
    }
    if (gopStructure.GetIdrPeriod() == ZERO_GOP_IDR_PERIOD) {
        gopStructure.SetIdrPeriod(h265QualityLevelProperties.preferredIdrPeriod);
    }
    if (gopStructure.GetConsecutiveBFrameCount() == CONSECUTIVE_B_FRAME_COUNT_MAX_VALUE) {
        gopStructure.SetConsecutiveBFrameCount(h265QualityLevelProperties.preferredConsecutiveBFrameCount);
    }
    if (constQp.qpIntra == 0) {
        constQp.qpIntra = h265QualityLevelProperties.preferredConstantQp.qpI;
    }
    if (constQp.qpInterP == 0) {
        constQp.qpInterP = h265QualityLevelProperties.preferredConstantQp.qpP;
    }
    if (constQp.qpInterB == 0) {
        constQp.qpInterB = h265QualityLevelProperties.preferredConstantQp.qpB;
    }
    numRefL0 = h265QualityLevelProperties.preferredMaxL0ReferenceCount;
    numRefL1 = h265QualityLevelProperties.preferredMaxL1ReferenceCount;

    return VK_SUCCESS;
}

int8_t EncoderConfigH265::InitDpbCount()
{
    dpbCount = 5;

    return VerifyDpbSize();
}

uint32_t EncoderConfigH265::GetMaxDpbSize(uint32_t pictureSizeInSamplesY, int32_t levelIdx)
{
    uint32_t maxDpbSize = 0;
    const uint32_t maxDpbPicBuf = 9;

    // From A.4.1 General tier and level limits
    if (pictureSizeInSamplesY <= (levelLimits[levelIdx].maxLumaPS >> 2)) {
        maxDpbSize = maxDpbPicBuf * 4;
    } else if (pictureSizeInSamplesY <= (levelLimits[levelIdx].maxLumaPS >> 1)) {
        maxDpbSize = maxDpbPicBuf * 2;
    } else if (pictureSizeInSamplesY <= ((3 * levelLimits[levelIdx].maxLumaPS) >> 2)) {
        maxDpbSize = (maxDpbPicBuf * 4) / 3;
    } else {
        maxDpbSize = maxDpbPicBuf;
    }

    return std::min<uint32_t>(maxDpbSize, STD_VIDEO_H265_MAX_DPB_SIZE);
}

uint32_t EncoderConfigH265::GetCtbAlignedPicSizeInSamples(uint32_t& picWidthInCtbsY, uint32_t& picHeightInCtbsY, bool minCtbsY)
{
    if (minCtbsY) {
        uint32_t minCbLog2SizeY = cuMinSize + 3;
        uint32_t minCbSizeY = 1 << minCbLog2SizeY;
        picWidthInCtbsY     = AlignSize(encodeWidth, minCbSizeY);
        picHeightInCtbsY    = AlignSize(encodeHeight, minCbSizeY);
    } else {
        uint32_t ctbLog2SizeY = cuSize + 3;
        uint32_t ctbSizeY     = 1 << ctbLog2SizeY;
        picWidthInCtbsY       = AlignSize(encodeWidth, ctbSizeY);
        picHeightInCtbsY      = AlignSize(encodeHeight, ctbSizeY);
    }
    return picWidthInCtbsY * picHeightInCtbsY;
}

int8_t EncoderConfigH265::VerifyDpbSize()
{
    uint32_t picWidthInCtbsY = 0, picHeightInCtbsY = 0;
    uint32_t picSize = GetCtbAlignedPicSizeInSamples(picWidthInCtbsY, picHeightInCtbsY);

    int32_t levelIdxFound = -1;
    for (size_t i = 0; i < levelLimitsTblSize; i++) {
        if (levelLimits[i].stdLevel == levelIdc) {
            levelIdxFound = (int32_t)i;
            break;
        }
    }

    if (levelIdxFound != -1) {
        uint32_t maxDpbSize = GetMaxDpbSize(picSize, levelIdxFound);
        if ((uint32_t)dpbCount > maxDpbSize) {
            assert(!"DpbSize is greater than the maximum supported value.");
            return (int8_t)(uint8_t)maxDpbSize;
        }
    } else {
        assert(!"Invalid level idc");
        return -1;
    }

    return dpbCount;
}

StdVideoH265SequenceParameterSetVui*
EncoderConfigH265::InitVuiParameters(StdVideoH265SequenceParameterSetVui *vuiInfo,
                                          StdVideoH265HrdParameters *pHrdParameters,
                                          StdVideoH265SubLayerHrdParameters *pSubLayerHrdParametersNal)
{

    SetupAspectRatio(vuiInfo, encodeWidth, encodeHeight, darWidth, darHeight);

    if (!!overscan_info_present_flag) {
        vuiInfo->flags.overscan_info_present_flag = true;
        vuiInfo->flags.overscan_appropriate_flag = !!overscan_appropriate_flag;
    }

    if (!!video_signal_type_present_flag) {
        vuiInfo->flags.video_signal_type_present_flag = true;

        vuiInfo->video_format = video_format;

        vuiInfo->flags.video_full_range_flag = !!video_full_range_flag;

        if (!!color_description_present_flag) {
            vuiInfo->flags.colour_description_present_flag = true;

            vuiInfo->colour_primaries = colour_primaries;
            vuiInfo->transfer_characteristics = transfer_characteristics;
            vuiInfo->matrix_coeffs = matrix_coefficients;
        }
    }

    vuiInfo->flags.chroma_loc_info_present_flag = chroma_loc_info_present_flag;

    vuiInfo->flags.neutral_chroma_indication_flag = 0;
    vuiInfo->flags.field_seq_flag = 0;
    vuiInfo->flags.frame_field_info_present_flag = 0;
    vuiInfo->flags.default_display_window_flag = 0;
    vuiInfo->flags.vui_poc_proportional_to_timing_flag = 0;
    vuiInfo->flags.tiles_fixed_structure_flag = 0;
    vuiInfo->flags.motion_vectors_over_pic_boundaries_flag = 1;
    vuiInfo->flags.restricted_ref_pic_lists_flag = 1;

    if (frameRateNumerator > 0 && frameRateDenominator > 0) {
        vuiInfo->vui_num_units_in_tick = frameRateDenominator;
        vuiInfo->vui_time_scale = frameRateNumerator;

        vuiInfo->flags.vui_timing_info_present_flag = true;
        // vuiInfo->flags.fixed_frame_rate_flag = true;
    }

    if (!!bitstream_restriction_flag) {
        vuiInfo->flags.bitstream_restriction_flag = true;
    }

    // TODO: set this to true when enabling buffering period SEI messages
    vuiInfo->flags.vui_hrd_parameters_present_flag = false;

    if (vuiInfo->flags.vui_hrd_parameters_present_flag) {
        memset(&pHrdParameters->cpb_cnt_minus1, 0, sizeof(pHrdParameters->cpb_cnt_minus1));  // one CPB
        pHrdParameters->tick_divisor_minus2 = 0;
        pHrdParameters->du_cpb_removal_delay_increment_length_minus1 = 0;
        pHrdParameters->dpb_output_delay_du_length_minus1 = 0;
        pHrdParameters->bit_rate_scale = 0;  // 64 bits units
        pHrdParameters->cpb_size_scale = 0;  // 16 bits units
        pHrdParameters->cpb_size_du_scale = 0;
        pHrdParameters->initial_cpb_removal_delay_length_minus1 = 23;
        pHrdParameters->au_cpb_removal_delay_length_minus1 = 15;  // has to be >= ld(2*gop_length+1)-1
        pHrdParameters->dpb_output_delay_length_minus1 = 5;    // has to be >= ld(2*(num_b_frames+1)+1)-1

        uint64_t bitrate = (hrdBitrate >> (6 + pHrdParameters->bit_rate_scale)) - 1;
        assert((bitrate & 0xFFFFFFFF00000000) == 0);

        uint64_t cbpsize = (vbvBufferSize >> (4 + pHrdParameters->cpb_size_scale)) - 1;
        assert((cbpsize & 0xFFFFFFFF00000000) == 0);

        pHrdParameters->flags.nal_hrd_parameters_present_flag = 1;
        pHrdParameters->flags.vcl_hrd_parameters_present_flag = 0;
        pHrdParameters->flags.sub_pic_hrd_params_present_flag = 0;
        pHrdParameters->flags.sub_pic_cpb_params_in_pic_timing_sei_flag = 0;
        pHrdParameters->flags.fixed_pic_rate_general_flag = 0;
        pHrdParameters->flags.fixed_pic_rate_within_cvs_flag = 0;
        pHrdParameters->flags.low_delay_hrd_flag = 0;

        pSubLayerHrdParametersNal->bit_rate_value_minus1[0] = (uint32_t)bitrate;
        pSubLayerHrdParametersNal->cpb_size_value_minus1[0] = (uint32_t)cbpsize;
        pSubLayerHrdParametersNal->cpb_size_du_value_minus1[0] = 0;
        pSubLayerHrdParametersNal->bit_rate_du_value_minus1[0] = 0;
        pSubLayerHrdParametersNal->cbr_flag = (rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR);
        pHrdParameters->pSubLayerHrdParametersNal = pSubLayerHrdParametersNal;
        vuiInfo->pHrdParameters = pHrdParameters;
    }

    // FIXME: chroma_sample_loc_type_top_field to be configured from settings.
    vuiInfo->chroma_sample_loc_type_top_field = 0;
    vuiInfo->chroma_sample_loc_type_bottom_field = 0;
    // display_window_flag
    vuiInfo->def_disp_win_left_offset = 0;
    vuiInfo->def_disp_win_right_offset = 0;
    vuiInfo->def_disp_win_top_offset = 0;
    vuiInfo->def_disp_win_bottom_offset = 0;
    vuiInfo->min_spatial_segmentation_idc = 0;
    vuiInfo->max_bytes_per_pic_denom = 0;
    vuiInfo->max_bits_per_min_cu_denom = 0;

    // FIXME: magic numbers.
    int32_t left_mvx_limit    = -4096;
    int32_t top_mvy_limit     = -1024;
    // int32_t right_mvx_limit   =  4095;
    // int32_t bottom_mvy_limit  =  1023;

    int32_t left_mvx_frac          =  left_mvx_limit & 0x3;
    int32_t left_mvx_int           = (left_mvx_limit >> 2) & 0xfff;
    int32_t top_mvy_frac           =  top_mvy_limit & 0x3;
    int32_t top_mvy_int            = (top_mvy_limit >> 2) & 0x3ff;
    // int32_t right_mvx_frac         =  right_mvx_limit & 0x3;
    // int32_t right_mvx_int          = (right_mvx_limit >> 2) & 0xfff;
    // int32_t bottom_mvy_frac        =  bottom_mvy_limit & 0x3;
    // int32_t bottom_mvy_int         = (bottom_mvy_limit >> 2) & 0x3ff;

    // Explicitly set the MV fractional components to 0 to avoid HW Bugs.
    if (true)
    {
        left_mvx_frac   = 0;
        top_mvy_frac    = 0;
        // right_mvx_frac  = 0;
        // bottom_mvy_frac = 0;
    }
    int32_t leftMvxLimit = left_mvx_int << 2 | left_mvx_frac;
    int32_t topMvyLimit  = top_mvy_int << 2 | top_mvy_frac;
    vuiInfo->log2_max_mv_length_horizontal = (uint8_t)FastIntLog2(std::max(IntAbs(leftMvxLimit) - 1, 1));
    vuiInfo->log2_max_mv_length_vertical   = (uint8_t)FastIntLog2(std::max(IntAbs(topMvyLimit)  - 1, 1));
    vuiInfo->vui_num_ticks_poc_diff_one_minus1 = 0;

    return vuiInfo;
}

bool EncoderConfigH265::IsSuitableLevel(uint32_t levelIdx, bool highTier)
{
    if (levelIdx >= levelLimitsTblSize) {
        assert(!"The h.265 level index is invalid");
        return false;
    }

    uint32_t widthCtbAligned = 0, heightCtbAligned = 0;
    uint32_t picSizeInSamples = GetCtbAlignedPicSizeInSamples(widthCtbAligned, heightCtbAligned);

    uint32_t maxCPBSize = highTier ? levelLimits[levelIdx].maxCPBSizeHighTier : levelLimits[levelIdx].maxCPBSizeMainTier;
    uint32_t maxBitRate = highTier ? levelLimits[levelIdx].maxBitRateHighTier : levelLimits[levelIdx].maxBitRateMainTier;
    uint32_t cpbFactor = GetCpbVclFactor();

    if (picSizeInSamples > levelLimits[levelIdx].maxLumaPS) {
        return false;
    }

    if (widthCtbAligned > (uint32_t)sqrt(levelLimits[levelIdx].maxLumaPS * 8.0)) {
        return false;
    }

    if (heightCtbAligned > (uint32_t)sqrt(levelLimits[levelIdx].maxLumaPS * 8.0)) {
        return false;
    }

    if ((vbvBufferSize != 0) && (vbvBufferSize > (maxCPBSize * cpbFactor))) {
        return false;
    }

    if (maxBitrate != 0 && (maxBitrate > maxBitRate * cpbFactor)) {
        return false;
    }

    if (averageBitrate != 0 && (averageBitrate > maxBitRate * cpbFactor)) {
        return false;
    }

    return true;
}

void EncoderConfigH265::InitializeSpsRefPicSet(SpsH265 *pSps)
{
    uint32_t mask = 0;

    pSps->sps.num_short_term_ref_pic_sets = 1;

    // Set up the short-term RPS in the SPS
    pSps->shortTermRefPicSet.flags.inter_ref_pic_set_prediction_flag = 0;

    pSps->shortTermRefPicSet.flags.delta_rps_sign = 0;
    pSps->shortTermRefPicSet.delta_idx_minus1 = pSps->sps.num_short_term_ref_pic_sets - 1;
    pSps->shortTermRefPicSet.use_delta_flag = 0; // Setting 0 for now
    pSps->shortTermRefPicSet.abs_delta_rps_minus1 = 0;

    pSps->shortTermRefPicSet.used_by_curr_pic_flag = 0;

    // Set number of backward references
    pSps->shortTermRefPicSet.num_negative_pics = pSps->decPicBufMgr.max_dec_pic_buffering_minus1[0];
    mask = (1 << std::min(pSps->shortTermRefPicSet.num_negative_pics, numRefL0)) - 1;
    assert((mask & (1 << MAX_NUM_REF_PICS)) == 0); // assert that we're not using more than 15 references.
    pSps->shortTermRefPicSet.used_by_curr_pic_s0_flag = (uint16_t)mask;

    // Set number of backward references (0 by default)
    pSps->shortTermRefPicSet.num_positive_pics = 0;
    pSps->shortTermRefPicSet.used_by_curr_pic_s1_flag = 0;

    memset(&pSps->shortTermRefPicSet.delta_poc_s0_minus1, 0, sizeof(pSps->shortTermRefPicSet.delta_poc_s0_minus1));
    memset(&pSps->shortTermRefPicSet.delta_poc_s1_minus1, 0, sizeof(pSps->shortTermRefPicSet.delta_poc_s1_minus1));

    // Set up the long-term RPS in the SPS (currently empty)
    pSps->sps.num_long_term_ref_pics_sps = 0;
    pSps->longTermRefPicsSps.used_by_curr_pic_lt_sps_flag = 0;
    memset(&pSps->longTermRefPicsSps.lt_ref_pic_poc_lsb_sps, 0, sizeof(pSps->longTermRefPicsSps.lt_ref_pic_poc_lsb_sps));
}

StdVideoH265ProfileTierLevel EncoderConfigH265::GetLevelTier()
{
    StdVideoH265ProfileTierLevel profileTierLevel{};
    profileTierLevel.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_INVALID;
    profileTierLevel.general_level_idc = STD_VIDEO_H265_LEVEL_IDC_INVALID;
    uint32_t levelIdx = 0;
    for (; levelIdx < levelLimitsTblSize; levelIdx++) {

        if (IsSuitableLevel(levelIdx, 0)) {
            profileTierLevel.general_level_idc = levelLimits[levelIdx].stdLevel;
            profileTierLevel.flags.general_tier_flag = 0; // Main tier
            break;
        }

        if ((levelLimits[levelIdx].levelIdc >= 120) && // level 4.0 and above
            IsSuitableLevel(levelIdx, 1)) {

            profileTierLevel.general_level_idc = levelLimits[levelIdx].stdLevel;
            profileTierLevel.flags.general_tier_flag = 1; // Main tier
            break;
        }
    }

    if (levelIdx >= levelLimitsTblSize) {
        assert(!"No suitable level selected");
    }

    return profileTierLevel;
}

bool EncoderConfigH265::InitRateControl()
{
    StdVideoH265ProfileTierLevel profileTierLevel = GetLevelTier();
    // Assigning default main profile if invalid.
    if (profileTierLevel.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_INVALID) {
        profileTierLevel.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
    }
    uint32_t level = profileTierLevel.general_level_idc;
    if (level >= levelLimitsTblSize) {
        assert(!"The h.265 level index is invalid");
        return false;
    }
    uint32_t cpbVclFactor = GetCpbVclFactor();

    // Safe default maximum bitrate
    uint32_t levelBitRate = std::max(averageBitrate, hrdBitrate);
    levelBitRate = std::max(levelBitRate, std::min(levelLimits[level].maxBitRateMainTier * 800u, 120000000u));

    // If no bitrate is specified, use the level limit
    if (averageBitrate == 0) {
        averageBitrate = hrdBitrate ? hrdBitrate : levelBitRate;
    }

    // If no HRD bitrate is specified, use 3x average for VBR (without going above level limit) or equal to average bitrate for CBR
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

    // Use the main tier level limit for the max VBV buffer size, and no more than 8 seconds at peak rate
    if (vbvBufferSize == 0) {
        vbvBufferSize = std::min(levelLimits[level].maxCPBSizeMainTier * cpbVclFactor, 100000000u);
        if (rateControlMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
            if ((vbvBufferSize >> 3) > hrdBitrate) {
                vbvBufferSize = hrdBitrate << 3;
            }
        }
    }

    if (vbvInitialDelay == 0) {
        // 90% occupancy or at least one second of fullness if possible
        vbvInitialDelay = std::max(vbvBufferSize - vbvBufferSize / 10, std::min(vbvBufferSize, hrdBitrate));
    } else if (vbvInitialDelay > vbvBufferSize) {
        vbvInitialDelay = vbvBufferSize;
    }

    // CBR (make peak bitrate = avg bitrate)
    if (rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR) {
        hrdBitrate = averageBitrate;
    }

    // Average bitrate can't be higher than max bitrate
    if (averageBitrate > hrdBitrate) {
        averageBitrate = hrdBitrate;
    }

    return true;
}

bool EncoderConfigH265::GetRateControlParameters(VkVideoEncodeRateControlInfoKHR *rcInfo,
                                                 VkVideoEncodeRateControlLayerInfoKHR *pRcLayerInfo,
                                                 VkVideoEncodeH265RateControlInfoKHR *rcInfoH265,
                                                 VkVideoEncodeH265RateControlLayerInfoKHR *rcLayerInfoH265)
{
    if (rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR) {
        rcInfo->rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
    } else {
        rcInfo->rateControlMode = rateControlMode;
    }

    pRcLayerInfo->frameRateNumerator = frameRateNumerator;
    pRcLayerInfo->frameRateDenominator = frameRateDenominator;
    pRcLayerInfo->averageBitrate = averageBitrate;
    pRcLayerInfo->maxBitrate = hrdBitrate;

    if ((averageBitrate > 0) || (hrdBitrate > 0)) {
        rcInfo->virtualBufferSizeInMs = (uint32_t)(vbvBufferSize * 1000ull / (hrdBitrate ? hrdBitrate : averageBitrate));
        rcInfo->initialVirtualBufferSizeInMs = (uint32_t)(vbvInitialDelay * 1000ull / (hrdBitrate ? hrdBitrate : averageBitrate));
    }

    rcInfoH265->consecutiveBFrameCount = gopStructure.GetConsecutiveBFrameCount();

    rcInfoH265->gopFrameCount = (gopStructure.GetGopFrameCount() > 0) ? gopStructure.GetGopFrameCount() : uint32_t(DEFAULT_GOP_FRAME_COUNT);
    rcInfoH265->idrPeriod = (gopStructure.GetIdrPeriod() > 0) ? gopStructure.GetIdrPeriod() : uint32_t(DEFAULT_GOP_IDR_PERIOD);

    if (rcInfo->rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
        rcLayerInfoH265->minQp = rcLayerInfoH265->maxQp = minQp;
    } else {
        rcLayerInfoH265->minQp = minQp;
        rcLayerInfoH265->maxQp = maxQp;
    }

    return true;
}

bool EncoderConfigH265::InitParamameters(VpsH265 *vpsInfo, SpsH265 *spsInfo,
                                         StdVideoH265PictureParameterSet *pps,
                                         StdVideoH265SequenceParameterSetVui* vui)
{
    uint32_t maxSubLayersMinus1 = (gopStructure.GetTemporalLayerCount() > 0) ? (gopStructure.GetTemporalLayerCount() - 1) : 0;
    assert(maxSubLayersMinus1 == 0);

    for (uint32_t i = 0; i <= maxSubLayersMinus1; i++) {
        spsInfo->decPicBufMgr.max_latency_increase_plus1[i] = 0;
        spsInfo->decPicBufMgr.max_dec_pic_buffering_minus1[i] = (uint8_t)(dpbCount - 1);
        spsInfo->decPicBufMgr.max_num_reorder_pics[i] = gopStructure.GetConsecutiveBFrameCount() ? 1 : 0;
    }

    spsInfo->profileTierLevel = GetLevelTier();
    spsInfo->profileTierLevel.flags.general_tier_flag = general_tier_flag;

    // Always insert profile tier flags assuming frame mode
    // as field Mode is not currently supported for HEVC
    spsInfo->profileTierLevel.flags.general_progressive_source_flag = 1;
    spsInfo->profileTierLevel.flags.general_interlaced_source_flag = 0;
    spsInfo->profileTierLevel.flags.general_non_packed_constraint_flag = 0;
    spsInfo->profileTierLevel.flags.general_frame_only_constraint_flag = 1;

    // Assigning default main profile if invalid.
    if (spsInfo->profileTierLevel.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_INVALID) {
        if (encodeChromaSubsampling == VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR) {
            if (input.bpp == 8) {
                spsInfo->profileTierLevel.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
            } else if (input.bpp == 10) {
                spsInfo->profileTierLevel.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
            } else {
                spsInfo->profileTierLevel.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS;
            }
        } else {
            spsInfo->profileTierLevel.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS;
        }
    }

    const uint32_t ctbLog2SizeY = cuSize + 3;
    const uint32_t minCbLog2SizeY = cuMinSize + 3;
    const uint32_t log2MinTransformBlockSize = std::min(minCbLog2SizeY, minTransformUnitSize + 2U);
    const uint32_t log2MaxTransformBlockSize = std::min(ctbLog2SizeY, maxTransformUnitSize + 2U);
    uint32_t picWidthAlignedToMinCbsY  = 0;
    uint32_t picHeightAlignedToMinCbsY = 0;
    GetCtbAlignedPicSizeInSamples(picWidthAlignedToMinCbsY, picHeightAlignedToMinCbsY, true);

    spsInfo->sps.flags.sps_temporal_id_nesting_flag = 1;
    spsInfo->sps.flags.separate_colour_plane_flag = 0;
    spsInfo->sps.flags.sps_sub_layer_ordering_info_present_flag = 1;
    spsInfo->sps.flags.scaling_list_enabled_flag = 0;
    spsInfo->sps.flags.sps_scaling_list_data_present_flag = 0;
    spsInfo->sps.flags.amp_enabled_flag = 1;
    spsInfo->sps.flags.sample_adaptive_offset_enabled_flag = 1; // PASCAL_OR_LATER this flag is 1 by default
    spsInfo->sps.flags.pcm_enabled_flag = 0;
    spsInfo->sps.flags.pcm_loop_filter_disabled_flag = 0;
    spsInfo->sps.flags.long_term_ref_pics_present_flag = 0; // Setting this flag to 0 by default for now.
    spsInfo->sps.flags.sps_temporal_mvp_enabled_flag = 0;
    spsInfo->sps.flags.strong_intra_smoothing_enabled_flag = 0;
    spsInfo->sps.flags.vui_parameters_present_flag = 1;
    spsInfo->sps.flags.sps_extension_present_flag = 0;
    spsInfo->sps.flags.sps_range_extension_flag = 0;
    spsInfo->sps.flags.transform_skip_rotation_enabled_flag = 0;
    spsInfo->sps.flags.transform_skip_context_enabled_flag = 0;
    spsInfo->sps.flags.implicit_rdpcm_enabled_flag = 0;
    spsInfo->sps.flags.explicit_rdpcm_enabled_flag = 0;
    spsInfo->sps.flags.extended_precision_processing_flag = 0;
    spsInfo->sps.flags.intra_smoothing_disabled_flag = 0;
    spsInfo->sps.flags.high_precision_offsets_enabled_flag = 0;
    spsInfo->sps.flags.persistent_rice_adaptation_enabled_flag = 0;
    spsInfo->sps.flags.cabac_bypass_alignment_enabled_flag = 0;
    spsInfo->sps.flags.sps_scc_extension_flag = 0;
    spsInfo->sps.flags.sps_curr_pic_ref_enabled_flag = 0;
    spsInfo->sps.flags.palette_mode_enabled_flag = 0;
    spsInfo->sps.flags.sps_palette_predictor_initializers_present_flag = 0;
    spsInfo->sps.flags.intra_boundary_filtering_disabled_flag = 0;

    spsInfo->sps.chroma_format_idc = (StdVideoH265ChromaFormatIdc)(FastIntLog2<uint32_t>(encodeChromaSubsampling) - 1);
    // pic_width_in_luma_samples specifies the width of each decoded picture in units of luma samples.
    // pic_width_in_luma_samples shall not be equal to 0 and shall be an integer multiple of MinCbSizeY.
    spsInfo->sps.pic_width_in_luma_samples  = picWidthAlignedToMinCbsY;
    // pic_height_in_luma_samples specifies the height of each decoded picture in units of luma samples.
    // pic_height_in_luma_samples shall not be equal to 0 and shall be an integer multiple of MinCbSizeY.
    spsInfo->sps.pic_height_in_luma_samples = picHeightAlignedToMinCbsY;

    if (verbose) {
        std::cout << "sps.pic_width_in_luma_samples: " << spsInfo->sps.pic_width_in_luma_samples
                  << ", sps.pic_height_in_luma_samples: " << spsInfo->sps.pic_height_in_luma_samples
                  << ", cuSize: " << (uint32_t)cuSize << ", cuMinSize: " << (uint32_t)cuMinSize << std::endl;
    }

    spsInfo->sps.sps_video_parameter_set_id = vpsId;
    spsInfo->sps.sps_max_sub_layers_minus1  = 0;
    spsInfo->sps.sps_seq_parameter_set_id   = spsId;
    spsInfo->sps.bit_depth_luma_minus8      = (uint8_t)(encodeBitDepthLuma - 8);
    spsInfo->sps.bit_depth_chroma_minus8    = (uint8_t)(encodeBitDepthChroma - 8);
    spsInfo->sps.log2_max_pic_order_cnt_lsb_minus4 = 4;
    spsInfo->sps.log2_min_luma_coding_block_size_minus3 = (uint8_t)(minCbLog2SizeY - 3);
    spsInfo->sps.log2_diff_max_min_luma_coding_block_size = (uint8_t)(ctbLog2SizeY - minCbLog2SizeY);
    spsInfo->sps.log2_min_luma_transform_block_size_minus2 = (uint8_t)(log2MinTransformBlockSize - 2);
    spsInfo->sps.log2_diff_max_min_luma_transform_block_size = (uint8_t)(log2MaxTransformBlockSize - log2MinTransformBlockSize);
    spsInfo->sps.max_transform_hierarchy_depth_inter = (uint8_t)(std::max(ctbLog2SizeY - log2MinTransformBlockSize, 1U));
    spsInfo->sps.max_transform_hierarchy_depth_intra = 3;
    spsInfo->sps.pcm_sample_bit_depth_luma_minus1 = 8 - 1;
    spsInfo->sps.pcm_sample_bit_depth_chroma_minus1 = 8 - 1;
    spsInfo->sps.log2_min_pcm_luma_coding_block_size_minus3 = (uint8_t)(minCbLog2SizeY - 3);
    spsInfo->sps.log2_diff_max_min_pcm_luma_coding_block_size = (uint8_t)(ctbLog2SizeY - minCbLog2SizeY);

    if (verbose) {
        std::cout << "sps.log2_min_luma_coding_block_size_minus3: "         << (uint32_t)spsInfo->sps.log2_min_luma_coding_block_size_minus3
                  << ", sps.log2_diff_max_min_luma_coding_block_size: "     << (uint32_t)spsInfo->sps.log2_diff_max_min_luma_coding_block_size
                  << ", sps.log2_min_luma_transform_block_size_minus2: "    << (uint32_t)spsInfo->sps.log2_min_luma_transform_block_size_minus2
                  << ", sps.log2_diff_max_min_luma_transform_block_size: "  << (uint32_t)spsInfo->sps.log2_diff_max_min_luma_transform_block_size
                  << ", sps.max_transform_hierarchy_depth_inter:"           << (uint32_t)spsInfo->sps.max_transform_hierarchy_depth_inter
                  << ", sps.log2_min_pcm_luma_coding_block_size_minus3: "   << (uint32_t)spsInfo->sps.log2_min_pcm_luma_coding_block_size_minus3
                  << ", sps.log2_diff_max_min_pcm_luma_coding_block_size: " << (uint32_t)spsInfo->sps.log2_diff_max_min_pcm_luma_coding_block_size
                  << std::endl;
    }

    uint32_t subWidthC  = (encodeChromaSubsampling == VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR) ? 1 : 2;
    uint32_t subHeightC = (encodeChromaSubsampling == VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR) ? 1 : 2;
    spsInfo->sps.conf_win_left_offset   = 0;
    spsInfo->sps.conf_win_right_offset  = (picWidthAlignedToMinCbsY - encodeWidth) / subWidthC;
    spsInfo->sps.conf_win_top_offset    = 0;
    spsInfo->sps.conf_win_bottom_offset = (picHeightAlignedToMinCbsY - encodeHeight) / subHeightC;
    spsInfo->sps.flags.conformance_window_flag = ((spsInfo->sps.conf_win_left_offset != 0) ||
                                                      (spsInfo->sps.conf_win_right_offset != 0) ||
                                                      (spsInfo->sps.conf_win_top_offset != 0) ||
                                                      (spsInfo->sps.conf_win_bottom_offset != 0));

    if (verbose) {
        std::cout << "sps.conf_win_left_offset: "     << spsInfo->sps.conf_win_left_offset
                  << ", sps.conf_win_right_offset: "  << spsInfo->sps.conf_win_right_offset
                  << ", sps.conf_win_top_offset: "    << spsInfo->sps.conf_win_top_offset
                  << ", sps.conf_win_bottom_offset: " << spsInfo->sps.conf_win_bottom_offset
                  << ", sps.flags.conformance_window_flag: " << spsInfo->sps.flags.conformance_window_flag
                  << std::endl;
    }

    spsInfo->sps.pScalingLists = NULL;

    spsInfo->sps.pSequenceParameterSetVui = vui;
    spsInfo->sps.pPredictorPaletteEntries = NULL;

    InitializeSpsRefPicSet(spsInfo);

    // Assign VPS members after the SPS has been filled in.
    vpsInfo->vpsInfo.flags.vps_temporal_id_nesting_flag = spsInfo->sps.flags.sps_temporal_id_nesting_flag;
    vpsInfo->vpsInfo.flags.vps_sub_layer_ordering_info_present_flag = 1;
    vpsInfo->vpsInfo.flags.vps_timing_info_present_flag = 0;
    vpsInfo->vpsInfo.flags.vps_poc_proportional_to_timing_flag = 0;
    vpsInfo->vpsInfo.vps_video_parameter_set_id = vpsId;
    vpsInfo->vpsInfo.vps_max_sub_layers_minus1 = (uint8_t)maxSubLayersMinus1;
    vpsInfo->vpsInfo.vps_num_units_in_tick = 0;
    vpsInfo->vpsInfo.vps_time_scale = 0;
    vpsInfo->vpsInfo.vps_num_ticks_poc_diff_one_minus1 = 0;

    vpsInfo->vpsInfo.pHrdParameters = &spsInfo->hrdParameters;
    vpsInfo->vpsInfo.pProfileTierLevel = &spsInfo->profileTierLevel;
    vpsInfo->vpsInfo.pDecPicBufMgr = &spsInfo->decPicBufMgr;

    pps->flags.dependent_slice_segments_enabled_flag = 0;
    pps->flags.output_flag_present_flag = 0;
    pps->flags.sign_data_hiding_enabled_flag = 0;
    pps->flags.cabac_init_present_flag = 1;
    pps->flags.constrained_intra_pred_flag = 0;
    pps->flags.transform_skip_enabled_flag = 1; // TODO: if (m_stHEVCEncInitParams.iBitDepthLuma == 10 && m_stHEVCEncInitParams.iBitDepthChroma==10 && (m_devCaps & NVVID_DEVCAPS_NVENC_PASCAL_OR_LATER)) m_stHEVCEncInitParams.bTransformSkipEnabledFlag = false;
    pps->flags.cu_qp_delta_enabled_flag = 1;
    pps->flags.pps_slice_chroma_qp_offsets_present_flag = 0;
    pps->flags.weighted_pred_flag = 0;
    pps->flags.weighted_bipred_flag = 0;
    pps->flags.transquant_bypass_enabled_flag = (tuningMode == VK_VIDEO_ENCODE_TUNING_MODE_LOSSLESS_KHR) ? 1 : 0;
    pps->flags.tiles_enabled_flag = 0;
    pps->flags.entropy_coding_sync_enabled_flag = 0;
    pps->flags.uniform_spacing_flag = 0;
    pps->flags.loop_filter_across_tiles_enabled_flag = 0;
    pps->flags.pps_loop_filter_across_slices_enabled_flag = 1;
    pps->flags.deblocking_filter_control_present_flag = 1;
    pps->flags.pps_scaling_list_data_present_flag = 0;
    pps->flags.lists_modification_present_flag = 0; // TODO: true for LTR
    pps->flags.slice_segment_header_extension_present_flag = 0;
    pps->flags.pps_extension_present_flag = 0;
    pps->flags.cross_component_prediction_enabled_flag = 0;
    pps->flags.chroma_qp_offset_list_enabled_flag = 0;
    pps->flags.pps_curr_pic_ref_enabled_flag = 0;
    pps->flags.residual_adaptive_colour_transform_enabled_flag = 0;
    pps->flags.pps_slice_act_qp_offsets_present_flag = 0;
    pps->flags.pps_palette_predictor_initializers_present_flag = 0;
    pps->flags.monochrome_palette_flag = 0;
    pps->flags.pps_range_extension_flag = 0;
    pps->pps_pic_parameter_set_id = ppsId;
    pps->pps_seq_parameter_set_id = spsId;
    pps->sps_video_parameter_set_id = vpsId;
    pps->num_extra_slice_header_bits = 0;
    pps->num_ref_idx_l0_default_active_minus1 = numRefL0 > 0 ? (uint8_t)(numRefL0 - 1) : 0;
    pps->num_ref_idx_l1_default_active_minus1 = numRefL1 > 0 ? (uint8_t)(numRefL1 - 1) : 0;
    pps->init_qp_minus26 = 0;
    pps->diff_cu_qp_delta_depth = 0;
    pps->pps_cb_qp_offset = 0;

    pps->pps_beta_offset_div2 = 0;
    pps->pps_tc_offset_div2 = 0;
    pps->log2_parallel_merge_level_minus2 = 0;
    pps->num_tile_columns_minus1 = 0;
    pps->num_tile_rows_minus1 = 0;

    return true;
}




