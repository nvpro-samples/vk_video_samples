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

#include "TraceClient.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <iterator>

namespace parser_trace {

uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

namespace {

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (alignment > 1) ? ((value + alignment - 1) / alignment) * alignment : value;
}

// The bitstream buffer the parser fills, in host memory.
class HostBitstreamBuffer : public VulkanBitstreamBuffer {
   public:
    HostBitstreamBuffer(VkDeviceSize size, VkDeviceSize offsetAlignment, VkDeviceSize sizeAlignment)
        : m_data(AlignUp(size, sizeAlignment)), m_offsetAlignment(offsetAlignment), m_sizeAlignment(sizeAlignment) {}

    VkDeviceSize GetMaxSize() const override { return m_data.size(); }
    VkDeviceSize GetOffsetAlignment() const override { return m_offsetAlignment; }
    VkDeviceSize GetSizeAlignment() const override { return m_sizeAlignment; }

    VkDeviceSize Resize(VkDeviceSize newSize, VkDeviceSize copySize, VkDeviceSize copyOffset) override {
        if (m_data.size() >= newSize) {
            return m_data.size();
        }
        std::vector<uint8_t> grown(AlignUp(newSize, m_sizeAlignment));
        if ((copySize != 0) && InRange(copyOffset, copySize)) {
            memcpy(grown.data(), m_data.data() + copyOffset, copySize);
        }
        m_data.swap(grown);
        return m_data.size();
    }

    VkDeviceSize Clone(VkDeviceSize newSize, VkDeviceSize copySize, VkDeviceSize copyOffset,
                       VkSharedBaseObj<VulkanBitstreamBuffer>& clone) override {
        auto buffer = std::make_shared<HostBitstreamBuffer>(newSize, m_offsetAlignment, m_sizeAlignment);
        if ((copySize != 0) && InRange(copyOffset, copySize) && (copySize <= buffer->m_data.size())) {
            memcpy(buffer->m_data.data(), m_data.data() + copyOffset, copySize);
        }
        clone = buffer;
        return newSize;
    }

    int64_t MemsetData(uint32_t value, VkDeviceSize offset, VkDeviceSize size) override {
        if (!InRange(offset, size)) {
            return -1;
        }
        memset(m_data.data() + offset, static_cast<int>(value), size);
        return static_cast<int64_t>(size);
    }

    int64_t CopyDataToBuffer(uint8_t* dstBuffer, VkDeviceSize dstOffset, VkDeviceSize srcOffset, VkDeviceSize size) const override {
        if (!InRange(srcOffset, size)) {
            return -1;
        }
        memcpy(dstBuffer + dstOffset, m_data.data() + srcOffset, size);
        return static_cast<int64_t>(size);
    }

    int64_t CopyDataToBuffer(VkSharedBaseObj<VulkanBitstreamBuffer>& dstBuffer, VkDeviceSize dstOffset, VkDeviceSize srcOffset,
                             VkDeviceSize size) const override {
        if (!InRange(srcOffset, size)) {
            return -1;
        }
        return dstBuffer->CopyDataFromBuffer(m_data.data() + srcOffset, 0, dstOffset, size);
    }

    int64_t CopyDataFromBuffer(const uint8_t* sourceBuffer, VkDeviceSize srcOffset, VkDeviceSize dstOffset,
                               VkDeviceSize size) override {
        if (!InRange(dstOffset, size)) {
            return -1;
        }
        memcpy(m_data.data() + dstOffset, sourceBuffer + srcOffset, size);
        return static_cast<int64_t>(size);
    }

    int64_t CopyDataFromBuffer(const VkSharedBaseObj<VulkanBitstreamBuffer>& sourceBuffer, VkDeviceSize srcOffset,
                               VkDeviceSize dstOffset, VkDeviceSize size) override {
        VkDeviceSize maxSize = 0;
        const uint8_t* source = sourceBuffer->GetReadOnlyDataPtr(srcOffset, maxSize);
        if ((source == nullptr) || (maxSize < size) || !InRange(dstOffset, size)) {
            return -1;
        }
        memcpy(m_data.data() + dstOffset, source, size);
        return static_cast<int64_t>(size);
    }

    uint8_t* GetDataPtr(VkDeviceSize offset, VkDeviceSize& maxSize) override {
        if (offset >= m_data.size()) {
            maxSize = 0;
            return nullptr;
        }
        maxSize = m_data.size() - offset;
        return m_data.data() + offset;
    }

    const uint8_t* GetReadOnlyDataPtr(VkDeviceSize offset, VkDeviceSize& maxSize) const override {
        if (offset >= m_data.size()) {
            maxSize = 0;
            return nullptr;
        }
        maxSize = m_data.size() - offset;
        return m_data.data() + offset;
    }

    void FlushRange(VkDeviceSize, VkDeviceSize) const override {}
    void InvalidateRange(VkDeviceSize, VkDeviceSize) const override {}
    VkBuffer GetBuffer() const override { return VK_NULL_HANDLE; }
    VkDeviceMemory GetDeviceMemory() const override { return VK_NULL_HANDLE; }

    uint32_t AddStreamMarker(uint32_t streamOffset) override {
        m_streamMarkers.push_back(streamOffset);
        return static_cast<uint32_t>(m_streamMarkers.size() - 1);
    }

    uint32_t SetStreamMarker(uint32_t streamOffset, uint32_t index) override {
        if (index >= m_streamMarkers.size()) {
            return uint32_t(-1);
        }
        m_streamMarkers[index] = streamOffset;
        return index;
    }

    uint32_t GetStreamMarker(uint32_t index) const override {
        return (index < m_streamMarkers.size()) ? m_streamMarkers[index] : uint32_t(-1);
    }

    uint32_t GetStreamMarkersCount() const override { return static_cast<uint32_t>(m_streamMarkers.size()); }

    const uint32_t* GetStreamMarkersPtr(uint32_t startIndex, uint32_t& maxCount) const override {
        maxCount = static_cast<uint32_t>(m_streamMarkers.size()) - startIndex;
        return m_streamMarkers.data() + startIndex;
    }

    uint32_t ResetStreamMarkers() override {
        uint32_t oldSize = static_cast<uint32_t>(m_streamMarkers.size());
        m_streamMarkers.clear();
        return oldSize;
    }

   private:
    bool InRange(VkDeviceSize offset, VkDeviceSize size) const {
        return (offset <= m_data.size()) && (size <= m_data.size() - offset);
    }

