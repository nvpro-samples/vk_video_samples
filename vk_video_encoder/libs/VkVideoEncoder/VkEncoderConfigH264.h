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

#ifndef VKVIDEOENCODER_VKENCODERCONFIG_H264_H_
#define VKVIDEOENCODER_VKENCODERCONFIG_H264_H_

#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkVideoEncoder/VkVideoEncoderStateH264.h"

struct EncoderConfigH264 : public EncoderConfig {

    /**
     * H.264 entropy coding modes.
     */
    enum EntropyCodingMode
    {
        ENTROPY_CODING_MODE_CABAC      = 0x1,   /**< Entropy coding mode is CABAC */
        ENTROPY_CODING_MODE_CAVLC      = 0x2    /**< Entropy coding mode is CAVLC */
    };

    /**
     * H.264 specific Adaptive Transform modes
     */
    enum AdaptiveTransformMode
    {
        ADAPTIVE_TRANSFORM_AUTOSELECT = 0x0,   /**< Adaptive Transform 8x8 mode is auto selected by the encoder driver*/
        ADAPTIVE_TRANSFORM_DISABLE    = 0x1,   /**< Adaptive Transform 8x8 mode disabled */
        ADAPTIVE_TRANSFORM_ENABLE     = 0x2,   /**< Adaptive Transform 8x8 mode should be used */
    };

    enum { FRAME_RATE_NUM_DEFAULT = 30000 };
    enum { FRAME_RATE_DEN_DEFAULT = 1001 };
    enum { IDR_PERIOD_DEFAULT = 30 };
    enum { GOP_LENGTH_DEFAULT = 30 };

    struct LevelLimits {
        uint32_t level_idc;  // 10 * Level Number
        uint32_t maxMBPS;    // MB/s
        uint32_t maxFS;      // MBs
        double   maxDPB;     // 1024 bytes
        uint32_t maxBR;      // 1200 bits/s
        uint32_t maxCPB;     // 1200 bits
        uint32_t maxVmvR;    // [-MaxVmvR..+MaxVmvR-0.25]
        uint32_t prog;       // frame_mbs_only_flag = 1
        StdVideoH264LevelIdc level;
    };

