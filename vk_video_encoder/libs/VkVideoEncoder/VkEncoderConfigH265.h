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

#ifndef VKVIDEOENCODER_VKENCODERCONFIG_H265_H_
#define VKVIDEOENCODER_VKENCODERCONFIG_H265_H_

#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkVideoEncoder/VkVideoEncoderStateH265.h"

struct EncoderConfigH265 : public EncoderConfig {

    enum { FRAME_RATE_NUM_DEFAULT = 30000 };
    enum { FRAME_RATE_DEN_DEFAULT = 1001 };
    enum { MAX_LEVELS = 14 };
    enum { MAX_NUM_REF_PICS = 15 };
    enum { LOG2_MB_SIZE = 4 };

    enum CuSize
    {
        CU_SIZE_8x8   = 0U,
        CU_SIZE_16x16 = 1U,
        CU_SIZE_32x32 = 2U,
        CU_SIZE_64x64 = 4U,
    };

    enum TransformUnitSize
    {
        TU_SIZE_4x4   = 0U,
        TU_SIZE_8x8   = 1U,
        TU_SIZE_16x16 = 2U,
        TU_SIZE_32x32 = 3U,
    };

    struct LevelLimits {
        int32_t              levelIdc;   // 30 * Level Number
        uint32_t             maxLumaPS;
        int32_t              maxCPBSizeMainTier; // 1000 bits
        int32_t              maxCPBSizeHighTier; // 1000 bits
        int32_t              maxSliceSegmentPerPicture;
        int32_t              maxTileRows;
        int32_t              maxTileCols;
        uint32_t             maxLumaSr;
        int32_t              maxBitRateMainTier; // 1000 bits/sec
        int32_t              maxBitRateHighTier; //1000 bits/sec
        int32_t              minCr;
        StdVideoH265LevelIdc stdLevel;
    };

    StdVideoH265ProfileIdc profile;
    StdVideoH265LevelIdc   levelIdc;
    VkVideoEncodeH265CapabilitiesKHR h265EncodeCapabilities;
    VkVideoEncodeH265QualityLevelPropertiesKHR h265QualityLevelProperties;
    VkVideoEncodeH265QuantizationMapCapabilitiesKHR h265QuantizationMapCapabilities;
    uint32_t               general_tier_flag : 1; // Specifies the level tier of the encoded bitstream.
    uint8_t                numRefL0;              // Specifies max number of L0 list reference frame used for prediction of a frame.
    uint8_t                numRefL1;              // Specifies max number of L1 list reference frame used for prediction of a frame.
    uint8_t                vpsId;                 // Specifies the VPS id of the picture header
    uint8_t                spsId;                 // Specifies the SPS id of the sequence header
    uint8_t                ppsId;                 // Specifies the PPS id of the picture header
    CuSize                 cuMinSize;
    CuSize                 cuSize;
    TransformUnitSize      minTransformUnitSize;
    TransformUnitSize      maxTransformUnitSize;
    uint32_t               vbvBufferSize;         // Specifies the VBV(HRD) buffer size. in bits. Set 0 to use the default VBV  buffer size.
    uint32_t               vbvInitialDelay;       // Specifies the VBV(HRD) initial delay in bits. Set 0 to use the default VBV  initial delay.
    VkVideoEncodeH265QpKHR minQp;                 // Specifies the const or minimum or QP used for rate control.
    VkVideoEncodeH265QpKHR maxQp;                 // Specifies the maximum QP used for rate control.
    const LevelLimits*     levelLimits;
    size_t                 levelLimitsTblSize;