    std::vector<uint8_t> m_data;
    std::vector<uint32_t> m_streamMarkers;
    VkDeviceSize m_offsetAlignment;
    VkDeviceSize m_sizeAlignment;
};

class TracePicture : public vkPicBuffBase {};

void AddReferenceInfo(Line& line, const StdVideoDecodeAV1ReferenceInfo& info) {
    line.Num("frame_type", info.frame_type)
        .Num("OrderHint", info.OrderHint)
        .Hex("RefFrameSignBias", info.RefFrameSignBias)
        .Array("SavedOrderHints", info.SavedOrderHints, STD_VIDEO_AV1_NUM_REF_FRAMES)
        .Flags("flags", {{"disable_frame_end_update_cdf", info.flags.disable_frame_end_update_cdf},
                         {"segmentation_enabled", info.flags.segmentation_enabled}});
}

void TraceAv1SequenceHeader(FILE* out, uint64_t index, const StdVideoAV1SequenceHeader& sps) {
    const StdVideoAV1SequenceHeaderFlags& f = sps.flags;
    Line("sps")
        .Num("index", static_cast<int64_t>(index))
        .Str("codec", "av1")
        .Num("seq_profile", sps.seq_profile)
        .Num("frame_width_bits_minus_1", sps.frame_width_bits_minus_1)
        .Num("frame_height_bits_minus_1", sps.frame_height_bits_minus_1)
        .Num("max_frame_width_minus_1", sps.max_frame_width_minus_1)
        .Num("max_frame_height_minus_1", sps.max_frame_height_minus_1)
        .Num("delta_frame_id_length_minus_2", sps.delta_frame_id_length_minus_2)
        .Num("additional_frame_id_length_minus_1", sps.additional_frame_id_length_minus_1)
        .Num("order_hint_bits_minus_1", sps.order_hint_bits_minus_1)
        .Num("seq_force_integer_mv", sps.seq_force_integer_mv)
        .Num("seq_force_screen_content_tools", sps.seq_force_screen_content_tools)
        .Flags("flags", {{"still_picture", f.still_picture},
                         {"reduced_still_picture_header", f.reduced_still_picture_header},
                         {"use_128x128_superblock", f.use_128x128_superblock},
                         {"enable_filter_intra", f.enable_filter_intra},
                         {"enable_intra_edge_filter", f.enable_intra_edge_filter},
                         {"enable_interintra_compound", f.enable_interintra_compound},
                         {"enable_masked_compound", f.enable_masked_compound},
                         {"enable_warped_motion", f.enable_warped_motion},
                         {"enable_dual_filter", f.enable_dual_filter},
                         {"enable_order_hint", f.enable_order_hint},
                         {"enable_jnt_comp", f.enable_jnt_comp},
                         {"enable_ref_frame_mvs", f.enable_ref_frame_mvs},
                         {"frame_id_numbers_present_flag", f.frame_id_numbers_present_flag},
                         {"enable_superres", f.enable_superres},
                         {"enable_cdef", f.enable_cdef},
                         {"enable_restoration", f.enable_restoration},
                         {"film_grain_params_present", f.film_grain_params_present},
                         {"timing_info_present_flag", f.timing_info_present_flag},
                         {"initial_display_delay_present_flag", f.initial_display_delay_present_flag}})
        .Emit(out);
    if (sps.pColorConfig != nullptr) {
        const StdVideoAV1ColorConfig& c = *sps.pColorConfig;
        Line("  color")
            .Num("BitDepth", c.BitDepth)
            .Num("subsampling_x", c.subsampling_x)
            .Num("subsampling_y", c.subsampling_y)
            .Num("color_primaries", c.color_primaries)
            .Num("transfer_characteristics", c.transfer_characteristics)
            .Num("matrix_coefficients", c.matrix_coefficients)
            .Num("chroma_sample_position", c.chroma_sample_position)
            .Flags("flags", {{"mono_chrome", c.flags.mono_chrome},
                             {"color_range", c.flags.color_range},
                             {"separate_uv_delta_q", c.flags.separate_uv_delta_q},
                             {"color_description_present_flag", c.flags.color_description_present_flag}})
            .Emit(out);
    }
    if ((sps.pTimingInfo != nullptr) && f.timing_info_present_flag) {
        const StdVideoAV1TimingInfo& t = *sps.pTimingInfo;
        Line("  timing")
            .Num("num_units_in_display_tick", t.num_units_in_display_tick)
            .Num("time_scale", t.time_scale)
            .Num("num_ticks_per_picture_minus_1", t.num_ticks_per_picture_minus_1)
            .Flags("flags", {{"equal_picture_interval", t.flags.equal_picture_interval}})
            .Emit(out);
    }
}

void TraceAv1Picture(FILE* out, const VkParserPictureData& pd) {
    const VkParserAv1PictureData& av1 = pd.CodecSpecific.av1;
    const StdVideoDecodeAV1PictureInfo& info = av1.std_info;
    const StdVideoDecodeAV1PictureInfoFlags& f = info.flags;
    const uint32_t tileCount = std::min<uint32_t>(av1.khr_info.tileCount, std::size(av1.tileOffsets));

    Line("  av1")
        .Num("showFrame", av1.showFrame)
        .Num("needsSessionReset", av1.needsSessionReset)
        .Num("upscaled_width", av1.upscaled_width)
        .Num("frame_width", av1.frame_width)
        .Num("frame_height", av1.frame_height)
        .Array("ref_frame_idx", av1.ref_frame_idx, STD_VIDEO_AV1_REFS_PER_FRAME)
        .Array("pic_idx", av1.pic_idx, STD_VIDEO_AV1_NUM_REF_FRAMES)
        .Num("frameHeaderOffset", av1.khr_info.frameHeaderOffset)
        .Num("tileCount", av1.khr_info.tileCount)
        .Array("tileOffsets", av1.tileOffsets, tileCount)
        .Array("tileSizes", av1.tileSizes, tileCount)
        .Emit(out);

    Line("  std")
        .Num("frame_type", info.frame_type)
        .Num("current_frame_id", info.current_frame_id)
        .Num("OrderHint", info.OrderHint)
        .Num("primary_ref_frame", info.primary_ref_frame)
        .Hex("refresh_frame_flags", info.refresh_frame_flags)
        .Num("interpolation_filter", info.interpolation_filter)
        .Num("TxMode", info.TxMode)
        .Num("delta_q_res", info.delta_q_res)
        .Num("delta_lf_res", info.delta_lf_res)
        .Array("SkipModeFrame", info.SkipModeFrame, STD_VIDEO_AV1_SKIP_MODE_FRAMES)
        .Num("coded_denom", info.coded_denom)
        .Array("OrderHints", info.OrderHints, STD_VIDEO_AV1_NUM_REF_FRAMES)
        .Array("expectedFrameId", info.expectedFrameId, STD_VIDEO_AV1_NUM_REF_FRAMES)
        .Flags("flags", {{"error_resilient_mode", f.error_resilient_mode},
                         {"disable_cdf_update", f.disable_cdf_update},
                         {"use_superres", f.use_superres},
                         {"render_and_frame_size_different", f.render_and_frame_size_different},
                         {"allow_screen_content_tools", f.allow_screen_content_tools},
                         {"is_filter_switchable", f.is_filter_switchable},
                         {"force_integer_mv", f.force_integer_mv},
                         {"frame_size_override_flag", f.frame_size_override_flag},
                         {"buffer_removal_time_present_flag", f.buffer_removal_time_present_flag},
                         {"allow_intrabc", f.allow_intrabc},
                         {"frame_refs_short_signaling", f.frame_refs_short_signaling},
                         {"allow_high_precision_mv", f.allow_high_precision_mv},
                         {"is_motion_mode_switchable", f.is_motion_mode_switchable},
                         {"use_ref_frame_mvs", f.use_ref_frame_mvs},
                         {"disable_frame_end_update_cdf", f.disable_frame_end_update_cdf},
                         {"allow_warped_motion", f.allow_warped_motion},
                         {"reduced_tx_set", f.reduced_tx_set},
                         {"reference_select", f.reference_select},
                         {"skip_mode_present", f.skip_mode_present},
                         {"delta_q_present", f.delta_q_present},
                         {"delta_lf_present", f.delta_lf_present},
                         {"delta_lf_multi", f.delta_lf_multi},
                         {"segmentation_enabled", f.segmentation_enabled},
                         {"segmentation_update_map", f.segmentation_update_map},
                         {"segmentation_temporal_update", f.segmentation_temporal_update},
                         {"segmentation_update_data", f.segmentation_update_data},
                         {"UsesLr", f.UsesLr},
                         {"usesChromaLr", f.usesChromaLr},
                         {"apply_grain", f.apply_grain}})
        .Emit(out);

    const StdVideoAV1TileInfo& tiles = av1.tileInfo;
    const uint32_t tileCols = std::min<uint32_t>(tiles.TileCols, std::size(av1.width_in_sbs_minus_1));
    const uint32_t tileRows = std::min<uint32_t>(tiles.TileRows, std::size(av1.height_in_sbs_minus_1));
    Line("  tiles")
        .Num("TileCols", tiles.TileCols)
        .Num("TileRows", tiles.TileRows)
        .Num("context_update_tile_id", tiles.context_update_tile_id)
        .Num("tile_size_bytes_minus_1", tiles.tile_size_bytes_minus_1)
        .Array("MiColStarts", av1.MiColStarts, tileCols + 1)
        .Array("MiRowStarts", av1.MiRowStarts, tileRows + 1)
        .Array("width_in_sbs_minus_1", av1.width_in_sbs_minus_1, tileCols)
        .Array("height_in_sbs_minus_1", av1.height_in_sbs_minus_1, tileRows)
        .Flags("flags", {{"uniform_tile_spacing_flag", tiles.flags.uniform_tile_spacing_flag}})
        .Emit(out);

    const StdVideoAV1Quantization& q = av1.quantization;
    Line("  quant")
        .Num("base_q_idx", q.base_q_idx)
        .Num("DeltaQYDc", q.DeltaQYDc)
        .Num("DeltaQUDc", q.DeltaQUDc)
        .Num("DeltaQUAc", q.DeltaQUAc)
        .Num("DeltaQVDc", q.DeltaQVDc)
        .Num("DeltaQVAc", q.DeltaQVAc)
        .Num("qm_y", q.qm_y)
        .Num("qm_u", q.qm_u)
        .Num("qm_v", q.qm_v)
        .Flags("flags", {{"using_qmatrix", q.flags.using_qmatrix}, {"diff_uv_delta", q.flags.diff_uv_delta}})
        .Emit(out);

    const StdVideoAV1Segmentation& seg = av1.segmentation;
    Line("  seg")
        .Array("FeatureEnabled", seg.FeatureEnabled, STD_VIDEO_AV1_MAX_SEGMENTS)
        .Array("FeatureData", &seg.FeatureData[0][0], STD_VIDEO_AV1_MAX_SEGMENTS * STD_VIDEO_AV1_SEG_LVL_MAX)
        .Emit(out);

    const StdVideoAV1LoopFilter& lf = av1.loopFilter;
    Line("  lf")
        .Array("loop_filter_level", lf.loop_filter_level, STD_VIDEO_AV1_MAX_LOOP_FILTER_STRENGTHS)
        .Num("loop_filter_sharpness", lf.loop_filter_sharpness)
        .Num("update_ref_delta", lf.update_ref_delta)
        .Array("loop_filter_ref_deltas", lf.loop_filter_ref_deltas, STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME)
        .Num("update_mode_delta", lf.update_mode_delta)
        .Array("loop_filter_mode_deltas", lf.loop_filter_mode_deltas, STD_VIDEO_AV1_LOOP_FILTER_ADJUSTMENTS)
        .Flags("flags", {{"loop_filter_delta_enabled", lf.flags.loop_filter_delta_enabled},
                         {"loop_filter_delta_update", lf.flags.loop_filter_delta_update}})
        .Emit(out);

    const StdVideoAV1CDEF& cdef = av1.CDEF;
    Line("  cdef")
        .Num("cdef_damping_minus_3", cdef.cdef_damping_minus_3)
        .Num("cdef_bits", cdef.cdef_bits)
        .Array("cdef_y_pri_strength", cdef.cdef_y_pri_strength, STD_VIDEO_AV1_MAX_CDEF_FILTER_STRENGTHS)
        .Array("cdef_y_sec_strength", cdef.cdef_y_sec_strength, STD_VIDEO_AV1_MAX_CDEF_FILTER_STRENGTHS)
        .Array("cdef_uv_pri_strength", cdef.cdef_uv_pri_strength, STD_VIDEO_AV1_MAX_CDEF_FILTER_STRENGTHS)
        .Array("cdef_uv_sec_strength", cdef.cdef_uv_sec_strength, STD_VIDEO_AV1_MAX_CDEF_FILTER_STRENGTHS)
        .Emit(out);

    const StdVideoAV1LoopRestoration& lr = av1.loopRestoration;
    Line("  lr")
        .Array("FrameRestorationType", lr.FrameRestorationType, STD_VIDEO_AV1_MAX_NUM_PLANES)
        .Array("LoopRestorationSize", lr.LoopRestorationSize, STD_VIDEO_AV1_MAX_NUM_PLANES)
        .Emit(out);

    const StdVideoAV1GlobalMotion& gm = av1.globalMotion;
    Line("  gm")
        .Array("GmType", gm.GmType, STD_VIDEO_AV1_NUM_REF_FRAMES)
        .Array("gm_params", &gm.gm_params[0][0], STD_VIDEO_AV1_NUM_REF_FRAMES * STD_VIDEO_AV1_GLOBAL_MOTION_PARAMS)
        .Emit(out);

    const StdVideoAV1FilmGrain& fg = av1.filmGrain;
    Line("  fg")
        .Num("grain_scaling_minus_8", fg.grain_scaling_minus_8)
        .Num("ar_coeff_lag", fg.ar_coeff_lag)
        .Num("ar_coeff_shift_minus_6", fg.ar_coeff_shift_minus_6)
        .Num("grain_scale_shift", fg.grain_scale_shift)
        .Num("grain_seed", fg.grain_seed)
        .Num("film_grain_params_ref_idx", fg.film_grain_params_ref_idx)
        .Num("num_y_points", fg.num_y_points)
        .Array("point_y_value", fg.point_y_value, STD_VIDEO_AV1_MAX_NUM_Y_POINTS)
        .Array("point_y_scaling", fg.point_y_scaling, STD_VIDEO_AV1_MAX_NUM_Y_POINTS)
        .Num("num_cb_points", fg.num_cb_points)
        .Array("point_cb_value", fg.point_cb_value, STD_VIDEO_AV1_MAX_NUM_CB_POINTS)
        .Array("point_cb_scaling", fg.point_cb_scaling, STD_VIDEO_AV1_MAX_NUM_CB_POINTS)
        .Num("num_cr_points", fg.num_cr_points)
        .Array("point_cr_value", fg.point_cr_value, STD_VIDEO_AV1_MAX_NUM_CR_POINTS)
        .Array("point_cr_scaling", fg.point_cr_scaling, STD_VIDEO_AV1_MAX_NUM_CR_POINTS)
        .Array("ar_coeffs_y_plus_128", fg.ar_coeffs_y_plus_128, STD_VIDEO_AV1_MAX_NUM_POS_LUMA)
        .Array("ar_coeffs_cb_plus_128", fg.ar_coeffs_cb_plus_128, STD_VIDEO_AV1_MAX_NUM_POS_CHROMA)
        .Array("ar_coeffs_cr_plus_128", fg.ar_coeffs_cr_plus_128, STD_VIDEO_AV1_MAX_NUM_POS_CHROMA)
        .Num("cb_mult", fg.cb_mult)
        .Num("cb_luma_mult", fg.cb_luma_mult)
        .Num("cb_offset", fg.cb_offset)
        .Num("cr_mult", fg.cr_mult)
        .Num("cr_luma_mult", fg.cr_luma_mult)
        .Num("cr_offset", fg.cr_offset)
        .Flags("flags", {{"chroma_scaling_from_luma", fg.flags.chroma_scaling_from_luma},
                         {"overlap_flag", fg.flags.overlap_flag},
                         {"clip_to_restricted_range", fg.flags.clip_to_restricted_range},
                         {"update_grain", fg.flags.update_grain}})
        .Emit(out);

    Line setup("  setup");
    AddReferenceInfo(setup, av1.setupSlotInfo);
    setup.Emit(out);

    for (uint32_t i = 0; i < STD_VIDEO_AV1_NUM_REF_FRAMES; i++) {
        Line slot("  slot");
        slot.Num("i", i).Num("pic", av1.pic_idx[i]);
        AddReferenceInfo(slot, av1.dpbSlotInfos[i]);
        slot.Emit(out);
    }
}

template <typename T>
uint32_t StructCrc32(const T& value) {
    return Crc32(reinterpret_cast<const uint8_t*>(&value), sizeof(value));
}

int64_t ParameterSetUpdate(const StdVideoPictureParametersSet* params) {
    return (params != nullptr) ? params->GetUpdateSequenceCount() : -1;
}

void TraceH264Hrd(FILE* out, const StdVideoH264HrdParameters& hrd) {
    const size_t count = std::min<size_t>(hrd.cpb_cnt_minus1 + 1u, STD_VIDEO_H264_CPB_CNT_LIST_SIZE);
    Line("  hrd")
        .Num("cpb_cnt_minus1", hrd.cpb_cnt_minus1)
        .Num("bit_rate_scale", hrd.bit_rate_scale)
        .Num("cpb_size_scale", hrd.cpb_size_scale)
        .Array("bit_rate_value_minus1", hrd.bit_rate_value_minus1, count)
        .Array("cpb_size_value_minus1", hrd.cpb_size_value_minus1, count)
        .Array("cbr_flag", hrd.cbr_flag, count)
        .Num("initial_cpb_removal_delay_length_minus1", hrd.initial_cpb_removal_delay_length_minus1)
        .Num("cpb_removal_delay_length_minus1", hrd.cpb_removal_delay_length_minus1)
        .Num("dpb_output_delay_length_minus1", hrd.dpb_output_delay_length_minus1)
        .Num("time_offset_length", hrd.time_offset_length)
        .Emit(out);
}

void TraceH264Sps(FILE* out, uint64_t index, int64_t update, const StdVideoH264SequenceParameterSet& sps) {
    const StdVideoH264SpsFlags& f = sps.flags;
    Line line("sps");
    line.Num("index", static_cast<int64_t>(index))
        .Str("codec", "h264")
        .Num("update", update)
        .Num("seq_parameter_set_id", sps.seq_parameter_set_id)
        .Num("profile_idc", sps.profile_idc)
        .Num("level_idc", sps.level_idc)
        .Num("chroma_format_idc", sps.chroma_format_idc)
        .Num("bit_depth_luma_minus8", sps.bit_depth_luma_minus8)
        .Num("bit_depth_chroma_minus8", sps.bit_depth_chroma_minus8)
        .Num("log2_max_frame_num_minus4", sps.log2_max_frame_num_minus4)
        .Num("pic_order_cnt_type", sps.pic_order_cnt_type)
        .Num("offset_for_non_ref_pic", sps.offset_for_non_ref_pic)
        .Num("offset_for_top_to_bottom_field", sps.offset_for_top_to_bottom_field)
        .Num("log2_max_pic_order_cnt_lsb_minus4", sps.log2_max_pic_order_cnt_lsb_minus4)
        .Num("num_ref_frames_in_pic_order_cnt_cycle", sps.num_ref_frames_in_pic_order_cnt_cycle)
        .Num("max_num_ref_frames", sps.max_num_ref_frames)
        .Num("pic_width_in_mbs_minus1", sps.pic_width_in_mbs_minus1)
        .Num("pic_height_in_map_units_minus1", sps.pic_height_in_map_units_minus1)
        .Num("frame_crop_left_offset", sps.frame_crop_left_offset)
        .Num("frame_crop_right_offset", sps.frame_crop_right_offset)
        .Num("frame_crop_top_offset", sps.frame_crop_top_offset)
        .Num("frame_crop_bottom_offset", sps.frame_crop_bottom_offset);
    if (sps.pOffsetForRefFrame != nullptr) {
        line.Array("offset_for_ref_frame", sps.pOffsetForRefFrame, sps.num_ref_frames_in_pic_order_cnt_cycle);
    }
    line.Flags("flags", {{"constraint_set0_flag", f.constraint_set0_flag},
                         {"constraint_set1_flag", f.constraint_set1_flag},
                         {"constraint_set2_flag", f.constraint_set2_flag},
                         {"constraint_set3_flag", f.constraint_set3_flag},
                         {"constraint_set4_flag", f.constraint_set4_flag},
                         {"constraint_set5_flag", f.constraint_set5_flag},
                         {"direct_8x8_inference_flag", f.direct_8x8_inference_flag},
                         {"mb_adaptive_frame_field_flag", f.mb_adaptive_frame_field_flag},
                         {"frame_mbs_only_flag", f.frame_mbs_only_flag},
                         {"delta_pic_order_always_zero_flag", f.delta_pic_order_always_zero_flag},
                         {"separate_colour_plane_flag", f.separate_colour_plane_flag},
                         {"gaps_in_frame_num_value_allowed_flag", f.gaps_in_frame_num_value_allowed_flag},
                         {"qpprime_y_zero_transform_bypass_flag", f.qpprime_y_zero_transform_bypass_flag},
                         {"frame_cropping_flag", f.frame_cropping_flag},
                         {"seq_scaling_matrix_present_flag", f.seq_scaling_matrix_present_flag},
                         {"vui_parameters_present_flag", f.vui_parameters_present_flag}})
        .Emit(out);
    if (sps.pScalingLists != nullptr) {
        Line("  scaling")
            .Hex("scaling_list_present_mask", sps.pScalingLists->scaling_list_present_mask)
            .Hex("use_default_scaling_matrix_mask", sps.pScalingLists->use_default_scaling_matrix_mask)
            .Hex("crc32", StructCrc32(*sps.pScalingLists))
            .Emit(out);
    }
    if (sps.pSequenceParameterSetVui != nullptr) {
        const StdVideoH264SequenceParameterSetVui& vui = *sps.pSequenceParameterSetVui;
        const StdVideoH264SpsVuiFlags& v = vui.flags;
        Line("  vui")
            .Num("aspect_ratio_idc", vui.aspect_ratio_idc)
            .Num("sar_width", vui.sar_width)
            .Num("sar_height", vui.sar_height)
            .Num("video_format", vui.video_format)
            .Num("colour_primaries", vui.colour_primaries)
            .Num("transfer_characteristics", vui.transfer_characteristics)
            .Num("matrix_coefficients", vui.matrix_coefficients)
            .Num("num_units_in_tick", vui.num_units_in_tick)
            .Num("time_scale", vui.time_scale)
            .Num("max_num_reorder_frames", vui.max_num_reorder_frames)
            .Num("max_dec_frame_buffering", vui.max_dec_frame_buffering)
            .Num("chroma_sample_loc_type_top_field", vui.chroma_sample_loc_type_top_field)
            .Num("chroma_sample_loc_type_bottom_field", vui.chroma_sample_loc_type_bottom_field)
            .Flags("flags", {{"aspect_ratio_info_present_flag", v.aspect_ratio_info_present_flag},
                             {"overscan_info_present_flag", v.overscan_info_present_flag},
                             {"overscan_appropriate_flag", v.overscan_appropriate_flag},
                             {"video_signal_type_present_flag", v.video_signal_type_present_flag},
                             {"video_full_range_flag", v.video_full_range_flag},
                             {"color_description_present_flag", v.color_description_present_flag},
                             {"chroma_loc_info_present_flag", v.chroma_loc_info_present_flag},
                             {"timing_info_present_flag", v.timing_info_present_flag},
                             {"fixed_frame_rate_flag", v.fixed_frame_rate_flag},
                             {"bitstream_restriction_flag", v.bitstream_restriction_flag},
                             {"nal_hrd_parameters_present_flag", v.nal_hrd_parameters_present_flag},
                             {"vcl_hrd_parameters_present_flag", v.vcl_hrd_parameters_present_flag}})
            .Emit(out);
        if (vui.pHrdParameters != nullptr) {
            TraceH264Hrd(out, *vui.pHrdParameters);
        }
    }
}

void TraceH264Pps(FILE* out, uint64_t index, int64_t update, const StdVideoH264PictureParameterSet& pps) {
    const StdVideoH264PpsFlags& f = pps.flags;
    Line("pps")
        .Num("index", static_cast<int64_t>(index))
        .Str("codec", "h264")
        .Num("update", update)
        .Num("seq_parameter_set_id", pps.seq_parameter_set_id)
        .Num("pic_parameter_set_id", pps.pic_parameter_set_id)
        .Num("num_ref_idx_l0_default_active_minus1", pps.num_ref_idx_l0_default_active_minus1)
        .Num("num_ref_idx_l1_default_active_minus1", pps.num_ref_idx_l1_default_active_minus1)
        .Num("weighted_bipred_idc", pps.weighted_bipred_idc)
        .Num("pic_init_qp_minus26", pps.pic_init_qp_minus26)
        .Num("pic_init_qs_minus26", pps.pic_init_qs_minus26)
        .Num("chroma_qp_index_offset", pps.chroma_qp_index_offset)
        .Num("second_chroma_qp_index_offset", pps.second_chroma_qp_index_offset)
        .Flags("flags", {{"transform_8x8_mode_flag", f.transform_8x8_mode_flag},
                         {"redundant_pic_cnt_present_flag", f.redundant_pic_cnt_present_flag},
                         {"constrained_intra_pred_flag", f.constrained_intra_pred_flag},
                         {"deblocking_filter_control_present_flag", f.deblocking_filter_control_present_flag},
                         {"weighted_pred_flag", f.weighted_pred_flag},
                         {"bottom_field_pic_order_in_frame_present_flag", f.bottom_field_pic_order_in_frame_present_flag},
                         {"entropy_coding_mode_flag", f.entropy_coding_mode_flag},
                         {"pic_scaling_matrix_present_flag", f.pic_scaling_matrix_present_flag}})
        .Emit(out);
    if (pps.pScalingLists != nullptr) {
        Line("  scaling")
            .Hex("scaling_list_present_mask", pps.pScalingLists->scaling_list_present_mask)
            .Hex("use_default_scaling_matrix_mask", pps.pScalingLists->use_default_scaling_matrix_mask)
            .Hex("crc32", StructCrc32(*pps.pScalingLists))
            .Emit(out);
    }
}

void TraceH264Picture(FILE* out, const VkParserPictureData& pd) {
    const VkParserH264PictureData& h = pd.CodecSpecific.h264;
    Line("  h264")
        .Num("field_pic_flag", pd.field_pic_flag)
        .Num("bottom_field_flag", pd.bottom_field_flag)
        .Num("second_field", pd.second_field)
        .Num("top_field_first", pd.top_field_first)
        .Num("repeat_first_field", pd.repeat_first_field)
        .Num("sps_update", ParameterSetUpdate(h.pStdSps))
        .Num("pps_update", ParameterSetUpdate(h.pStdPps))
        .Num("seq_parameter_set_id", h.seq_parameter_set_id)
        .Num("pic_parameter_set_id", h.pic_parameter_set_id)
        .Num("frame_num", h.frame_num)
        .Array("CurrFieldOrderCnt", h.CurrFieldOrderCnt, 2)
        .Num("num_ref_idx_l0_active_minus1", h.num_ref_idx_l0_active_minus1)
        .Num("num_ref_idx_l1_active_minus1", h.num_ref_idx_l1_active_minus1)
        .Num("weighted_pred_flag", h.weighted_pred_flag)
        .Num("weighted_bipred_idc", h.weighted_bipred_idc)
        .Num("pic_init_qp_minus26", h.pic_init_qp_minus26)
        .Num("pic_init_qs_minus26", h.pic_init_qs_minus26)
        .Num("redundant_pic_cnt_present_flag", h.redundant_pic_cnt_present_flag)
        .Num("deblocking_filter_control_present_flag", h.deblocking_filter_control_present_flag)
        .Num("transform_8x8_mode_flag", h.transform_8x8_mode_flag)
        .Num("MbaffFrameFlag", h.MbaffFrameFlag)
        .Num("constrained_intra_pred_flag", h.constrained_intra_pred_flag)
        .Num("entropy_coding_mode_flag", h.entropy_coding_mode_flag)
        .Num("pic_order_present_flag", h.pic_order_present_flag)
        .Num("chroma_qp_index_offset", h.chroma_qp_index_offset)
        .Num("second_chroma_qp_index_offset", h.second_chroma_qp_index_offset)
        .Num("fmo_aso_enable", h.fmo_aso_enable)
        .Num("num_slice_groups_minus1", h.num_slice_groups_minus1)
        .Num("slice_group_map_type", h.slice_group_map_type)
        .Num("slice_group_change_rate_minus1", h.slice_group_change_rate_minus1)
        .Emit(out);
    for (size_t i = 0; i < std::size(h.dpb); i++) {
        const VkParserH264DpbEntry& e = h.dpb[i];
        const bool empty = (e.pPicBuf == nullptr) && (e.FrameIdx == 0) && (e.is_long_term == 0) && (e.not_existing == 0) &&
                           (e.used_for_reference == 0) && (e.FieldOrderCnt[0] == 0) && (e.FieldOrderCnt[1] == 0);
        if (!empty) {
            Line("  dpb")
                .Num("i", static_cast<int64_t>(i))
                .Num("pic", (e.pPicBuf != nullptr) ? static_cast<vkPicBuffBase*>(e.pPicBuf)->m_picIdx : -1)
                .Num("FrameIdx", e.FrameIdx)
                .Num("is_long_term", e.is_long_term)
                .Num("not_existing", e.not_existing)
                .Num("used_for_reference", e.used_for_reference)
                .Array("FieldOrderCnt", e.FieldOrderCnt, 2)
                .Emit(out);
        }
    }
}

void TraceH265Ptl(FILE* out, const StdVideoH265ProfileTierLevel& ptl) {
    const StdVideoH265ProfileTierLevelFlags& f = ptl.flags;
    Line("  ptl")
        .Num("general_profile_idc", ptl.general_profile_idc)
        .Num("general_level_idc", ptl.general_level_idc)
        .Flags("flags", {{"general_tier_flag", f.general_tier_flag},
                         {"general_progressive_source_flag", f.general_progressive_source_flag},
                         {"general_interlaced_source_flag", f.general_interlaced_source_flag},
                         {"general_non_packed_constraint_flag", f.general_non_packed_constraint_flag},
                         {"general_frame_only_constraint_flag", f.general_frame_only_constraint_flag}})
        .Emit(out);
}

void TraceH265DecPicBufMgr(FILE* out, const StdVideoH265DecPicBufMgr& dpbm) {
    Line("  dpbm")
        .Array("max_latency_increase_plus1", dpbm.max_latency_increase_plus1, STD_VIDEO_H265_SUBLAYERS_LIST_SIZE)
        .Array("max_dec_pic_buffering_minus1", dpbm.max_dec_pic_buffering_minus1, STD_VIDEO_H265_SUBLAYERS_LIST_SIZE)
        .Array("max_num_reorder_pics", dpbm.max_num_reorder_pics, STD_VIDEO_H265_SUBLAYERS_LIST_SIZE)
        .Emit(out);
}

void TraceH265SubLayerHrd(FILE* out, const char* tag, const StdVideoH265SubLayerHrdParameters* subLayers,
                          const StdVideoH265HrdParameters& hrd, uint32_t subLayerCount) {
    for (uint32_t i = 0; (subLayers != nullptr) && (i < subLayerCount); i++) {
        const StdVideoH265SubLayerHrdParameters& s = subLayers[i];
        const size_t count = std::min<size_t>(hrd.cpb_cnt_minus1[i] + 1u, STD_VIDEO_H265_CPB_CNT_LIST_SIZE);
        Line(tag)
            .Num("i", i)
            .Array("bit_rate_value_minus1", s.bit_rate_value_minus1, count)
            .Array("cpb_size_value_minus1", s.cpb_size_value_minus1, count)
            .Array("cpb_size_du_value_minus1", s.cpb_size_du_value_minus1, count)
            .Array("bit_rate_du_value_minus1", s.bit_rate_du_value_minus1, count)
            .Hex("cbr_flag", s.cbr_flag)
            .Emit(out);
    }
}

void TraceH265Hrd(FILE* out, const StdVideoH265HrdParameters& hrd, uint32_t maxSubLayersMinus1) {
    const StdVideoH265HrdFlags& f = hrd.flags;
    const uint32_t subLayerCount = std::min<uint32_t>(maxSubLayersMinus1 + 1u, STD_VIDEO_H265_SUBLAYERS_LIST_SIZE);
    Line("  hrd")
        .Num("tick_divisor_minus2", hrd.tick_divisor_minus2)
        .Num("du_cpb_removal_delay_increment_length_minus1", hrd.du_cpb_removal_delay_increment_length_minus1)
        .Num("dpb_output_delay_du_length_minus1", hrd.dpb_output_delay_du_length_minus1)
        .Num("bit_rate_scale", hrd.bit_rate_scale)
        .Num("cpb_size_scale", hrd.cpb_size_scale)
        .Num("cpb_size_du_scale", hrd.cpb_size_du_scale)
        .Num("initial_cpb_removal_delay_length_minus1", hrd.initial_cpb_removal_delay_length_minus1)
        .Num("au_cpb_removal_delay_length_minus1", hrd.au_cpb_removal_delay_length_minus1)
        .Num("dpb_output_delay_length_minus1", hrd.dpb_output_delay_length_minus1)
        .Array("cpb_cnt_minus1", hrd.cpb_cnt_minus1, STD_VIDEO_H265_SUBLAYERS_LIST_SIZE)
        .Array("elemental_duration_in_tc_minus1", hrd.elemental_duration_in_tc_minus1, STD_VIDEO_H265_SUBLAYERS_LIST_SIZE)
        .Hex("fixed_pic_rate_general_flag", f.fixed_pic_rate_general_flag)
        .Hex("fixed_pic_rate_within_cvs_flag", f.fixed_pic_rate_within_cvs_flag)
        .Hex("low_delay_hrd_flag", f.low_delay_hrd_flag)
        .Flags("flags", {{"nal_hrd_parameters_present_flag", f.nal_hrd_parameters_present_flag},
                         {"vcl_hrd_parameters_present_flag", f.vcl_hrd_parameters_present_flag},
                         {"sub_pic_hrd_params_present_flag", f.sub_pic_hrd_params_present_flag},
                         {"sub_pic_cpb_params_in_pic_timing_sei_flag", f.sub_pic_cpb_params_in_pic_timing_sei_flag}})
        .Emit(out);
    if (f.nal_hrd_parameters_present_flag) {
        TraceH265SubLayerHrd(out, "  hrd-nal", hrd.pSubLayerHrdParametersNal, hrd, subLayerCount);
    }
    if (f.vcl_hrd_parameters_present_flag) {
        TraceH265SubLayerHrd(out, "  hrd-vcl", hrd.pSubLayerHrdParametersVcl, hrd, subLayerCount);
    }
}

void TraceH265Vps(FILE* out, uint64_t index, int64_t update, const StdVideoH265VideoParameterSet& vps) {
    const StdVideoH265VpsFlags& f = vps.flags;
    Line("vps")
        .Num("index", static_cast<int64_t>(index))
        .Str("codec", "h265")
        .Num("update", update)
        .Num("vps_video_parameter_set_id", vps.vps_video_parameter_set_id)
        .Num("vps_max_sub_layers_minus1", vps.vps_max_sub_layers_minus1)
        .Num("vps_num_units_in_tick", vps.vps_num_units_in_tick)
        .Num("vps_time_scale", vps.vps_time_scale)
        .Num("vps_num_ticks_poc_diff_one_minus1", vps.vps_num_ticks_poc_diff_one_minus1)
        .Flags("flags", {{"vps_temporal_id_nesting_flag", f.vps_temporal_id_nesting_flag},
                         {"vps_sub_layer_ordering_info_present_flag", f.vps_sub_layer_ordering_info_present_flag},
                         {"vps_timing_info_present_flag", f.vps_timing_info_present_flag},
                         {"vps_poc_proportional_to_timing_flag", f.vps_poc_proportional_to_timing_flag}})
        .Emit(out);
    if (vps.pProfileTierLevel != nullptr) {
        TraceH265Ptl(out, *vps.pProfileTierLevel);
    }
    if (vps.pDecPicBufMgr != nullptr) {
        TraceH265DecPicBufMgr(out, *vps.pDecPicBufMgr);
    }
    if (vps.pHrdParameters != nullptr) {
        TraceH265Hrd(out, *vps.pHrdParameters, vps.vps_max_sub_layers_minus1);
    }
}

void TraceH265ShortTermRefPicSet(FILE* out, uint32_t index, const StdVideoH265ShortTermRefPicSet& rps) {
    Line("  strps")
        .Num("i", index)
        .Num("delta_idx_minus1", rps.delta_idx_minus1)
        .Hex("use_delta_flag", rps.use_delta_flag)
        .Num("abs_delta_rps_minus1", rps.abs_delta_rps_minus1)
        .Hex("used_by_curr_pic_flag", rps.used_by_curr_pic_flag)
        .Hex("used_by_curr_pic_s0_flag", rps.used_by_curr_pic_s0_flag)
        .Hex("used_by_curr_pic_s1_flag", rps.used_by_curr_pic_s1_flag)
        .Num("num_negative_pics", rps.num_negative_pics)
        .Num("num_positive_pics", rps.num_positive_pics)
        .Array("delta_poc_s0_minus1", rps.delta_poc_s0_minus1, STD_VIDEO_H265_MAX_DPB_SIZE)
        .Array("delta_poc_s1_minus1", rps.delta_poc_s1_minus1, STD_VIDEO_H265_MAX_DPB_SIZE)
        .Flags("flags", {{"inter_ref_pic_set_prediction_flag", rps.flags.inter_ref_pic_set_prediction_flag},
                         {"delta_rps_sign", rps.flags.delta_rps_sign}})
        .Emit(out);
}

void TraceH265Sps(FILE* out, uint64_t index, int64_t update, const StdVideoH265SequenceParameterSet& sps) {
    const StdVideoH265SpsFlags& f = sps.flags;
    Line("sps")
        .Num("index", static_cast<int64_t>(index))
        .Str("codec", "h265")
        .Num("update", update)
        .Num("sps_video_parameter_set_id", sps.sps_video_parameter_set_id)
        .Num("sps_max_sub_layers_minus1", sps.sps_max_sub_layers_minus1)
        .Num("sps_seq_parameter_set_id", sps.sps_seq_parameter_set_id)
        .Num("chroma_format_idc", sps.chroma_format_idc)
        .Num("pic_width_in_luma_samples", sps.pic_width_in_luma_samples)
        .Num("pic_height_in_luma_samples", sps.pic_height_in_luma_samples)
        .Num("bit_depth_luma_minus8", sps.bit_depth_luma_minus8)
        .Num("bit_depth_chroma_minus8", sps.bit_depth_chroma_minus8)
        .Num("log2_max_pic_order_cnt_lsb_minus4", sps.log2_max_pic_order_cnt_lsb_minus4)
        .Num("log2_min_luma_coding_block_size_minus3", sps.log2_min_luma_coding_block_size_minus3)
        .Num("log2_diff_max_min_luma_coding_block_size", sps.log2_diff_max_min_luma_coding_block_size)
        .Num("log2_min_luma_transform_block_size_minus2", sps.log2_min_luma_transform_block_size_minus2)
        .Num("log2_diff_max_min_luma_transform_block_size", sps.log2_diff_max_min_luma_transform_block_size)
        .Num("max_transform_hierarchy_depth_inter", sps.max_transform_hierarchy_depth_inter)
        .Num("max_transform_hierarchy_depth_intra", sps.max_transform_hierarchy_depth_intra)
        .Num("num_short_term_ref_pic_sets", sps.num_short_term_ref_pic_sets)
        .Num("num_long_term_ref_pics_sps", sps.num_long_term_ref_pics_sps)
        .Num("pcm_sample_bit_depth_luma_minus1", sps.pcm_sample_bit_depth_luma_minus1)
        .Num("pcm_sample_bit_depth_chroma_minus1", sps.pcm_sample_bit_depth_chroma_minus1)
        .Num("log2_min_pcm_luma_coding_block_size_minus3", sps.log2_min_pcm_luma_coding_block_size_minus3)
        .Num("log2_diff_max_min_pcm_luma_coding_block_size", sps.log2_diff_max_min_pcm_luma_coding_block_size)
        .Num("palette_max_size", sps.palette_max_size)
        .Num("delta_palette_max_predictor_size", sps.delta_palette_max_predictor_size)
        .Num("motion_vector_resolution_control_idc", sps.motion_vector_resolution_control_idc)
        .Num("sps_num_palette_predictor_initializers_minus1", sps.sps_num_palette_predictor_initializers_minus1)
        .Num("conf_win_left_offset", sps.conf_win_left_offset)
        .Num("conf_win_right_offset", sps.conf_win_right_offset)
        .Num("conf_win_top_offset", sps.conf_win_top_offset)
        .Num("conf_win_bottom_offset", sps.conf_win_bottom_offset)
        .Flags("flags", {{"sps_temporal_id_nesting_flag", f.sps_temporal_id_nesting_flag},
                         {"separate_colour_plane_flag", f.separate_colour_plane_flag},
                         {"conformance_window_flag", f.conformance_window_flag},
                         {"sps_sub_layer_ordering_info_present_flag", f.sps_sub_layer_ordering_info_present_flag},
                         {"scaling_list_enabled_flag", f.scaling_list_enabled_flag},
                         {"sps_scaling_list_data_present_flag", f.sps_scaling_list_data_present_flag},
                         {"amp_enabled_flag", f.amp_enabled_flag},
                         {"sample_adaptive_offset_enabled_flag", f.sample_adaptive_offset_enabled_flag},
                         {"pcm_enabled_flag", f.pcm_enabled_flag},
                         {"pcm_loop_filter_disabled_flag", f.pcm_loop_filter_disabled_flag},
                         {"long_term_ref_pics_present_flag", f.long_term_ref_pics_present_flag},
                         {"sps_temporal_mvp_enabled_flag", f.sps_temporal_mvp_enabled_flag},
                         {"strong_intra_smoothing_enabled_flag", f.strong_intra_smoothing_enabled_flag},
                         {"vui_parameters_present_flag", f.vui_parameters_present_flag},
                         {"sps_extension_present_flag", f.sps_extension_present_flag},
                         {"sps_range_extension_flag", f.sps_range_extension_flag},
                         {"transform_skip_rotation_enabled_flag", f.transform_skip_rotation_enabled_flag},
                         {"transform_skip_context_enabled_flag", f.transform_skip_context_enabled_flag},
                         {"implicit_rdpcm_enabled_flag", f.implicit_rdpcm_enabled_flag},
                         {"explicit_rdpcm_enabled_flag", f.explicit_rdpcm_enabled_flag},
                         {"extended_precision_processing_flag", f.extended_precision_processing_flag},
                         {"intra_smoothing_disabled_flag", f.intra_smoothing_disabled_flag},
                         {"high_precision_offsets_enabled_flag", f.high_precision_offsets_enabled_flag},
                         {"persistent_rice_adaptation_enabled_flag", f.persistent_rice_adaptation_enabled_flag},
                         {"cabac_bypass_alignment_enabled_flag", f.cabac_bypass_alignment_enabled_flag},
                         {"sps_scc_extension_flag", f.sps_scc_extension_flag},
                         {"sps_curr_pic_ref_enabled_flag", f.sps_curr_pic_ref_enabled_flag},
                         {"palette_mode_enabled_flag", f.palette_mode_enabled_flag},
                         {"sps_palette_predictor_initializers_present_flag", f.sps_palette_predictor_initializers_present_flag},
                         {"intra_boundary_filtering_disabled_flag", f.intra_boundary_filtering_disabled_flag}})
        .Emit(out);
    if (sps.pProfileTierLevel != nullptr) {
        TraceH265Ptl(out, *sps.pProfileTierLevel);
    }
    if (sps.pDecPicBufMgr != nullptr) {
        TraceH265DecPicBufMgr(out, *sps.pDecPicBufMgr);
    }
    if (sps.pScalingLists != nullptr) {
        Line("  scaling").Hex("crc32", StructCrc32(*sps.pScalingLists)).Emit(out);
    }
    for (uint32_t i = 0; (sps.pShortTermRefPicSet != nullptr) && (i < sps.num_short_term_ref_pic_sets); i++) {
        TraceH265ShortTermRefPicSet(out, i, sps.pShortTermRefPicSet[i]);
    }
    if (sps.pLongTermRefPicsSps != nullptr) {
        Line("  ltrps")
            .Hex("used_by_curr_pic_lt_sps_flag", sps.pLongTermRefPicsSps->used_by_curr_pic_lt_sps_flag)
            .Array("lt_ref_pic_poc_lsb_sps", sps.pLongTermRefPicsSps->lt_ref_pic_poc_lsb_sps,
                   std::min<size_t>(sps.num_long_term_ref_pics_sps, STD_VIDEO_H265_MAX_LONG_TERM_REF_PICS_SPS))
            .Emit(out);
    }
    if (sps.pSequenceParameterSetVui != nullptr) {
        const StdVideoH265SequenceParameterSetVui& vui = *sps.pSequenceParameterSetVui;
        const StdVideoH265SpsVuiFlags& v = vui.flags;
        Line("  vui")
            .Num("aspect_ratio_idc", vui.aspect_ratio_idc)
            .Num("sar_width", vui.sar_width)
            .Num("sar_height", vui.sar_height)
            .Num("video_format", vui.video_format)
            .Num("colour_primaries", vui.colour_primaries)
            .Num("transfer_characteristics", vui.transfer_characteristics)
            .Num("matrix_coeffs", vui.matrix_coeffs)
            .Num("chroma_sample_loc_type_top_field", vui.chroma_sample_loc_type_top_field)
            .Num("chroma_sample_loc_type_bottom_field", vui.chroma_sample_loc_type_bottom_field)
            .Num("def_disp_win_left_offset", vui.def_disp_win_left_offset)
            .Num("def_disp_win_right_offset", vui.def_disp_win_right_offset)
            .Num("def_disp_win_top_offset", vui.def_disp_win_top_offset)
            .Num("def_disp_win_bottom_offset", vui.def_disp_win_bottom_offset)
            .Num("vui_num_units_in_tick", vui.vui_num_units_in_tick)
            .Num("vui_time_scale", vui.vui_time_scale)
            .Num("vui_num_ticks_poc_diff_one_minus1", vui.vui_num_ticks_poc_diff_one_minus1)
            .Num("min_spatial_segmentation_idc", vui.min_spatial_segmentation_idc)
            .Num("max_bytes_per_pic_denom", vui.max_bytes_per_pic_denom)
            .Num("max_bits_per_min_cu_denom", vui.max_bits_per_min_cu_denom)
            .Num("log2_max_mv_length_horizontal", vui.log2_max_mv_length_horizontal)
            .Num("log2_max_mv_length_vertical", vui.log2_max_mv_length_vertical)
            .Flags("flags", {{"aspect_ratio_info_present_flag", v.aspect_ratio_info_present_flag},
                             {"overscan_info_present_flag", v.overscan_info_present_flag},
                             {"overscan_appropriate_flag", v.overscan_appropriate_flag},
                             {"video_signal_type_present_flag", v.video_signal_type_present_flag},
                             {"video_full_range_flag", v.video_full_range_flag},
                             {"colour_description_present_flag", v.colour_description_present_flag},
                             {"chroma_loc_info_present_flag", v.chroma_loc_info_present_flag},
                             {"neutral_chroma_indication_flag", v.neutral_chroma_indication_flag},
                             {"field_seq_flag", v.field_seq_flag},
                             {"frame_field_info_present_flag", v.frame_field_info_present_flag},
                             {"default_display_window_flag", v.default_display_window_flag},
                             {"vui_timing_info_present_flag", v.vui_timing_info_present_flag},
                             {"vui_poc_proportional_to_timing_flag", v.vui_poc_proportional_to_timing_flag},
                             {"vui_hrd_parameters_present_flag", v.vui_hrd_parameters_present_flag},
                             {"bitstream_restriction_flag", v.bitstream_restriction_flag},
                             {"tiles_fixed_structure_flag", v.tiles_fixed_structure_flag},
                             {"motion_vectors_over_pic_boundaries_flag", v.motion_vectors_over_pic_boundaries_flag},
                             {"restricted_ref_pic_lists_flag", v.restricted_ref_pic_lists_flag}})
            .Emit(out);
        if (vui.pHrdParameters != nullptr) {
            TraceH265Hrd(out, *vui.pHrdParameters, sps.sps_max_sub_layers_minus1);
        }
    }
    if (sps.pPredictorPaletteEntries != nullptr) {
        Line("  palette").Hex("crc32", StructCrc32(*sps.pPredictorPaletteEntries)).Emit(out);
    }
}

void TraceH265Pps(FILE* out, uint64_t index, int64_t update, const StdVideoH265PictureParameterSet& pps) {
    const StdVideoH265PpsFlags& f = pps.flags;
    Line("pps")
        .Num("index", static_cast<int64_t>(index))
        .Str("codec", "h265")
        .Num("update", update)
        .Num("pps_pic_parameter_set_id", pps.pps_pic_parameter_set_id)
        .Num("pps_seq_parameter_set_id", pps.pps_seq_parameter_set_id)
        .Num("sps_video_parameter_set_id", pps.sps_video_parameter_set_id)
        .Num("num_extra_slice_header_bits", pps.num_extra_slice_header_bits)
        .Num("num_ref_idx_l0_default_active_minus1", pps.num_ref_idx_l0_default_active_minus1)
        .Num("num_ref_idx_l1_default_active_minus1", pps.num_ref_idx_l1_default_active_minus1)
        .Num("init_qp_minus26", pps.init_qp_minus26)
        .Num("diff_cu_qp_delta_depth", pps.diff_cu_qp_delta_depth)
        .Num("pps_cb_qp_offset", pps.pps_cb_qp_offset)
        .Num("pps_cr_qp_offset", pps.pps_cr_qp_offset)
        .Num("pps_beta_offset_div2", pps.pps_beta_offset_div2)
        .Num("pps_tc_offset_div2", pps.pps_tc_offset_div2)
        .Num("log2_parallel_merge_level_minus2", pps.log2_parallel_merge_level_minus2)
        .Num("log2_max_transform_skip_block_size_minus2", pps.log2_max_transform_skip_block_size_minus2)
        .Num("diff_cu_chroma_qp_offset_depth", pps.diff_cu_chroma_qp_offset_depth)
        .Num("chroma_qp_offset_list_len_minus1", pps.chroma_qp_offset_list_len_minus1)
        .Array("cb_qp_offset_list", pps.cb_qp_offset_list, STD_VIDEO_H265_CHROMA_QP_OFFSET_LIST_SIZE)
        .Array("cr_qp_offset_list", pps.cr_qp_offset_list, STD_VIDEO_H265_CHROMA_QP_OFFSET_LIST_SIZE)
        .Num("log2_sao_offset_scale_luma", pps.log2_sao_offset_scale_luma)
        .Num("log2_sao_offset_scale_chroma", pps.log2_sao_offset_scale_chroma)
        .Num("pps_act_y_qp_offset_plus5", pps.pps_act_y_qp_offset_plus5)
        .Num("pps_act_cb_qp_offset_plus5", pps.pps_act_cb_qp_offset_plus5)
        .Num("pps_act_cr_qp_offset_plus3", pps.pps_act_cr_qp_offset_plus3)
        .Num("pps_num_palette_predictor_initializers", pps.pps_num_palette_predictor_initializers)
        .Num("luma_bit_depth_entry_minus8", pps.luma_bit_depth_entry_minus8)
        .Num("chroma_bit_depth_entry_minus8", pps.chroma_bit_depth_entry_minus8)
        .Num("num_tile_columns_minus1", pps.num_tile_columns_minus1)
        .Num("num_tile_rows_minus1", pps.num_tile_rows_minus1)
        .Array("column_width_minus1", pps.column_width_minus1, STD_VIDEO_H265_CHROMA_QP_OFFSET_TILE_COLS_LIST_SIZE)
        .Array("row_height_minus1", pps.row_height_minus1, STD_VIDEO_H265_CHROMA_QP_OFFSET_TILE_ROWS_LIST_SIZE)
        .Flags("flags", {{"dependent_slice_segments_enabled_flag", f.dependent_slice_segments_enabled_flag},
                         {"output_flag_present_flag", f.output_flag_present_flag},
                         {"sign_data_hiding_enabled_flag", f.sign_data_hiding_enabled_flag},
                         {"cabac_init_present_flag", f.cabac_init_present_flag},
                         {"constrained_intra_pred_flag", f.constrained_intra_pred_flag},
                         {"transform_skip_enabled_flag", f.transform_skip_enabled_flag},
                         {"cu_qp_delta_enabled_flag", f.cu_qp_delta_enabled_flag},
                         {"pps_slice_chroma_qp_offsets_present_flag", f.pps_slice_chroma_qp_offsets_present_flag},
                         {"weighted_pred_flag", f.weighted_pred_flag},
                         {"weighted_bipred_flag", f.weighted_bipred_flag},
                         {"transquant_bypass_enabled_flag", f.transquant_bypass_enabled_flag},
                         {"tiles_enabled_flag", f.tiles_enabled_flag},
                         {"entropy_coding_sync_enabled_flag", f.entropy_coding_sync_enabled_flag},
                         {"uniform_spacing_flag", f.uniform_spacing_flag},
                         {"loop_filter_across_tiles_enabled_flag", f.loop_filter_across_tiles_enabled_flag},
                         {"pps_loop_filter_across_slices_enabled_flag", f.pps_loop_filter_across_slices_enabled_flag},
                         {"deblocking_filter_control_present_flag", f.deblocking_filter_control_present_flag},
                         {"deblocking_filter_override_enabled_flag", f.deblocking_filter_override_enabled_flag},
                         {"pps_deblocking_filter_disabled_flag", f.pps_deblocking_filter_disabled_flag},
                         {"pps_scaling_list_data_present_flag", f.pps_scaling_list_data_present_flag},
                         {"lists_modification_present_flag", f.lists_modification_present_flag},
                         {"slice_segment_header_extension_present_flag", f.slice_segment_header_extension_present_flag},
                         {"pps_extension_present_flag", f.pps_extension_present_flag},
                         {"cross_component_prediction_enabled_flag", f.cross_component_prediction_enabled_flag},
                         {"chroma_qp_offset_list_enabled_flag", f.chroma_qp_offset_list_enabled_flag},
                         {"pps_curr_pic_ref_enabled_flag", f.pps_curr_pic_ref_enabled_flag},
                         {"residual_adaptive_colour_transform_enabled_flag", f.residual_adaptive_colour_transform_enabled_flag},
                         {"pps_slice_act_qp_offsets_present_flag", f.pps_slice_act_qp_offsets_present_flag},
                         {"pps_palette_predictor_initializers_present_flag", f.pps_palette_predictor_initializers_present_flag},
                         {"monochrome_palette_flag", f.monochrome_palette_flag},
                         {"pps_range_extension_flag", f.pps_range_extension_flag}})
        .Emit(out);
    if (pps.pScalingLists != nullptr) {
        Line("  scaling").Hex("crc32", StructCrc32(*pps.pScalingLists)).Emit(out);
    }
    if (pps.pPredictorPaletteEntries != nullptr) {
        Line("  palette").Hex("crc32", StructCrc32(*pps.pPredictorPaletteEntries)).Emit(out);
    }
}

void TraceH265Picture(FILE* out, const VkParserPictureData& pd) {
    const VkParserHevcPictureData& h = pd.CodecSpecific.hevc;
    int64_t refPics[sizeof(h.RefPics) / sizeof(h.RefPics[0])];
    for (size_t i = 0; i < std::size(refPics); i++) {
        refPics[i] = (h.RefPics[i] != nullptr) ? static_cast<vkPicBuffBase*>(h.RefPics[i])->m_picIdx : -1;
    }
    Line("  hevc")
        .Num("vps_update", ParameterSetUpdate(h.pStdVps))
        .Num("sps_update", ParameterSetUpdate(h.pStdSps))
        .Num("pps_update", ParameterSetUpdate(h.pStdPps))
        .Num("vps_video_parameter_set_id", h.vps_video_parameter_set_id)
        .Num("seq_parameter_set_id", h.seq_parameter_set_id)
        .Num("pic_parameter_set_id", h.pic_parameter_set_id)
        .Num("IrapPicFlag", h.IrapPicFlag)
        .Num("IdrPicFlag", h.IdrPicFlag)
        .Num("short_term_ref_pic_set_sps_flag", h.short_term_ref_pic_set_sps_flag)
        .Num("CurrPicOrderCntVal", h.CurrPicOrderCntVal)
        .Num("NumBitsForShortTermRPSInSlice", h.NumBitsForShortTermRPSInSlice)
        .Num("NumDeltaPocsOfRefRpsIdx", h.NumDeltaPocsOfRefRpsIdx)
        .Num("ProfileLevel", h.ProfileLevel)
        .Num("ColorPrimaries", h.ColorPrimaries)
        .Num("bit_depth_luma_minus8", h.bit_depth_luma_minus8)
        .Num("bit_depth_chroma_minus8", h.bit_depth_chroma_minus8)
        .Emit(out);
    Line("  rps")
        .Num("NumPocTotalCurr", h.NumPocTotalCurr)
        .Num("NumPocStCurrBefore", h.NumPocStCurrBefore)
        .Num("NumPocStCurrAfter", h.NumPocStCurrAfter)
        .Num("NumPocLtCurr", h.NumPocLtCurr)
        .Array("RefPics", refPics, std::size(refPics))
        .Array("PicOrderCntVal", h.PicOrderCntVal, std::size(h.PicOrderCntVal))
        .Array("IsLongTerm", h.IsLongTerm, std::size(h.IsLongTerm))
        .Array("RefPicSetStCurrBefore", h.RefPicSetStCurrBefore, std::size(h.RefPicSetStCurrBefore))
        .Array("RefPicSetStCurrAfter", h.RefPicSetStCurrAfter, std::size(h.RefPicSetStCurrAfter))
        .Array("RefPicSetLtCurr", h.RefPicSetLtCurr, std::size(h.RefPicSetLtCurr))
        .Emit(out);
    if (h.mv_hevc_enable || (h.nuh_layer_id != 0)) {
        Line("  mv-hevc")
            .Num("mv_hevc_enable", h.mv_hevc_enable)
            .Num("nuh_layer_id", h.nuh_layer_id)
            .Num("default_ref_layers_active_flag", h.default_ref_layers_active_flag)
            .Num("NumDirectRefLayers", h.NumDirectRefLayers)
            .Num("max_one_active_ref_layer_flag", h.max_one_active_ref_layer_flag)
            .Num("poc_lsb_not_present_flag", h.poc_lsb_not_present_flag)
            .Num("NumActiveRefLayerPics0", h.NumActiveRefLayerPics0)
            .Num("NumActiveRefLayerPics1", h.NumActiveRefLayerPics1)
            .Array("RefPicSetInterLayer0", h.RefPicSetInterLayer0, std::size(h.RefPicSetInterLayer0))
            .Array("RefPicSetInterLayer1", h.RefPicSetInterLayer1, std::size(h.RefPicSetInterLayer1))
            .Emit(out);
    }
}

void TraceVp9Picture(FILE* out, const VkParserPictureData& pd) {
    const VkParserVp9PictureData& v = pd.CodecSpecific.vp9;
    const StdVideoDecodeVP9PictureInfo& info = v.stdPictureInfo;
    const StdVideoDecodeVP9PictureInfoFlags& f = info.flags;
    const auto picIdx = [](VkPicIf* picture) { return (picture != nullptr) ? static_cast<vkPicBuffBase*>(picture)->m_picIdx : -1; };
    Line("  vp9")
        .Num("FrameWidth", v.FrameWidth)
        .Num("FrameHeight", v.FrameHeight)
        .Num("MiCols", v.MiCols)
        .Num("MiRows", v.MiRows)
        .Num("Sb64Cols", v.Sb64Cols)
        .Num("Sb64Rows", v.Sb64Rows)
        .Num("renderWidth", v.renderWidth)
        .Num("renderHeight", v.renderHeight)
        .Num("frame_to_show_map_idx", v.frame_to_show_map_idx)
        .Num("show_existing_frame", v.show_existing_frame)
        .Array("ref_frame_idx", v.ref_frame_idx, STD_VIDEO_VP9_REFS_PER_FRAME)
        .Array("pic_idx", v.pic_idx, STD_VIDEO_VP9_NUM_REF_FRAMES)
        .Num("last", picIdx(v.pLastRef))
        .Num("golden", picIdx(v.pGoldenRef))
        .Num("altref", picIdx(v.pAltRef))
        .Num("FrameIsIntra", v.FrameIsIntra)
        .Num("ChromaFormat", v.ChromaFormat)
        .Num("numTiles", v.numTiles)
        .Num("compressedHeaderSize", v.compressedHeaderSize)
        .Num("uncompressedHeaderOffset", v.uncompressedHeaderOffset)
        .Num("compressedHeaderOffset", v.compressedHeaderOffset)
        .Num("tilesOffset", v.tilesOffset)
        .Emit(out);
    Line("  std")
        .Num("profile", info.profile)
        .Num("frame_type", info.frame_type)
        .Num("frame_context_idx", info.frame_context_idx)
        .Num("reset_frame_context", info.reset_frame_context)
        .Hex("refresh_frame_flags", info.refresh_frame_flags)
        .Hex("ref_frame_sign_bias_mask", info.ref_frame_sign_bias_mask)
        .Num("interpolation_filter", info.interpolation_filter)
        .Num("base_q_idx", info.base_q_idx)
        .Num("delta_q_y_dc", info.delta_q_y_dc)
        .Num("delta_q_uv_dc", info.delta_q_uv_dc)
        .Num("delta_q_uv_ac", info.delta_q_uv_ac)
        .Num("tile_cols_log2", info.tile_cols_log2)
        .Num("tile_rows_log2", info.tile_rows_log2)
        .Flags("flags", {{"error_resilient_mode", f.error_resilient_mode},
                         {"intra_only", f.intra_only},
                         {"allow_high_precision_mv", f.allow_high_precision_mv},
                         {"refresh_frame_context", f.refresh_frame_context},
                         {"frame_parallel_decoding_mode", f.frame_parallel_decoding_mode},
                         {"segmentation_enabled", f.segmentation_enabled},
                         {"show_frame", f.show_frame},
                         {"UsePrevFrameMvs", f.UsePrevFrameMvs}})
        .Emit(out);
    const StdVideoVP9ColorConfig& c = v.stdColorConfig;
    Line("  color")
        .Num("BitDepth", c.BitDepth)
        .Num("subsampling_x", c.subsampling_x)
        .Num("subsampling_y", c.subsampling_y)
        .Num("color_space", c.color_space)
        .Flags("flags", {{"color_range", c.flags.color_range}})
        .Emit(out);
    const StdVideoVP9LoopFilter& lf = v.stdLoopFilter;
    Line("  lf")
        .Num("loop_filter_level", lf.loop_filter_level)
        .Num("loop_filter_sharpness", lf.loop_filter_sharpness)
        .Num("update_ref_delta", lf.update_ref_delta)
        .Array("loop_filter_ref_deltas", lf.loop_filter_ref_deltas, STD_VIDEO_VP9_MAX_REF_FRAMES)
        .Num("update_mode_delta", lf.update_mode_delta)
        .Array("loop_filter_mode_deltas", lf.loop_filter_mode_deltas, STD_VIDEO_VP9_LOOP_FILTER_ADJUSTMENTS)
        .Flags("flags", {{"loop_filter_delta_enabled", lf.flags.loop_filter_delta_enabled},
                         {"loop_filter_delta_update", lf.flags.loop_filter_delta_update}})
        .Emit(out);
    const StdVideoVP9Segmentation& seg = v.stdSegmentation;
    Line("  seg")
        .Array("segmentation_tree_probs", seg.segmentation_tree_probs, STD_VIDEO_VP9_MAX_SEGMENTATION_TREE_PROBS)
        .Array("segmentation_pred_prob", seg.segmentation_pred_prob, STD_VIDEO_VP9_MAX_SEGMENTATION_PRED_PROB)
        .Array("FeatureEnabled", seg.FeatureEnabled, STD_VIDEO_VP9_MAX_SEGMENTS)
        .Array("FeatureData", &seg.FeatureData[0][0], STD_VIDEO_VP9_MAX_SEGMENTS * STD_VIDEO_VP9_SEG_LVL_MAX)
        .Flags("flags", {{"segmentation_update_map", seg.flags.segmentation_update_map},
                         {"segmentation_temporal_update", seg.flags.segmentation_temporal_update},
                         {"segmentation_update_data", seg.flags.segmentation_update_data},
                         {"segmentation_abs_or_delta_update", seg.flags.segmentation_abs_or_delta_update}})
        .Emit(out);
}

// Records every callback. Pictures come from a fixed pool, lowest free index first, so the picture
// indices in the trace depend only on the parser's reference counting.
class TraceClient final : public TraceClientIf {
   public:
    TraceClient(FILE* out, VkVideoCodecOperationFlagBitsKHR codec, uint32_t poolSize) : m_out(out), m_codec(codec) {
        for (uint32_t i = 0; i < poolSize; i++) {
            m_pictures.emplace_back(std::make_unique<TracePicture>());
            m_pictures.back()->m_picIdx = static_cast<int32_t>(i);
        }
    }

