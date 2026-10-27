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

#include <cstdarg>
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

// A Baseline SPS for a 64x64 picture with POC type 0. The ue(v) fields are 64 bits, for codes past 32.
// High profile (100) adds chroma_format_idc 1 and the bit depths.
struct H264Sps {
    uint64_t id = 0;
    uint32_t profileIdc = 66;
    uint32_t levelIdc = 30;
    uint64_t bitDepthLumaMinus8 = 0;
    uint64_t log2MaxFrameNumMinus4 = 0;
    uint64_t picOrderCntType = 0;
    uint64_t log2MaxPocLsbMinus4 = 0;
    uint64_t maxNumRefFrames = 1;
    uint32_t widthInMbsMinus1 = 3;
    uint32_t heightInMapUnitsMinus1 = 3;
};

std::vector<uint8_t> H264SpsNal(const H264Sps& sps) {
    BitWriter w;
    w.U(8, sps.profileIdc).U(8, 0).U(8, sps.levelIdc).Ue(sps.id);  // constraint flags and reserved bits 0, sps id
    if (sps.profileIdc == 100) {
        w.Ue(1).Ue(sps.bitDepthLumaMinus8).Ue(0).Flag(false).Flag(false);  // 4:2:0, bit depths, no bypass, no matrices
    }
    w.Ue(sps.log2MaxFrameNumMinus4).Ue(sps.picOrderCntType);
    if (sps.picOrderCntType == 0) {
        w.Ue(sps.log2MaxPocLsbMinus4);
    }
    w.Ue(sps.maxNumRefFrames).Flag(false);  // gaps_in_frame_num_value_allowed_flag
    w.Ue(sps.widthInMbsMinus1).Ue(sps.heightInMapUnitsMinus1);
    w.Flag(true).Flag(true).Flag(false).Flag(false);  // frame_mbs_only, direct_8x8_inference, cropping, VUI
    return AnnexBNal({0x67}, w.TrailingBits().Data());
}

// The PPS fields the cases vary: a CAVLC PPS of SPS 0 with one slice group and deblocking control. With
// bottomFieldPicOrder a frame's slices code delta_pic_order_cnt_bottom.
struct H264Pps {
    uint32_t id = 0;
    bool bottomFieldPicOrder = false;
    uint64_t numRefIdxL0DefaultActiveMinus1 = 0;
    int64_t picInitQpMinus26 = 0;
    int64_t chromaQpIndexOffset = 0;
};

