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

// vk-video-parser-crafted runs NvVideoParser over streams built in code, each reaching one parser
// path, and checks what the parser hands its client: the parameter sets, sequences and pictures of
// the trace TraceClient writes. A case that runs past its CTest timeout hangs the parser; one built
// with AddressSanitizer or UBSan also fails on any report. See ../README.md.
//
//   vk-video-parser-crafted --list
//   vk-video-parser-crafted --case <name> [--trace <file>] [--write <file>]
//   vk-video-parser-crafted --expect-cases <name>...

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "BitWriter.h"
#include "TraceClient.h"

using namespace parser_trace;

namespace {

// What a case gives the parser, an H.264 or H.265 byte stream or packets for AV1 and VP9, and what
// its last run gave back.
struct CaseContext {
    std::vector<uint8_t> byteStream;
    std::vector<Packet> packets;
    std::string codec;
    std::string trace;
};

// A parser run's trace, as the lines TraceClient writes.
class Trace {
   public:
    explicit Trace(const std::string& text) {
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);) {
            m_lines.push_back(line);
        }
    }

    // The lines that start with tag followed by a space or the end of the line.
    std::vector<const std::string*> Lines(const std::string& tag) const {
        std::vector<const std::string*> found;
        for (const std::string& line : m_lines) {
            if ((line.compare(0, tag.size(), tag) == 0) && ((line.size() == tag.size()) || (line[tag.size()] == ' '))) {
                found.push_back(&line);
            }
        }
        return found;
    }

    // The value of key in the occurrence-th line with the tag, or "(absent)".
    std::string Field(const std::string& tag, const std::string& key, size_t occurrence = 0) const {
        const std::vector<const std::string*> lines = Lines(tag);
        if (occurrence >= lines.size()) {
            return "(absent)";
        }
        const std::string token = " " + key + "=";
        const size_t begin = lines[occurrence]->find(token);
        if (begin == std::string::npos) {
            return "(absent)";
        }
        const size_t valueBegin = begin + token.size();
        return lines[occurrence]->substr(valueBegin, lines[occurrence]->find(' ', valueBegin) - valueBegin);
    }

    std::string Text() const {
        std::string text;
        for (const std::string& line : m_lines) {
            text += line + "\n";
        }
        return text;
    }

   private:
    std::vector<std::string> m_lines;
};

// The checks of one case; a case passes when none failed.
class Checks {
   public:
    explicit Checks(const Trace& trace) : m_trace(trace) {}

    void Count(const std::string& tag, size_t expected) {
        const size_t count = m_trace.Lines(tag).size();
        if (count != expected) {
            m_failures.push_back("expected " + std::to_string(expected) + " '" + tag + "' lines, the trace has " +
                                 std::to_string(count));
        }
    }

    void Field(const std::string& tag, const std::string& key, const std::string& expected, size_t occurrence = 0) {
        const std::string value = m_trace.Field(tag, key, occurrence);
        if (value != expected) {
            m_failures.push_back("'" + tag + "' line " + std::to_string(occurrence) + ": " + key + "=" + value + ", expected " +
                                 expected);
        }
    }

    const std::vector<std::string>& Failures() const { return m_failures; }

   private:
    const Trace& m_trace;
    std::vector<std::string> m_failures;
};

// Runs the parser over the case's stream with a TraceClient that has poolSize pictures, and returns
// the trace, which the context keeps.
Trace Run(CaseContext& stream, const char* codecName, uint32_t poolSize = 32) {
    const CodecInfo* codec = FindCodec(codecName);
    FILE* out = tmpfile();
    stream.codec = codecName;
    if ((codec == nullptr) || (out == nullptr)) {
        stream.trace = "run-failed\n";
        return Trace(stream.trace);
    }
    {
        // The client outlives the parser: the parser releases its pictures when it is destroyed.
        std::unique_ptr<TraceClientIf> client = CreateTraceClient(out, codec->operation, poolSize);
        VkSharedBaseObj<VulkanVideoDecodeParser> parser;
        if ((CreateParser(*codec, *client, 256, 256, parser) != VK_SUCCESS) || !parser) {
            Line("create-failed").Emit(out);
        } else {
            if (!stream.byteStream.empty()) {
                ParseByteStreamPartially(*parser, stream.byteStream, out);
            }
            ParsePackets(*parser, stream.packets, out);
            EndStream(*parser, *client, out);
        }
    }
    std::string text;
    rewind(out);
    char chunk[4096];
    for (size_t count = 0; (count = fread(chunk, 1, sizeof(chunk), out)) > 0;) {
        text.append(chunk, count);
    }
    fclose(out);
    stream.trace = text;
    return Trace(text);
}

void Append(std::vector<uint8_t>& stream, const std::vector<uint8_t>& bytes) {
    stream.insert(stream.end(), bytes.begin(), bytes.end());
}

// ---- H.264 (Rec. ITU-T H.264 7.3) ----

// A Baseline SPS for a 64x64 picture with POC type 0.
struct H264Sps {
    uint64_t id = 0;  // ue(v) codes past 32 bits: 2^32 takes 32 leading zeros
    uint32_t profileIdc = 66;
    uint32_t levelIdc = 30;
    uint32_t log2MaxFrameNumMinus4 = 0;
    uint32_t log2MaxPocLsbMinus4 = 0;
    uint32_t maxNumRefFrames = 1;
    uint32_t widthInMbsMinus1 = 3;
    uint32_t heightInMapUnitsMinus1 = 3;
};

std::vector<uint8_t> H264SpsNal(const H264Sps& sps) {
    BitWriter w;
    w.U(8, sps.profileIdc).U(8, 0).U(8, sps.levelIdc).Ue(sps.id);       // constraint flags and reserved bits 0, sps id
    w.Ue(sps.log2MaxFrameNumMinus4).Ue(0).Ue(sps.log2MaxPocLsbMinus4);  // pic_order_cnt_type 0
    w.Ue(sps.maxNumRefFrames).Flag(false);                              // gaps_in_frame_num_value_allowed_flag
    w.Ue(sps.widthInMbsMinus1).Ue(sps.heightInMapUnitsMinus1);
    w.Flag(true).Flag(true).Flag(false).Flag(false);  // frame_mbs_only, direct_8x8_inference, cropping, VUI
    return AnnexBNal({0x67}, w.TrailingBits().Data());
}

