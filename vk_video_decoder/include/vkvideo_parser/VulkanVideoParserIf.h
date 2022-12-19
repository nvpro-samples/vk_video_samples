/*
 * Copyright 2021 NVIDIA Corporation.
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

#ifndef _VULKANVIDEOPARSER_IF_H_
#define _VULKANVIDEOPARSER_IF_H_

#include "PictureBufferBase.h"
#include "VkParserVideoRefCountBase.h"
#include "vulkan_interfaces.h"
#include "vk_video/vulkan_video_codecs_common.h"

#define NV_VULKAN_VIDEO_PARSER_API_VERSION_0_9_7 VK_MAKE_VIDEO_STD_VERSION(0, 9, 7)

#define NV_VULKAN_VIDEO_PARSER_API_VERSION   NV_VULKAN_VIDEO_PARSER_API_VERSION_0_9_7

typedef uint32_t FrameRate; // Packed 18-bit numerator & 14-bit denominator

// Definitions for video_format
enum {
    VideoFormatComponent = 0,
    VideoFormatPAL,
    VideoFormatNTSC,
    VideoFormatSECAM,
    VideoFormatMAC,
    VideoFormatUnspecified,
    VideoFormatReserved6,
    VideoFormatReserved7
};

// Definitions for color_primaries
enum {
    ColorPrimariesForbidden = 0,
    ColorPrimariesBT709 = 1,
    ColorPrimariesUnspecified = 2,
    ColorPrimariesReserved = 3,
    ColorPrimariesBT470M = 4,
    ColorPrimariesBT470BG = 5,
    ColorPrimariesSMPTE170M = 6, // Also, ITU-R BT.601
    ColorPrimariesSMPTE240M = 7,
    ColorPrimariesGenericFilm = 8,
    ColorPrimariesBT2020 = 9,
    // below are defined in AOM standard
    ColorPrimariesXYZ = 10, /**< SMPTE 428 (CIE 1921 XYZ) */
    ColorPrimariesSMPTE431 = 11, /**< SMPTE RP 431-2 */
    ColorPrimariesSMPTE432 = 12, /**< SMPTE EG 432-1  */
    ColorPrimariesRESERVED13 = 13, /**< For future use (values 13 - 21)  */
    ColorPrimariesEBU3213 = 22, /**< EBU Tech. 3213-E  */
    ColorPrimariesRESERVED23 = 23 /**< For future use (values 23 - 255)  */
};

// Definitions for transfer_characteristics
enum {
    TransferCharacteristicsForbidden = 0,
    TransferCharacteristicsBT709 = 1,
    TransferCharacteristicsUnspecified = 2,
    TransferCharacteristicsReserved = 3,
    TransferCharacteristicsBT470M = 4,
    TransferCharacteristicsBT470BG = 5,
    TransferCharacteristicsSMPTE170M = 6,
    TransferCharacteristicsSMPTE240M = 7,
    TransferCharacteristicsLinear = 8,
    TransferCharacteristicsLog100 = 9,
    TransferCharacteristicsLog316 = 10,
    TransferCharacteristicsIEC61966_2_4 = 11,
    TransferCharacteristicsBT1361 = 12,
    TransferCharacteristicsIEC61966_2_1 = 13,
    TransferCharacteristicsBT2020 = 14,
    TransferCharacteristicsBT2020_2 = 15,
    TransferCharacteristicsST2084 = 16,
    TransferCharacteristicsST428_1 = 17,
    // below are defined in AOM standard
    TransferCharacteristicsHLG = 18, /**< BT.2100 HLG, ARIB STD-B67 */
    TransferCharacteristicsRESERVED19 = 19 /**< For future use (values 19-255) */
};

// Definitions for matrix_coefficients
enum {
    MatrixCoefficientsForbidden = 0,
    MatrixCoefficientsBT709 = 1,
    MatrixCoefficientsUnspecified = 2,
    MatrixCoefficientsReserved = 3,
    MatrixCoefficientsFCC = 4,
    MatrixCoefficientsBT470BG = 5,
    MatrixCoefficientsSMPTE170M = 6,
    MatrixCoefficientsSMPTE240M = 7,
    MatrixCoefficientsYCgCo = 8,
    MatrixCoefficientsBT2020_NCL = 9, // Non-constant luminance
    MatrixCoefficientsBT2020_CL = 10, // Constant luminance
    // below are defined in AOM standard
    MatrixCoefficientsSMPTE2085 = 11, /**< SMPTE ST 2085 YDzDx */
    MatrixCoefficientsCHROMAT_NCL = 12, /**< Chromaticity-derived non-constant luminance */
    MatrixCoefficientsCHROMAT_CL = 13, /**< Chromaticity-derived constant luminance */
    MatrixCoefficientsICTCP = 14, /**< BT.2100 ICtCp */
    MatrixCoefficientsRESERVED15 = 15
};