    ~TraceClient() override = default;

    int32_t BeginSequence(const VkParserSequenceInfo* si) override {
        Line("seq")
            .Num("index", static_cast<int64_t>(m_sequences++))
            .Hex("eCodec", si->eCodec)
            .Num("isSVC", si->isSVC)
            .Hex("frameRate", si->frameRate)
            .Num("bProgSeq", si->bProgSeq)
            .Num("nDisplayWidth", si->nDisplayWidth)
            .Num("nDisplayHeight", si->nDisplayHeight)
            .Num("nCodedWidth", si->nCodedWidth)
            .Num("nCodedHeight", si->nCodedHeight)
            .Num("nDisplayOffsetX", si->nDisplayOffsetX)
            .Num("nDisplayOffsetY", si->nDisplayOffsetY)
            .Num("nMaxWidth", si->nMaxWidth)
            .Num("nMaxHeight", si->nMaxHeight)
            .Num("nChromaFormat", si->nChromaFormat)
            .Num("uBitDepthLumaMinus8", si->uBitDepthLumaMinus8)
            .Num("uBitDepthChromaMinus8", si->uBitDepthChromaMinus8)
            .Num("uVideoFullRange", si->uVideoFullRange)
            .Num("lBitrate", si->lBitrate)
            .Num("lDARWidth", si->lDARWidth)
            .Num("lDARHeight", si->lDARHeight)
            .Num("lVideoFormat", si->lVideoFormat)
            .Num("lColorPrimaries", si->lColorPrimaries)
            .Num("lTransferCharacteristics", si->lTransferCharacteristics)
            .Num("lMatrixCoefficients", si->lMatrixCoefficients)
            .Num("nMinNumDpbSlots", si->nMinNumDpbSlots)
            .Num("nMinNumDecodeSurfaces", si->nMinNumDecodeSurfaces)
            .Num("codecProfile", si->codecProfile)
            .Num("hasFilmGrain", si->hasFilmGrain)
            .Num("canUseFields", si->canUseFields)
            .Num("cbSequenceHeader", si->cbSequenceHeader)
            .Hex("sequenceHeaderCrc32",
                 Crc32(si->SequenceHeaderData, std::min<size_t>(std::max(si->cbSequenceHeader, 0), VK_MAX_SEQ_HDR_LEN)))
            .Emit(m_out);
        return std::max<int32_t>(si->nMinNumDecodeSurfaces, 1);
    }

