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

#ifndef VKVIDEOENCODER_VKENCODERCONFIG_AV1_H_
#define VKVIDEOENCODER_VKENCODERCONFIG_AV1_H_

#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkVideoEncoder/VkVideoEncoderStateAV1.h"

#define FRAME_ID_BITS 15
#define DELTA_FRAME_ID_BITS 14
#define ORDER_HINT_BITS 7

#define BASE_QIDX_INTRA 114
#define BASE_QIDX_INTER_P 131
#define BASE_QIDX_INTER_B 147

struct EncoderConfigAV1 : public EncoderConfig {

    enum { FRAME_RATE_NUM_DEFAULT = 30000 };
    enum { FRAME_RATE_DEN_DEFAULT = 1001 };
    enum { IDR_PERIOD_DEFAULT = 60 };
    enum { GOP_LENGTH_DEFAULT = 60 };

    struct LevelLimits {
        StdVideoAV1Level    level;
        uint32_t            maxPicSize;     // samples
        uint32_t            maxHSize;       // samples
        uint32_t            maxVSize;       // samples
        uint64_t            maxDisplayRate; // samples/sec
        uint64_t            maxDecodeRate;  // samples/sec
        uint32_t            maxHeaderRate;  // /sec
        uint32_t            mainBps;        // bits/sec
        uint32_t            highBps;        // bits/sec
        double              mainCR;         // ratio
        double              highCR;         // ratio
        uint32_t            maxTiles;       //
        uint32_t            maxTileCols;    //
    };

    EncoderConfigAV1()
    {
        static const LevelLimits levelLimitsTbl[] = {
            { STD_VIDEO_AV1_LEVEL_2_0,       147456,    2048,   1152,      4423680,       5529600,    150,      1500000,           0,   2,   -1,     8,    4 },  // level 2.0
            { STD_VIDEO_AV1_LEVEL_2_1,       278784,    2816,   1584,      8363520,      10454400,    150,      3000000,           0,   2,   -1,     8,    4 },  // level 2.1
            { STD_VIDEO_AV1_LEVEL_INVALID,   278784,    2816,   1584,      8363520,      10454400,    150,      3000000,           0,   2,   -1,     8,    4 },  // level 2.2 - undefined
            { STD_VIDEO_AV1_LEVEL_INVALID,   278784,    2816,   1584,      8363520,      10454400,    150,      3000000,           0,   2,   -1,     8,    4 },  // level 2.3 - undefined
            { STD_VIDEO_AV1_LEVEL_3_0,       665856,    4352,   2448,     19975680,      24969600,    150,      6000000,           0,   2,   -1,    16,    6 },  // level 3.0
            { STD_VIDEO_AV1_LEVEL_3_1,      1065024,    5504,   3096,     31950720,      39938400,    150,     10000000,           0,   2,   -1,    16,    6 },  // level 3.1
            { STD_VIDEO_AV1_LEVEL_INVALID,  1065024,    5504,   3096,     31950720,      39938400,    150,     10000000,           0,   2,   -1,    16,    6 },  // level 3.2 - undefined
            { STD_VIDEO_AV1_LEVEL_INVALID,  1065024,    5504,   3096,     31950720,      39938400,    150,     10000000,           0,   2,   -1,    16,    6 },  // level 3.3 - undefined
            { STD_VIDEO_AV1_LEVEL_4_0,      2359296,    6144,   3456,     70778880,      77856768,    300,     12000000,    30000000,   4,    4,    32,    8 },  // level 4.0
            { STD_VIDEO_AV1_LEVEL_4_1,      2359296,    6144,   3456,    141557760,     155713536,    300,     20000000,    50000000,   4,    4,    32,    8 },  // level 4.1
            { STD_VIDEO_AV1_LEVEL_INVALID,  2359296,    6144,   3456,    141557760,     155713536,    300,     20000000,    50000000,   4,    4,    32,    8 },  // level 4.2 - undefined
            { STD_VIDEO_AV1_LEVEL_INVALID,  2359296,    6144,   3456,    141557760,     155713536,    300,     20000000,    50000000,   4,    4,    32,    8 },  // level 4.3 - undefined
            { STD_VIDEO_AV1_LEVEL_5_0,      8912896,    8192,   4352,    267386880,     273715200,    300,     30000000,   100000000,   6,    4,    64,    8 },  // level 5.0
            { STD_VIDEO_AV1_LEVEL_5_1,      8912896,    8192,   4352,    534773760,     547430400,    300,     40000000,   160000000,   8,    4,    64,    8 },  // level 5.1
            { STD_VIDEO_AV1_LEVEL_5_2,      8912896,    8192,   4352,   1069547520,    1094860800,    300,     60000000,   240000000,   8,    4,    64,    8 },  // level 5.2
            { STD_VIDEO_AV1_LEVEL_5_3,      8912896,    8192,   4352,   1069547520,    1176502272,    300,     60000000,   240000000,   8,    4,    64,    8 },  // level 5.3
            { STD_VIDEO_AV1_LEVEL_6_0,     35651584,   16384,   8704,   1069547520,    1176502272,    300,     60000000,   240000000,   8,    4,   128,   16 },  // level 6.0
            { STD_VIDEO_AV1_LEVEL_6_1,     35651584,   16384,   8704,   2139095040,    2189721600,    300,    100000000,   480000000,   8,    4,   128,   16 },  // level 6.1
            { STD_VIDEO_AV1_LEVEL_6_2,     35651584,   16384,   8704,   4278190080,    4379443200,    300,    160000000,   800000000,   8,    4,   128,   16 },  // level 6.2
            { STD_VIDEO_AV1_LEVEL_6_3,     35651584,   16384,   8704,   4278190080,    4706009088,    300,    160000000,   800000000,   8,    4,   128,   16 },  // level 6.3
            { STD_VIDEO_AV1_LEVEL_INVALID, 35651584,   16384,   8704,   4278190080,    4706009088,    300,    160000000,   800000000,   8,    4,   128,   16 },  // level 7.0 - undefined
            { STD_VIDEO_AV1_LEVEL_INVALID, 35651584,   16384,   8704,   4278190080,    4706009088,    300,    160000000,   800000000,   8,    4,   128,   16 },  // level 7.1 - undefined
            { STD_VIDEO_AV1_LEVEL_INVALID, 35651584,   16384,   8704,   4278190080,    4706009088,    300,    160000000,   800000000,   8,    4,   128,   16 },  // level 7.2 - undefined
            { STD_VIDEO_AV1_LEVEL_INVALID, 35651584,   16384,   8704,   4278190080,    4706009088,    300,    160000000,   800000000,   8,    4,   128,   16 },  // level 7.3 - undefined
        };

        levelLimits = levelLimitsTbl;
        levelLimitsSize = ARRAYSIZE(levelLimitsTbl);

        frameRateNumerator = FRAME_RATE_NUM_DEFAULT;
        frameRateDenominator = FRAME_RATE_DEN_DEFAULT;
    }
    virtual ~EncoderConfigAV1() {}

