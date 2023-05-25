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

#ifndef _VULKANAV1DECODER_H_
#define _VULKANAV1DECODER_H_

#include "VulkanVideoDecoder.h"

#include <array>

#ifdef ENABLE_AV1_DECODER

#define ALIGN(value, n)         (((value) + (n) - 1) & ~((n) - 1))
#define CLAMP(value, low, high) ((value) < (low) ? (low) : ((value) > (high) ? (high) : (value)))

#define BIT32_MAX                  (0xffffffff)
#define SINT16_MAX                  (0x7fff)
#define SINT16_MIN                  (-0x7fff - 1)

#define MAX_NUM_TEMPORAL_LAYERS     8
#define MAX_NUM_SPATIAL_LAYERS      4
#define MAX_NUM_OPERATING_POINTS    MAX_NUM_TEMPORAL_LAYERS * MAX_NUM_SPATIAL_LAYERS

#define LEVEL_MAJOR_BITS            3
#define LEVEL_MINOR_BITS            2
#define LEVEL_BITS                  (LEVEL_MAJOR_BITS + LEVEL_MINOR_BITS)

#define LEVEL_MAJOR_MIN             2
#define LEVEL_MAJOR_MAX             ((1 << LEVEL_MAJOR_BITS) - 1 + LEVEL_MAJOR_MIN)
#define LEVEL_MINOR_MIN             0
#define LEVEL_MINOR_MAX             ((1 << LEVEL_MINOR_BITS) - 1)
#define OP_POINTS_CNT_MINUS_1_BITS  5
#define OP_POINTS_IDC_BITS          12

#define REFS_PER_FRAME              7               // number of reference frames that can be used for inter prediction
#define TOTAL_REFS_PER_FRAME        8               // number of reference frame types (including intra prediction)
#define NUM_REF_FRAMES              8               // number of frames that can be stored for future reference
#define REF_FRAMES_BITS             3
#define PRIMARY_REF_NONE            7               // number to indicate that there is no primary reference frame

#define GM_GLOBAL_MODELS_PER_FRAME  7
#define SUPERRES_NUM                8               // numerator for upscaling ratio
#define SUPERRES_DENOM_MIN          9               // smallest denominator for upscaling ratio
#define SUPERRES_DENOM_BITS         3               // number pf bits sent to specify denominator of upscaling ratio

// The minimum tile width or height is fixed at one superblock
#define MAX_TILE_WIDTH              (4096)          // maximum widht of a tile in units of luma samples
#define MAX_TILE_AREA               (4096 * 2304)   // maximum area of a tile in units of luma samples
#define MAX_TILE_ROWS               64u              // maximum number of tile rows
#define MAX_TILE_COLS               64u              // maximum number of tile columns
#define MAX_TILES                   512             // maximum number of tiles
#define MIN_TILE_SIZE_BYTES         1

#define MAX_SEGMENTS                8               // number of segments allowed in segmentation map
#define MAX_SEG_LVL                 8               // number of segment features

#define NONE_FRAME                  -1
#define INTRA_FRAME                 0
#define LAST_FRAME                  1
#define LAST2_FRAME                 2
#define LAST3_FRAME                 3
#define GOLDEN_FRAME                4
#define BWDREF_FRAME                5
#define ALTREF2_FRAME               6
#define ALTREF_FRAME                7

#define SELECT_SCREEN_CONTENT_TOOLS     2   // value that indicates the allow_screen_content_tools syntax element is coded
#define SELECT_INTEGER_MV               2   // value that indicates the force_integer_mv syntax element is coded

#define RESTORE_NONE                    0
#define RESTORE_WIENER                  1
#define RESTORE_SGRPROJ                 2
#define RESTORE_SWITCHABLE              3