// Maximum raw sequence header length (all codecs)
#define VK_MAX_SEQ_HDR_LEN (1024) // 1024 bytes

typedef struct VkParserH264DpbEntry {
    VkPicIf* pPicBuf; // ptr to reference frame
    int32_t FrameIdx; // frame_num(short-term) or LongTermFrameIdx(long-term)
    int32_t is_long_term; // 0=short term reference, 1=long term reference
    int32_t not_existing; // non-existing reference frame (corresponding PicIdx
        // should be set to -1)
    int32_t used_for_reference; // 0=unused, 1=top_field, 2=bottom_field,
        // 3=both_fields
    int32_t FieldOrderCnt[2]; // field order count of top and bottom fields
} VkParserH264DpbEntry;

typedef struct VkParserH264PictureData {
    // SPS
    const StdVideoH264SequenceParameterSet* pStdSps;
    VkParserVideoRefCountBase*              pSpsClientObject;
    // PPS
    const StdVideoH264PictureParameterSet*    pStdPps;
    VkParserVideoRefCountBase*              pPpsClientObject;
    uint8_t  pic_parameter_set_id;          // PPS ID
    uint8_t  seq_parameter_set_id;          // SPS ID
    int32_t num_ref_idx_l0_active_minus1;
    int32_t num_ref_idx_l1_active_minus1;
    int32_t weighted_pred_flag;
    int32_t weighted_bipred_idc;
    int32_t pic_init_qp_minus26;
    int32_t redundant_pic_cnt_present_flag;
    uint8_t deblocking_filter_control_present_flag;
    uint8_t transform_8x8_mode_flag;
    uint8_t MbaffFrameFlag;
    uint8_t constrained_intra_pred_flag;
    uint8_t entropy_coding_mode_flag;
    uint8_t pic_order_present_flag;
    int8_t chroma_qp_index_offset;
    int8_t second_chroma_qp_index_offset;
    int32_t frame_num;
    int32_t CurrFieldOrderCnt[2];
    uint8_t fmo_aso_enable;
    uint8_t num_slice_groups_minus1;
    uint8_t slice_group_map_type;
    int8_t pic_init_qs_minus26;
    uint32_t slice_group_change_rate_minus1;
    const uint8_t* pMb2SliceGroupMap;
    // DPB
    VkParserH264DpbEntry dpb[16 + 1]; // List of reference frames within the DPB

    // Quantization Matrices (raster-order)
    union {
        // MVC extension
        struct {
            int32_t num_views_minus1;
            int32_t view_id;
            uint8_t inter_view_flag;
            uint8_t num_inter_view_refs_l0;
            uint8_t num_inter_view_refs_l1;
            uint8_t MVCReserved8Bits;
            int32_t InterViewRefsL0[16];
            int32_t InterViewRefsL1[16];
        } mvcext;
        // SVC extension
        struct {
            uint8_t profile_idc;
            uint8_t level_idc;
            uint8_t DQId;
            uint8_t DQIdMax;
            uint8_t disable_inter_layer_deblocking_filter_idc;
            uint8_t ref_layer_chroma_phase_y_plus1;
            int8_t inter_layer_slice_alpha_c0_offset_div2;
            int8_t inter_layer_slice_beta_offset_div2;
            uint16_t DPBEntryValidFlag;

            union {
                struct {
                    uint8_t inter_layer_deblocking_filter_control_present_flag : 1;
                    uint8_t extended_spatial_scalability_idc : 2;
                    uint8_t adaptive_tcoeff_level_prediction_flag : 1;
                    uint8_t slice_header_restriction_flag : 1;
                    uint8_t chroma_phase_x_plus1_flag : 1;
                    uint8_t chroma_phase_y_plus1 : 2;
                    uint8_t tcoeff_level_prediction_flag : 1;
                    uint8_t constrained_intra_resampling_flag : 1;
                    uint8_t ref_layer_chroma_phase_x_plus1_flag : 1;
                    uint8_t store_ref_base_pic_flag : 1;
                    uint8_t Reserved : 4;
                } f;
                uint8_t ucBitFields[2];
            };

            union {
                int16_t seq_scaled_ref_layer_left_offset;
                int16_t scaled_ref_layer_left_offset;
            };
            union {
                int16_t seq_scaled_ref_layer_top_offset;
                int16_t scaled_ref_layer_top_offset;
            };
            union {
                int16_t seq_scaled_ref_layer_right_offset;
                int16_t scaled_ref_layer_right_offset;
            };
            union {
                int16_t seq_scaled_ref_layer_bottom_offset;
                int16_t scaled_ref_layer_bottom_offset;
            };
        } svcext;
    };
} VkParserH264PictureData;