// A CAVLC PPS with one slice group and deblocking control.
std::vector<uint8_t> H264PpsNal() {
    BitWriter w;
    w.Ue(0).Ue(0).Flag(false).Flag(false);  // pps id, sps id, entropy_coding_mode_flag, bottom_field_pic_order...
    w.Ue(0);                                // num_slice_groups_minus1
    w.Ue(0).Ue(0).Flag(false).U(2, 0);      // num_ref_idx defaults, weighted_pred_flag, weighted_bipred_idc
    w.Se(0).Se(0).Se(0);                    // pic_init_qp_minus26, pic_init_qs_minus26, chroma_qp_index_offset
    w.Flag(true).Flag(false).Flag(false);   // deblocking control, constrained intra, redundant_pic_cnt
    return AnnexBNal({0x68}, w.TrailingBits().Data());
}

// A PPS with two slice groups of slice_group_map_type 6, which codes a slice group for each of
// picSizeInMapUnitsMinus1 + 1 map units; slice_group_id is written for the first idsWritten of them.
std::vector<uint8_t> H264PpsExplicitSliceGroupsNal(uint64_t picSizeInMapUnitsMinus1, uint32_t idsWritten) {
    BitWriter w;
    w.Ue(0).Ue(0).Flag(false).Flag(false);      // pps id, sps id, entropy_coding_mode_flag, bottom_field_pic_order...
    w.Ue(1).Ue(6).Ue(picSizeInMapUnitsMinus1);  // num_slice_groups_minus1, slice_group_map_type, map units
    for (uint32_t i = 0; i < idsWritten; i++) {
        w.U(1, i & 1);  // slice_group_id[i]
    }
    w.Ue(0).Ue(0).Flag(false).U(2, 0);     // num_ref_idx defaults, weighted_pred_flag, weighted_bipred_idc
    w.Se(0).Se(0).Se(0);                   // pic_init_qp_minus26, pic_init_qs_minus26, chroma_qp_index_offset
    w.Flag(true).Flag(false).Flag(false);  // deblocking control, constrained intra, redundant_pic_cnt
    return AnnexBNal({0x68}, w.TrailingBits().Data());
}

// A PPS with numSliceGroupsMinus1 + 1 slice groups of slice_group_map_type 0, whose map codes a run
// length for each of mapGroups groups; a valid PPS codes one for each slice group, and none when
// there is one slice group.
std::vector<uint8_t> H264PpsSliceGroupsNal(uint32_t ppsId, uint64_t numSliceGroupsMinus1, uint32_t mapGroups) {
    BitWriter w;
    w.Ue(ppsId).Ue(0).Flag(false).Flag(false);  // pps id, sps id, entropy_coding_mode_flag, bottom_field_pic_order...
    w.Ue(numSliceGroupsMinus1);
    if (mapGroups > 0) {
        w.Ue(0);  // slice_group_map_type
        for (uint32_t i = 0; i < mapGroups; i++) {
            w.Ue(0);  // run_length_minus1[i]
        }
    }
    w.Ue(0).Ue(0).Flag(false).U(2, 0);     // num_ref_idx defaults, weighted_pred_flag, weighted_bipred_idc
    w.Se(0).Se(0).Se(0);                   // pic_init_qp_minus26, pic_init_qs_minus26, chroma_qp_index_offset
    w.Flag(true).Flag(false).Flag(false);  // deblocking control, constrained intra, redundant_pic_cnt
    return AnnexBNal({0x68}, w.TrailingBits().Data());
}

// An IDR I slice of the whole picture; its slice data is filler the parser does not read.
std::vector<uint8_t> H264IdrSliceNal(const H264Sps& sps) {
    BitWriter w;
    w.Ue(0).Ue(7).Ue(0);                          // first_mb_in_slice, slice_type I, pps id
    w.U(sps.log2MaxFrameNumMinus4 + 4, 0).Ue(0);  // frame_num, idr_pic_id
    w.U(sps.log2MaxPocLsbMinus4 + 4, 0);          // pic_order_cnt_lsb
    w.Flag(false).Flag(false);                    // no_output_of_prior_pics_flag, long_term_reference_flag
    w.Se(0).Ue(1);                                // slice_qp_delta, disable_deblocking_filter_idc
    w.TrailingBits().Bytes({0x55, 0xAA, 0x55, 0xAA});
    return AnnexBNal({0x65}, w.Data());
}

// ---- H.265 (Rec. ITU-T H.265 7.3) ----

// profile_tier_level(1, maxSubLayersMinus1): Main profile, level 2, no sub-layer profile or level.
void H265ProfileTierLevel(BitWriter& w, uint32_t maxSubLayersMinus1 = 0) {
    w.U(2, 0).Flag(false).U(5, 1).U(32, 0x60000000);  // profile space, tier, Main, compatibility flags 1 and 2
    w.Flag(true).Flag(false).Flag(false).Flag(true);  // progressive, interlaced, non-packed, frame-only
    w.Zeros(44).U(8, 60);                             // reserved bits, general_level_idc
    if (maxSubLayersMinus1 > 0) {
        w.U(16, 0);  // sub_layer_profile_present_flag, sub_layer_level_present_flag, reserved_zero_2bits
    }
}

// The VPS fields the cases vary. The default VPS has one layer set, of layer 0.
struct H265Vps {
    uint32_t id = 0;
    uint32_t maxSubLayersMinus1 = 0;
    uint32_t numLayerSetsMinus1 = 0;
    bool endAfterMaxLayerId = false;  // the NAL unit ends after vps_max_layer_id, zero-padded to a byte
};

