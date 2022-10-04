#ifndef VULKAN_VIDEO_CODECS_ASSERT_OFFSETS_H_
#define VULKAN_VIDEO_CODECS_ASSERT_OFFSETS_H_ 1

/*
** Copyright 2022 The Khronos Group Inc.
**
** SPDX-License-Identifier: Apache-2.0
*/

#define VK_PRINT_OFFSETOF(structure, member) char (*dummy)[sizeof(char[offsetof(structure, member)])] = 1
#define VK_PRINT_SIZEOF(structure) char (*dummy)[sizeof(char[sizeof(structure)])] = 1

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member) // GCC and CLANG
#endif

#define VK_STRINGIFY_HELPER(X) #X
#define VK_STRINGIFY(X) STRINGIFY_HELPER(X)
#define VK_CT_ASSERT_STRUCT_OFFSET(structure, member, offset)  \
    static_assert((offsetof(structure, member) == offset), \
            "Member " VK_STRINGIFY_HELPER(member) " of structure " VK_STRINGIFY_HELPER(structure) \
            " is not at offset " VK_STRINGIFY_HELPER(offset) ".")

#define VK_CT_ASSERT_STRUCT_SIZE(structure, size)  \
    static_assert((sizeof(structure) == size), \
            "The size of structure " VK_STRINGIFY_HELPER(structure) \
            " is not  " VK_STRINGIFY_HELPER(size) ".")

#include "vk_video/vulkan_video_codecs_common.h"
#include "vk_video/vulkan_video_codec_h264std.h"
#include "vk_video/vulkan_video_codec_h264std_decode.h"
#include "vk_video/vulkan_video_codec_h264std_encode.h"
#include "vk_video/vulkan_video_codec_h265std.h"
#include "vk_video/vulkan_video_codec_h265std_decode.h"
#include "vk_video/vulkan_video_codec_h265std_encode.h"

// vulkan_video_codecs_common.h
// vulkan_video_codec_h264std.h

struct StdVideoH264HrdParameters;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264HrdParameters, cpb_size_scale, 2);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264HrdParameters, bit_rate_value_minus1, 4);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264HrdParameters, cpb_size_value_minus1, 132);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264HrdParameters, cbr_flag, 260);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264HrdParameters, initial_cpb_removal_delay_length_minus1, 292);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264HrdParameters, time_offset_length, 304);

struct StdVideoH264SequenceParameterSetVui;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSetVui, sar_width, 8);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSetVui, video_format, 12);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSetVui, num_units_in_tick, 16);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSetVui, time_scale, 20);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSetVui, chroma_sample_loc_type_bottom_field, 27);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSetVui, pHrdParameters, 32);

struct StdVideoH264ScalingLists;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264ScalingLists, ScalingList4x4, 4);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264ScalingLists, ScalingList8x8, 100);

struct StdVideoH264SequenceParameterSet;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSet, seq_parameter_set_id, 16);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSet, pic_order_cnt_type, 20);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSet, pic_width_in_mbs_minus1, 36);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSet, reserved2, 60);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264SequenceParameterSet, pOffsetForRefFrame, 64);

struct StdVideoH264PictureParameterSet;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264PictureParameterSet, weighted_bipred_idc, 8);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264PictureParameterSet, second_chroma_qp_index_offset, 15);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH264PictureParameterSet, pScalingLists, 16);

// vulkan_video_codec_h264std_decode.h
struct StdVideoDecodeH264PictureInfo;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoDecodeH264PictureInfo, PicOrderCnt, 12);

struct StdVideoDecodeH264ReferenceInfo;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoDecodeH264ReferenceInfo, PicOrderCnt, 8);

// vulkan_video_codec_h264std_encode.h
// SKIP until the spec is ready

// vulkan_video_codec_h265std.h
struct StdVideoH265DecPicBufMgr;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265DecPicBufMgr, max_num_reorder_pics, 35);

struct StdVideoH265SubLayerHrdParameters; // all 32-bit packed values
VK_CT_ASSERT_STRUCT_SIZE(StdVideoH265SubLayerHrdParameters, (sizeof(uint32_t) * STD_VIDEO_H265_CPB_CNT_LIST_SIZE * 4) + \
                                                            (sizeof(uint32_t)));

struct StdVideoH265HrdParameters;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265HrdParameters, elemental_duration_in_tc_minus1, 20);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265HrdParameters, pSubLayerHrdParametersNal, 40);

struct StdVideoH265ProfileTierLevel;
// Nothing to check with StdVideoH265ProfileTierLevel

struct StdVideoH265VideoParameterSet;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265VideoParameterSet, vps_num_ticks_poc_diff_one_minus1, 16);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265VideoParameterSet, pDecPicBufMgr, 24);

struct StdVideoH265ScalingLists;
// Nothing to check with StdVideoH265ScalingLists

struct StdVideoH265SequenceParameterSetVui;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265SequenceParameterSetVui, def_disp_win_left_offset, 20);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265SequenceParameterSetVui, max_bytes_per_pic_denom, 44);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265SequenceParameterSetVui, log2_max_mv_length_vertical, 47);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265SequenceParameterSetVui, pHrdParameters, 48);

struct StdVideoH265PredictorPaletteEntries;
// Nothing to check with StdVideoH265ScalingLists

struct StdVideoH265ShortTermRefPicSet;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265ShortTermRefPicSet, delta_idx_minus1, 4);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265ShortTermRefPicSet, reserved1, 18);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265ShortTermRefPicSet, delta_poc_s0_minus1, 24);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265ShortTermRefPicSet, delta_poc_s1_minus1, 56);

struct StdVideoH265LongTermRefPicsSps;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265LongTermRefPicsSps, lt_ref_pic_poc_lsb_sps, 4);

struct StdVideoH265SequenceParameterSet;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265SequenceParameterSet, conf_win_left_offset, 40);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265SequenceParameterSet, conf_win_bottom_offset, 52);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265SequenceParameterSet, pProfileTierLevel, 56);

struct StdVideoH265PictureParameterSet;


// VK_PRINT_OFFSETOF(StdVideoH265PictureParameterSet, reserved3);
// VK_PRINT_SIZEOF(StdVideoH265DecPicBufMgr);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265PictureParameterSet, column_width_minus1, 44);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoH265PictureParameterSet, pScalingLists, 128);
// vulkan_video_codec_h264std_decode.h

struct StdVideoDecodeH265PictureInfo;

VK_CT_ASSERT_STRUCT_OFFSET(StdVideoDecodeH265PictureInfo, PicOrderCntVal, 8);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoDecodeH265PictureInfo, RefPicSetStCurrBefore, 16);
VK_CT_ASSERT_STRUCT_OFFSET(StdVideoDecodeH265PictureInfo, RefPicSetLtCurr, 32);

struct StdVideoDecodeH265ReferenceInfo;

// vulkan_video_codec_h264std_encode.h
// SKIP until the spec is ready

#endif /* VULKAN_VIDEO_CODECS_ASSERT_OFFSETS_H_ */