typedef struct VkParserHevcPictureData {
    // VPS
    const StdVideoH265VideoParameterSet*    pStdVps;
    VkParserVideoRefCountBase*              pVpsClientObject;

    // SPS
    const StdVideoH265SequenceParameterSet* pStdSps;
    VkParserVideoRefCountBase*              pSpsClientObject;

    // PPS
    const StdVideoH265PictureParameterSet*  pStdPps;
    VkParserVideoRefCountBase*              pPpsClientObject;

    uint8_t pic_parameter_set_id;       // PPS ID
    uint8_t seq_parameter_set_id;       // SPS ID
    uint8_t vps_video_parameter_set_id; // VPS ID

    uint8_t IrapPicFlag;
    uint8_t IdrPicFlag;

    // RefPicSets
    int32_t NumBitsForShortTermRPSInSlice;
    int32_t NumDeltaPocsOfRefRpsIdx;
    int32_t NumPocTotalCurr;
    int32_t NumPocStCurrBefore;
    int32_t NumPocStCurrAfter;
    int32_t NumPocLtCurr;
    int32_t CurrPicOrderCntVal;
    VkPicIf* RefPics[16];
    int32_t PicOrderCntVal[16];
    uint8_t IsLongTerm[16]; // 1=long-term reference
    int8_t RefPicSetStCurrBefore[8];
    int8_t RefPicSetStCurrAfter[8];
    int8_t RefPicSetLtCurr[8];

    // various profile related
    // 0 = invalid, 1 = Main, 2 = Main10, 3 = still picture, 4 = Main 12,
    // 5 = MV-HEVC Main8
    uint8_t ProfileLevel;
    uint8_t ColorPrimaries; // ColorPrimariesBTXXXX enum
    uint8_t bit_depth_luma_minus8;
    uint8_t bit_depth_chroma_minus8;

    // MV-HEVC related fields
    uint8_t mv_hevc_enable;
    uint8_t nuh_layer_id;
    uint8_t default_ref_layers_active_flag;
    uint8_t NumDirectRefLayers;
    uint8_t max_one_active_ref_layer_flag;
    uint8_t poc_lsb_not_present_flag;
    uint8_t pad0[2];

    int32_t NumActiveRefLayerPics0;
    int32_t NumActiveRefLayerPics1;
    int8_t RefPicSetInterLayer0[8];
    int8_t RefPicSetInterLayer1[8];

} VkParserHevcPictureData;

typedef struct VkParserVp8PictureData {
    // ivf
    uint32_t frameSize;
    long timeStamp;

    // Frame tag
    int32_t frame_type;
    int32_t version;
    int32_t show_frame;
    uint32_t first_partition_size;

    VkPicIf* pLastRef;
    VkPicIf* pGoldenRef;
    VkPicIf* pAltRef;

    int32_t width;
    int32_t height;
    int32_t horizontal_scale;
    int32_t vertical_scale;

    // Frame header
    union Vp8FrameHeaderPartial {
        uint32_t headerData;
        struct {
            uint32_t color_space : 1;
            uint32_t clamping_type : 1;
            uint32_t segmentation_enabled : 1;
            uint32_t update_mb_segmentation_data : 1;
            uint32_t Segment_feature_mode : 1;
            uint32_t filter_type : 1;
            uint32_t loop_filter_level : 6;
            uint32_t sharpness_level : 3;
            uint32_t log2_nbr_of_dct_partitions : 2;
            uint32_t refresh_golden_frame : 1;
            uint32_t refresh_alternate_frame : 1;
            uint32_t copy_buffer_to_golden : 2;
            uint32_t copy_buffer_to_alternate : 2;
            uint32_t sign_bias_golden : 1;
            uint32_t sign_bias_alternate : 1;
            uint32_t refresh_entropy_probs : 1;
            uint32_t refresh_last : 1;
        } header;
    } frameHeader_partial;
} VkParserVp8PictureData;