    bool AllocPictureBuffer(VkPicIf** ppPicBuf) override {
        for (auto& picture : m_pictures) {
            if (picture->IsAvailable()) {
                picture->AddRef();
                *ppPicBuf = picture.get();
                return true;
            }
        }
        Line("alloc-failed").Num("pool", static_cast<int64_t>(m_pictures.size())).Emit(m_out);
        *ppPicBuf = nullptr;
        return false;
    }

    bool DecodePicture(VkParserPictureData* pd) override {
        uint32_t crc = 0;
        if (pd->bitstreamData) {
            VkDeviceSize maxSize = 0;
            const uint8_t* data = pd->bitstreamData->GetReadOnlyDataPtr(pd->bitstreamDataOffset, maxSize);
            if ((data != nullptr) && (maxSize >= pd->bitstreamDataLen)) {
                crc = Crc32(data, pd->bitstreamDataLen);
            }
        }
        Line("pic")
            .Num("index", static_cast<int64_t>(m_decoded++))
            .Num("curr", PicIdx(pd->pCurrPic))
            .Num("PicWidthInMbs", pd->PicWidthInMbs)
            .Num("FrameHeightInMbs", pd->FrameHeightInMbs)
            .Num("progressive_frame", pd->progressive_frame)
            .Num("ref_pic_flag", pd->ref_pic_flag)
            .Num("intra_pic_flag", pd->intra_pic_flag)
            .Num("chroma_format", pd->chroma_format)
            .Num("picture_order_count", pd->picture_order_count)
            .Num("current_dpb_id", pd->current_dpb_id)
            .Num("firstSliceIndex", pd->firstSliceIndex)
            .Num("numSlices", pd->numSlices)
            .Num("bitstreamDataOffset", static_cast<int64_t>(pd->bitstreamDataOffset))
            .Num("bitstreamDataLen", static_cast<int64_t>(pd->bitstreamDataLen))
            .Hex("crc32", crc)
            .Emit(m_out);
        switch (m_codec) {
            case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
                TraceH264Picture(m_out, *pd);
                break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
                TraceH265Picture(m_out, *pd);
                break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
                TraceAv1Picture(m_out, *pd);
                break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR:
                TraceVp9Picture(m_out, *pd);
                break;
            default:
                break;
        }
        return true;
    }

