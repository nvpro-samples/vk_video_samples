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

// vk-video-parser-trace runs NvVideoParser over a stream with no Vulkan device and writes a
// canonical text trace of every callback the parser makes: sequence info, parameter sets, each
// picture with its DPB snapshot and a CRC32 of its bytes, and the display order.
//
// The trace is deterministic for a given parser and stream, so a golden comparison catches any
// change in what the parser hands a decoder. scripts/av1_reference_model.py checks the AV1 fields
// against the AV1 specification. See ../README.md.

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vkvideo_parser/VulkanVideoParserIf.h"
#include "NvVideoParser/nvVulkanVideoParser.h"

namespace {

// One trace line: a tag followed by key=value tokens. Arrays print as [a,b,c] and flag sets as
// name|name, or "-" when no flag is set.
class Line {
   public:
    explicit Line(const char* tag) : m_text(tag) {}

    Line& Num(const char* key, int64_t value) {
        Key(key);
        m_text += std::to_string(value);
        return *this;
    }

    Line& Hex(const char* key, uint64_t value) {
        char text[24];
        snprintf(text, sizeof(text), "0x%" PRIx64, value);
        Key(key);
        m_text += text;
        return *this;
    }

    Line& Str(const char* key, const char* value) {
        Key(key);
        m_text += value;
        return *this;
    }

    template <typename T>
    Line& Array(const char* key, const T* values, size_t count) {
        Key(key);
        m_text += '[';
        for (size_t i = 0; i < count; i++) {
            if (i != 0) {
                m_text += ',';
            }
            m_text += std::to_string(static_cast<int64_t>(values[i]));
        }
        m_text += ']';
        return *this;
    }

    Line& Flags(const char* key, std::initializer_list<std::pair<const char*, uint32_t>> flags) {
        Key(key);
        bool any = false;
        for (const auto& flag : flags) {
            if (flag.second != 0) {
                if (any) {
                    m_text += '|';
                }
                m_text += flag.first;
                any = true;
            }
        }
        if (!any) {
            m_text += '-';
        }
        return *this;
    }

    void Emit(FILE* out) const {
        fputs(m_text.c_str(), out);
        fputc('\n', out);
    }

   private:
    void Key(const char* key) {
        m_text += ' ';
        m_text += key;
        m_text += '=';
    }

    std::string m_text;
};

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
    const uint32_t tileCount = std::min<uint32_t>(av1.khr_info.tileCount, 64);

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
    const uint32_t tileCols = std::min<uint32_t>(tiles.TileCols, 64);
    const uint32_t tileRows = std::min<uint32_t>(tiles.TileRows, 64);
    Line("  tiles")
        .Num("TileCols", tiles.TileCols)
        .Num("TileRows", tiles.TileRows)
        .Num("context_update_tile_id", tiles.context_update_tile_id)
        .Num("tile_size_bytes_minus_1", tiles.tile_size_bytes_minus_1)
        .Array("MiColStarts", av1.MiColStarts, std::min<uint32_t>(tileCols + 1, 64))
        .Array("MiRowStarts", av1.MiRowStarts, std::min<uint32_t>(tileRows + 1, 64))
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

// Records every callback. Pictures come from a fixed pool, lowest free index first, so the picture
// indices in the trace depend only on the parser's reference counting.
class TraceClient : public VkParserVideoDecodeClient {
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
        if (m_codec == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
            TraceAv1Picture(m_out, *pd);
        }
        return true;
    }