std::vector<uint8_t> H265VpsNal(const H265Vps& vps = H265Vps()) {
    BitWriter w;
    w.U(4, vps.id).Flag(true).Flag(true).U(6, 0).U(3, vps.maxSubLayersMinus1).Flag(true);  // id, base layer, layers, sub-layers
    w.U(16, 0xffff);
    H265ProfileTierLevel(w, vps.maxSubLayersMinus1);
    w.Flag(true);  // vps_sub_layer_ordering_info_present_flag
    for (uint32_t i = 0; i <= vps.maxSubLayersMinus1; i++) {
        w.Ue(1).Ue(0).Ue(0);  // max_dec_pic_buffering_minus1 1, max_num_reorder_pics, max_latency_increase_plus1
    }
    w.U(6, 0);  // vps_max_layer_id
    if (vps.endAfterMaxLayerId) {
        return AnnexBNal({0x40, 0x01}, w.AlignZero().Data());
    }
    w.Ue(vps.numLayerSetsMinus1);
    for (uint32_t i = 1; i <= vps.numLayerSetsMinus1; i++) {
        w.Flag(true);  // layer_id_included_flag[i][0]
    }
    w.Flag(false).Flag(false);  // vps_timing_info_present_flag, vps_extension_flag
    return AnnexBNal({0x40, 0x01}, w.TrailingBits().Data());
}

// A VPS of two independent layers, 0 and 1, with numLayerSetsMinus1 + 1 layer sets of both layers,
// and an extension (Rec. ITU-T H.265 F.7.3.2.1.1) with one multiview scalability dimension that
// adds numAddLayerSets layer sets of layer 1. The extension ends after the additional layer sets.
std::vector<uint8_t> H265VpsExtensionNal(uint32_t numLayerSetsMinus1, uint32_t numAddLayerSets) {
    BitWriter w;
    w.U(4, 0).Flag(true).Flag(true).U(6, 1).U(3, 0).Flag(true).U(16, 0xffff);  // id, base layer, 2 layers, sub-layers
    H265ProfileTierLevel(w);
    w.Flag(true).Ue(1).Ue(0).Ue(0);    // sub_layer_ordering_info: max_dec_pic_buffering_minus1 1
    w.U(6, 1).Ue(numLayerSetsMinus1);  // vps_max_layer_id 1, vps_num_layer_sets_minus1
    for (uint32_t i = 1; i <= numLayerSetsMinus1; i++) {
        w.Flag(true).Flag(true);  // layer_id_included_flag[i][0] and [i][1]
    }
    w.Flag(false).Flag(true).AlignOne();  // vps_timing_info_present_flag, vps_extension_flag, alignment
    w.U(8, 60).Flag(false);               // profile_tier_level(0, 0): general_level_idc; splitting_flag
    w.U(16, 0x4000).U(3, 0);              // scalability_mask_flag[1] (multiview); dimension_id_len_minus1[0]
    w.Flag(false).U(1, 1);                // vps_nuh_layer_id_present_flag; dimension_id[1][0]
    w.U(4, 0).Flag(false);                // view_id_len; direct_dependency_flag[1][0]: layer 1 is independent
    w.Ue(numAddLayerSets);
    for (uint32_t i = 0; i < numAddLayerSets; i++) {
        w.U(1, 1);  // highest_layer_idx_plus1[i][1]
    }
    return AnnexBNal({0x40, 0x01}, w.TrailingBits().Data());
}

// scaling_list_data() (Rec. ITU-T H.265 7.3.4) of predicted lists, one for each delta, in order:
// scaling_list_pred_mode_flag 0, the delta as scaling_list_pred_matrix_id_delta. 20 deltas code
// every list; fewer end the structure early.
void H265PredictedScalingLists(BitWriter& w, const std::vector<uint64_t>& deltas) {
    for (const uint64_t delta : deltas) {
        w.Flag(false).Ue(delta);
    }
}

// The SPS fields the cases vary. The default SPS is for a 64x64 picture with 16x16 coding tree
// blocks and one short-term RPS: one negative picture, used.
struct H265Sps {
    uint32_t id = 0;
    bool scalingListEnabled = false;  // scaling_list_enabled_flag
    // with scalingListEnabled, the lists of H265PredictedScalingLists; none for the default lists
    std::vector<uint64_t> scalingListDeltas;
    // num_short_term_ref_pic_sets and the sets
    std::function<void(BitWriter&)> refPicSets;
    // num_long_term_ref_pics_sps and the pictures, after a long_term_ref_pics_present_flag of 1
    std::function<void(BitWriter&)> longTermRefPics;
    // vui_parameters(), after a vui_parameters_present_flag of 1
    std::function<void(BitWriter&)> vui;
};

std::vector<uint8_t> H265SpsNal(const H265Sps& sps = H265Sps()) {
    BitWriter w;
    w.U(4, 0).U(3, 0).Flag(true);  // vps id, sps_max_sub_layers_minus1, sps_temporal_id_nesting_flag
    H265ProfileTierLevel(w);
    w.Ue(sps.id).Ue(1).Ue(64).Ue(64).Flag(false);  // sps id, 4:2:0, width, height, conformance window
    w.Ue(0).Ue(0).Ue(4);                           // bit depths, log2_max_pic_order_cnt_lsb_minus4
    w.Flag(true).Ue(1).Ue(0).Ue(0);                // sub_layer_ordering_info
    w.Ue(0).Ue(1).Ue(0).Ue(2).Ue(0).Ue(0);         // coding and transform block sizes, hierarchy depths
    w.Flag(sps.scalingListEnabled);
    if (sps.scalingListEnabled) {
        w.Flag(!sps.scalingListDeltas.empty());  // sps_scaling_list_data_present_flag
        H265PredictedScalingLists(w, sps.scalingListDeltas);
    }
    w.Flag(false).Flag(false).Flag(false);  // AMP, SAO, PCM
    if (sps.refPicSets) {
        sps.refPicSets(w);
    } else {
        w.Ue(1).Ue(1).Ue(0).Ue(0).Flag(true);  // one st_ref_pic_set: one negative picture, used
    }
    w.Flag(static_cast<bool>(sps.longTermRefPics));  // long_term_ref_pics_present_flag
    if (sps.longTermRefPics) {
        sps.longTermRefPics(w);
    }
    w.Flag(false).Flag(false);           // temporal MVP, strong intra smoothing
    w.Flag(static_cast<bool>(sps.vui));  // vui_parameters_present_flag
    if (sps.vui) {
        sps.vui(w);
    }
    w.Flag(false);  // sps_extension_present_flag
    return AnnexBNal({0x42, 0x01}, w.TrailingBits().Data());
}