    bool UpdatePictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>& params,
                                 VkSharedBaseObj<VkVideoRefCountBase>& client) override {
        const uint64_t index = m_parameterSets++;
        const int64_t update = ParameterSetUpdate(params.get());
        if (!params) {
            Line("params").Num("index", static_cast<int64_t>(index)).Emit(m_out);
        } else if (params->GetStdH264Sps() != nullptr) {
            TraceH264Sps(m_out, index, update, *params->GetStdH264Sps());
        } else if (params->GetStdH264Pps() != nullptr) {
            TraceH264Pps(m_out, index, update, *params->GetStdH264Pps());
        } else if (params->GetStdH265Vps() != nullptr) {
            TraceH265Vps(m_out, index, update, *params->GetStdH265Vps());
        } else if (params->GetStdH265Sps() != nullptr) {
            TraceH265Sps(m_out, index, update, *params->GetStdH265Sps());
        } else if (params->GetStdH265Pps() != nullptr) {
            TraceH265Pps(m_out, index, update, *params->GetStdH265Pps());
        } else if (params->GetStdAV1Sps() != nullptr) {
            TraceAv1SequenceHeader(m_out, index, *params->GetStdAV1Sps());
        } else {
            Line("params").Num("index", static_cast<int64_t>(index)).Num("type", params->GetStdType()).Emit(m_out);
        }
        client = std::make_shared<VkVideoRefCountBase>();
        return true;
    }