    bool UpdatePictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>& params,
                                 VkSharedBaseObj<VkVideoRefCountBase>& client) override {
        const StdVideoAV1SequenceHeader* av1Sps = params ? params->GetStdAV1Sps() : nullptr;
        if (av1Sps != nullptr) {
            TraceAv1SequenceHeader(m_out, m_parameterSets++, *av1Sps);
        } else {
            Line("params").Num("index", static_cast<int64_t>(m_parameterSets++)).Emit(m_out);
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

    uint64_t Decoded() const { return m_decoded; }
    uint64_t Displayed() const { return m_displayed; }

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

struct Packet {
    std::vector<uint8_t> data;
    int64_t pts;
};

bool ReadFile(const char* path, std::vector<uint8_t>& data) {
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    uint8_t chunk[1 << 16];
    size_t count = 0;
    while ((count = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        data.insert(data.end(), chunk, chunk + count);
    }
    const bool ok = (ferror(file) == 0);
    fclose(file);
    return ok;
}

uint32_t ReadLe32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// AV1 leb128() (AV1 4.10.5). Returns the number of bytes read, or 0 when the value is truncated.
size_t ReadLeb128(const std::vector<uint8_t>& data, size_t pos, uint64_t& value) {
    value = 0;
    for (size_t i = 0; i < 8; i++) {
        if (pos + i >= data.size()) {
            return 0;
        }
        value |= static_cast<uint64_t>(data[pos + i] & 0x7f) << (7 * i);
        if ((data[pos + i] & 0x80) == 0) {
            return i + 1;
        }
    }
    return 0;
}

void WriteLeb128(std::vector<uint8_t>& out, uint64_t value) {
    do {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        out.push_back(byte | ((value != 0) ? 0x80 : 0));
    } while (value != 0);
}

bool ReadIvf(const std::vector<uint8_t>& file, std::vector<Packet>& packets, std::string& error) {
    if ((file.size() < 32) || (memcmp(file.data(), "DKIF", 4) != 0)) {
        error = "not an IVF file";
        return false;
    }
    size_t pos = file[6] | (file[7] << 8);
    while (pos + 12 <= file.size()) {
        const uint32_t size = ReadLe32(&file[pos]);
        const int64_t pts =
            static_cast<int64_t>(ReadLe32(&file[pos + 4]) | (static_cast<uint64_t>(ReadLe32(&file[pos + 8])) << 32));
        pos += 12;
        if (size > file.size() - pos) {
            error = "truncated IVF frame";
            return false;
        }
        packets.push_back({std::vector<uint8_t>(file.begin() + pos, file.begin() + pos + size), pts});
        pos += size;
    }
    return true;
}

// Low-overhead OBU stream (AV1 5.2): every OBU carries its size. A temporal delimiter starts a
// temporal unit, which is one parser packet.
bool ReadObuStream(const std::vector<uint8_t>& file, std::vector<Packet>& packets, std::string& error) {
    std::vector<uint8_t> unit;
    size_t pos = 0;
    while (pos < file.size()) {
        const uint8_t header = file[pos];
        const uint32_t type = (header >> 3) & 0xf;
        const size_t headerSize = 1 + ((header >> 2) & 1);
        if ((header & 0x02) == 0) {
            error = "OBU without obu_has_size_field in a low-overhead stream";
            return false;
        }
        uint64_t payloadSize = 0;
        const size_t lebSize = ReadLeb128(file, pos + headerSize, payloadSize);
        if ((lebSize == 0) || (payloadSize > file.size() - (pos + headerSize + lebSize))) {
            error = "truncated OBU";
            return false;
        }
        const size_t obuSize = headerSize + lebSize + payloadSize;
        if ((type == 2) && !unit.empty()) {  // OBU_TEMPORAL_DELIMITER
            packets.push_back({unit, static_cast<int64_t>(packets.size())});
            unit.clear();
        }
        unit.insert(unit.end(), file.begin() + pos, file.begin() + pos + obuSize);
        pos += obuSize;
    }
    if (!unit.empty()) {
        packets.push_back({unit, static_cast<int64_t>(packets.size())});
    }
    return true;
}

// Annex B stream (AV1 B.2): temporal_unit(size) of frame_unit(size) of obu_length + OBU. Each OBU
// is rewritten with obu_has_size_field set, so every temporal unit becomes one low-overhead packet.
bool ReadAnnexB(const std::vector<uint8_t>& file, std::vector<Packet>& packets, std::string& error) {
    size_t pos = 0;
    while (pos < file.size()) {
        uint64_t unitSize = 0;
        size_t lebSize = ReadLeb128(file, pos, unitSize);
        if ((lebSize == 0) || (unitSize > file.size() - (pos + lebSize))) {
            error = "truncated temporal unit";
            return false;
        }
        pos += lebSize;
        const size_t unitEnd = pos + unitSize;
        std::vector<uint8_t> unit;
        while (pos < unitEnd) {
            uint64_t frameUnitSize = 0;
            lebSize = ReadLeb128(file, pos, frameUnitSize);
            if ((lebSize == 0) || (frameUnitSize > unitEnd - (pos + lebSize))) {
                error = "truncated frame unit";
                return false;
            }
            pos += lebSize;
            const size_t frameUnitEnd = pos + frameUnitSize;
            while (pos < frameUnitEnd) {
                uint64_t obuLength = 0;
                lebSize = ReadLeb128(file, pos, obuLength);
                if ((lebSize == 0) || (obuLength == 0) || (obuLength > frameUnitEnd - (pos + lebSize))) {
                    error = "truncated OBU";
                    return false;
                }
                pos += lebSize;
                const uint8_t header = file[pos];
                const size_t headerSize = 1 + ((header >> 2) & 1);
                if (obuLength < headerSize) {
                    error = "OBU shorter than its header";
                    return false;
                }
                if ((header & 0x02) != 0) {
                    unit.insert(unit.end(), file.begin() + pos, file.begin() + pos + obuLength);
                } else {
                    unit.push_back(header | 0x02);
                    unit.insert(unit.end(), file.begin() + pos + 1, file.begin() + pos + headerSize);
                    WriteLeb128(unit, obuLength - headerSize);
                    unit.insert(unit.end(), file.begin() + pos + headerSize, file.begin() + pos + obuLength);
                }
                pos += obuLength;
            }
        }
        packets.push_back({unit, static_cast<int64_t>(packets.size())});
    }
    return true;
}

void WriteLe(std::vector<uint8_t>& out, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

// Writes the packets as an IVF file, so that another tool (ffmpeg's trace_headers) reads exactly the
// temporal units the parser was given.
bool WriteIvf(const char* path, const std::vector<Packet>& packets) {
    std::vector<uint8_t> header;
    header.insert(header.end(), {'D', 'K', 'I', 'F'});
    WriteLe(header, 0, 2);   // version
    WriteLe(header, 32, 2);  // header size
    header.insert(header.end(), {'A', 'V', '0', '1'});
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

void ParserLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

int Usage(const char* program) {
    fprintf(stderr,
            "Usage: %s --codec av1 --format ivf|obu|annexb -i <stream> [-o <trace>]\n"
            "          [--offset-alignment N] [--size-alignment N] [--pool N] [--write-ivf <file>]\n"
            "Runs NvVideoParser over the stream with no Vulkan device and writes a canonical trace of\n"
            "every parser callback. obu is the low-overhead OBU format, annexb is AV1 Annex B.\n"
            "--write-ivf also writes the temporal units the parser is given as an IVF file.\n",
            program);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const char* codecName = nullptr;
    const char* format = nullptr;
    const char* input = nullptr;
    const char* output = nullptr;
    const char* ivfOutput = nullptr;
    uint32_t offsetAlignment = 256;
    uint32_t sizeAlignment = 256;
    uint32_t poolSize = 32;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            return Usage(argv[0]);
        }
        if (arg == "--codec") {
            codecName = argv[++i];
        } else if (arg == "--format") {
            format = argv[++i];
        } else if (arg == "-i") {
            input = argv[++i];
        } else if (arg == "-o") {
            output = argv[++i];
        } else if (arg == "--offset-alignment") {
            offsetAlignment = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--size-alignment") {
            sizeAlignment = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--pool") {
            poolSize = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--write-ivf") {
            ivfOutput = argv[++i];
        } else {
            return Usage(argv[0]);
        }
    }
    if ((codecName == nullptr) || (format == nullptr) || (input == nullptr)) {
        return Usage(argv[0]);
    }
    if (strcmp(codecName, "av1") != 0) {
        fprintf(stderr, "Only --codec av1 is traced.\n");
        return 2;
    }

    std::vector<uint8_t> file;
    if (!ReadFile(input, file)) {
        fprintf(stderr, "Cannot read %s\n", input);
        return 1;
    }
    std::vector<Packet> packets;
    std::string error;
    bool read = false;
    if (strcmp(format, "ivf") == 0) {
        read = ReadIvf(file, packets, error);
    } else if (strcmp(format, "obu") == 0) {
        read = ReadObuStream(file, packets, error);
    } else if (strcmp(format, "annexb") == 0) {
        read = ReadAnnexB(file, packets, error);
    } else {
        return Usage(argv[0]);
    }
    if (!read) {
        fprintf(stderr, "%s: %s\n", input, error.c_str());
        return 1;
    }
    if ((ivfOutput != nullptr) && !WriteIvf(ivfOutput, packets)) {
        fprintf(stderr, "Cannot write %s\n", ivfOutput);
        return 1;
    }

    FILE* out = stdout;
    if (output != nullptr) {
        out = fopen(output, "w");
        if (out == nullptr) {
            fprintf(stderr, "Cannot write %s\n", output);
            return 1;
        }
    }

    Line("trace")
        .Num("version", 1)
        .Str("codec", codecName)
        .Str("format", format)
        .Num("packets", static_cast<int64_t>(packets.size()))
        .Num("offsetAlignment", offsetAlignment)
        .Num("sizeAlignment", sizeAlignment)
        .Num("pool", poolSize)
        .Emit(out);

    const VkVideoCodecOperationFlagBitsKHR codec = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    static const VkExtensionProperties av1StdExtensionVersion = {VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_EXTENSION_NAME,
                                                                 VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_SPEC_VERSION};
    // The client outlives the parser: the parser releases its pictures when it is destroyed.
    TraceClient client(out, codec, poolSize);
    int status = 0;
    {
        VkParserInitDecodeParameters params = {};
        params.interfaceVersion = NV_VULKAN_VIDEO_PARSER_API_VERSION;
        params.pClient = &client;
        params.defaultMinBufferSize = 2 * 1024 * 1024;
        params.bufferOffsetAlignment = offsetAlignment;
        params.bufferSizeAlignment = sizeAlignment;
        params.referenceClockRate = 0;
        params.errorThreshold = 0;

        VkSharedBaseObj<VulkanVideoDecodeParser> parser;
        const VkResult result = CreateVulkanVideoDecodeParser(codec, &av1StdExtensionVersion, ParserLog, 0, &params, parser);
        if ((result != VK_SUCCESS) || !parser) {
            fprintf(stderr, "CreateVulkanVideoDecodeParser failed: %d\n", static_cast<int>(result));
            status = 1;
        } else {
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
                if (!parser->ParseByteStream(&packet, &parsedBytes)) {
                    Line("pkt-error").Num("index", static_cast<int64_t>(i)).Emit(out);
                }
            }
            VkParserBitstreamPacket endOfStream = {};
            endOfStream.bEOS = 1;
            parser->ParseByteStream(&endOfStream);
            Line("eos")
                .Num("pictures", static_cast<int64_t>(client.Decoded()))
                .Num("displayed", static_cast<int64_t>(client.Displayed()))
                .Emit(out);
        }
    }

    if (out != stdout) {
        fclose(out);
    }
    return status;
}