// A PPS of SPS 0. With scalingListDeltas it sets pps_scaling_list_data_present_flag and codes the
// lists of H265PredictedScalingLists.
std::vector<uint8_t> H265PpsNal(uint32_t id = 0, const std::vector<uint64_t>& scalingListDeltas = {}) {
    BitWriter w;
    w.Ue(id).Ue(0).Flag(false).Flag(false).U(3, 0);  // ids, dependent slices, output flag, extra slice header bits
    w.Flag(false).Flag(false).Ue(0).Ue(0).Se(0);     // sign hiding, cabac init, num_ref_idx defaults, init_qp
    w.Flag(false).Flag(false).Flag(false);           // constrained intra, transform skip, cu_qp_delta
    w.Se(0).Se(0).Flag(false);                       // cb and cr offsets, slice chroma offsets
    w.Flag(false).Flag(false).Flag(false);           // weighted pred and bipred, transquant bypass
    w.Flag(false).Flag(false).Flag(false);           // tiles, entropy sync, loop filter across slices
    w.Flag(false).Flag(!scalingListDeltas.empty());  // deblocking control, pps_scaling_list_data_present_flag
    H265PredictedScalingLists(w, scalingListDeltas);
    w.Flag(false).Ue(0).Flag(false).Flag(false);  // list modification, parallel merge level, header extension, extensions
    return AnnexBNal({0x44, 0x01}, w.TrailingBits().Data());
}

// An IDR_W_RADL I slice; its slice data is filler the parser does not read.
std::vector<uint8_t> H265IdrSliceNal() {
    BitWriter w;
    w.Flag(true).Flag(false).Ue(0).Ue(2).Se(0);        // first slice, no_output_of_prior_pics, pps id, I, slice_qp_delta
    w.TrailingBits().Bytes({0x55, 0xAA, 0x55, 0xAA});  // byte_alignment(), slice data
    return AnnexBNal({0x26, 0x01}, w.Data());
}

// A TRAIL_R P slice with picture order count LSB pocLsb and the SPS's first reference picture set;
// its slice data is filler the parser does not read. longTerm, for an SPS with long-term pictures,
// writes the slice's, from num_long_term_sps to the last delta_poc_msb_present_flag.
std::vector<uint8_t> H265TrailSliceNal(uint32_t pocLsb, const std::function<void(BitWriter&)>& longTerm = nullptr) {
    BitWriter w;
    w.Flag(true).Ue(0).Ue(1);   // first slice, pps id, slice_type P
    w.U(8, pocLsb).Flag(true);  // slice_pic_order_cnt_lsb, short_term_ref_pic_set_sps_flag
    if (longTerm) {
        longTerm(w);
    }
    w.Flag(false).Ue(0).Se(0);                         // num_ref_idx_active_override_flag, merge candidates, slice_qp_delta
    w.TrailingBits().Bytes({0x55, 0xAA, 0x55, 0xAA});  // byte_alignment(), slice data
    return AnnexBNal({0x02, 0x01}, w.Data());
}

// ---- AV1 (AV1 bitstream specification 5.5) ----

// A temporal delimiter and a sequence header for 64x64 Main 8-bit 4:2:0 with timing info; the
// timing info ends with numTicksPerPictureMinus1 as uvlc().
std::vector<uint8_t> Av1SequenceHeaderUnit(uint64_t numTicksPerPictureMinus1, bool filmGrain = false) {
    BitWriter w;
    w.U(3, 0).Flag(false).Flag(false).Flag(true);                    // seq_profile, still_picture, reduced header, timing info
    w.U(32, 1).U(32, 30).Flag(true).Uvlc(numTicksPerPictureMinus1);  // timing_info(), equal_picture_interval
    w.Flag(false).Flag(false).U(5, 0).U(12, 0).U(5, 0);              // decoder model, display delay, one operating point
    w.U(4, 5).U(4, 5).U(6, 63).U(6, 63);                             // frame size bits and maxima
    w.Flag(false).Flag(false).Flag(false).Flag(false);               // frame ids, 128x128 superblocks, filter intra, edge filter
    w.Flag(false).Flag(false).Flag(false).Flag(false);               // interintra, masked compound, warped motion, dual filter
    w.Flag(true).Flag(false).Flag(false);                            // order hints, jnt_comp, ref_frame_mvs
    w.Flag(true).Flag(true).U(3, 6);                                 // screen content and integer mv SELECT, order_hint_bits 7
    w.Flag(false).Flag(false).Flag(false);                           // superres, CDEF, restoration
    w.Flag(false).Flag(false).Flag(false).Flag(false);               // color_config: 8-bit, not mono, no description, limited
    w.U(2, 0).Flag(false).Flag(filmGrain);                           // chroma_sample_position, separate_uv_delta_q, film grain
    std::vector<uint8_t> unit = Obu(2, {});                          // OBU_TEMPORAL_DELIMITER
    Append(unit, Obu(1, w.TrailingBits().Data()));                   // OBU_SEQUENCE_HEADER
    return unit;
}

// A shown 64x64 key frame for the sequence header of Av1SequenceHeaderUnit with film grain, as an
// OBU_FRAME of one tile, whose data is filler. Its film grain has numYPoints luma scaling points.
std::vector<uint8_t> Av1KeyFrameObu(uint32_t numYPoints) {
    BitWriter w;
    w.Flag(false).U(2, 0).Flag(true);   // show_existing_frame, KEY_FRAME, show_frame
    w.Flag(false).Flag(false);          // disable_cdf_update, allow_screen_content_tools
    w.Flag(false).U(7, 0).Flag(false);  // frame_size_override_flag, order_hint, render_and_frame_size_different
    w.Flag(true).Flag(true);            // disable_frame_end_update_cdf, uniform_tile_spacing_flag
    w.U(8, 60).Flag(false).Flag(false).Flag(false).Flag(false);  // base_q_idx, no delta_q, no qmatrix
    w.Flag(false).Flag(false);                                   // segmentation_enabled, delta_q_present
    w.U(6, 0).U(6, 0).U(3, 0).Flag(false);                       // loop filter levels, sharpness, deltas
    w.Flag(false).Flag(false);                                   // tx_mode_select, reduced_tx_set
    w.Flag(true).U(16, 1234).U(4, numYPoints);                   // film_grain_params(): apply_grain, grain_seed, num_y_points
    for (uint32_t i = 0; i < numYPoints; i++) {
        w.U(8, 16 * i).U(8, 64);  // point_y_value, increasing; point_y_scaling
    }
    w.Flag(false).U(4, 0).U(4, 0);      // chroma_scaling_from_luma, num_cb_points, num_cr_points
    w.U(2, 0).U(2, 0).U(2, 0).U(2, 0);  // grain_scaling_minus_8, ar_coeff_lag, ar_coeff_shift_minus_6, grain_scale_shift
    w.Flag(false).Flag(false);          // overlap_flag, clip_to_restricted_range
    w.AlignZero().Bytes({0x55, 0xAA, 0x55, 0xAA});  // byte_alignment(), the tile
    return Obu(6, w.Data());                        // OBU_FRAME
}