typedef enum _AV1_SEGLEVEL_FEATURES
{
    AV1_SEG_LVL_ALT_Q,       // Use alternate Quantizer ....
    AV1_SEG_LVL_ALT_LF_Y_V,  // Use alternate loop filter value on y plane vertical
    AV1_SEG_LVL_ALT_LF_Y_H,  // Use alternate loop filter value on y plane horizontal
    AV1_SEG_LVL_ALT_LF_U,    // Use alternate loop filter value on u plane
    AV1_SEG_LVL_ALT_LF_V,    // Use alternate loop filter value on v plane
    AV1_SEG_LVL_REF_FRAME,   // Optional Segment reference frame
    AV1_SEG_LVL_SKIP,        // Optional Segment (0,0) + skip mode
    AV1_SEG_LVL_GLOBALMV,
    AV1_SEG_LVL_MAX
} AV1_SEGLEVEL_FEATURES;


// Profile-0.  8-bit and 10-bit 4:2:0 and 4:0:0 only.
// Profile-1.  8-bit and 10-bit 4:4:4
// Profile-2.  8-bit and 10-bit 4:2:2 and 4:0:0
//            12-bit  4:0:0, 4:2:0, 4:2:2 and 4:4:4
typedef enum _AV1_PROFILE 
{
    AV1_PROFILE_0,
    AV1_PROFILE_1,
    AV1_PROFILE_2,
    AV1_MAX_PROFILES,
} AV1_PROFILE;

typedef enum _AV1_level
{
    LEVEL_0 = 0,
    LEVEL_2_0 = LEVEL_0,
    LEVEL_1 = 1,
    LEVEL_2_1 = LEVEL_1,
    LEVEL_2 = 2,
    LEVEL_2_2 = LEVEL_2,
    LEVEL_3 = 3,
    LEVEL_2_3 = LEVEL_3,
    LEVEL_4 = 4,
    LEVEL_3_0 = LEVEL_4,
    LEVEL_5 = 5,
    LEVEL_3_1 = LEVEL_5,
    LEVEL_6 = 6,
    LEVEL_3_2 = LEVEL_6,
    LEVEL_7 = 7,
    LEVEL_3_3 = LEVEL_7,
    LEVEL_8 = 8,
    LEVEL_4_0 = LEVEL_8,
    LEVEL_9 = 9,
    LEVEL_4_1 = LEVEL_9,
    LEVEL_10 = 10,
    LEVEL_4_2 = LEVEL_10,
    LEVEL_11 = 11,
    LEVEL_4_3 = LEVEL_11,
    LEVEL_12 = 12,
    LEVEL_5_0 = LEVEL_12,
    LEVEL_13 = 13,
    LEVEL_5_1 = LEVEL_13,
    LEVEL_14 = 14,
    LEVEL_5_2 = LEVEL_14,
    LEVEL_15 = 15,
    LEVEL_5_3 = LEVEL_15,
    LEVEL_16 = 16,
    LEVEL_6_0 = LEVEL_16,
    LEVEL_17 = 17,
    LEVEL_6_1 = LEVEL_17,
    LEVEL_18 = 18,
    LEVEL_6_2 = LEVEL_18,
    LEVEL_19 = 19,
    LEVEL_6_3 = LEVEL_19,
    LEVEL_20 = 20,
    LEVEL_7_0 = LEVEL_20,
    LEVEL_21 = 21,
    LEVEL_7_1 = LEVEL_21,
    LEVEL_22 = 22,
    LEVEL_7_2 = LEVEL_22,
    LEVEL_23 = 23,
    LEVEL_7_3 = LEVEL_23,

    LEVEL_MAX = 31
} AV1_LEVEL;

// OBU types
typedef enum _AV1_OBU_TYPE 
{
    AV1_OBU_SEQUENCE_HEADER         = 1,
    AV1_OBU_TEMPORAL_DELIMITER      = 2,
    AV1_OBU_FRAME_HEADER            = 3,
    AV1_OBU_TILE_GROUP              = 4,
    AV1_OBU_METADATA                = 5,
    AV1_OBU_FRAME                   = 6,
    AV1_OBU_REDUNDANT_FRAME_HEADER  = 7,
    AV1_OBU_TILE_LIST               = 8,
    AV1_OBU_PADDING                 = 15,
} AV1_OBU_TYPE;