typedef struct VkParserVp9PictureData {
    uint32_t width;
    uint32_t height;

    // Frame Indexes
    VkPicIf* pLastRef;
    VkPicIf* pGoldenRef;
    VkPicIf* pAltRef;

    uint32_t keyFrame;
    uint32_t version;
    uint32_t showFrame;
    uint32_t errorResilient;
    uint32_t bit_depth_minus8;
    uint32_t colorSpace;
    uint32_t subsamplingX;
    uint32_t subsamplingY;
    uint32_t activeRefIdx[3];
    uint32_t intraOnly;
    uint32_t resetFrameContext;
    uint32_t frameParallelDecoding;
    uint32_t refreshFrameFlags;
    uint8_t refFrameSignBias[4];
    uint32_t frameContextIdx;
    uint32_t allow_high_precision_mv;
    uint32_t mcomp_filter_type;
    uint32_t loopFilterLevel;
    uint32_t loopFilterSharpness;
    uint32_t log2_tile_columns;
    uint32_t log2_tile_rows;
    int32_t mbRefLfDelta[4];
    int32_t mbModeLfDelta[2];
    int32_t segmentMapTemporalUpdate;
    uint8_t segmentFeatureEnable[8][4];
    uint8_t mb_segment_tree_probs[7];
    uint8_t segment_pred_probs[3];
    int16_t segmentFeatureData[8][4];
    uint32_t scaledWidth;
    uint32_t scaledHeight;
    uint32_t scalingActive;
    uint32_t segmentEnabled;
    uint32_t prevIsKeyFrame;
    uint32_t PrevShowFrame;
    uint32_t modeRefLfEnabled;
    int32_t qpYAc;
    int32_t qpYDc;
    int32_t qpChDc;
    int32_t qpChAc;
    uint32_t segmentMapUpdate;
    uint32_t segmentFeatureMode;
    uint32_t refreshEntropyProbs;
    uint32_t frameTagSize;
    uint32_t offsetToDctParts;
} VkParserVp9PictureData;

typedef struct VkParserAv1FilmGrain {
    uint16_t apply_grain : 1;
    uint16_t update_grain : 1;
    uint16_t scaling_shift_minus8 : 2;
    uint16_t chroma_scaling_from_luma : 1;
    uint16_t overlap_flag : 1;
    uint16_t ar_coeff_shift_minus6 : 2;
    uint16_t ar_coeff_lag : 2;
    uint16_t grain_scale_shift : 2;
    uint16_t clip_to_restricted_range : 1;
    uint16_t reserved : 3;

    uint16_t grain_seed;

    uint8_t num_y_points;
    uint8_t scaling_points_y[14][2];
    uint8_t num_cb_points;
    uint8_t scaling_points_cb[10][2];
    uint8_t num_cr_points;
    uint8_t scaling_points_cr[10][2];

    int16_t ar_coeffs_y[24];
    int16_t ar_coeffs_cb[25];
    int16_t ar_coeffs_cr[25];
    uint8_t cb_mult; // 8 bits
    uint8_t cb_luma_mult; // 8 bits
    int16_t cb_offset; // 9 bits
    uint8_t cr_mult; // 8 bits
    uint8_t cr_luma_mult; // 8 bits
    int16_t cr_offset; // 9 bits
} VkParserAv1FilmGrain;

typedef struct VkParserAv1GlobalMotionParameters {
    uint32_t wmtype;
    int32_t wmmat[6];
    int8_t invalid;
    int8_t reserved[3];
} VkParserAv1GlobalMotionParameters;