    EncoderConfigH265()
      : profile(STD_VIDEO_H265_PROFILE_IDC_MAIN)
      , levelIdc(STD_VIDEO_H265_LEVEL_IDC_5_2)
      , h265EncodeCapabilities()
      , general_tier_flag(false)
      , numRefL0(1)
      , numRefL1(1)
      , vpsId(0)
      , spsId(0)
      , ppsId(0)
      , cuMinSize(CU_SIZE_16x16)            // TODO: adjust based on device capabilities
      , cuSize(CU_SIZE_32x32)               // TODO: adjust based on device capabilities
      , minTransformUnitSize(TU_SIZE_4x4)   // TODO: adjust based on device capabilities
      , maxTransformUnitSize(TU_SIZE_32x32) // TODO: adjust based on device capabilities
      , vbvBufferSize()
      , vbvInitialDelay()
      , minQp{}
      , maxQp{}
    {
        // Table A-1
        static const LevelLimits levelLimitsTbl[] = {
            // Level, MaxLumaPS, MaxCPB(Main), MaxCPB(High), MaxSliceSegPerPic, MaxTileRows, MaxTileCols,   MaxLumaSr, MaxBR(Main), MaxBR(High), MinCr(Main)
            {     30,     36864,          350,           -1,                16,           1,           1,      552960,         128,     -1,           2, STD_VIDEO_H265_LEVEL_IDC_1_0},
            {     60,    122880,         1500,           -1,                16,           1,           1,     3686400,        1500,     -1,           2, STD_VIDEO_H265_LEVEL_IDC_2_0},
            {     63,    245760,         3000,           -1,                20,           1,           1,     7372800,        3000,     -1,           2, STD_VIDEO_H265_LEVEL_IDC_2_1},
            {     90,    552960,         6000,           -1,                30,           2,           2,    16588800,        6000,     -1,           2, STD_VIDEO_H265_LEVEL_IDC_3_0},
            {     93,    983040,        10000,           -1,                40,           3,           3,    33177600,       10000,     -1,           2, STD_VIDEO_H265_LEVEL_IDC_3_1},
            {    120,   2228224,        12000,        30000,                75,           5,           5,    66846720,       12000,  30000,           4, STD_VIDEO_H265_LEVEL_IDC_4_0},
            {    123,   2228224,        20000,        50000,                75,           5,           5,   133693440,       20000,  50000,           4, STD_VIDEO_H265_LEVEL_IDC_4_1},
            {    150,   8912896,        25000,       100000,               200,          11,          10,   267386880,       25000, 100000,           6, STD_VIDEO_H265_LEVEL_IDC_5_0},
            {    153,   8912896,        40000,       160000,               200,          11,          10,   534773760,       40000, 160000,           8, STD_VIDEO_H265_LEVEL_IDC_5_1},
            {    156,   8912896,        60000,       240000,               200,          11,          10,  1069547520,       60000, 240000,           8, STD_VIDEO_H265_LEVEL_IDC_5_2},
            {    180,  35651584,        60000,       240000,               600,          22,          20,  1069547520,       60000, 240000,           8, STD_VIDEO_H265_LEVEL_IDC_6_0},
            {    183,  35651584,       120000,       480000,               600,          22,          20,  2139095040,      120000, 480000,           8, STD_VIDEO_H265_LEVEL_IDC_6_1},
            {    186,  35651584,       240000,       800000,               600,          22,          20, 4278190080U,      240000, 800000,           6, STD_VIDEO_H265_LEVEL_IDC_6_2},
            {    187,  67108864,       240000,       800000,               600,          22,          20, 4278190080U,      240000, 800000,           6, STD_VIDEO_H265_LEVEL_IDC_6_2},      //8kx8k support
        };

        levelLimits = levelLimitsTbl;
        levelLimitsTblSize = ARRAYSIZE(levelLimitsTbl);

        frameRateNumerator = FRAME_RATE_NUM_DEFAULT;
        frameRateDenominator = FRAME_RATE_DEN_DEFAULT;

    }

    virtual ~EncoderConfigH265() {}

    virtual EncoderConfigH265* GetEncoderConfigh265() {
        return this;
    }

    uint32_t GetCtbAlignedPicSizeInSamples(uint32_t& picWidthInCtbsY, uint32_t& picHeightInCtbsY, bool minCtbsY = false);

    uint32_t GetCpbVclFactor();

    virtual VkResult InitializeParameters()
    {
        VkResult result = EncoderConfig::InitializeParameters();
        if (result != VK_SUCCESS) {
            return result;
        }
        // TODO: more h.265 parameters init ...
        return VK_SUCCESS;
    }

    virtual VkResult InitDeviceCapabilities(const VulkanDeviceContext* vkDevCtx);

    virtual uint32_t GetDefaultVideoProfileIdc() { return STD_VIDEO_H265_PROFILE_IDC_MAIN; };

    // 1. First h.265 determine the number of the Dpb buffers required
    virtual int8_t InitDpbCount();

    uint32_t GetMaxDpbSize(uint32_t pictureSizeInSamplesY, int32_t levelIndex);
    int8_t VerifyDpbSize();

    // 2. First h.265 determine the rate control parameters
    virtual bool InitRateControl();

    virtual uint8_t GetMaxBFrameCount() { return  static_cast<uint8_t>(h265EncodeCapabilities.maxBPictureL0ReferenceCount); }

    bool GetRateControlParameters(VkVideoEncodeRateControlInfoKHR *rcInfo,
                                  VkVideoEncodeRateControlLayerInfoKHR *pRcLayerInfo,
                                  VkVideoEncodeH265RateControlInfoKHR *rcInfoH265,
                                  VkVideoEncodeH265RateControlLayerInfoKHR *rcLayerInfoH265);

    // 3. Init the default h.265 VUI parameters
    StdVideoH265SequenceParameterSetVui*
    InitVuiParameters(StdVideoH265SequenceParameterSetVui *vuiInfo,
                      StdVideoH265HrdParameters *pHrdParameters,
                      StdVideoH265SubLayerHrdParameters *pSubLayerHrdParametersNal);

    // 4. Init the default h.265 PPS and SPS parameters
    bool InitParamameters(VpsH265 *vpsInfo, SpsH265 *spsInfo,
                          StdVideoH265PictureParameterSet *pps,
                          StdVideoH265SequenceParameterSetVui* vui = nullptr);

    bool IsSuitableLevel(uint32_t levelIdx, bool highTier);
    StdVideoH265ProfileTierLevel GetLevelTier();
    void InitializeSpsRefPicSet(SpsH265 *pSps);

};

#endif /* VKVIDEOENCODER_VKENCODERCONFIG_H265_H_ */