enum COLOR_PRIMARIES
{
    CP_BT_709 = 1,
    CP_UNSPECIFIED = 2,
    CP_BT_470_M = 4,
    CP_BT_470_B_G = 5,
    CP_BT_601 = 6,
    CP_SMPTE_240 = 7,
    CP_GENERIC_FILM = 8,
    CP_BT_2020 = 9,
    CP_XYZ = 10,
    CP_SMPTE_431 = 11,
    CP_SMPTE_432 = 12,
    CP_EBU_3213 = 22
};

enum CHROMA_SAMPLE_POSITION
{
    CSP_UNKNOWN = 0,
    CSP_VERTICAL,
    CSP_COLOCATED,
    CSP_RESERVED
};

enum TRANSFER_CHARACTERISTICS {
    TC_RESERVED_0 = 0,
    TC_BT_709,
    TC_UNSPECIFIED,
    TC_RESERVED_3,
    TC_BT_470_M,
    TC_BT_470_B_G,
    TC_BT_601,
    TC_SMPTE_240,
    TC_LINEAR,
    TC_LOG_100,
    TC_LOG_100_SQRT10,
    TC_IEC_61966,
    TC_BT_1361,
    TC_SRGB,
    TC_BT_2020_10_BIT,
    TC_BT_2020_12_BIT,
    TC_SMPTE_2084,
    TC_SMPTE_428,
    TC_HLG
};

enum MATRIX_COEFFICIENTS {
    MC_IDENTITY = 0,
    MC_BT_709,
    MC_UNSPECIFIED,
    MC_RESERVED_3,
    MC_FCC,
    MC_BT_470_B_G,
    MC_BT_601,
    MC_SMPTE_240,
    MC_SMPTE_YCGCO,
    MC_BT_2020_NCL,
    MC_BT_2020_CL,
    MC_SMPTE_2085,
    MC_CHROMAT_NCL,
    MC_CHROMAT_CL,
    MC_ICTCP
};

typedef enum _AV1_BLOCK_SIZE
{
    AV1_BLOCK_4X4,
    AV1_BLOCK_4X8,
    AV1_BLOCK_8X4,
    AV1_BLOCK_8X8,
    AV1_BLOCK_8X16,
    AV1_BLOCK_16X8,
    AV1_BLOCK_16X16,
    AV1_BLOCK_16X32,
    AV1_BLOCK_32X16,
    AV1_BLOCK_32X32,
    AV1_BLOCK_32X64,
    AV1_BLOCK_64X32,
    AV1_BLOCK_64X64,
    AV1_BLOCK_64X128,
    AV1_BLOCK_128X64,
    AV1_BLOCK_128X128,
    AV1_BLOCK_4X16,
    AV1_BLOCK_16X4,
    AV1_BLOCK_8X32,
    AV1_BLOCK_32X8,
    AV1_BLOCK_16X64,
    AV1_BLOCK_64X16,
} AV1_BLOCK_SIZE;

typedef enum _AV1_TX_MODE 
{
    AV1_ONLY_4X4          = 0,
    AV1_TX_MODE_LARGEST   = 1,
    AV1_TX_MODE_SELECT    = 2,
} AV1_TX_MODE;

typedef enum _AV1_PRED_MODE_TYPE 
{
  AV1_SINGLE_PREDICTION_ONLY = 0,
  AV1_REFERENCE_MODE_SELECT  = 1,
} AV1_PRED_MODE_TYPE;

typedef enum _AV1_INTERP_FILTER_TYPE
{
    AV1_EIGHTTAP_REGULAR,
    AV1_EIGHTTAP_SMOOTH,
    AV1_MULTITAP_SHARP,
    AV1_BILINEAR,
    AV1_INTERP_FILTERS_ALL,
    AV1_SWITCHABLE_FILTERS = AV1_BILINEAR,
    AV1_SWITCHABLE = AV1_SWITCHABLE_FILTERS + 1, /* the last switchable one */
    AV1_EXTRA_FILTERS = AV1_INTERP_FILTERS_ALL - AV1_SWITCHABLE_FILTERS,
} AV1_INTERP_FILTER_TYPE;

// global motion
typedef enum _AV1_TRANSFORMATION_TYPE
{
    IDENTITY          = 0,        // identity transformation, 0-parameter
    TRANSLATION       = 1,        // translational motion 2-parameter
    ROTZOOM           = 2,        // simplified affine with rotation + zoom only, 4-parameter
    AFFINE            = 3,        // affine, 6-parameter
    TRANS_TYPES,
} AV1_TRANSFORMATION_TYPE;