    virtual int DoParseArguments(int argc, const char* argv[]) override;

    virtual VkResult InitializeParameters() override
    {
        VkResult result = EncoderConfig::InitializeParameters();
        if (result != VK_SUCCESS) {
            return result;
        }

        hrdBitrate = maxBitrate;
        pic_width_in_sbs = DivUp<uint32_t>(encodeWidth, 64);
        pic_height_in_sbs = DivUp<uint32_t>(encodeHeight, 16);

        if ((pic_width_in_sbs > 0) && (pic_height_in_sbs > 0)) {
            return VK_SUCCESS;
        }

        assert(!"Invalid pic_width_in_sbs and pic_height_in_sbs");
        return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
    }

    virtual VkResult InitDeviceCapabilities(const VulkanDeviceContext* vkDevCtx) override;

    virtual uint32_t GetDefaultVideoProfileIdc() override { return STD_VIDEO_AV1_PROFILE_MAIN; }

    virtual int8_t InitDpbCount() override;

    virtual bool InitRateControl() override;

    virtual uint8_t GetMaxBFrameCount() override { return  static_cast<uint8_t>(av1EncodeCapabilities.maxBidirectionalCompoundReferenceCount); }

    bool GetRateControlParameters(VkVideoEncodeRateControlInfoKHR* rcInfo,
                                  VkVideoEncodeRateControlLayerInfoKHR* rcLayerInfo,
                                  VkVideoEncodeAV1RateControlInfoKHR* rcInfoAV1,
                                  VkVideoEncodeAV1RateControlLayerInfoKHR* rcLayerInfoAV1);

    bool InitSequenceHeader(StdVideoAV1SequenceHeader* seqHeader,
                            StdVideoEncodeAV1OperatingPointInfo* opInfo);

    virtual EncoderConfigAV1* GetEncoderConfigAV1() override {
        return this;
    }

    bool ValidateLevel(uint32_t lLevel, uint32_t lTier);

    bool DetermineLevelTier();

    uint32_t GetLevelMaxBitrate(uint32_t lLevel, uint32_t lTier) {
        if (lLevel < STD_VIDEO_AV1_LEVEL_4_0) {
            lTier = 0;
        }

        uint32_t _maxBitrate = lTier ? levelLimits[lLevel].highBps : levelLimits[lLevel].mainBps;
        uint32_t bitrateProfileFactor = (profile == STD_VIDEO_AV1_PROFILE_MAIN) ? 1 :
                                            ((profile == STD_VIDEO_AV1_PROFILE_HIGH) ? 1 : 3);

        return _maxBitrate * bitrateProfileFactor;
    }