std::vector<uint8_t> H264PpsNal(const H264Pps& pps = H264Pps()) {
    BitWriter w;
    w.Ue(pps.id).Ue(0).Flag(false).Flag(
        pps.bottomFieldPicOrder);                    // pps id, sps id, entropy_coding_mode_flag, bottom_field_pic_order...
    w.Ue(0);                                         // num_slice_groups_minus1
    w.Ue(pps.numRefIdxL0DefaultActiveMinus1).Ue(0);  // num_ref_idx defaults
    w.Flag(false).U(2, 0);                           // weighted_pred_flag, weighted_bipred_idc
    w.Se(pps.picInitQpMinus26)
        .Se(0)
        .Se(pps.chromaQpIndexOffset);      // pic_init_qp_minus26, pic_init_qs_minus26, chroma_qp_index_offset
    w.Flag(true).Flag(false).Flag(false);  // deblocking control, constrained intra, redundant_pic_cnt
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
std::vector<uint8_t> H264PpsSliceGroupsNal(uint32_t ppsId, uint64_t numSliceGroupsMinus1, uint32_t mapGroups,
                                           uint64_t mapType = 0) {
    BitWriter w;
    w.Ue(ppsId).Ue(0).Flag(false).Flag(false);  // pps id, sps id, entropy_coding_mode_flag, bottom_field_pic_order...
    w.Ue(numSliceGroupsMinus1);
    if (mapGroups > 0) {
        w.Ue(mapType);  // slice_group_map_type, coding run lengths when it reads as 0
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

// An I slice of the whole picture, of a reference picture, an IDR picture when idr, with frame_num
// frameNum, pic_order_cnt_lsb pocLsb and, for a PPS with bottomFieldPicOrder, delta_pic_order_cnt_bottom
// deltaBottom. Its slice data is filler the parser does not read.
std::vector<uint8_t> H264ReferenceISliceNal(const H264Sps& sps, bool idr, uint32_t frameNum, uint32_t pocLsb, int64_t deltaBottom) {
    BitWriter w;
    w.Ue(0).Ue(7).Ue(0);  // first_mb_in_slice, slice_type I, pps id
    w.U(sps.log2MaxFrameNumMinus4 + 4, frameNum);
    if (idr) {
        w.Ue(0);  // idr_pic_id
    }
    w.U(sps.log2MaxPocLsbMinus4 + 4, pocLsb).Se(deltaBottom);
    if (idr) {
        w.Flag(false).Flag(false);  // no_output_of_prior_pics_flag, long_term_reference_flag
    } else {
        w.Flag(false);  // adaptive_ref_pic_marking_mode_flag
    }
    w.Se(0).Ue(1);  // slice_qp_delta, disable_deblocking_filter_idc
    w.TrailingBits().Bytes({0x55, 0xAA, 0x55, 0xAA});
    return AnnexBNal({static_cast<uint8_t>(idr ? 0x65 : 0x21)}, w.Data());  // nal_ref_idc 3 or 1
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

// A VPS of two independent layers, 0 and layerIdInNuh, with one layer set, of layer 0, and a whole
// extension (Rec. ITU-T H.265 F.7.3.2.1.1) with splitting_flag 1, so that the second layer's
// dimension ids are bit fields of its nuh_layer_id. scalabilityMask holds scalability_mask_flag[0]
// in its most significant of 16 bits, and dimensionIdLenMinus1 the lengths coded: those of every
// dimension but the last.
std::vector<uint8_t> H265VpsSplittingNal(uint32_t id, uint32_t scalabilityMask, const std::vector<uint32_t>& dimensionIdLenMinus1,
                                         uint32_t layerIdInNuh) {
    BitWriter w;
    w.U(4, id).Flag(true).Flag(true).U(6, 1).U(3, 0).Flag(true).U(16, 0xffff);  // id, base layer, 2 layers, sub-layers
    H265ProfileTierLevel(w);
    w.Flag(true).Ue(1).Ue(0).Ue(0);       // sub_layer_ordering_info: max_dec_pic_buffering_minus1 1
    w.U(6, layerIdInNuh).Ue(0);           // vps_max_layer_id, vps_num_layer_sets_minus1
    w.Flag(false).Flag(true).AlignOne();  // vps_timing_info_present_flag, vps_extension_flag, alignment
    w.U(8, 60).Flag(true);                // profile_tier_level(0, 0): general_level_idc; splitting_flag
    w.U(16, scalabilityMask);
    for (const uint32_t lenMinus1 : dimensionIdLenMinus1) {
        w.U(3, lenMinus1);  // dimension_id_len_minus1[j]
    }
    w.Flag(true).U(6, layerIdInNuh);        // vps_nuh_layer_id_present_flag, layer_id_in_nuh[1]
    w.U(4, 0).Flag(false).Ue(0);            // view_id_len, direct_dependency_flag[1][0], num_add_layer_sets
    w.Flag(false).Flag(false).Flag(false);  // sub-layers max, max_tid_ref_present_flag, default_ref_layers_active_flag
    w.Ue(1).Ue(0);                          // vps_num_profile_tier_level_minus1, vps_num_rep_formats_minus1
    w.U(16, 64).U(16, 64).Flag(true);       // rep_format(): 64x64, chroma_and_bit_depth_vps_present_flag
    w.U(2, 1).U(4, 0).U(4, 0).Flag(false);  // 4:2:0, 8-bit, no conformance window
    w.Flag(false).Flag(false).Flag(false);  // max_one_active_ref_layer_flag, vps_poc_lsb_aligned_flag, poc_lsb_not_present
    return AnnexBNal({0x40, 0x01}, w.TrailingBits().Data());
}

// The SPS fields the cases vary. The default SPS is for a 64x64 picture with 16x16 coding tree
// blocks and one short-term RPS: one negative picture, used.
struct H265Sps {
    uint32_t id = 0;
    uint32_t log2MaxPocLsbMinus4 = 4;
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
    w.Ue(0).Ue(0).Ue(sps.log2MaxPocLsbMinus4);     // bit depths, log2_max_pic_order_cnt_lsb_minus4
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

// The PPS fields the cases vary: a PPS of SPS 0. With scalingListDeltas it sets
// pps_scaling_list_data_present_flag and codes the lists of H265PredictedScalingLists.
struct H265Pps {
    uint32_t id = 0;
    std::vector<uint64_t> scalingListDeltas;
    int64_t initQpMinus26 = 0;
    int64_t cbQpOffset = 0;
};

std::vector<uint8_t> H265PpsNal(const H265Pps& pps = H265Pps()) {
    const std::vector<uint64_t>& scalingListDeltas = pps.scalingListDeltas;
    BitWriter w;
    w.Ue(pps.id).Ue(0).Flag(false).Flag(false).U(3, 0);           // ids, dependent slices, output flag, extra slice header bits
    w.Flag(false).Flag(false).Ue(0).Ue(0).Se(pps.initQpMinus26);  // sign hiding, cabac init, num_ref_idx defaults, init_qp
    w.Flag(false).Flag(false).Flag(false);                        // constrained intra, transform skip, cu_qp_delta
    w.Se(pps.cbQpOffset).Se(0).Flag(false);                       // cb and cr offsets, slice chroma offsets
    w.Flag(false).Flag(false).Flag(false);                        // weighted pred and bipred, transquant bypass
    w.Flag(false).Flag(false).Flag(false);                        // tiles, entropy sync, loop filter across slices
    w.Flag(false).Flag(!scalingListDeltas.empty());               // deblocking control, pps_scaling_list_data_present_flag
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

// A slice segment of an IDR_W_RADL picture that is not its first, in layer nuhLayerId: slice segment
// address 8 of the 16 coding tree blocks. Its slice data is filler the parser does not read.
std::vector<uint8_t> H265IdrSliceSegmentNal(uint32_t nuhLayerId) {
    BitWriter w;
    w.Flag(false).Flag(false).Ue(0).U(4, 8);           // first slice, no_output_of_prior_pics, pps id, address
    w.Ue(2).Se(0);                                     // slice_type I, slice_qp_delta
    w.TrailingBits().Bytes({0x55, 0xAA, 0x55, 0xAA});  // byte_alignment(), slice data
    return AnnexBNal({static_cast<uint8_t>(0x26 | (nuhLayerId >> 5)), static_cast<uint8_t>(((nuhLayerId & 0x1f) << 3) | 1)},
                     w.Data());
}

// A TRAIL_R P slice with picture order count LSB pocLsb and the SPS's first reference picture set;
// its slice data is filler the parser does not read. longTerm, for an SPS with long-term pictures,
// writes the slice's, from num_long_term_sps to the last delta_poc_msb_present_flag. A non-negative
// numRefIdxL0ActiveMinus1 overrides the PPS's count.
std::vector<uint8_t> H265TrailSliceNal(uint32_t pocLsb, const std::function<void(BitWriter&)>& longTerm = nullptr,
                                       int64_t numRefIdxL0ActiveMinus1 = -1) {
    BitWriter w;
    w.Flag(true).Ue(0).Ue(1);   // first slice, pps id, slice_type P
    w.U(8, pocLsb).Flag(true);  // slice_pic_order_cnt_lsb, short_term_ref_pic_set_sps_flag
    if (longTerm) {
        longTerm(w);
    }
    w.Flag(numRefIdxL0ActiveMinus1 >= 0);  // num_ref_idx_active_override_flag
    if (numRefIdxL0ActiveMinus1 >= 0) {
        w.Ue(static_cast<uint64_t>(numRefIdxL0ActiveMinus1));
    }
    w.Ue(0).Se(0);                                     // num_ref_idx_active_override_flag, merge candidates, slice_qp_delta
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
// OBU_FRAME of one tile, whose data is filler. Its film grain has numYPoints luma scaling points,
// yStep apart.
std::vector<uint8_t> Av1KeyFrameObu(uint32_t numYPoints, uint32_t yStep = 16) {
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
        w.U(8, yStep * i).U(8, 64);  // point_y_value, point_y_scaling
    }
    w.Flag(false).U(4, 0).U(4, 0);      // chroma_scaling_from_luma, num_cb_points, num_cr_points
    w.U(2, 0).U(2, 0).U(2, 0).U(2, 0);  // grain_scaling_minus_8, ar_coeff_lag, ar_coeff_shift_minus_6, grain_scale_shift
    w.Flag(false).Flag(false);          // overlap_flag, clip_to_restricted_range
    w.AlignZero().Bytes({0x55, 0xAA, 0x55, 0xAA});  // byte_alignment(), the tile
    return Obu(6, w.Data());                        // OBU_FRAME
}

// A shown 64x64 inter frame for the sequence header of Av1SequenceHeaderUnit with film grain, as an
// OBU_FRAME of one tile, whose data is filler. Its seven references name buffer 0, it takes its size
// from the first of them (found_ref), and it refreshes buffer 0.
std::vector<uint8_t> Av1InterFrameObu() {
    BitWriter w;
    w.Flag(false).U(2, 1).Flag(true);       // show_existing_frame, INTER_FRAME, show_frame
    w.Flag(false).Flag(false).Flag(false);  // error_resilient_mode, disable_cdf_update, allow_screen_content_tools
    w.Flag(true).U(7, 1).U(3, 7);           // frame_size_override_flag, order_hint, primary_ref_frame NONE
    w.U(8, 0x01).Flag(false);               // refresh_frame_flags, frame_refs_short_signaling
    for (int i = 0; i < 7; i++) {
        w.U(3, 0);  // ref_frame_idx[i]
    }
    w.Flag(true);                                                // frame_size_with_refs(): found_ref
    w.Flag(false).Flag(true).Flag(false);                        // allow_high_precision_mv, switchable filter, motion mode
    w.Flag(true).Flag(true);                                     // disable_frame_end_update_cdf, uniform_tile_spacing_flag
    w.U(8, 60).Flag(false).Flag(false).Flag(false).Flag(false);  // base_q_idx, no delta_q, no qmatrix
    w.Flag(false).Flag(false);                                   // segmentation_enabled, delta_q_present
    w.U(6, 0).U(6, 0).U(3, 0).Flag(false);                       // loop filter levels, sharpness, deltas
    w.Flag(false).Flag(false).Flag(false);                       // tx_mode_select, reference_select, reduced_tx_set
    w.Zeros(7).Flag(false);                                      // is_global for each reference, apply_grain
    w.AlignZero().Bytes({0x55, 0xAA, 0x55, 0xAA});               // byte_alignment(), the tile
    return Obu(6, w.Data());                                     // OBU_FRAME
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

// A shown profile 0 key frame of 64x64, with frame_marker frameMarker and frame_sync_code syncCode; its
// compressed header and tile are filler.
std::vector<uint8_t> Vp9KeyFrame(uint32_t syncCode = 0x498342, uint32_t frameMarker = 2) {
    const std::vector<uint8_t> compressedHeader = {0x55, 0xAA, 0x55, 0xAA};
    BitWriter w;
    w.U(2, frameMarker).Flag(false).Flag(false);       // frame_marker, profile 0
    w.Flag(false).Flag(false).Flag(true).Flag(false);  // show_existing_frame, KEY_FRAME, show_frame, error res
    w.U(24, syncCode);                                 // frame_sync_code
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

// A superframe index (VP9 Annex B) for frames of the given sizes, each in two bytes.
std::vector<uint8_t> Vp9SuperframeIndex(const std::vector<uint32_t>& sizes) {
    // superframe_marker, bytes_per_framesize_minus_1 1, frames_in_superframe_minus_1
    const uint8_t marker = static_cast<uint8_t>(0xc0 | (1 << 3) | (sizes.size() - 1));
    std::vector<uint8_t> index = {marker};
    for (const uint32_t size : sizes) {
        index.push_back(static_cast<uint8_t>(size & 0xff));
        index.push_back(static_cast<uint8_t>(size >> 8));
    }
    index.push_back(marker);
    return index;
}

// What a parser logs through CaptureLog, the log function the client gives it.
std::string& CapturedLog() {
    static std::string log;
    return log;
}

void CaptureLog(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    CapturedLog() += text;
}

struct CraftedCase {
    const char* name;
    std::function<std::vector<std::string>(CaseContext&)> run;
};

const std::vector<CraftedCase>& Cases() {
    static const std::vector<CraftedCase> cases = {
        // a codec operation that no parser decodes is refused before a parser exists to initialize.
        {"ParserUnsupportedCodec",
         [](CaseContext&) {
             VkParserInitDecodeParameters params = {};
             params.interfaceVersion = NV_VULKAN_VIDEO_PARSER_API_VERSION;
             VkSharedBaseObj<VulkanVideoDecodeParser> parser;
             const VkResult result = CreateVulkanVideoDecodeParser(VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, nullptr, ParserLog,
                                                                   0, &params, parser);
             std::vector<std::string> failures;
             if (result == VK_SUCCESS) {
                 failures.push_back("an encode operation's parser was created");
             }
             if (parser) {
                 failures.push_back("the failed call returned a parser");
             }
             return failures;
         }},
        // the client's log function gets each message whole. An H.264 parser asked for the next Std
        // version logs the version it supports.
        {"ParserLogForwarding",
         [](CaseContext&) {
             VkExtensionProperties next = {VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME,
                                           VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION + 1};
             VkParserInitDecodeParameters params = {};
             params.interfaceVersion = NV_VULKAN_VIDEO_PARSER_API_VERSION;
             VkSharedBaseObj<VulkanVideoDecodeParser> parser;
             CreateVulkanVideoDecodeParser(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, &next, CaptureLog, 0, &params, parser);
             const std::string expected = "The supported decoder h.264 Codec STD version is version " +
                                          std::to_string(VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION) +
                                          " of " VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME "\n";
             std::vector<std::string> failures;
             if (CapturedLog().find(expected) == std::string::npos) {
                 failures.push_back("the log is \"" + CapturedLog() + "\", without \"" + expected + "\"");
             }
             return failures;
         }},
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
        // BottomFieldOrderCnt is TopFieldOrderCnt + delta_pic_order_cnt_bottom (8-5). After an IDR
        // picture, a picture with pic_order_cnt_lsb 12 of 16 takes PicOrderCntMsb -16 (8-3), so its
        // TopFieldOrderCnt is -4, and its delta_pic_order_cnt_bottom of -2^31 + 1, the least se(v)
        // codes, takes the sum past 32 bits; it is held at -2^31. A delta of -2^31, which no se(v)
        // codes, drops its slice.
        {"H264BottomFieldOrderCntPast32Bits",
         [](CaseContext& stream) {
             const H264Sps sps;
             Append(stream.byteStream, H264SpsNal(sps));
             Append(stream.byteStream, H264PpsNal({.bottomFieldPicOrder = true}));
             Append(stream.byteStream, H264ReferenceISliceNal(sps, true, 0, 0, 0));
             Append(stream.byteStream, H264ReferenceISliceNal(sps, false, 1, 12, -2147483647));
             Append(stream.byteStream, H264ReferenceISliceNal(sps, false, 2, 14, -2147483648LL));
             const Trace trace = Run(stream, "h264");
             Checks checks(trace);
             checks.Count("pic", 2);
             checks.Field("  h264", "CurrFieldOrderCnt", "[-4,-2147483648]", 1);
             return checks.Failures();
         }},
        // an SPS value is range-checked before it is narrowed to its field or cast to an enumeration.
        // SPS 1's log2_max_frame_num_minus4 260 and SPS 2's max_num_ref_frames 257 fit their ranges once
        // narrowed to 8 bits, SPS 3's pic_order_cnt_type 2^32 - 2 is outside its enumeration's values, and
        // SPS 4's bit_depth_luma_minus8 7 passes 6 (7.4.2.1.1). SPS 0 is valid.
        {"H264SpsValuesPastTheirRange",
         [](CaseContext& stream) {
             Append(stream.byteStream, H264SpsNal(H264Sps()));
             Append(stream.byteStream, H264SpsNal({.id = 1, .log2MaxFrameNumMinus4 = 260}));
             Append(stream.byteStream, H264SpsNal({.id = 2, .maxNumRefFrames = 257}));
             Append(stream.byteStream, H264SpsNal({.id = 3, .picOrderCntType = 0xFFFFFFFEu}));
             Append(stream.byteStream, H264SpsNal({.id = 4, .profileIdc = 100, .bitDepthLumaMinus8 = 7}));
             const Trace trace = Run(stream, "h264");
             Checks checks(trace);
             checks.Count("sps", 1);
             checks.Field("sps", "seq_parameter_set_id", "0");
             return checks.Failures();
         }},
        // PPS values past their ranges (7.4.2.2). PPS 1's num_ref_idx_l0_default_active_minus1 256 and
        // PPS 4's slice_group_map_type 65536 narrow to 0, PPS 2's pic_init_qp_minus26 200 narrows to -56,
        // and PPS 3's chroma_qp_index_offset is 13. PPS 0 is valid.
        {"H264PpsValuesPastTheirRange",
         [](CaseContext& stream) {
             Append(stream.byteStream, H264SpsNal(H264Sps()));
             Append(stream.byteStream, H264PpsNal());
             Append(stream.byteStream, H264PpsNal({.id = 1, .numRefIdxL0DefaultActiveMinus1 = 256}));
             Append(stream.byteStream, H264PpsNal({.id = 2, .picInitQpMinus26 = 200}));
             Append(stream.byteStream, H264PpsNal({.id = 3, .chromaQpIndexOffset = 13}));
             Append(stream.byteStream, H264PpsSliceGroupsNal(4, 1, 2, 65536));
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
        // the VCL NAL units of a picture share its nuh_layer_id (Rec. ITU-T H.265 7.4.2.2). A layer-0
        // IDR picture goes on with a slice segment of layer 1, which has no SPS, and ends at the next
        // picture's first slice: it ends with layer 0's parameter sets, and the other layer's segment
        // is dropped.
        {"H265SliceOfAnotherLayer",
         [](CaseContext& stream) {
             Append(stream.byteStream, H265VpsNal());
             Append(stream.byteStream, H265SpsNal());
             Append(stream.byteStream, H265PpsNal());
             Append(stream.byteStream, H265IdrSliceNal());
             Append(stream.byteStream, H265IdrSliceSegmentNal(1));
             Append(stream.byteStream, H265TrailSliceNal(1));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("pic", 2);
             checks.Field("pic", "numSlices", "1", 0);
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
        // with splitting_flag 1 the scalability dimensions split the 6 bits of nuh_layer_id
        // (Rec. ITU-T H.265 F.7.4.3.1.1). VPS 0 splits them 3 and 3 between two dimensions; VPS 1 gives
        // the first dimension 8 bits, and the second starts past the sixth; VPS 2 has no dimension.
        {"H265VpsSplittingDimensions",
         [](CaseContext& stream) {
             Append(stream.byteStream, H265VpsSplittingNal(0, 0x6000, {2}, 9));  // multiview and spatial
             Append(stream.byteStream, H265VpsSplittingNal(1, 0x6000, {7}, 9));
             Append(stream.byteStream, H265VpsSplittingNal(2, 0x0000, {}, 1));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("vps", 2);
             checks.Field("vps", "vps_video_parameter_set_id", "0", 0);
             checks.Field("vps", "vps_video_parameter_set_id", "2", 1);
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
             Append(stream.byteStream, H265PpsNal({.id = 0, .scalingListDeltas = valid}));
             Append(stream.byteStream, H265PpsNal({.id = 1, .scalingListDeltas = {0, 2}}));
             Append(stream.byteStream, H265PpsNal({.id = 2, .scalingListDeltas = wrapped}));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("sps", 1);
             checks.Field("sps", "sps_seq_parameter_set_id", "0");
             checks.Count("pps", 1);
             checks.Field("pps", "pps_pic_parameter_set_id", "0");
             return checks.Failures();
         }},
        // an SPS value is range-checked before it is narrowed to its 8-bit field. SPS 256 narrows to the
        // id of SPS 0, which it would replace; SPS 1's log2_max_pic_order_cnt_lsb_minus4 260 narrows to 4.
        {"H265SpsValuesPastTheirRange",
         [](CaseContext& stream) {
             Append(stream.byteStream, H265VpsNal());
             Append(stream.byteStream, H265SpsNal());
             Append(stream.byteStream, H265SpsNal({.id = 256}));
             Append(stream.byteStream, H265SpsNal({.id = 1, .log2MaxPocLsbMinus4 = 260}));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("sps", 1);
             checks.Field("sps", "sps_seq_parameter_set_id", "0");
             return checks.Failures();
         }},
        // PPS 1's init_qp_minus26 266 narrows to 10, PPS 2's pps_cb_qp_offset 261 to 5, and a P slice's
        // num_ref_idx_l0_active_minus1 256 to 0 (0 to 14, 7.4.7.1). PPS 0 and the slices after are valid.
        {"H265PpsSliceValuesPastTheirRange",
         [](CaseContext& stream) {
             Append(stream.byteStream, H265VpsNal());
             Append(stream.byteStream, H265SpsNal());
             Append(stream.byteStream, H265PpsNal());
             Append(stream.byteStream, H265PpsNal({.id = 1, .initQpMinus26 = 266}));
             Append(stream.byteStream, H265PpsNal({.id = 2, .cbQpOffset = 261}));
             Append(stream.byteStream, H265IdrSliceNal());
             Append(stream.byteStream, H265TrailSliceNal(1, nullptr, 256));
             Append(stream.byteStream, H265TrailSliceNal(2));
             const Trace trace = Run(stream, "h265");
             Checks checks(trace);
             checks.Count("pps", 1);
             checks.Count("pic", 2);
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
        // an inter frame before any key frame takes its size from a reference buffer that holds no
        // frame (found_ref); AV1 6.8.2 requires each reference it names to be valid. Then a key frame
        // and the same inter frame, which the parser takes.
        {"Av1InterFrameEmptyReference",
         [](CaseContext& stream) {
             std::vector<uint8_t> unit = Av1SequenceHeaderUnit(0, true);
             Append(unit, Av1InterFrameObu());
             stream.packets.push_back({unit, 0});
             unit = Obu(2, {});
             Append(unit, Av1KeyFrameObu(1));
             stream.packets.push_back({unit, 1});
             unit = Obu(2, {});
             Append(unit, Av1InterFrameObu());
             stream.packets.push_back({unit, 2});
             const Trace trace = Run(stream, "av1");
             Checks checks(trace);
             checks.Count("pic", 2);
             return checks.Failures();
         }},
        // an OBU must fit in the rest of its temporal unit, and a padding OBU's obu_size of
        // 2^32 - 16 does not; with its header the sum, as an int, was -10, which the check let pass.
        // Then a key frame, which the parser takes.
        {"Av1ObuSizePastPacket",
         [](CaseContext& stream) {
             std::vector<uint8_t> unit = Av1SequenceHeaderUnit(0, true);
             Append(unit, Obu(15, std::vector<uint8_t>(32, 0x55), 0xFFFFFFF0u));  // OBU_PADDING
             stream.packets.push_back({unit, 0});
             unit = Obu(2, {});
             Append(unit, Av1KeyFrameObu(1));
             stream.packets.push_back({unit, 1});
             const Trace trace = Run(stream, "av1");
             Checks checks(trace);
             checks.Count("pic", 1);
             return checks.Failures();
         }},
        // leb128() codes at most 2^32 - 1 (AV1 4.10.5). A padding OBU's obu_size of 2^32 + 3,
        // which 32 bits held as 3, is followed by 3 bytes and a key frame: the temporal unit is
        // dropped, and the next one's key frame taken.
        {"Av1ObuSizeLeb128Past32Bits",
         [](CaseContext& stream) {
             std::vector<uint8_t> unit = Av1SequenceHeaderUnit(0, true);
             Append(unit, Obu(15, {0x55, 0x55, 0x55}, 0x100000003u));  // OBU_PADDING
             Append(unit, Av1KeyFrameObu(1));
             stream.packets.push_back({unit, 0});
             unit = Obu(2, {});
             Append(unit, Av1KeyFrameObu(1));
             stream.packets.push_back({unit, 1});
             const Trace trace = Run(stream, "av1");
             Checks checks(trace);
             checks.Count("pic", 1);
             return checks.Failures();
         }},
        // film grain points increase (AV1 6.8.20); a key frame whose two luma points are both 0 is
        // refused, where a build with assertions stopped and one without took it.
        {"Av1FilmGrainPointsNotIncreasing",
         [](CaseContext& stream) {
             std::vector<uint8_t> unit = Av1SequenceHeaderUnit(0, true);
             Append(unit, Av1KeyFrameObu(2, 0));
             stream.packets.push_back({unit, 0});
             unit = Obu(2, {});
             Append(unit, Av1KeyFrameObu(2));
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
        // a superframe's index gives the size of each of its frames (VP9 Annex B), and they are the
        // frames the parser takes. The first packet is a superframe of two key frames; the second is a
        // key frame before an index of eight frames of no bytes, which leaves the key frame out.
        {"Vp9SuperframeIndex",
         [](CaseContext& stream) {
             const std::vector<uint8_t> frame = Vp9KeyFrame();
             const uint32_t frameSize = static_cast<uint32_t>(frame.size());
             std::vector<uint8_t> superframe = frame;
             Append(superframe, frame);
             Append(superframe, Vp9SuperframeIndex({frameSize, frameSize}));
             stream.packets.push_back({superframe, 0});
             std::vector<uint8_t> uncovered = frame;
             Append(uncovered, Vp9SuperframeIndex(std::vector<uint32_t>(8, 0)));
             stream.packets.push_back({uncovered, 1});
             const Trace trace = Run(stream, "vp9");
             Checks checks(trace);
             checks.Count("pic", 2);
             return checks.Failures();
         }},
        // frame_marker is 2 (VP9 6.2); a frame with another is refused, where a build with assertions
        // stopped.
        {"Vp9BadFrameMarker",
         [](CaseContext& stream) {
             stream.packets.push_back({Vp9KeyFrame(0x498342, 1), 0});
             stream.packets.push_back({Vp9KeyFrame(), 1});
             const Trace trace = Run(stream, "vp9");
             Checks checks(trace);
             checks.Count("pic", 1);
             return checks.Failures();
         }},
        // frame_sync_code is 0x49, 0x83, 0x42 (VP9 6.2); a key frame with another is not a frame.
        {"Vp9BadSyncCode",
         [](CaseContext& stream) {
             stream.packets.push_back({Vp9KeyFrame(0x498343), 0});
             stream.packets.push_back({Vp9KeyFrame(), 1});
             const Trace trace = Run(stream, "vp9");
             Checks checks(trace);
             checks.Count("pic", 1);
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