// The order of values in the wmmat matrix below is best described
// by the homography:
//      [x'     (m2 m3 m0   [x
//  z .  y'  =   m4 m5 m1 *  y
//       1]      m6 m7 1)    1]
struct AV1WarpedMotionParams 
{
  AV1_TRANSFORMATION_TYPE wmtype;
  int32_t wmmat[6];
  int8_t invalid;
};

#define WARPEDMODEL_PREC_BITS 16
static const AV1WarpedMotionParams default_warp_params = 
{
  IDENTITY,
  { 0, 0, (1 << WARPEDMODEL_PREC_BITS), 0, 0, (1 << WARPEDMODEL_PREC_BITS) },
  0,
};

typedef struct _AV1ObuHeader
{
    uint32_t        header_size;
    uint32_t        payload_size;
    AV1_OBU_TYPE    type;
    bool            has_size_field;
    bool            has_extension;
    uint8_t         reserved[2];
    // Below feilds exist if has_extension is set
    int32_t         temporal_id;
    int32_t         spatial_id;
} AV1ObuHeader;

// Sequence header structure.
struct av1_seq_param_s : public StdVideoPictureParametersSet, public StdVideoAV1SequenceHeader
{
    static const char* m_refClassId;
    AV1_PROFILE     profile;                        // should use StdVideoAV1SequenceHeader.seq_profile // features that can be used like bit-depth, monochrome and chroma subsampling
    uint8_t         frame_id_length{};  // length minus _2 ...
    uint8_t         delta_frame_id_length{};
    int32_t         force_screen_content_tools{}; // 0 - force off
                                                // 1 - force on
                                                // 2 - adaptive
    int32_t         force_integer_mv{};           // 0 - Not to force. MV can be in 1/4 or 1/8
                                                // 1 - force to integer
                                                // 2 - adaptive
    // Operating point info.
    int32_t         operating_points_cnt_minus_1{};
    int32_t         operating_point_idc[MAX_NUM_OPERATING_POINTS]{};  // specifies which spatial and temporal layers should be decoded
    bool            display_model_info_present{};
    bool            decoder_model_info_present{};
    AV1_LEVEL       level[MAX_NUM_OPERATING_POINTS]{};                // resolution, bitrate etc
    uint8_t         tier[MAX_NUM_OPERATING_POINTS]{};

    uint32_t        color_primaries{};
    uint32_t        transfer_characteristics{};
    uint32_t        matrix_coefficients{};
    uint32_t        chroma_sample_position{};

    VkSharedBaseObj<VkVideoRefCountBase> client;

    int32_t GetVpsId(bool& isVps) const override {
        isVps = false;
        return -1;
    }

    int32_t GetSpsId(bool& isSps) const override {
        isSps = false;
        return -1;
    }

    int32_t GetPpsId(bool& isPps) const override {
        isPps = false;
        return -1;
    }

    int32_t GetAv1SpsId(bool& isSps) const override {
        isSps = true;
        return 0; // @review: what is the equivalent of parameter_set_id for AV1?
    }

    const StdVideoAV1SequenceHeader*    GetStdAV1Sps() const override { return this; }

    const char* GetRefClassId() const override { return m_refClassId; }

    uint64_t SetSequenceCount(uint64_t updateSequenceCount) {
        assert(updateSequenceCount <= std::numeric_limits<uint32_t>::max());
        m_updateSequenceCount = (uint32_t)updateSequenceCount;
        return m_updateSequenceCount;
    }

    bool GetClientObject(VkSharedBaseObj<VkVideoRefCountBase>& clientObject) const override
    {
        clientObject = client;
        return !!clientObject;
    }

    explicit av1_seq_param_s(uint64_t updateSequenceCount)
    : StdVideoPictureParametersSet(TYPE_AV1_SPS, AV1_SPS_TYPE, m_refClassId, updateSequenceCount)
    , StdVideoAV1SequenceHeader()
    , profile(AV1_PROFILE_0)
    {
    }