    double GetLevelMinCR(uint32_t lLevel, uint32_t lTier, double decodeRate) {
        if (lLevel < STD_VIDEO_AV1_LEVEL_4_0) {
            lTier = 0;
        }

        double minCRBase = lTier ? levelLimits[lLevel].highCR : levelLimits[lLevel].mainCR;
        double speedAdj = decodeRate / levelLimits[lLevel].maxDisplayRate;

        return std::max(minCRBase * speedAdj, 0.8);
    }

    uint32_t GetLevelBitrate(uint32_t lLevel, uint32_t lTier) {
        if (lLevel < STD_VIDEO_AV1_LEVEL_4_0) {
            lTier = 0;
        }
        uint32_t bitrateProfileFactor = (profile == STD_VIDEO_AV1_PROFILE_MAIN) ? 1 :
                                            ((profile == STD_VIDEO_AV1_PROFILE_HIGH) ? 2 : 3);
        uint32_t _maxBitrate = (lTier == 0) ? levelLimits[lLevel].mainBps : levelLimits[lLevel].highBps;
        return _maxBitrate * bitrateProfileFactor;
    }

    double GetMinCompressRatio(uint32_t lLevel, uint32_t lTier, uint32_t decodeRate) {
        double speedAdj = (double)decodeRate / (double)levelLimits[lLevel].maxDisplayRate;
        double minCompBasis = (lTier == 0) ? levelLimits[lLevel].mainCR : levelLimits[lLevel].highCR;
        return std::max(0.8, minCompBasis * speedAdj);
    }

    uint32_t GetUncompressedSize() {
        uint32_t picSizeProfileFactor = (profile == STD_VIDEO_AV1_PROFILE_MAIN) ? 15 :
                                            ((profile == STD_VIDEO_AV1_PROFILE_HIGH) ? 30 : 36);
        return ((encodeWidth * encodeHeight * picSizeProfileFactor) >> 3);
    }

    StdVideoAV1Profile                      profile{ STD_VIDEO_AV1_PROFILE_MAIN };
    StdVideoAV1Level                        level{ STD_VIDEO_AV1_LEVEL_5_0 };
    uint8_t                                 tier{};
    VkVideoEncodeAV1CapabilitiesKHR         av1EncodeCapabilities{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR };
    VkVideoEncodeAV1QualityLevelPropertiesKHR av1QualityLevelProperties{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_QUALITY_LEVEL_PROPERTIES_KHR };
    VkVideoEncodeAV1QuantizationMapCapabilitiesKHR av1QuantizationMapCapabilities { VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_QUANTIZATION_MAP_CAPABILITIES_KHR };
    uint32_t                                vbvBufferSize{};
    uint32_t                                vbvInitialDelay{};
    uint32_t                                pic_width_in_sbs{};
    uint32_t                                pic_height_in_sbs{};
    VkVideoEncodeAV1QIndexKHR               minQIndex{};
    VkVideoEncodeAV1QIndexKHR               maxQIndex{255, 255, 255};
    VkVideoEncodeAV1RateControlInfoKHR      rcInfoAV1{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_INFO_KHR };
    VkVideoEncodeAV1RateControlLayerInfoKHR rcLayerInfoAV1{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_LAYER_INFO_KHR };
    VkVideoEncodeRateControlInfoKHR         rcInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR };
    VkVideoEncodeRateControlLayerInfoKHR    rcLayerInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR };
    const LevelLimits*                      levelLimits;
    size_t                                  levelLimitsSize;

    bool                                    enableTiles{};
    bool                                    customTileConfig{};
    StdVideoAV1TileInfo                     tileConfig{};
    uint16_t                                tileWidthInSbsMinus1[STD_VIDEO_AV1_MAX_TILE_COLS]{};
    uint16_t                                tileHeightInSbsMinus1[STD_VIDEO_AV1_MAX_TILE_ROWS]{};

    bool                                    enableQuant{};
    bool                                    customQuantConfig{};
    StdVideoAV1Quantization                 quantConfig{};

    bool                                    enableLf{};
    bool                                    customLfConfig{};
    StdVideoAV1LoopFilter                   lfConfig{};

    bool                                    enableCdef{};
    bool                                    customCdefConfig{};
    StdVideoAV1CDEF                         cdefConfig{};

    bool                                    enableLr{};
    bool                                    customLrConfig{};
    StdVideoAV1LoopRestoration              lrConfig{};
};

#endif /* VKVIDEOENCODER_VKENCODERCONFIG_AV1_H_ */