typedef struct VkParserAv1PictureData {
    VkPicIf* pDecodePic;
    uint32_t width;
    uint32_t superres_width;
    uint32_t height;
    uint32_t frame_offset;

    // sequence header
    uint32_t profile : 3; // 0 = profile0, 1 = profile1, 2 = profile2
    uint32_t use_128x128_superblock : 1; // superblock 0:64x64, 1: 128x128
    uint32_t subsampling_x : 1; // 0:400,1:420,others:reserved for future
    uint32_t
        subsampling_y : 1; // (subsampling_x, _y) 1,1 = 420, 1,0 = 422, 0,0 = 444
    uint32_t mono_chrome : 1;
    uint32_t bit_depth_minus8 : 4;
    uint32_t enable_filter_intra : 1; // tool enable in seq level, 0 : disable 1:
        // frame header control
    uint32_t enable_intra_edge_filter : 1;
    uint32_t enable_interintra_compound : 1;
    uint32_t enable_masked_compound : 1;
    uint32_t enable_dual_filter : 1; // enable or disable vertical and horiz
        // filter selection
    uint32_t enable_order_hint : 1; // 0 - disable order hint, and related tools
    uint32_t order_hint_bits_minus1 : 3; // is used to compute OrderHintBits
    uint32_t enable_jnt_comp : 1; // 0 - disable joint compound modes
    uint32_t enable_superres : 1;
    uint32_t enable_cdef : 1;
    uint32_t enable_restoration : 1;
    uint32_t enable_fgs : 1;
    uint32_t reserved0 : 7;

    // frame header
    uint32_t frame_type : 2; // Key frame, Inter frame, intra only, s-frame
    uint32_t show_frame : 1; // Key frame, Inter frame, intra only, s-frame
    uint32_t error_resilient_mode : 1;
    uint32_t disable_cdf_update : 1; // disable CDF update during symbol decoding
    uint32_t allow_screen_content_tools : 1; // screen content tool enable
    uint32_t force_integer_mv : 1; // AMVR enable
    uint32_t coded_denom : 3; // The denominator minus9  of the superres scale
    uint32_t allow_intrabc : 1; // IBC enable
    uint32_t allow_high_precision_mv : 1; // 1/8 precision mv enable
    uint32_t interp_filter : 3; // interpolation filter : EIGHTTAP_REGULAR,....
    uint32_t switchable_motion_mode : 1; // 0 : simple motion mode, 1 : SIMPLE,
        // OBMC, LOCAL  WARP
    uint32_t use_ref_frame_mvs : 1; // 1: current frame can use the previous
        // frame mv information, MFMV
    uint32_t disable_frame_end_update_cdf : 1; // backward update flag
    uint32_t delta_q_present : 1; // quantizer index delta values are present in
        // the block level
    uint32_t delta_q_res : 2; // left shift will apply to decoded quantizer index
        // delta values
    uint32_t delta_lf_present : 1; // specified whether loop filter delta values
        // are present in the block level
    uint32_t delta_lf_res : 2; // specifies  the left shift will apply  to
        // decoded  loop  filter values
    uint32_t
        delta_lf_multi : 1; // seperate loop filter deltas for Hy,Vy,U,V edges
    uint32_t
        using_qmatrix : 1; // 1: quantizer matrix will be used to compute
        // quantizers, iqt will select iqmatrix when enable
    uint32_t
        coded_lossless : 1; // 1 means all segments use lossless coding.Framem is
        // fully lossless, CDEF/DBF will disable
    uint32_t use_superres : 1; // frame level frame for using_superres
    uint32_t reserved1 : 4;

    uint32_t num_tile_cols : 8; // horizontal tile numbers in frame, max is 64
    uint32_t num_tile_rows : 8; // vertical tile numbers in frame, max is 64
    uint32_t context_update_tile_id : 16; // which tile cdf will be seleted as
        // the backward update CDF,
        // MAXTILEROW=64, MAXTILECOL=64, 12bits
    uint16_t tile_row_start_sb[65]; // valid for 0 <= i <= tile_rows
    uint16_t tile_col_start_sb[65]; // valid for 0 <= i <= tile_cols
    uint32_t cdef_damping_minus_3 : 2; // controls the amount of damping in the
        // deringing filter
    uint32_t cdef_bits : 2; // the number of bits needed to specify which CDEF
        // filter to apply
    uint32_t tx_mode : 2; // 0:ONLY4x4,3:LARGEST,4:SELECT
    uint32_t reference_mode : 1; // single,compound,select
    uint32_t skip_mode : 1; // skip mode
    uint32_t SkipModeFrame0 : 4;
    uint32_t SkipModeFrame1 : 4;
    uint32_t allow_warped_motion : 1; // sequence level & frame level warp enable
    uint32_t reduced_tx_set : 1; // whether the frame is  restricted to oa reduced
        // subset of the full set of transform types
    uint32_t loop_filter_delta_enabled : 1;
    uint32_t reserved2 : 13; // reserved bits

    // Quantization
    uint8_t base_qindex; // the maximum qp is 255
    int8_t qp_y_dc_delta_q;
    int8_t qp_u_dc_delta_q;
    int8_t qp_v_dc_delta_q;
    int8_t qp_u_ac_delta_q;
    int8_t qp_v_ac_delta_q;
    int8_t qm_y;
    int8_t qm_u;
    int8_t qm_v;

    // cdef
    uint8_t cdef_y_pri_strength[8]; // 4bit for one
    uint8_t cdef_y_sec_strength[8]; // 2bit for one
    uint8_t cdef_uv_pri_strength[8]; // 4bit for one
    uint8_t cdef_uv_sec_strength[8]; // 2bit for one

    // segmentation
    uint8_t segmentation_enabled;
    uint8_t segmentation_update_map;
    uint8_t segmentation_update_data;
    uint8_t segmentation_temporal_update;
    int16_t segmentation_feature_enable[8][8];
    int16_t segmentation_feature_data[8][8];
    int32_t last_active_segid; // The highest numbered segment id that has some
        // enabled feature.
    uint8_t segid_preskip;
    uint8_t segment_quant_sign; // sign bit  for  segment alternative QP

    // loopfilter
    uint8_t loop_filter_level[2];
    uint8_t loop_filter_level_u;
    uint8_t loop_filter_level_v;
    uint8_t loop_filter_sharpness;
    int8_t loop_filter_ref_deltas[8];
    int8_t loop_filter_mode_deltas[2];

    // loop restoration
    uint8_t lr_type[3]; // 0: NONE, 1: WIENER, 2: SGR, 3: SWITCHABLE
    uint8_t lr_unit_size[3]; // 0: 32,   1: 64,     2: 128, 3: 256

    uint8_t temporal_layer_id : 4; // temporal layer id
    uint8_t spatial_layer_id : 4; // spatial layer id

    // film grain params
    VkParserAv1FilmGrain fgs;

    // order: Last frame,Last2 frame,Last3 frame,Golden frame,BWDREF frame,ALTREF2
    // frame,ALTREF frame
    uint8_t primary_ref_frame;
    uint8_t ref_frame[7];
    VkPicIf* ref_frame_map[8];

    VkParserAv1GlobalMotionParameters ref_global_motion[7];
} VkParserAv1PictureData;