    ~av1_seq_param_s() override {
        client = nullptr;
    }

    static VkResult Create(uint64_t updateSequenceCount,
                           VkSharedBaseObj<av1_seq_param_s>& spsAV1PictureParametersSet)
    {
        VkSharedBaseObj<av1_seq_param_s> av1PictureParametersSet(
            new av1_seq_param_s(updateSequenceCount));
        if (av1PictureParametersSet) {
            spsAV1PictureParametersSet = av1PictureParametersSet;
            return VK_SUCCESS;
        }
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
};

typedef struct _av1_timing_info_t
{
    uint32_t        num_units_in_display_tick;
    uint32_t        time_scale;
    bool            equal_picture_interval;
    uint32_t        num_ticks_per_picture;
} av1_timing_info_t;

typedef struct _av1_dec_model_info 
{
    uint32_t        num_units_in_decoding_tick;
    int32_t         encoder_decoder_buffer_delay_length;
    int32_t         buffer_removal_time_length;
    int32_t         frame_presentation_time_length;
} av1_dec_model_info_t;

typedef struct _av1_dec_model_op_params 
{
    bool            decoder_model_param_present;
    uint32_t        bitrate;
    uint32_t        buffer_size;
    int32_t         cbr_flag;
    int32_t         decoder_buffer_delay;
    int32_t         encoder_buffer_delay;
    int32_t         low_delay_mode_flag;
    int32_t         display_model_param_present;
    int32_t         initial_display_delay;
} av1_dec_model_op_params_t;


typedef struct _av1_film_grain_s {
    uint16_t        apply_grain              : 1;
    uint16_t        update_grain             : 1;
    uint16_t        scaling_shift_minus8     : 2;
    uint16_t        chroma_scaling_from_luma : 1;
    uint16_t        overlap_flag             : 1;
    uint16_t        ar_coeff_shift_minus6    : 2;
    uint16_t        ar_coeff_lag             : 2;
    uint16_t        grain_scale_shift        : 2;
    uint16_t        clip_to_restricted_range : 1;
    uint16_t        reserved                 : 3;

    uint16_t        grain_seed;

    uint8_t         num_y_points;
    uint8_t         scaling_points_y[14][2];
    uint8_t         num_cb_points;
    uint8_t         scaling_points_cb[10][2];
    uint8_t         num_cr_points;
    uint8_t         scaling_points_cr[10][2];

    int16_t         ar_coeffs_y[24];
    int16_t         ar_coeffs_cb[25];
    int16_t         ar_coeffs_cr[25];
    uint8_t         cb_mult;       // 8 bits
    uint8_t         cb_luma_mult;  // 8 bits
    int16_t         cb_offset;    // 9 bits
    uint8_t         cr_mult;       // 8 bits
    uint8_t         cr_luma_mult;  // 8 bits
    int16_t         cr_offset;    // 9 bits
} av1_film_grain_s;

typedef struct _GlobalMotionParams {
    uint32_t        wmtype;
    int32_t         wmmat[6];
    int8_t          invalid;
    int8_t          reserved[3];
} GlobalMotionParams;

typedef struct _av1_ref_frames_s
{
    VkPicIf*                buffer;
    StdVideoAV1FrameType    frame_type;
    av1_film_grain_s        film_grain_params;
    AV1WarpedMotionParams   global_models[GM_GLOBAL_MODELS_PER_FRAME];
    int8_t                  lf_ref_delta[NUM_REF_FRAMES];
    int8_t                  lf_mode_delta[2];
    bool                    showable_frame;
    struct
    {
        int16_t             feature_enable[8][8];
        int16_t             feature_data[8][8];
        int32_t             last_active_id;
        uint8_t             preskip_id;
        uint8_t             reserved[3];
    } seg;

    // Temporary variables.
    uint32_t                primary_ref_frame; // if not 0 -- may not alloc a slot. Re-resolve this per frame per dpb index.
    uint32_t                base_q_index;
    bool                    disable_frame_end_update_cdf;
    bool                    segmentation_enabled;

    int8_t                  RefFrameSignBias[8];
    uint8_t                 ref_order_hint[8];
    uint8_t                 order_hint;
    // 
    //int32_t                 ref_frame_map;
    //int32_t                 ref_frame_id;
    //int32_t                 RefValid;
    //int32_t                 ref_frame_idx;
    //int32_t                 active_ref_idx;
    //
    //int32_t                 RefOrderHint;
} av1_ref_frames_s;




// AV1 decoder class
class VulkanAV1Decoder : public VulkanVideoDecoder
{
protected:
	VkSharedBaseObj<av1_seq_param_s> m_sps; // active sps
//    av1_seq_param_s             m_sps;
    VkParserAv1PictureData      m_PicData;
    // common params
    int32_t                     temporal_id;
    int32_t                     spatial_id;
    bool                        m_bSPSReceived;
    bool                        m_bSPSChanged;
    bool                        m_bAnnexb;
    uint8_t                     timing_info_present;
    av1_timing_info_t           timing_info;
    av1_dec_model_info_t        buffer_model;
    av1_dec_model_op_params_t   op_params[MAX_NUM_OPERATING_POINTS + 1];
    uint32_t                    op_frame_timing[MAX_NUM_OPERATING_POINTS + 1];
    