// A temporal delimiter and a sequence header whose timing info ends after equal_picture_interval in
// zero bytes, so num_ticks_per_picture_minus1 reads uvlc() over zeros to the end of the packet.
std::vector<uint8_t> Av1TruncatedTimingUnit() {
    BitWriter w;
    w.U(3, 0).Flag(false).Flag(false).Flag(true);  // seq_profile, still_picture, reduced header, timing info
    w.U(32, 1).U(32, 30).Flag(true);               // timing_info(): equal_picture_interval
    w.AlignZero().Bytes({0, 0, 0, 0, 0, 0, 0, 0});
    std::vector<uint8_t> unit = Obu(2, {});  // OBU_TEMPORAL_DELIMITER
    Append(unit, Obu(1, w.Data()));          // OBU_SEQUENCE_HEADER
    return unit;
}

// ---- VP9 (VP9 bitstream specification 6.2) ----

// A shown profile 0 key frame of 64x64; its compressed header and tile are filler.
std::vector<uint8_t> Vp9KeyFrame() {
    const std::vector<uint8_t> compressedHeader = {0x55, 0xAA, 0x55, 0xAA};
    BitWriter w;
    w.U(2, 2).Flag(false).Flag(false);                 // frame_marker, profile 0
    w.Flag(false).Flag(false).Flag(true).Flag(false);  // show_existing_frame, KEY_FRAME, show_frame, error res
    w.U(8, 0x49).U(8, 0x83).U(8, 0x42);                // frame_sync_code
    w.U(3, 1).Flag(false);                             // color_space BT.601, color_range
    w.U(16, 63).U(16, 63).Flag(false);                 // frame size, render size equal
    w.Flag(true).Flag(true).U(2, 0);                   // refresh_frame_context, parallel mode, context 0
    w.U(6, 10).U(3, 0).Flag(false);                    // loop filter level and sharpness, no deltas
    w.U(8, 60).Flag(false).Flag(false).Flag(false);    // base_q_idx, no delta_q
    w.Flag(false).Flag(false);                         // segmentation off, tile_rows_log2 0
    w.U(16, compressedHeader.size()).AlignZero();      // header_size_in_bytes, trailing_bits
    w.Bytes(compressedHeader).Bytes({0x55, 0xAA, 0x55, 0xAA});
    return w.Data();
}

struct CraftedCase {
    const char* name;
    std::function<std::vector<std::string>(CaseContext&)> run;
};