typedef struct VkParserPictureData {
    int32_t PicWidthInMbs; // Coded Frame Size
    int32_t FrameHeightInMbs; // Coded Frame Height
    VkPicIf* pCurrPic; // Current picture (output)
    int32_t field_pic_flag; // 0=frame picture, 1=field picture
    int32_t bottom_field_flag; // 0=top field, 1=bottom field (ignored if
        // field_pic_flag=0)
    int32_t second_field; // Second field of a complementary field pair
    int32_t progressive_frame; // Frame is progressive
    int32_t top_field_first; // Frame pictures only
    int32_t repeat_first_field; // For 3:2 pulldown (number of additional fields,
        // 2=frame doubling, 4=frame tripling)
    int32_t ref_pic_flag; // Frame is a reference frame
    int32_t intra_pic_flag; // Frame is entirely intra coded (no temporal
        // dependencies)
    int32_t chroma_format; // Chroma Format (should match sequence info)
    int32_t picture_order_count; // picture order count (if known)
    uint8_t* pbSideData; // Encryption Info
    uint32_t nSideDataLen; // Encryption Info length

    // Bitstream data
    uint32_t nBitstreamDataLen; // Number of bytes in bitstream data buffer
    uint8_t* pBitstreamData; // Ptr to bitstream data for this picture (slice-layer)
    uint32_t nNumSlices; // Number of slices(tiles in case of AV1) in this picture
    const uint32_t* pSliceDataOffsets; // nNumSlices entries, contains offset of each slice
        // within the bitstream data buffer

    // Codec-specific data
    union {
        VkParserH264PictureData h264;
        VkParserHevcPictureData hevc;
        VkParserVp8PictureData vp8;
        VkParserVp9PictureData vp9;
        VkParserAv1PictureData av1;
    } CodecSpecific;
    // Dpb Id for the setup (current picture to be reference) slot
    int8_t current_dpb_id;
} VkParserPictureData;