    uint8_t                     last_frame_type;
    uint8_t                     last_intra_only;
    uint8_t                     all_lossless;

    // frame header
    uint16_t                    m_dwWidth;
    uint16_t                    m_dwHeight;
    int32_t                     render_width;
    int32_t                     render_height;

    uint32_t                    intra_only;
    int32_t                     showable_frame;
    int32_t                     last_show_frame;
    int32_t                     show_existing_frame;
    int32_t                     tu_presentation_delay;

    int32_t                     primary_ref_frame;
    int32_t                     current_frame_id;
    int32_t                     frame_offset;
    int32_t                     refresh_frame_flags;

    int32_t                     lossless[MAX_SEGMENTS];

    uint8_t                     tile_sz_mag;
    uint32_t                    log2_tile_cols;
    uint32_t                    log2_tile_rows;

    // global motion
    AV1WarpedMotionParams       global_motions[GM_GLOBAL_MODELS_PER_FRAME];
#if 1
    int32_t                     ref_frame_id[NUM_REF_FRAMES];
    int32_t                     RefValid[NUM_REF_FRAMES];
    int32_t                     ref_frame_idx[REFS_PER_FRAME];

    int32_t                     RefOrderHint[NUM_REF_FRAMES];
#endif
    av1_ref_frames_s            m_pBuffers[NUM_REF_FRAMES];

    VkPicIf*                    m_pCurrPic;
    
    bool                        m_bOutputAllLayers;
    int32_t                     m_OperatingPointIDCActive;
    int                         m_numOutFrames;
    VkPicIf*                    m_pOutFrame[MAX_NUM_SPATIAL_LAYERS];
    bool                        m_showableFrame[MAX_NUM_SPATIAL_LAYERS];

    std::array<int, 256>        m_pSliceOffsets;
    int                         m_numTiles;
public:
    VulkanAV1Decoder(VkVideoCodecOperationFlagBitsKHR std);
    virtual ~VulkanAV1Decoder();

    bool                    ParseByteStream(const VkParserBitstreamPacket* pck, size_t* pParsedBytes) override;

protected:
    bool                    IsPictureBoundary(int32_t) override             { return true; };
    int32_t                 ParseNalUnit() override                         { return NALU_UNKNOWN; };
    bool                    DecodePicture(VkParserPictureData *)            { return false; };
    bool                    end_of_picture(const uint8_t* pdataIn, uint32_t dataSize, uint32_t dataOffset, uint8_t* pbSideDataIn = NULL, uint32_t sideDataSize = 0);
    void                    InitParser() override;
    bool                    BeginPicture(VkParserPictureData *pnvpd) override;
    void                    lEndPicture(VkPicIf* pDispPic, bool bEvict);
    bool                    ParseOneFrame(const uint8_t* pdatain, int32_t datasize, const VkParserBitstreamPacket* pck, int* pParsedBytes);
    void                    EndOfStream() override;