    bool DisplayPicture(VkPicIf* pPicBuf, int64_t llPTS) override {
        Line("disp").Num("index", static_cast<int64_t>(m_displayed++)).Num("pic", PicIdx(pPicBuf)).Num("pts", llPTS).Emit(m_out);
        return true;
    }

    void UnhandledNALU(const uint8_t* pbData, size_t cbData) override {
        Line("unhandled").Num("bytes", static_cast<int64_t>(cbData)).Hex("crc32", Crc32(pbData, cbData)).Emit(m_out);
    }

    VkDeviceSize GetBitstreamBuffer(VkDeviceSize size, VkDeviceSize minBitstreamBufferOffsetAlignment,
                                    VkDeviceSize minBitstreamBufferSizeAlignment, const uint8_t* pInitializeBufferMemory,
                                    VkDeviceSize initializeBufferMemorySize,
                                    VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer) override {
        auto buffer = std::make_shared<HostBitstreamBuffer>(std::max(size, initializeBufferMemorySize),
                                                            minBitstreamBufferOffsetAlignment, minBitstreamBufferSizeAlignment);
        if ((pInitializeBufferMemory != nullptr) && (initializeBufferMemorySize != 0)) {
            buffer->CopyDataFromBuffer(pInitializeBufferMemory, 0, 0, initializeBufferMemorySize);
        }
        bitstreamBuffer = buffer;
        return buffer->GetMaxSize();
    }