const std::vector<CraftedCase>& Cases() {
    static const std::vector<CraftedCase> cases = {
        // Proves the H.264 builders: one SPS, one PPS and an IDR picture that the parser takes.
        {"H264CraftedStream",
         [](CaseContext& stream) {
             const H264Sps sps;
             Append(stream.byteStream, H264SpsNal(sps));
             Append(stream.byteStream, H264PpsNal());
             Append(stream.byteStream, H264IdrSliceNal(sps));
             const Trace trace = Run(stream, "h264");
             Checks checks(trace);
             checks.Count("sps", 1);
             checks.Count("pps", 1);
             checks.Count("pic", 1);
             checks.Field("sps", "pic_width_in_mbs_minus1", "3");
             checks.Field("sps", "max_num_ref_frames", "1");
             checks.Field("  h264", "frame_num", "0");
             return checks.Failures();
         }},
        // an explicit slice group map is coded for pic_size_in_map_units_minus1 + 1 map units,
        // which the PPS reads with ue(); 0xFFFFFFFF never ends the loop over them. The SPS's picture
        // has 16 map units, so 15 is the value 7.4.2.2 requires.
        {"H264PpsSliceGroupMapUnits",
         [](CaseContext& stream) {
             const H264Sps sps;
             Append(stream.byteStream, H264SpsNal(sps));
             Append(stream.byteStream, H264PpsExplicitSliceGroupsNal(0xFFFFFFFFu, 0));
             Append(stream.byteStream, H264PpsExplicitSliceGroupsNal(16, 17));
             Append(stream.byteStream, H264PpsExplicitSliceGroupsNal(15, 16));
             const Trace trace = Run(stream, "h264");
             Checks checks(trace);
             checks.Count("sps", 1);
             checks.Count("pps", 1);
             return checks.Failures();
         }},
        // ue() with 31 and with 32 leading zero bits, in seq_parameter_set_id. 31 code 2^31 - 1 to
        // 2^32 - 2 (9.1), past int; 32 code 2^32 - 1 or more, which 32 bits do not hold, so 2^32 must
        // not wrap to the valid id 0. Neither id is valid; the third SPS's, 1, is.
        {"H264UeLongCodes",
         [](CaseContext& stream) {
             H264Sps sps;
             sps.id = 0xFFFFFFFEu;
             Append(stream.byteStream, H264SpsNal(sps));
             sps.id = 0x100000000u;
             Append(stream.byteStream, H264SpsNal(sps));
             sps.id = 1;
             Append(stream.byteStream, H264SpsNal(sps));
             const Trace trace = Run(stream, "h264");
             Checks checks(trace);
             checks.Count("sps", 1);
             checks.Field("sps", "seq_parameter_set_id", "1");
             return checks.Failures();
         }},
        // num_slice_groups_minus1 is at most 7 (Rec. ITU-T H.264 A.2.1), and the PPS's check tested
        // the field before it was set. 256 also narrowed to 0 before the check: that PPS codes no
        // map, as one slice group would.
        {"H264PpsSliceGroups",
         [](CaseContext& stream) {
             Append(stream.byteStream, H264SpsNal(H264Sps()));
             Append(stream.byteStream, H264PpsSliceGroupsNal(0, 7, 8));
             Append(stream.byteStream, H264PpsSliceGroupsNal(1, 8, 9));
             Append(stream.byteStream, H264PpsSliceGroupsNal(2, 256, 0));
             const Trace trace = Run(stream, "h264");
             Checks checks(trace);
             checks.Count("pps", 1);
             checks.Field("pps", "pic_parameter_set_id", "0");
             return checks.Failures();
         }},
        // Proves the H.265 builders: one VPS, SPS and PPS and an IDR picture that the parser takes.
        {"H265CraftedStream",
         [](CaseContext& stream) {
             Append(stream.byteStream, H265VpsNal());
             Append(stream.byteStream, H265SpsNal());
             Append(stream.byteStream, H265PpsNal());
             Append(stream.byteStream, H265IdrSliceNal());
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("vps", 1);
             checks.Count("sps", 1);
             checks.Count("pps", 1);
             checks.Count("pic", 1);
             checks.Field("sps", "pic_width_in_luma_samples", "64");
             checks.Field("sps", "num_short_term_ref_pic_sets", "1");
             checks.Field("  strps", "num_negative_pics", "1");
             checks.Field("  hevc", "IdrPicFlag", "1");
             return checks.Failures();
         }},
        // a VPS that ends after vps_max_layer_id reads vps_num_layer_sets_minus1 from the zeros past
        // its end, 0xFFFFFFFF, whose count of layer sets wraps to 0 and passes a check on the count.
        {"H265VpsLayerSetCountWrap",
         [](CaseContext& stream) {
             H265Vps vps;
             vps.endAfterMaxLayerId = true;
             Append(stream.byteStream, H265VpsNal(vps));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("vps", 0);
             return checks.Failures();
         }},
        // vps_num_layer_sets_minus1 is at most 1023 (Rec. ITU-T H.265 7.4.3.1).
        {"H265VpsLayerSetCountLimit",
         [](CaseContext& stream) {
             H265Vps largest;
             largest.numLayerSetsMinus1 = 1023;
             H265Vps beyond;
             beyond.id = 1;
             beyond.numLayerSetsMinus1 = 1024;
             Append(stream.byteStream, H265VpsNal(largest));
             Append(stream.byteStream, H265VpsNal(beyond));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("vps", 1);
             checks.Field("vps", "vps_video_parameter_set_id", "0");
             return checks.Failures();
         }},
        // the additional layer sets of a VPS extension follow its vps_num_layer_sets, so 1024
        // layer sets and one more index row 1024 of arrays of 1024 rows.
        {"H265VpsExtensionLayerSetIndex",
         [](CaseContext& stream) {
             Append(stream.byteStream, H265VpsExtensionNal(1023, 1));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("vps", 0);
             return checks.Failures();
         }},
        // a P slice whose reference picture set has 9 pictures before it, used; the Std picture
        // information holds 8 per list (STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE).
        {"H265RefPicSetCurrBefore9",
         [](CaseContext& stream) {
             Append(stream.byteStream, H265VpsNal());
             Append(stream.byteStream, H265SpsNal({.refPicSets = [](BitWriter& w) {
                        w.Ue(1).Ue(9).Ue(0);  // one set: 9 negative pictures
                        for (int i = 0; i < 9; i++) {
                            w.Ue(0).Flag(true);  // delta_poc_s0_minus1, used_by_curr_pic_s0_flag
                        }
                    }}));
             Append(stream.byteStream, H265PpsNal());
             Append(stream.byteStream, H265IdrSliceNal());
             Append(stream.byteStream, H265TrailSliceNal(1));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("pic", 1);
             return checks.Failures();
         }},
        // num_long_term_sps is at most num_long_term_ref_pics_sps (Rec. ITU-T H.265 7.4.7.1), and
        // the slice's check tested the field before it was set. The SPS lists one long-term picture,
        // which no picture uses; the first P slice names it once, the second twice.
        {"H265SliceNumLongTermSps",
         [](CaseContext& stream) {
             const auto longTermSps = [](uint32_t count) {
                 return [count](BitWriter& w) {
                     w.Ue(count).Ue(0);  // num_long_term_sps, num_long_term_pics
                     for (uint32_t i = 0; i < count; i++) {
                         w.Flag(false);  // delta_poc_msb_present_flag[i]
                     }
                 };
             };
             Append(stream.byteStream, H265VpsNal());
             Append(stream.byteStream, H265SpsNal({.longTermRefPics = [](BitWriter& w) {
                        w.Ue(1).U(8, 100).Flag(false);  // lt_ref_pic_poc_lsb_sps 100, used_by_curr_pic_lt_sps_flag
                    }}));
             Append(stream.byteStream, H265PpsNal());
             Append(stream.byteStream, H265IdrSliceNal());
             Append(stream.byteStream, H265TrailSliceNal(1, longTermSps(1)));
             Append(stream.byteStream, H265TrailSliceNal(2, longTermSps(2)));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("pic", 2);
             checks.Field("  hevc", "CurrPicOrderCntVal", "1", 1);
             return checks.Failures();
         }},
        // scaling_list_pred_matrix_id_delta is at most matrixId (Rec. ITU-T H.265 7.4.5), so that
        // refMatrixId, matrixId - delta, names this list or one before it. SPS 1 and PPS 2 predict
        // list 0 with a delta of 2^32 - 1, -1 as an int. PPS 1 predicts 4x4 list 1 from list -1, which
        // the reader refuses; the PPS's other fields follow that list, where a PPS that ignores the
        // refusal reads them.
        {"H265ScalingListRefMatrix",
         [](CaseContext& stream) {
             std::vector<uint64_t> valid(20, 0);  // the default lists
             valid[1] = 1;                        // 4x4 list 1 from list 0
             std::vector<uint64_t> wrapped(20, 0);
             wrapped[0] = 0xFFFFFFFFu;
             Append(stream.byteStream, H265VpsNal());
             Append(stream.byteStream, H265SpsNal({.scalingListEnabled = true}));
             Append(stream.byteStream, H265SpsNal({.id = 1, .scalingListEnabled = true, .scalingListDeltas = wrapped}));
             Append(stream.byteStream, H265PpsNal(0, valid));
             Append(stream.byteStream, H265PpsNal(1, {0, 2}));
             Append(stream.byteStream, H265PpsNal(2, wrapped));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("sps", 1);
             checks.Field("sps", "sps_seq_parameter_set_id", "0");
             checks.Count("pps", 1);
             checks.Field("pps", "pps_pic_parameter_set_id", "0");
             return checks.Failures();
         }},
        // vps_max_sub_layers_minus1 7, one past the 7 sub-layers H.265 allows (7.4.3.1) and the
        // Std VPS holds.
        {"H265VpsSubLayers8",
         [](CaseContext& stream) {
             H265Vps vps;
             vps.maxSubLayersMinus1 = 7;
             Append(stream.byteStream, H265VpsNal(vps));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("vps", 0);
             return checks.Failures();
         }},
        // an SPS whose VUI HRD parameters have cpb_cnt_minus1 32, one past the 32 CPB entries
        // H.265 allows (E.3.2) and the Std sub-layer HRD parameters hold.
        {"H265SpsHrdCpbCount",
         [](CaseContext& stream) {
             Append(stream.byteStream, H265VpsNal());
             Append(stream.byteStream, H265SpsNal({.vui = [](BitWriter& w) {
                        w.Zeros(8);                                    // aspect ratio ... default display window
                        w.Flag(true).U(32, 1).U(32, 30).Flag(false);   // vui_timing_info_present_flag, timing
                        w.Flag(true);                                  // vui_hrd_parameters_present_flag
                        w.Flag(true).Flag(false).Flag(false);          // nal and vcl HRD, sub_pic_hrd_params_present_flag
                        w.U(4, 0).U(4, 0).U(5, 23).U(5, 23).U(5, 23);  // scales, delay lengths
                        w.Flag(true).Ue(0).Ue(32);  // fixed_pic_rate_general_flag, elemental_duration, cpb_cnt_minus1
                        for (int i = 0; i <= 32; i++) {
                            w.Ue(0).Ue(0).Flag(false);  // bit_rate_value_minus1, cpb_size_value_minus1, cbr_flag
                        }
                        w.Flag(false);  // bitstream_restriction_flag
                    }}));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("sps", 0);
             return checks.Failures();
         }},
        // num_short_term_ref_pic_sets 257, which a uint8_t holds as 1. 64 is the most H.265
        // allows (7.4.3.2.1) and the SPS holds.
        {"H265SpsNumStRps",
         [](CaseContext& stream) {
             Append(stream.byteStream, H265VpsNal());
             Append(stream.byteStream, H265SpsNal({.refPicSets = [](BitWriter& w) {
                        w.Ue(257).Ue(0).Ue(0);  // num_short_term_ref_pic_sets; set 0: no pictures
                        for (int i = 1; i < 257; i++) {
                            w.Flag(false).Ue(0).Ue(0);  // set i: not predicted, no pictures
                        }
                    }}));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("sps", 0);
             return checks.Failures();
         }},
        // a set predicted from one of 16 negative pictures with DeltaRPS -1 derives 17 negative
        // pictures (7.4.8), one more than the arrays they are written to hold.
        {"H265InterRpsOverflow",
         [](CaseContext& stream) {
             Append(stream.byteStream, H265VpsNal());
             Append(stream.byteStream, H265SpsNal({.refPicSets = [](BitWriter& w) {
                        w.Ue(2).Ue(16).Ue(0);  // num_short_term_ref_pic_sets; set 0: 16 negative pictures
                        for (int i = 0; i < 16; i++) {
                            w.Ue(0).Flag(true);  // delta_poc_s0_minus1, used_by_curr_pic_s0_flag
                        }
                        w.Flag(true).Flag(true).Ue(0);  // set 1: inter_ref_pic_set_prediction_flag, DeltaRPS -1
                        for (int j = 0; j <= 16; j++) {
                            w.Flag(true);  // used_by_curr_pic_flag[j]
                        }
                    }}));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("sps", 0);
             return checks.Failures();
         }},
        // Proves the AV1 sequence header builder: every field after the timing info, uvlc() included,
        // reads as written. The parser keeps the timing info out of the Std sequence header.
        {"Av1CraftedSequenceHeader",
         [](CaseContext& stream) {
             stream.packets.push_back({Av1SequenceHeaderUnit(5), 0});
             const Trace trace = Run(stream, "av1");
             Checks checks(trace);
             checks.Count("sps", 1);
             checks.Field("sps", "max_frame_width_minus_1", "63");
             checks.Field("sps", "order_hint_bits_minus_1", "6");
             checks.Field("sps", "seq_force_screen_content_tools", "2");
             checks.Field("sps", "seq_force_integer_mv", "2");
             checks.Field("sps", "flags", "enable_order_hint");
             return checks.Failures();
         }},
        // uvlc() over the zeros past the end of a packet meets no one bit.
        {"Av1UvlcTruncated",
         [](CaseContext& stream) {
             stream.packets.push_back({Av1TruncatedTimingUnit(), 0});
             const Trace trace = Run(stream, "av1");
             Checks checks(trace);
             checks.Count("sps", 0);
             return checks.Failures();
         }},
        // a key frame, then a tile group alone, before any sequence header, which frame headers and
        // pictures are parsed with (AV1 7.5); then a sequence header and the key frame, which the
        // parser takes.
        {"Av1FrameBeforeSequenceHeader",
         [](CaseContext& stream) {
             std::vector<uint8_t> unit = Obu(2, {});  // OBU_TEMPORAL_DELIMITER
             Append(unit, Av1KeyFrameObu(1));
             stream.packets.push_back({unit, 0});
             unit = Obu(2, {});
             Append(unit, Obu(4, {0x55, 0xAA, 0x55, 0xAA}));  // OBU_TILE_GROUP: one tile, of filler
             stream.packets.push_back({unit, 1});
             unit = Av1SequenceHeaderUnit(0, true);
             Append(unit, Av1KeyFrameObu(1));
             stream.packets.push_back({unit, 2});
             const Trace trace = Run(stream, "av1");
             Checks checks(trace);
             checks.Count("pic", 1);
             return checks.Failures();
         }},
        // after a key frame, a temporal unit of a tile group alone, which no frame header of its
        // temporal unit precedes (AV1 7.5).
        {"Av1TileGroupWithoutFrameHeader",
         [](CaseContext& stream) {
             std::vector<uint8_t> unit = Av1SequenceHeaderUnit(0, true);
             Append(unit, Av1KeyFrameObu(1));
             stream.packets.push_back({unit, 0});
             unit = Obu(2, {});
             Append(unit, Obu(4, {0x55, 0xAA, 0x55, 0xAA}));
             stream.packets.push_back({unit, 1});
             const Trace trace = Run(stream, "av1");
             Checks checks(trace);
             checks.Count("pic", 1);
             return checks.Failures();
         }},
        // Proves the AV1 key frame builder, and the limit: 14 luma scaling points are the most
        // StdVideoAV1FilmGrain holds (STD_VIDEO_AV1_MAX_NUM_Y_POINTS), and 15 fit the 4-bit syntax
        // element.
        {"Av1FilmGrainPointCount",
         [](CaseContext& stream) {
             std::vector<uint8_t> unit = Av1SequenceHeaderUnit(0, true);
             Append(unit, Av1KeyFrameObu(14));
             stream.packets.push_back({unit, 0});
             unit = Obu(2, {});
             Append(unit, Av1KeyFrameObu(15));
             stream.packets.push_back({unit, 1});
             const Trace trace = Run(stream, "av1");
             Checks checks(trace);
             checks.Count("pic", 1);
             checks.Field("  fg", "num_y_points", "14");
             return checks.Failures();
         }},
        // a client with one picture, which the first key frame keeps in all eight reference
        // buffers, has none for the second.
        {"Vp9PicturePoolExhausted",
         [](CaseContext& stream) {
             stream.packets.push_back({Vp9KeyFrame(), 0});
             stream.packets.push_back({Vp9KeyFrame(), 1});
             const Trace trace = Run(stream, "vp9", 1);
             Checks checks(trace);
             checks.Count("pic", 1);
             checks.Count("alloc-failed", 1);
             return checks.Failures();
         }},
        // Proves the VP9 key frame builder.
        {"Vp9CraftedKeyFrame",
         [](CaseContext& stream) {
             stream.packets.push_back({Vp9KeyFrame(), 0});
             const Trace trace = Run(stream, "vp9");
             Checks checks(trace);
             checks.Count("pic", 1);
             checks.Field("  vp9", "FrameWidth", "64");
             checks.Field("  std", "base_q_idx", "60");
             checks.Field("  lf", "loop_filter_level", "10");
             return checks.Failures();
         }},
    };
    return cases;
}