    uint32_t read_u16_le(const void *vmem) {
        uint32_t val;
        const uint8_t *mem = (const uint8_t *)vmem;

        val = mem[1] << 8;
        val |= mem[0];
        return val;
    }

    uint32_t read_u24_le(const void *vmem) {
        uint32_t val;
        const uint8_t *mem = (const uint8_t *)vmem;

        val = mem[2] << 16;
        val |= mem[1] << 8;
        val |= mem[0];
        return val;
    }

    uint32_t read_u32_le(const void *vmem) {
        uint32_t val;
        const uint8_t *mem = (const uint8_t *)vmem;

        val = ((uint32_t)mem[3]) << 24;
        val |= mem[2] << 16;
        val |= mem[1] << 8;
        val |= mem[0];
        return val;
    }

    size_t read_tile_group_size(const uint8_t* src, int size)
    {
          switch (size) {
            case 1: return src[0];
            case 2: return read_u16_le(src);
            case 3: return read_u24_le(src);
            case 4: return read_u32_le(src);
            default: assert(0 && "Invalid size"); return (size_t)(-1);
        }
    }

    bool                    ParseOBUHeaderAndSize(const uint8_t* pData, uint32_t datasize, AV1ObuHeader* hdr);
    bool                    ReadObuSize(const uint8_t* pData, uint32_t datasize, uint32_t* obu_size, uint32_t* length_feild_size);
    bool                    ReadObuHeader(const uint8_t* pData, uint32_t datasize, AV1ObuHeader* hdr);

    bool                    ParseObuTemporalDelimiter();
    bool                    ParseObuSequenceHeader();
    bool                    ParseObuFrameHeader();
    bool                    ParseObuTileGroupHeader(int& tile_start, int& tile_end, bool &last_tile_group, bool tile_start_implicit);
    bool                    ReadFilmGrainParams();
        
    void                    ReadTimingInfoHeader();
    void                    ReadDecoderModelInfo();
    uint32_t                ReadUvlc();
    void                    SetupFrameSize(int32_t frame_size_override_flag);
    int32_t                 SetupFrameSizeWithRefs();

    bool                    DecodeTileInfo();
    void                    CalcTileOffsets(const uint8_t *base, const uint8_t *end, int offset, int tile_start, int tile_end, bool isFrameOBU);
    inline int32_t          ReadSignedBits(uint32_t bits);
    inline int32_t          ReadDeltaQ(uint32_t bits);
    uint32_t                SwGetUniform(uint32_t max_value);

    void                    DecodeQuantizationData();
    void                    DecodeSegmentationData();
    void                    DecodeLoopFilterdata();
    void                    DecodeCDEFdata();
    void                    DecodeLoopRestorationData();
    void                    SetFrameRefs(int32_t last_frame_idx, int32_t gold_frame_idx);
    int32_t                 GetRelativeDist1(int32_t a, int32_t b);
    int32_t                 IsSkipModeAllowed();
    uint32_t                DecodeGlobalMotionParams();
    int32_t                 ReadGlobalMotionParams(AV1WarpedMotionParams *params, const AV1WarpedMotionParams *ref_params, int32_t allow_hp);

    int16_t                 Read_signed_primitive_refsubexpfin(uint16_t n, uint16_t k, int16_t ref);
    uint16_t                Read_primitive_refsubexpfin(uint16_t n, uint16_t k, uint16_t ref);
    uint16_t                Read_primitive_subexpfin(uint16_t n, uint16_t k);
    uint16_t                Read_primitive_quniform(uint16_t n);
    void                    UpdateFramePointers(VkPicIf* currentPicture);
    bool                    IsFrameIntra() { return (m_PicData.frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY || m_PicData.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY); }
    int32_t                 ChooseOperatingPoint();
    bool                    AddBuffertoOutputQueue(VkPicIf* pDispPic, bool bShowableFrame);
    void                    AddBuffertoDispQueue(VkPicIf* pDispPic);

    void                    CreatePrivateContext() override {}
    void                    FreeContext() override {}
    int                     GetRelativeDist(int a, int b);
};
#endif // ENABLE_AV1_DECODER

#endif // _VULKANAV1DECODER_H_