    uint64_t Decoded() const override { return m_decoded; }
    uint64_t Displayed() const override { return m_displayed; }

   private:
    static int64_t PicIdx(VkPicIf* picture) { return (picture != nullptr) ? static_cast<vkPicBuffBase*>(picture)->m_picIdx : -1; }

    FILE* m_out;
    VkVideoCodecOperationFlagBitsKHR m_codec;
    std::vector<std::unique_ptr<TracePicture>> m_pictures;
    uint64_t m_sequences = 0;
    uint64_t m_parameterSets = 0;
    uint64_t m_decoded = 0;
    uint64_t m_displayed = 0;
};

const CodecInfo kCodecs[] = {
    {"h264",
     VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
     {VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION}},
    {"h265",
     VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
     {VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION}},
    {"av1",
     VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
     {VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_SPEC_VERSION}},
    {"vp9",
     VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR,
     {VK_STD_VULKAN_VIDEO_CODEC_VP9_DECODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_VP9_DECODE_SPEC_VERSION}},
};

}  // namespace

const CodecInfo* FindCodec(const char* name) {
    for (const CodecInfo& codec : kCodecs) {
        if (strcmp(name, codec.name) == 0) {
            return &codec;
        }
    }
    return nullptr;
}

std::unique_ptr<TraceClientIf> CreateTraceClient(FILE* out, VkVideoCodecOperationFlagBitsKHR codec, uint32_t poolSize) {
    return std::make_unique<TraceClient>(out, codec, poolSize);
}