bool WriteFile(const char* path, const std::string& text) {
    FILE* file = fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
    return (fclose(file) == 0) && ok;
}

bool WriteStream(const char* path, const CaseContext& stream) {
    if (stream.packets.empty()) {
        return WriteFile(path, std::string(stream.byteStream.begin(), stream.byteStream.end()));
    }
    return WriteIvf(path, (stream.codec == "vp9") ? "VP90" : "AV01", stream.packets);
}

int Usage(const char* program) {
    fprintf(stderr,
            "Usage: %s --list\n"
            "       %s --case <name> [--trace <file>] [--write <file>]\n"
            "       %s --expect-cases <name>...\n"
            "Runs one crafted parser case: exit 0 when its checks hold, 1 when one fails.\n"
            "--trace writes the parser trace, --write the crafted stream (H.264 and H.265 as a byte\n"
            "stream, AV1 and VP9 as IVF). --expect-cases fails unless the names are exactly the cases.\n",
            program, program, program);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if ((argc == 2) && (strcmp(argv[1], "--list") == 0)) {
        for (const CraftedCase& crafted : Cases()) {
            printf("%s\n", crafted.name);
        }
        return 0;
    }
    if ((argc >= 2) && (strcmp(argv[1], "--expect-cases") == 0)) {
        std::set<std::string> expected(argv + 2, argv + argc);
        std::set<std::string> actual;
        for (const CraftedCase& crafted : Cases()) {
            actual.insert(crafted.name);
        }
        for (const std::string& name : actual) {
            if (expected.count(name) == 0) {
                fprintf(stderr, "case %s is not registered with CTest\n", name.c_str());
            }
        }
        for (const std::string& name : expected) {
            if (actual.count(name) == 0) {
                fprintf(stderr, "CTest names %s, which is not a case\n", name.c_str());
            }
        }
        return (expected == actual) ? 0 : 1;
    }
    const char* caseName = nullptr;
    const char* tracePath = nullptr;
    const char* writePath = nullptr;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--case") == 0) {
            caseName = argv[i + 1];
        } else if (strcmp(argv[i], "--trace") == 0) {
            tracePath = argv[i + 1];
        } else if (strcmp(argv[i], "--write") == 0) {
            writePath = argv[i + 1];
        } else {
            return Usage(argv[0]);
        }
    }
    if ((caseName == nullptr) || ((argc % 2) == 0)) {
        return Usage(argv[0]);
    }
    for (const CraftedCase& crafted : Cases()) {
        if (strcmp(crafted.name, caseName) != 0) {
            continue;
        }
        CaseContext stream;
        const std::vector<std::string> failures = crafted.run(stream);
        if ((writePath != nullptr) && !WriteStream(writePath, stream)) {
            fprintf(stderr, "Cannot write %s\n", writePath);
        }
        if ((tracePath != nullptr) && !WriteFile(tracePath, stream.trace)) {
            fprintf(stderr, "Cannot write %s\n", tracePath);
        }
        for (const std::string& failure : failures) {
            fprintf(stderr, "%s: %s\n", caseName, failure.c_str());
        }
        printf("%s %s\n", failures.empty() ? "PASS" : "FAIL", caseName);
        return failures.empty() ? 0 : 1;
    }
    fprintf(stderr, "No case %s\n", caseName);
    return 2;
}