// Packet input for parsing
typedef struct VkParserBitstreamPacket {
    const uint8_t* pByteStream; // Ptr to byte stream data
    int32_t nDataLength; // Data length for this packet
    int32_t bEOS; // true if this is an End-Of-Stream packet (flush everything)
    int32_t bPTSValid; // true if llPTS is valid (also used to detect frame
        // boundaries for VC1 SP/MP)
    int32_t bDiscontinuity; // true if DecMFT is signalling a discontinuity
    int32_t bPartialParsing; // 0: parse entire packet, 1: parse until next
        // decode/display event
    int64_t llPTS; // Presentation Time Stamp for this packet (clock rate
        // specified at initialization)
    bool bDisablePP; // optional flag for VC1
    bool bEOP; // true if the packet in pByteStream is exactly one frame
    uint8_t* pbSideData; // Auxiliary encryption information
    int32_t nSideDataLength; // Auxiliary encrypton information length
} VkParserBitstreamPacket;

typedef struct VkParserOperatingPointInfo {
    VkVideoCodecOperationFlagBitsKHR eCodec;
    union {
        struct {
            uint8_t operating_points_cnt;
            uint8_t reserved24_bits[3];
            uint16_t operating_points_idc[32];
        } av1;
        uint8_t CodecReserved[1024];
    };
} VkParserOperatingPointInfo;

// Sequence information
typedef struct VkParserSequenceInfo {
    VkVideoCodecOperationFlagBitsKHR eCodec; // Compression Standard
    bool isSVC; // h.264 SVC
    FrameRate frameRate;     // Frame Rate stored in the bitstream
    int32_t bProgSeq;        // Progressive Sequence
    int32_t nDisplayWidth;   // Displayed Horizontal Size
    int32_t nDisplayHeight;  // Displayed Vertical Size
    int32_t nCodedWidth;     // Coded Picture Width
    int32_t nCodedHeight;    // Coded Picture Height
    int32_t nMaxWidth;       // Max width within sequence
    int32_t nMaxHeight;      // Max height within sequence
    uint8_t nChromaFormat;         // Chroma Format (0=4:0:0, 1=4:2:0, 2=4:2:2, 3=4:4:4)
    uint8_t uBitDepthLumaMinus8;   // Luma bit depth (0=8bit)
    uint8_t uBitDepthChromaMinus8; // Chroma bit depth (0=8bit)
    uint8_t uVideoFullRange;       // 0=16-235, 1=0-255
    int32_t lBitrate;              // Video bitrate (bps)
    int32_t lDARWidth,
        lDARHeight; // Display Aspect Ratio = lDARWidth : lDARHeight
    int32_t lVideoFormat; // Video Format (VideoFormatXXX)
    int32_t lColorPrimaries; // Colour Primaries (ColorPrimariesXXX)
    int32_t lTransferCharacteristics; // Transfer Characteristics
    int32_t lMatrixCoefficients; // Matrix Coefficients
    int32_t cbSequenceHeader; // Number of bytes in SequenceHeaderData
    int32_t nMinNumDpbSlots;       // Minimum number of DPB slots for correct decoding
    int32_t nMinNumDecodeSurfaces; // Minimum number of decode surfaces for correct decoding
    uint8_t SequenceHeaderData[VK_MAX_SEQ_HDR_LEN]; // Raw sequence header data
        // (codec-specific)
    uint8_t* pbSideData; // Auxiliary encryption information
    uint32_t cbSideData; // Auxiliary encryption information length
    uint32_t codecProfile;
} VkParserSequenceInfo;

struct VkParserSliceInfo;

enum {
    VK_PARSER_CAPS_MVC = 0x01,
    VK_PARSER_CAPS_SVC = 0x02,
};

typedef struct VkParserDisplayMasteringInfo {
    // H.265 Annex D.2.27
    uint16_t display_primaries_x[3];
    uint16_t display_primaries_y[3];
    uint16_t white_point_x;
    uint16_t white_point_y;
    uint32_t max_display_mastering_luminance;
    uint32_t min_display_mastering_luminance;
} VkParserDisplayMasteringInfo;