// Gives the parser a whole byte stream as the decoder sample does for an elementary stream
// (VulkanVideoProcessor::ParserProcessNextDataChunk): the rest of the stream on each call, with
// bPartialParsing, which returns after the first decode or display callback. The parser always
// consumes at least one byte of a non-empty packet.
static void WriteLe(std::vector<uint8_t>& out, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

// Writes the packets as an IVF file, so that another tool (ffmpeg's trace_headers) reads exactly the
// temporal units the parser was given.
bool WriteIvf(const char* path, const char* fourcc, const std::vector<Packet>& packets) {
    std::vector<uint8_t> header;
    header.insert(header.end(), {'D', 'K', 'I', 'F'});
    WriteLe(header, 0, 2);   // version
    WriteLe(header, 32, 2);  // header size
    header.insert(header.end(), fourcc, fourcc + 4);
    WriteLe(header, 0, 4);  // width and height, unknown here
    WriteLe(header, 1, 4);  // time base denominator
    WriteLe(header, 1, 4);  // time base numerator
    WriteLe(header, packets.size(), 4);
    WriteLe(header, 0, 4);
    FILE* file = fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = (fwrite(header.data(), 1, header.size(), file) == header.size());
    for (const Packet& packet : packets) {
        std::vector<uint8_t> frameHeader;
        WriteLe(frameHeader, packet.data.size(), 4);
        WriteLe(frameHeader, static_cast<uint64_t>(packet.pts), 8);
        ok = ok && (fwrite(frameHeader.data(), 1, frameHeader.size(), file) == frameHeader.size());
        ok = ok && (fwrite(packet.data.data(), 1, packet.data.size(), file) == packet.data.size());
    }
    return (fclose(file) == 0) && ok;
}

VkResult CreateParser(const CodecInfo& codec, TraceClientIf& client, uint32_t offsetAlignment, uint32_t sizeAlignment,
                      VkSharedBaseObj<VulkanVideoDecodeParser>& parser) {
    VkParserInitDecodeParameters params = {};
    params.interfaceVersion = NV_VULKAN_VIDEO_PARSER_API_VERSION;
    params.pClient = &client;
    params.defaultMinBufferSize = 2 * 1024 * 1024;
    params.bufferOffsetAlignment = offsetAlignment;
    params.bufferSizeAlignment = sizeAlignment;
    params.referenceClockRate = 0;
    params.errorThreshold = 0;
    return CreateVulkanVideoDecodeParser(codec.operation, &codec.stdExtension, ParserLog, 0, &params, parser);
}

void ParsePackets(VulkanVideoDecodeParser& parser, const std::vector<Packet>& packets, FILE* out) {
    for (size_t i = 0; i < packets.size(); i++) {
        Line("pkt")
            .Num("index", static_cast<int64_t>(i))
            .Num("bytes", static_cast<int64_t>(packets[i].data.size()))
            .Num("pts", packets[i].pts)
            .Emit(out);
        VkParserBitstreamPacket packet = {};
        packet.pByteStream = packets[i].data.data();
        packet.nDataLength = packets[i].data.size();
        packet.llPTS = packets[i].pts;
        packet.bPTSValid = 1;
        packet.bEOP = 1;
        size_t parsedBytes = 0;
        if (!parser.ParseByteStream(&packet, &parsedBytes)) {
            Line("pkt-error").Num("index", static_cast<int64_t>(i)).Emit(out);
        }
    }
}

void EndStream(VulkanVideoDecodeParser& parser, const TraceClientIf& client, FILE* out) {
    VkParserBitstreamPacket endOfStream = {};
    endOfStream.bEOS = 1;
    parser.ParseByteStream(&endOfStream);
    Line("eos")
        .Num("pictures", static_cast<int64_t>(client.Decoded()))
        .Num("displayed", static_cast<int64_t>(client.Displayed()))
        .Emit(out);
}

void ParseByteStreamPartially(VulkanVideoDecodeParser& parser, const std::vector<uint8_t>& stream, FILE* out) {
    size_t offset = 0;
    for (uint64_t call = 0; offset < stream.size(); call++) {
        VkParserBitstreamPacket packet = {};
        packet.pByteStream = stream.data() + offset;
        packet.nDataLength = stream.size() - offset;
        packet.bPartialParsing = 1;
        size_t parsedBytes = 0;
        const bool parsed = parser.ParseByteStream(&packet, &parsedBytes);
        Line("pkt")
            .Num("index", static_cast<int64_t>(call))
            .Num("offset", static_cast<int64_t>(offset))
            .Num("parsed", static_cast<int64_t>(parsedBytes))
            .Emit(out);
        if (!parsed) {
            Line("pkt-error").Num("index", static_cast<int64_t>(call)).Emit(out);
        }
        if ((parsedBytes == 0) || (parsedBytes > packet.nDataLength)) {
            Line("pkt-stalled").Num("index", static_cast<int64_t>(call)).Emit(out);
            break;
        }
        offset += parsedBytes;
    }
}

void ParserLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

}  // namespace parser_trace
