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
    w.U(8, sps.profileIdc).U(8, 0).U(8, sps.levelIdc).Ue(0);            // constraint flags and reserved bits 0, sps id 0
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

// profile_tier_level(1, 0): Main profile, level 2.
void H265ProfileTierLevel(BitWriter& w) {
    w.U(2, 0).Flag(false).U(5, 1).U(32, 0x60000000);  // profile space, tier, Main, compatibility flags 1 and 2
    w.Flag(true).Flag(false).Flag(false).Flag(true);  // progressive, interlaced, non-packed, frame-only
    w.Zeros(44).U(8, 60);                             // reserved bits, general_level_idc
}

// The VPS fields the cases vary. The default VPS has one layer set, of layer 0.
struct H265Vps {
    uint32_t id = 0;
    uint32_t numLayerSetsMinus1 = 0;
    bool endAfterMaxLayerId = false;  // the NAL unit ends after vps_max_layer_id, zero-padded to a byte
};

std::vector<uint8_t> H265VpsNal(const H265Vps& vps = H265Vps()) {
    BitWriter w;
    w.U(4, vps.id).Flag(true).Flag(true).U(6, 0).U(3, 0).Flag(true).U(16, 0xffff);  // id, base layer, layers, sub-layers
    H265ProfileTierLevel(w);
    w.Flag(true).Ue(1).Ue(0).Ue(0);  // sub_layer_ordering_info: max_dec_pic_buffering_minus1 1
    w.U(6, 0);                       // vps_max_layer_id
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

// An SPS for a 64x64 picture with 16x16 coding tree blocks and one short-term RPS: one negative
// picture, used.
std::vector<uint8_t> H265SpsNal() {
    BitWriter w;
    w.U(4, 0).U(3, 0).Flag(true);  // vps id, sps_max_sub_layers_minus1, sps_temporal_id_nesting_flag
    H265ProfileTierLevel(w);
    w.Ue(0).Ue(1).Ue(64).Ue(64).Flag(false);            // sps id, 4:2:0, width, height, conformance window
    w.Ue(0).Ue(0).Ue(4);                                // bit depths, log2_max_pic_order_cnt_lsb_minus4
    w.Flag(true).Ue(1).Ue(0).Ue(0);                     // sub_layer_ordering_info
    w.Ue(0).Ue(1).Ue(0).Ue(2).Ue(0).Ue(0);              // coding and transform block sizes, hierarchy depths
    w.Flag(false).Flag(false).Flag(false).Flag(false);  // scaling lists, AMP, SAO, PCM
    w.Ue(1).Ue(1).Ue(0).Ue(0).Flag(true);               // one st_ref_pic_set: one negative picture, used
    w.Flag(false).Flag(false).Flag(false);              // long-term pictures, temporal MVP, strong intra smoothing
    w.Flag(false).Flag(false);                          // VUI, extensions
    return AnnexBNal({0x42, 0x01}, w.TrailingBits().Data());
}

std::vector<uint8_t> H265PpsNal() {
    BitWriter w;
    w.Ue(0).Ue(0).Flag(false).Flag(false).U(3, 0);  // ids, dependent slices, output flag, extra slice header bits
    w.Flag(false).Flag(false).Ue(0).Ue(0).Se(0);    // sign hiding, cabac init, num_ref_idx defaults, init_qp
    w.Flag(false).Flag(false).Flag(false);          // constrained intra, transform skip, cu_qp_delta
    w.Se(0).Se(0).Flag(false);                      // cb and cr offsets, slice chroma offsets
    w.Flag(false).Flag(false).Flag(false);          // weighted pred and bipred, transquant bypass
    w.Flag(false).Flag(false).Flag(false);          // tiles, entropy sync, loop filter across slices
    w.Flag(false).Flag(false).Flag(false);          // deblocking control, scaling lists, list modification
    w.Ue(0).Flag(false).Flag(false);                // parallel merge level, slice header extension, extensions
    return AnnexBNal({0x44, 0x01}, w.TrailingBits().Data());
}

// An IDR_W_RADL I slice; its slice data is filler the parser does not read.
std::vector<uint8_t> H265IdrSliceNal() {
    BitWriter w;
    w.Flag(true).Flag(false).Ue(0).Ue(2).Se(0);        // first slice, no_output_of_prior_pics, pps id, I, slice_qp_delta
    w.TrailingBits().Bytes({0x55, 0xAA, 0x55, 0xAA});  // byte_alignment(), slice data
    return AnnexBNal({0x26, 0x01}, w.Data());
}

// ---- AV1 (AV1 bitstream specification 5.5) ----

// A temporal delimiter and a sequence header for 64x64 Main 8-bit 4:2:0 with timing info; the
// timing info ends with numTicksPerPictureMinus1 as uvlc().
std::vector<uint8_t> Av1SequenceHeaderUnit(uint64_t numTicksPerPictureMinus1) {
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
    w.U(2, 0).Flag(false).Flag(false);                               // chroma_sample_position, separate_uv_delta_q, film grain
    std::vector<uint8_t> unit = Obu(2, {});                          // OBU_TEMPORAL_DELIMITER
    Append(unit, Obu(1, w.TrailingBits().Data()));                   // OBU_SEQUENCE_HEADER
    return unit;
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