    EncoderConfigH264()
        : profileIdc(STD_VIDEO_H264_PROFILE_IDC_INVALID)
        , levelIdc(STD_VIDEO_H264_LEVEL_IDC_5_0)
        , h264EncodeCapabilities()
        , hrdBitrate(maxBitrate)
        , pic_width_in_mbs(DivUp<uint32_t>(encodeWidth, 16))
        , pic_height_in_map_units(DivUp<uint32_t>(encodeHeight, 16))
        , numRefL0(0)
        , numRefL1(0)
        , numRefFrames(0)
        , entropyCodingMode(ENTROPY_CODING_MODE_CABAC)
        , adaptiveTransformMode(ADAPTIVE_TRANSFORM_ENABLE)
        , spsId(0)
        , ppsId()
        , numSlicesPerPicture(DEFAULT_NUM_SLICES_PER_PICTURE)
        , vbvBufferSize(0)
        , vbvInitialDelay(0)
        , minQp{0, 0, 0}
        , maxQp{0, 0, 0}
        , rcInfoH264{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR }
        , rcLayerInfoH264{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR }
        , rcInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR, &rcInfoH264 }
        , rcLayerInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR, &rcLayerInfoH264 }
        , disable_deblocking_filter_idc(STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED)
        , qpprime_y_zero_transform_bypass_flag(true)
        , constrained_intra_pred_flag(false)
        , levelLimits()
        , levelLimitsSize()
    {
            // Level limits (Table A-1)
            static const LevelLimits levelLimitsTbl[] = {
                // level_idc, maxMBPS, maxFS, maxDPB,  maxBR, maxCPB, maxVmvR, prog, level
                {10, 1485, 99, 148.5, 64, 175, 64, 1, STD_VIDEO_H264_LEVEL_IDC_1_0},
                {11, 3000, 396, 337.5, 192, 500, 128, 1, STD_VIDEO_H264_LEVEL_IDC_1_1},
                {12, 6000, 396, 891.0, 384, 1000, 128, 1, STD_VIDEO_H264_LEVEL_IDC_1_2},
                {13, 11880, 396, 891.0, 768, 2000, 128, 1, STD_VIDEO_H264_LEVEL_IDC_1_3},
                {20, 11880, 396, 891.0, 2000, 2000, 128, 1, STD_VIDEO_H264_LEVEL_IDC_2_0},
                {21, 19800, 792, 1782.0, 4000, 4000, 256, 0, STD_VIDEO_H264_LEVEL_IDC_2_1},
                {22, 20250, 1620, 3037.5, 4000, 4000, 256, 0, STD_VIDEO_H264_LEVEL_IDC_2_2},
                {30, 40500, 1620, 3037.5, 10000, 10000, 256, 0, STD_VIDEO_H264_LEVEL_IDC_3_0},
                {31, 108000, 3600, 6750.0, 14000, 14000, 512, 0, STD_VIDEO_H264_LEVEL_IDC_3_1},
                {32, 216000, 5120, 7680.0, 20000, 20000, 512, 0, STD_VIDEO_H264_LEVEL_IDC_3_2},
                {40, 245760, 8192, 12288.0, 20000, 25000, 512, 0, STD_VIDEO_H264_LEVEL_IDC_4_0},
                {41, 245760, 8192, 12288.0, 50000, 62500, 512, 0, STD_VIDEO_H264_LEVEL_IDC_4_1},
                {42, 522240, 8704, 13056.0, 50000, 62500, 512, 0, STD_VIDEO_H264_LEVEL_IDC_4_2},
                {50, 589824, 22080, 41400.0, 135000, 135000, 512, 0, STD_VIDEO_H264_LEVEL_IDC_5_0},
                {51, 983040, 36864, 69120.0, 240000, 240000, 512, 0, STD_VIDEO_H264_LEVEL_IDC_5_1},
                {52, 2073600, 36864, 69120.0, 240000, 240000, 512, 0, STD_VIDEO_H264_LEVEL_IDC_5_2},
            };

            levelLimits = levelLimitsTbl;
            levelLimitsSize = ARRAYSIZE(levelLimitsTbl);

            frameRateNumerator = FRAME_RATE_NUM_DEFAULT;
            frameRateDenominator = FRAME_RATE_DEN_DEFAULT;
    }

    virtual ~EncoderConfigH264() {}

    virtual EncoderConfigH264* GetEncoderConfigh264() {
        return this;
    }

    StdVideoH264ProfileIdc                     profileIdc;
    StdVideoH264LevelIdc                       levelIdc;
    VkVideoEncodeH264CapabilitiesKHR           h264EncodeCapabilities;
    VkVideoEncodeH264QualityLevelPropertiesKHR h264QualityLevelProperties;
    VkVideoEncodeH264QuantizationMapCapabilitiesKHR h264QuantizationMapCapabilities;
    uint32_t                                   hrdBitrate;           // hypothetical reference decoder bitrate
    uint32_t                                   pic_width_in_mbs;
    uint32_t                                   pic_height_in_map_units;
    uint8_t                                    numRefL0;
    uint8_t                                    numRefL1;
    uint8_t                                    numRefFrames;
    EntropyCodingMode                          entropyCodingMode;     // Specifies the entropy coding mode. Check support for CABAC mode!
    AdaptiveTransformMode                      adaptiveTransformMode; // Specifies the AdaptiveTransform Mode.
    uint8_t                                    spsId;                 // Specifies the SPS id of the sequence header
    uint8_t                                    ppsId;                 // Specifies the PPS id of the picture header
    uint32_t                                   numSlicesPerPicture;   // sliceModeData specifies number of slices in the picture.
    uint32_t                                   vbvBufferSize;         // Specifies the VBV(HRD) buffer size. in bits. Set 0 to use the default VBV  buffer size.
    uint32_t                                   vbvInitialDelay;       // Specifies the VBV(HRD) initial delay in bits. Set 0 to use the default VBV  initial delay.
    VkVideoEncodeH264QpKHR                     minQp;                 // Specifies the const or minimum or QP used for rate control.
    VkVideoEncodeH264QpKHR                     maxQp;                 // Specifies the maximum QP used for rate control.
    VkVideoEncodeH264RateControlInfoKHR        rcInfoH264;
    VkVideoEncodeH264RateControlLayerInfoKHR   rcLayerInfoH264;
    VkVideoEncodeRateControlInfoKHR            rcInfo;
    VkVideoEncodeRateControlLayerInfoKHR       rcLayerInfo;

    StdVideoH264DisableDeblockingFilterIdc     disable_deblocking_filter_idc;

    uint32_t qpprime_y_zero_transform_bypass_flag : 1;
    uint32_t constrained_intra_pred_flag : 1;

    const LevelLimits* levelLimits;
    size_t levelLimitsSize;

    StdVideoH264LevelIdc DetermineLevel(uint8_t dpbSize,
                                        uint32_t bitrate,
                                        uint32_t vbvBufferSize,
                                        double frameRate);

    static void SetAspectRatio(StdVideoH264SequenceParameterSetVui *vui, int32_t width, int32_t height,
                               int32_t darWidth, int32_t darHeight);

    virtual VkResult InitializeParameters()
    {
        VkResult result = EncoderConfig::InitializeParameters();
        if (result != VK_SUCCESS) {
            return result;
        }

        hrdBitrate = maxBitrate;
        pic_width_in_mbs = DivUp<uint32_t>(encodeWidth, 16);
        pic_height_in_map_units = DivUp<uint32_t>(encodeHeight, 16);

        if ((pic_width_in_mbs > 0) && (pic_height_in_map_units > 0)) {
            return VK_SUCCESS;
        }

        assert(!"Invalid pic_width_in_mbs and pic_height_in_map_units");
        return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
    }

    virtual VkResult InitDeviceCapabilities(const VulkanDeviceContext* vkDevCtx);

    virtual uint32_t GetDefaultVideoProfileIdc() { return STD_VIDEO_H264_PROFILE_IDC_HIGH; };

    // 1. First h.264 determine the number of the Dpb buffers required
    virtual int8_t InitDpbCount();

    // 2. First h.264 determine the rate control parameters
    virtual bool InitRateControl();

    virtual uint8_t GetMaxBFrameCount() { return static_cast<uint8_t>(h264EncodeCapabilities.maxBPictureL0ReferenceCount); }

    bool GetRateControlParameters(VkVideoEncodeRateControlInfoKHR *rcInfo,
                                  VkVideoEncodeRateControlLayerInfoKHR *pRcLayerInfo,
                                  VkVideoEncodeH264RateControlInfoKHR *rcInfoH264,
                                  VkVideoEncodeH264RateControlLayerInfoKHR *rcLayerInfoH264);

    // 3. Init the default h.264 VUI parameters
    StdVideoH264SequenceParameterSetVui* InitVuiParameters(StdVideoH264SequenceParameterSetVui *vui,
                                                           StdVideoH264HrdParameters *pHrdParameters);

    // 4. Init the default h.264 PPS and SPS parameters
    bool InitSpsPpsParameters(StdVideoH264SequenceParameterSet *sps,
                              StdVideoH264PictureParameterSet *pps,
                              StdVideoH264SequenceParameterSetVui* vui = nullptr);

};

#endif /* VKVIDEOENCODER_VKENCODERCONFIG_H264_H_ */