enum VkParserPictureParametersUpdateType {
    VK_PICTURE_PARAMETERS_UPDATE_H264_SPS = 0,
    VK_PICTURE_PARAMETERS_UPDATE_H264_PPS,
    VK_PICTURE_PARAMETERS_UPDATE_H265_VPS,
    VK_PICTURE_PARAMETERS_UPDATE_H265_SPS,
    VK_PICTURE_PARAMETERS_UPDATE_H265_PPS,
};

typedef struct VkPictureParameters {
    VkParserPictureParametersUpdateType   updateType;
    union {
        const StdVideoH264SequenceParameterSet* pH264Sps;
        const StdVideoH264PictureParameterSet*  pH264Pps;
        const StdVideoH265VideoParameterSet*    pH265Vps;
        const StdVideoH265SequenceParameterSet* pH265Sps;
        const StdVideoH265PictureParameterSet*  pH265Pps;
    };
    uint32_t updateSequenceCount;
} VkPictureParameters;

// Interface to allow decoder to communicate with the client
class VkParserVideoDecodeClient {
public:
    virtual int32_t
    BeginSequence(const VkParserSequenceInfo* pnvsi)
        = 0; // Returns max number of reference frames
        // (always at least 2 for MPEG-2)
    virtual bool
    AllocPictureBuffer(VkPicIf** ppPicBuf)
        = 0; // Returns a new VkPicIf interface
    virtual bool DecodePicture(
        VkParserPictureData* pParserPictureData)
        = 0; // Called when a picture is
        // ready to be decoded
    virtual bool UpdatePictureParameters(
        VkPictureParameters* pPictureParameters,
        VkSharedBaseObj<VkParserVideoRefCountBase>& pictureParametersObject,
        uint64_t updateSequenceCount)
        = 0; // Called when a picture is
        // ready to be decoded
    virtual bool DisplayPicture(
        VkPicIf* pPicBuf,
        int64_t llPTS)
        = 0; // Called when a picture is ready to be displayed
    virtual void UnhandledNALU(
        const uint8_t* pbData,
        int32_t cbData)
        = 0; // Called for custom NAL parsing (not required)
    virtual uint32_t GetDecodeCaps() { return 0; } // NVD_CAPS_XXX
    virtual int32_t GetOperatingPoint(VkParserOperatingPointInfo* pOPInfo)
    {
        return 0;
    } // called from sequence header of av1 scalable video streams
protected:
    virtual ~VkParserVideoDecodeClient() { }
};

// Initialization parameters for decoder class
typedef struct VkParserInitDecodeParameters {
    uint32_t                   interfaceVersion;
    VkParserVideoDecodeClient* pClient; // should always be present if using parsing functionality
    uint64_t lReferenceClockRate; // ticks per second of PTS clock
        // (0=default=10000000=10Mhz)
    int32_t  lErrorThreshold; // threshold for deciding to bypass of picture (0=do
        // not decode, 100=always decode)
    VkParserSequenceInfo* pExternalSeqInfo; // optional external sequence header
        // data from system layer

    // If set, Picture Parameters are going to be provided via UpdatePictureParameters callback
    bool     bOutOfBandPictureParameters;
} VkParserInitDecodeParameters;

// High-level interface to video decoder (Note that parsing and decoding
// functionality are decoupled from each other)
class VulkanVideoDecodeParser : public virtual VkParserVideoRefCountBase {
public:
    virtual VkResult Initialize(VkParserInitDecodeParameters* pParserPictureData) = 0;
    virtual bool Deinitialize() = 0;
    virtual bool DecodePicture(VkParserPictureData* pParserPictureData) = 0;
    virtual bool ParseByteStream(const VkParserBitstreamPacket* pck,
        int32_t* pParsedBytes = NULL)
        = 0;
    virtual bool DecodeSliceInfo(VkParserSliceInfo* psli,
        const VkParserPictureData* pParserPictureData,
        int32_t iSlice)
        = 0;
    virtual bool GetDisplayMasteringInfo(VkParserDisplayMasteringInfo* pdisp) = 0;
};

/////////////////////////////////////////////////////////////////////////////////////////

#endif // _VULKANVIDEOPARSER_IF_H_
