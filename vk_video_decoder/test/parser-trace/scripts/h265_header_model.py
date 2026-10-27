#!/usr/bin/env python3
# Copyright 2026 NVIDIA Corporation.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""H.265 header model of the parser trace test.

Reads an H.265 stream's syntax elements from ffmpeg's trace_headers dump and checks the parser's
trace against them, in categories:
  vps, sps, pps      the fields of each parameter set, in order, against their syntax elements
  vps-flags, sps-flags, pps-flags  their flags
  ptl, ptl-level, ptl-flags  the profile, the level (Std level enum) and the flags of a VPS or SPS
                     profile_tier_level()
  vps-dpbm, sps-dpbm the DPB sizes of a VPS or SPS
  sps-strps          each SPS short-term reference picture set; Std fields hold syntax values, and an
                     inter-predicted set its inter syntax
  sps-ltrps          the SPS long-term reference picture POCs and flags
  sps-vui, sps-hrd   the coded VUI and HRD fields of the SPS
  hdr-poc            PicOrderCntVal (H.265 8.3.1)
  hdr-flags          IrapPicFlag, IdrPicFlag, the PPS id, short_term_ref_pic_set_sps_flag
  hdr-rps-bits       NumBitsForShortTermRPSInSlice and NumDeltaPocsOfRefRpsIdx
  rps-counts         NumPocStCurrBefore, NumPocStCurrAfter, NumPocLtCurr, NumPocTotalCurr (8.3.2)
  rps-pocs           the POC of each reference each list names
  picture-count      a picture in the trace or the model but not both (alignment is by POC LSB)
  params-count       a parameter set in the trace or the dump but not both
RASL pictures of a CRA that starts the stream or follows an end of sequence are not decoded; the
model leaves them out.
"""
import difflib

import header_dump as hd

DEMUXER = "hevc"
KINDS = ("Video Parameter Set", "Sequence Parameter Set", "Picture Parameter Set", "Slice Segment Header",
         "End of Sequence", "End of Bitstream")
TITLE = "H.265 syntax elements read by h265_header_model.py"
LEVELS = {30: 0, 60: 1, 63: 2, 90: 3, 93: 4, 120: 5, 123: 6, 150: 7, 153: 8, 156: 9, 180: 10, 183: 11, 186: 12}
PARAM_TAG = {"Video Parameter Set": "vps", "Sequence Parameter Set": "sps", "Picture Parameter Set": "pps"}

VPS_FIELDS = ["vps_video_parameter_set_id", "vps_max_sub_layers_minus1", "vps_num_units_in_tick", "vps_time_scale",
              "vps_num_ticks_poc_diff_one_minus1"]
VPS_FLAGS = ["vps_temporal_id_nesting_flag", "vps_sub_layer_ordering_info_present_flag", "vps_timing_info_present_flag",
             "vps_poc_proportional_to_timing_flag"]
SPS_FIELDS = ["sps_video_parameter_set_id", "sps_max_sub_layers_minus1", "sps_seq_parameter_set_id", "chroma_format_idc",
              "pic_width_in_luma_samples", "pic_height_in_luma_samples", "bit_depth_luma_minus8", "bit_depth_chroma_minus8",
              "log2_max_pic_order_cnt_lsb_minus4", "log2_min_luma_coding_block_size_minus3",
              "log2_diff_max_min_luma_coding_block_size", "log2_min_luma_transform_block_size_minus2",
              "log2_diff_max_min_luma_transform_block_size", "max_transform_hierarchy_depth_inter",
              "max_transform_hierarchy_depth_intra", "num_short_term_ref_pic_sets", "num_long_term_ref_pics_sps",
              "pcm_sample_bit_depth_luma_minus1", "pcm_sample_bit_depth_chroma_minus1",
              "log2_min_pcm_luma_coding_block_size_minus3", "log2_diff_max_min_pcm_luma_coding_block_size",
              "conf_win_left_offset", "conf_win_right_offset", "conf_win_top_offset", "conf_win_bottom_offset"]
SPS_FLAGS = ["sps_temporal_id_nesting_flag", "separate_colour_plane_flag", "conformance_window_flag",
             "sps_sub_layer_ordering_info_present_flag", "scaling_list_enabled_flag", "sps_scaling_list_data_present_flag",
             "amp_enabled_flag", "sample_adaptive_offset_enabled_flag", "pcm_enabled_flag", "pcm_loop_filter_disabled_flag",
             "long_term_ref_pics_present_flag", "sps_temporal_mvp_enabled_flag", "strong_intra_smoothing_enabled_flag",
             "vui_parameters_present_flag", "sps_extension_present_flag", "sps_range_extension_flag",
             "transform_skip_rotation_enabled_flag", "transform_skip_context_enabled_flag", "implicit_rdpcm_enabled_flag",
             "explicit_rdpcm_enabled_flag", "extended_precision_processing_flag", "intra_smoothing_disabled_flag",
             "high_precision_offsets_enabled_flag", "persistent_rice_adaptation_enabled_flag",
             "cabac_bypass_alignment_enabled_flag"]
PTL_FLAGS = ["general_tier_flag", "general_progressive_source_flag", "general_interlaced_source_flag",
             "general_non_packed_constraint_flag", "general_frame_only_constraint_flag"]
VUI_FIELDS = ["aspect_ratio_idc", "sar_width", "sar_height", "video_format", "colour_primaries", "transfer_characteristics",
              "chroma_sample_loc_type_top_field", "chroma_sample_loc_type_bottom_field", "def_disp_win_left_offset",
              "def_disp_win_right_offset", "def_disp_win_top_offset", "def_disp_win_bottom_offset", "vui_num_units_in_tick",
              "vui_time_scale", "vui_num_ticks_poc_diff_one_minus1", "min_spatial_segmentation_idc", "max_bytes_per_pic_denom",
              "max_bits_per_min_cu_denom", "log2_max_mv_length_horizontal", "log2_max_mv_length_vertical"]
VUI_FLAGS = ["aspect_ratio_info_present_flag", "overscan_info_present_flag", "overscan_appropriate_flag",
             "video_signal_type_present_flag", "video_full_range_flag", "colour_description_present_flag",
             "chroma_loc_info_present_flag", "neutral_chroma_indication_flag", "field_seq_flag",
             "frame_field_info_present_flag", "default_display_window_flag", "vui_timing_info_present_flag",
             "vui_poc_proportional_to_timing_flag", "vui_hrd_parameters_present_flag", "bitstream_restriction_flag",
             "tiles_fixed_structure_flag", "motion_vectors_over_pic_boundaries_flag", "restricted_ref_pic_lists_flag"]
HRD_FIELDS = ["tick_divisor_minus2", "du_cpb_removal_delay_increment_length_minus1", "dpb_output_delay_du_length_minus1",
              "bit_rate_scale", "cpb_size_scale", "cpb_size_du_scale", "initial_cpb_removal_delay_length_minus1",
              "au_cpb_removal_delay_length_minus1", "dpb_output_delay_length_minus1"]
PPS_FIELDS = ["pps_pic_parameter_set_id", "pps_seq_parameter_set_id", "num_extra_slice_header_bits",
              "num_ref_idx_l0_default_active_minus1", "num_ref_idx_l1_default_active_minus1", "init_qp_minus26",
              "diff_cu_qp_delta_depth", "pps_cb_qp_offset", "pps_cr_qp_offset", "pps_beta_offset_div2", "pps_tc_offset_div2",
              "log2_parallel_merge_level_minus2", "num_tile_columns_minus1", "num_tile_rows_minus1"]
PPS_FLAGS = ["dependent_slice_segments_enabled_flag", "output_flag_present_flag", "sign_data_hiding_enabled_flag",
             "cabac_init_present_flag", "constrained_intra_pred_flag", "transform_skip_enabled_flag",
             "cu_qp_delta_enabled_flag", "pps_slice_chroma_qp_offsets_present_flag", "weighted_pred_flag",
             "weighted_bipred_flag", "transquant_bypass_enabled_flag", "tiles_enabled_flag",
             "entropy_coding_sync_enabled_flag", "uniform_spacing_flag", "loop_filter_across_tiles_enabled_flag",
             "pps_loop_filter_across_slices_enabled_flag", "deblocking_filter_control_present_flag",
             "deblocking_filter_override_enabled_flag", "pps_deblocking_filter_disabled_flag",
             "pps_scaling_list_data_present_flag", "lists_modification_present_flag",
             "slice_segment_header_extension_present_flag", "pps_extension_present_flag"]


class Cursor:
    """Reads a unit's syntax elements in order, checking each name."""

    def __init__(self, unit, index):
        self.unit = unit
        self.index = index

    def peek(self):
        return self.unit.fields[self.index][1] if self.index < len(self.unit.fields) else None

    def read(self, name):
        if self.peek() != name:
            raise ValueError(f"{self.unit.kind}: expected {name} at element {self.index}, found {self.peek()}")
        value = self.unit.fields[self.index][2]
        self.index += 1
        return value

    def optional(self, name, default=0):
        return self.read(name) if self.peek() == name else default

    def bit_position(self):
        return self.unit.fields[self.index][0] if self.index < len(self.unit.fields) else None


SLICE_START = {"forbidden_zero_bit", "nal_unit_type", "nuh_layer_id", "nuh_temporal_id_plus1",
               "first_slice_segment_in_pic_flag", "no_output_of_prior_pics_flag", "slice_pic_parameter_set_id",
               "dependent_slice_segment_flag", "slice_segment_address", "slice_type", "pic_output_flag",
               "colour_plane_id", "slice_pic_order_cnt_lsb", "short_term_ref_pic_set_sps_flag"}


def keep(unit):
    """The fields of a unit the model reads, for the compact dump: a parameter set whole, a slice
    header up to the element after its reference picture set syntax, whose position ends the set."""
    if unit.kind != "Slice Segment Header":
        return unit.fields
    i = unit.position("short_term_ref_pic_set_sps_flag")
    if i is None:
        return [f for f in unit.fields if f[1] in SLICE_START]
    last = i
    for j in range(i + 1, len(unit.fields)):
        name = unit.fields[j][1].split("[")[0]
        if name in ("inter_ref_pic_set_prediction_flag", "delta_idx_minus1", "delta_rps_sign", "abs_delta_rps_minus1",
                    "used_by_curr_pic_flag", "use_delta_flag", "num_negative_pics", "num_positive_pics",
                    "delta_poc_s0_minus1", "used_by_curr_pic_s0_flag", "delta_poc_s1_minus1", "used_by_curr_pic_s1_flag",
                    "short_term_ref_pic_set_idx", "num_long_term_sps", "num_long_term_pics", "lt_idx_sps", "poc_lsb_lt",
                    "used_by_curr_pic_lt_flag", "delta_poc_msb_present_flag", "delta_poc_msb_cycle_lt"):
            last = j
        else:
            break
    return unit.fields[:last + 2]


def bits(values):
    mask = 0
    for i, v in enumerate(values):
        mask |= (1 if v else 0) << i
    return mask


def read_st_rps(cur, idx, num_sets, sets):
    """st_ref_pic_set(idx) (H.265 7.3.7) with its derived variables (7.4.8)."""
    rps = {"inter": cur.optional("inter_ref_pic_set_prediction_flag") if idx != 0 else 0}
    if rps["inter"]:
        rps["delta_idx_minus1"] = cur.optional("delta_idx_minus1") if idx == num_sets else 0
        rps["delta_rps_sign"] = cur.read("delta_rps_sign")
        rps["abs_delta_rps_minus1"] = cur.read("abs_delta_rps_minus1")
        ref = sets[idx - (rps["delta_idx_minus1"] + 1)]
        used, use_delta = [], []
        for j in range(ref["NumDeltaPocs"] + 1):
            used.append(cur.read(f"used_by_curr_pic_flag[{j}]"))
            use_delta.append(cur.read(f"use_delta_flag[{j}]") if not used[j] else 1)
        rps["used_by_curr_pic_flag"], rps["use_delta_flag"] = used, use_delta
        rps["NumDeltaPocsOfRef"] = ref["NumDeltaPocs"]
        delta_rps = (1 - 2 * rps["delta_rps_sign"]) * (rps["abs_delta_rps_minus1"] + 1)
        s0, u0, s1, u1 = [], [], [], []
        nneg, npos = ref["NumNegativePics"], ref["NumPositivePics"]
        for j in range(npos - 1, -1, -1):
            d = ref["DeltaPocS1"][j] + delta_rps
            if d < 0 and use_delta[nneg + j]:
                s0.append(d), u0.append(used[nneg + j])
        if delta_rps < 0 and use_delta[ref["NumDeltaPocs"]]:
            s0.append(delta_rps), u0.append(used[ref["NumDeltaPocs"]])
        for j in range(nneg):
            d = ref["DeltaPocS0"][j] + delta_rps
            if d < 0 and use_delta[j]:
                s0.append(d), u0.append(used[j])
        for j in range(nneg - 1, -1, -1):
            d = ref["DeltaPocS0"][j] + delta_rps
            if d > 0 and use_delta[j]:
                s1.append(d), u1.append(used[j])
        if delta_rps > 0 and use_delta[ref["NumDeltaPocs"]]:
            s1.append(delta_rps), u1.append(used[ref["NumDeltaPocs"]])
        for j in range(npos):
            d = ref["DeltaPocS1"][j] + delta_rps
            if d > 0 and use_delta[nneg + j]:
                s1.append(d), u1.append(used[nneg + j])
    else:
        nneg = cur.read("num_negative_pics")
        npos = cur.read("num_positive_pics")
        rps["delta_poc_s0_minus1"], rps["used_by_curr_pic_s0_flag"] = [], []
        rps["delta_poc_s1_minus1"], rps["used_by_curr_pic_s1_flag"] = [], []
        s0, u0, s1, u1 = [], [], [], []
        poc = 0
        for i in range(nneg):
            rps["delta_poc_s0_minus1"].append(cur.read(f"delta_poc_s0_minus1[{i}]"))
            rps["used_by_curr_pic_s0_flag"].append(cur.read(f"used_by_curr_pic_s0_flag[{i}]"))
            poc -= rps["delta_poc_s0_minus1"][i] + 1
            s0.append(poc), u0.append(rps["used_by_curr_pic_s0_flag"][i])
        poc = 0
        for i in range(npos):
            rps["delta_poc_s1_minus1"].append(cur.read(f"delta_poc_s1_minus1[{i}]"))
            rps["used_by_curr_pic_s1_flag"].append(cur.read(f"used_by_curr_pic_s1_flag[{i}]"))
            poc += rps["delta_poc_s1_minus1"][i] + 1
            s1.append(poc), u1.append(rps["used_by_curr_pic_s1_flag"][i])
    rps.update(DeltaPocS0=s0, UsedByCurrPicS0=u0, DeltaPocS1=s1, UsedByCurrPicS1=u1, NumNegativePics=len(s0),
               NumPositivePics=len(s1), NumDeltaPocs=len(s0) + len(s1))
    return rps


def read_sps(unit):
    """The SPS fields by name, with its reference picture sets in order."""
    sps = {name: value for _, name, value in unit.fields}
    sets = []
    i = unit.position("num_short_term_ref_pic_sets")
    if i is not None:
        cur = Cursor(unit, i + 1)
        for idx in range(sps["num_short_term_ref_pic_sets"]):
            sets.append(read_st_rps(cur, idx, sps["num_short_term_ref_pic_sets"], sets))
    sps["sets"] = sets
    return sps


def coded(unit, name):
    return unit.has(name)


class Picture:
    def __init__(self, index, nal_unit_type, poc):
        self.index = index
        self.nal_unit_type = nal_unit_type
        self.poc = poc
        self.expect = {}


def model_pictures(units):
    """The pictures the parser decodes, in decoding order, with what the trace has to show; and the
    parameter set units."""
    vps, sps, pps = {}, {}, {}
    pictures = []
    params = []
    prev_tid0_poc = 0
    first = True
    after_eos = False
    skip_rasl = False
    for unit in units:
        if unit.kind in PARAM_TAG:
            params.append(unit)
            if unit.kind == "Video Parameter Set":
                vps[unit.get("vps_video_parameter_set_id")] = unit
            elif unit.kind == "Sequence Parameter Set":
                sps[unit.get("sps_seq_parameter_set_id")] = (unit, read_sps(unit))
            else:
                pps[unit.get("pps_pic_parameter_set_id")] = unit
            continue
        if unit.kind in ("End of Sequence", "End of Bitstream"):
            after_eos = True
            continue
        if not unit.get("first_slice_segment_in_pic_flag", 0):
            continue
        nut = unit.get("nal_unit_type")
        tid = unit.get("nuh_temporal_id_plus1", 1) - 1
        p = pps.get(unit.get("slice_pic_parameter_set_id"))
        if p is None or p.get("pps_seq_parameter_set_id") not in sps:
            continue
        s_unit, s = sps[p.get("pps_seq_parameter_set_id")]
        irap = 16 <= nut <= 23
        idr = nut in (19, 20)
        no_rasl_output = irap and (idr or 16 <= nut <= 18 or first or after_eos)
        if irap:
            skip_rasl = no_rasl_output
        first = False
        after_eos = False
        if nut in (8, 9) and skip_rasl:
            continue
        max_lsb = 1 << (s["log2_max_pic_order_cnt_lsb_minus4"] + 4)
        lsb = 0 if idr else unit.get("slice_pic_order_cnt_lsb", 0)
        if irap and no_rasl_output:
            msb = 0
        else:
            prev_lsb = prev_tid0_poc & (max_lsb - 1)
            prev_msb = prev_tid0_poc - prev_lsb
            if lsb < prev_lsb and prev_lsb - lsb >= max_lsb // 2:
                msb = prev_msb + max_lsb
            elif lsb > prev_lsb and lsb - prev_lsb > max_lsb // 2:
                msb = prev_msb - max_lsb
            else:
                msb = prev_msb
        poc = msb + lsb
        sub_layer_non_ref = nut <= 14 and nut % 2 == 0
        if tid == 0 and not (6 <= nut <= 9) and not sub_layer_non_ref:
            prev_tid0_poc = poc
        pic = Picture(len(pictures), nut, poc)
        e = pic.expect
        e["CurrPicOrderCntVal"] = poc
        e["IrapPicFlag"] = int(irap)
        e["IdrPicFlag"] = int(idr)
        e["pic_parameter_set_id"] = unit.get("slice_pic_parameter_set_id")
        before, after, lt_curr, lt_msb = [], [], [], []
        e["short_term_ref_pic_set_sps_flag"] = 0
        e["NumBitsForShortTermRPSInSlice"] = 0
        e["NumDeltaPocsOfRefRpsIdx"] = 0
        if not idr:
            sps_flag = unit.get("short_term_ref_pic_set_sps_flag")
            e["short_term_ref_pic_set_sps_flag"] = sps_flag
            i = unit.position("short_term_ref_pic_set_sps_flag")
            cur = Cursor(unit, i + 1)
            if sps_flag:
                rps = s["sets"][cur.optional("short_term_ref_pic_set_idx")]
            else:
                start = cur.bit_position()
                rps = read_st_rps(cur, s["num_short_term_ref_pic_sets"], s["num_short_term_ref_pic_sets"], s["sets"])
                e["NumBitsForShortTermRPSInSlice"] = cur.bit_position() - start
                if rps["inter"]:
                    e["NumDeltaPocsOfRefRpsIdx"] = rps["NumDeltaPocsOfRef"]
            for d, used in zip(rps["DeltaPocS0"], rps["UsedByCurrPicS0"]):
                if used:
                    before.append(poc + d)
            for d, used in zip(rps["DeltaPocS1"], rps["UsedByCurrPicS1"]):
                if used:
                    after.append(poc + d)
            if s.get("long_term_ref_pics_present_flag"):
                num_sps = cur.optional("num_long_term_sps") if s.get("num_long_term_ref_pics_sps", 0) > 0 else 0
                num_pics = cur.read("num_long_term_pics")
                cycle = 0
                for i in range(num_sps + num_pics):
                    if i < num_sps:
                        idx = cur.optional(f"lt_idx_sps[{i}]")
                        lsb_lt = s_unit.get(f"lt_ref_pic_poc_lsb_sps[{idx}]")
                        used = s_unit.get(f"used_by_curr_pic_lt_sps_flag[{idx}]")
                    else:
                        lsb_lt = cur.read(f"poc_lsb_lt[{i}]")
                        used = cur.read(f"used_by_curr_pic_lt_flag[{i}]")
                    present = cur.read(f"delta_poc_msb_present_flag[{i}]")
                    delta = cur.read(f"delta_poc_msb_cycle_lt[{i}]") if present else 0
                    cycle = delta if i in (0, num_sps) else cycle + delta
                    poc_lt = lsb_lt
                    if present:
                        poc_lt += poc - cycle * max_lsb - (poc & (max_lsb - 1))
                    if used:
                        lt_curr.append(poc_lt)
                        lt_msb.append(present)
        e["before"], e["after"], e["lt"], e["lt_msb"], e["max_lsb"] = before, after, lt_curr, lt_msb, max_lsb
        pictures.append(pic)
    return pictures, params


def compare_params(report, params, entries, verbose):
    trace = {tag: [e for e in entries if e.tag == tag] for tag in ("vps", "sps", "pps")}
    counts = {tag: 0 for tag in trace}
    for unit in params:
        tag = PARAM_TAG[unit.kind]
        n = counts[tag]
        counts[tag] += 1
        key = f"{tag} {n}"
        if n >= len(trace[tag]):
            report.check("params-count", key, "present in the trace", True, False)
            continue
        t = trace[tag][n]
        fields, flags = t.fields, t.fields.get("flags", set())
        if tag == "vps":
            for name in VPS_FIELDS:
                report.check("vps", key, name, unit.get(name, 0), fields.get(name))
            for name in VPS_FLAGS:
                report.check("vps-flags", key, name, unit.get(name, 0), int(name in flags))
            compare_ptl(report, key, unit, t)
            compare_dpbm(report, key, unit, t, "vps_")
        elif tag == "sps":
            sps = read_sps(unit)
            for name in SPS_FIELDS:
                report.check("sps", key, name, unit.get(name, 0), fields.get(name))
            for name in SPS_FLAGS:
                report.check("sps-flags", key, name, unit.get(name, 0), int(name in flags))
            compare_ptl(report, key, unit, t)
            compare_dpbm(report, key, unit, t, "sps_")
            compare_strps(report, key, sps, t.sub.get("strps", []))
            if unit.get("long_term_ref_pics_present_flag", 0):
                n_lt = unit.get("num_long_term_ref_pics_sps", 0)
                lt = t.one("ltrps") or {}
                report.check("sps-ltrps", key, "lt_ref_pic_poc_lsb_sps",
                             [unit.get(f"lt_ref_pic_poc_lsb_sps[{i}]") for i in range(n_lt)],
                             lt.get("lt_ref_pic_poc_lsb_sps", [])[:n_lt])
                report.check("sps-ltrps", key, "used_by_curr_pic_lt_sps_flag",
                             bits([unit.get(f"used_by_curr_pic_lt_sps_flag[{i}]") for i in range(n_lt)]),
                             lt.get("used_by_curr_pic_lt_sps_flag", 0) & ((1 << n_lt) - 1))
            if unit.get("vui_parameters_present_flag", 0):
                vui = t.one("vui") or {}
                for name in VUI_FIELDS:
                    if coded(unit, name):
                        report.check("sps-vui", key, name, unit.get(name), vui.get(name))
                if coded(unit, "matrix_coefficients"):
                    report.check("sps-vui", key, "matrix_coeffs", unit.get("matrix_coefficients"), vui.get("matrix_coeffs"))
                for name in VUI_FLAGS:
                    if coded(unit, name):
                        report.check("sps-vui", key, name, unit.get(name), int(name in vui.get("flags", set())))
                if unit.get("vui_hrd_parameters_present_flag", 0):
                    compare_hrd(report, key, unit, t, unit.get("sps_max_sub_layers_minus1", 0))
        else:
            for name in PPS_FIELDS:
                report.check("pps", key, name, unit.get(name, 0), fields.get(name))
            for name in PPS_FLAGS:
                # Not coded, these two are inferred to be 1 (H.265 7.4.3.3); with no tiles they are moot.
                if name in ("uniform_spacing_flag", "loop_filter_across_tiles_enabled_flag") and not coded(unit, name):
                    continue
                report.check("pps-flags", key, name, unit.get(name, 0), int(name in flags))
    for tag, items in trace.items():
        for n in range(counts[tag], len(items)):
            report.check("params-count", f"{tag} {n}", "present in the dump", True, False)


def compare_ptl(report, key, unit, t):
    ptl = t.one("ptl") or {}
    report.check("ptl", key, "general_profile_idc", unit.get("general_profile_idc"), ptl.get("general_profile_idc"))
    report.check("ptl-level", key, "general_level_idc", LEVELS.get(unit.get("general_level_idc"), "unknown level"),
                 ptl.get("general_level_idc"))
    for name in PTL_FLAGS:
        report.check("ptl-flags", key, name, unit.get(name, 0), int(name in ptl.get("flags", set())))


def compare_dpbm(report, key, unit, t, prefix):
    dpbm = t.one("dpbm") or {}
    category = prefix.rstrip("_") + "-dpbm"
    for i in range(7):
        for syntax, std in (("max_dec_pic_buffering_minus1", "max_dec_pic_buffering_minus1"),
                            ("max_num_reorder_pics", "max_num_reorder_pics"),
                            ("max_latency_increase_plus1", "max_latency_increase_plus1")):
            name = f"{prefix}{syntax}[{i}]"
            if coded(unit, name):
                report.check(category, key, f"{std}[{i}]", unit.get(name), dpbm.get(std, [None] * 7)[i])


def compare_strps(report, key, sps, lines):
    by_index = {line["i"]: line for line in lines}
    for idx, rps in enumerate(sps["sets"]):
        t = by_index.get(idx)
        k = f"{key} set {idx}"
        if t is None:
            report.check("sps-strps", k, "present", True, False)
            continue
        flags = t.get("flags", set())
        report.check("sps-strps", k, "inter_ref_pic_set_prediction_flag", rps["inter"],
                     int("inter_ref_pic_set_prediction_flag" in flags))
        if rps["inter"]:
            report.check("sps-strps", k, "delta_rps_sign", rps["delta_rps_sign"], int("delta_rps_sign" in flags))
            report.check("sps-strps", k, "abs_delta_rps_minus1", rps["abs_delta_rps_minus1"], t.get("abs_delta_rps_minus1"))
            n = rps["NumDeltaPocsOfRef"] + 1
            report.check("sps-strps", k, "used_by_curr_pic_flag", bits(rps["used_by_curr_pic_flag"]),
                         t.get("used_by_curr_pic_flag", 0) & ((1 << n) - 1))
            report.check("sps-strps", k, "use_delta_flag", bits(rps["use_delta_flag"]),
                         t.get("use_delta_flag", 0) & ((1 << n) - 1))
        else:
            nneg, npos = len(rps["delta_poc_s0_minus1"]), len(rps["delta_poc_s1_minus1"])
            report.check("sps-strps", k, "num_negative_pics", nneg, t.get("num_negative_pics"))
            report.check("sps-strps", k, "num_positive_pics", npos, t.get("num_positive_pics"))
            report.check("sps-strps", k, "delta_poc_s0_minus1", rps["delta_poc_s0_minus1"],
                         t.get("delta_poc_s0_minus1", [])[:nneg])
            report.check("sps-strps", k, "delta_poc_s1_minus1", rps["delta_poc_s1_minus1"],
                         t.get("delta_poc_s1_minus1", [])[:npos])
            report.check("sps-strps", k, "used_by_curr_pic_s0_flag", bits(rps["used_by_curr_pic_s0_flag"]),
                         t.get("used_by_curr_pic_s0_flag", 0) & ((1 << nneg) - 1))
            report.check("sps-strps", k, "used_by_curr_pic_s1_flag", bits(rps["used_by_curr_pic_s1_flag"]),
                         t.get("used_by_curr_pic_s1_flag", 0) & ((1 << npos) - 1))


def compare_hrd(report, key, unit, t, max_sub_layers_minus1):
    """The SPS VUI hrd_parameters(1, sps_max_sub_layers_minus1) (H.265 E.2.2), as the trace's hrd,
    hrd-nal and hrd-vcl lines show them."""
    hrd = t.one("hrd") or {}
    for name in HRD_FIELDS:
        if coded(unit, name):
            report.check("sps-hrd", key, name, unit.get(name), hrd.get(name))
    nal = unit.get("nal_hrd_parameters_present_flag", 0)
    vcl = unit.get("vcl_hrd_parameters_present_flag", 0)
    start = unit.position("fixed_pic_rate_general_flag[0]")
    if start is None:
        return
    cur = Cursor(unit, start)
    expected = {"hrd-nal": [], "hrd-vcl": []}
    cpb_cnt = []
    for i in range(max_sub_layers_minus1 + 1):
        general = cur.read(f"fixed_pic_rate_general_flag[{i}]")
        within = cur.optional(f"fixed_pic_rate_within_cvs_flag[{i}]", 1) if not general else 1
        low_delay = 0
        if within:
            cur.read(f"elemental_duration_in_tc_minus1[{i}]")
        else:
            low_delay = cur.read(f"low_delay_hrd_flag[{i}]")
        cnt = cur.read(f"cpb_cnt_minus1[{i}]") if not low_delay else cur.optional(f"cpb_cnt_minus1[{i}]")
        cpb_cnt.append(cnt)
        for present, tag in ((nal, "hrd-nal"), (vcl, "hrd-vcl")):
            if not present:
                continue
            layer = {"bit_rate_value_minus1": [], "cpb_size_value_minus1": [], "cbr_flag": 0}
            for j in range(cnt + 1):
                layer["bit_rate_value_minus1"].append(cur.read(f"bit_rate_value_minus1[{j}]"))
                layer["cpb_size_value_minus1"].append(cur.read(f"cpb_size_value_minus1[{j}]"))
                cur.optional(f"cpb_size_du_value_minus1[{j}]")
                cur.optional(f"bit_rate_du_value_minus1[{j}]")
                layer["cbr_flag"] |= cur.read(f"cbr_flag[{j}]") << j
            expected[tag].append(layer)
    report.check("sps-hrd", key, "cpb_cnt_minus1", cpb_cnt, hrd.get("cpb_cnt_minus1", [])[:len(cpb_cnt)])
    for tag, layers in expected.items():
        lines = {line["i"]: line for line in t.sub.get(tag, [])}
        for i, layer in enumerate(layers):
            line = lines.get(i, {})
            for name, value in layer.items():
                actual = line.get(name)
                if isinstance(value, list) and isinstance(actual, list):
                    actual = actual[:len(value)]
                report.check("sps-hrd", key, f"{tag} {i} {name}", value, actual)


def compare_picture(report, pic, entry):
    key = f"picture {pic.index}"
    e = pic.expect
    hevc = entry.one("hevc") or {}
    rps = entry.one("rps") or {}
    report.check("hdr-poc", key, "CurrPicOrderCntVal", e["CurrPicOrderCntVal"], hevc.get("CurrPicOrderCntVal"))
    for name in ("IrapPicFlag", "IdrPicFlag", "short_term_ref_pic_set_sps_flag"):
        report.check("hdr-flags", key, name, e[name], hevc.get(name))
    report.check("hdr-flags", key, "pic_parameter_set_id", e["pic_parameter_set_id"], hevc.get("pic_parameter_set_id"))
    for name in ("NumBitsForShortTermRPSInSlice", "NumDeltaPocsOfRefRpsIdx"):
        report.check("hdr-rps-bits", key, name, e[name], hevc.get(name))
    counts = {"NumPocStCurrBefore": len(e["before"]), "NumPocStCurrAfter": len(e["after"]), "NumPocLtCurr": len(e["lt"])}
    for name, value in counts.items():
        report.check("rps-counts", key, name, value, rps.get(name))
    report.check("rps-counts", key, "NumPocTotalCurr", sum(counts.values()), rps.get("NumPocTotalCurr"))
    pocs = rps.get("PicOrderCntVal", [])

    def named(list_name, count):
        indices = rps.get(list_name, [])[:count]
        return [pocs[i] if 0 <= i < len(pocs) else None for i in indices]

    report.check("rps-pocs", key, "RefPicSetStCurrBefore POCs", e["before"], named("RefPicSetStCurrBefore", len(e["before"])))
    report.check("rps-pocs", key, "RefPicSetStCurrAfter POCs", e["after"], named("RefPicSetStCurrAfter", len(e["after"])))
    lt = named("RefPicSetLtCurr", len(e["lt"]))
    expected_lt = [p if msb else p & (e["max_lsb"] - 1) for p, msb in zip(e["lt"], e["lt_msb"])]
    actual_lt = [p if (p is None or msb) else p & (e["max_lsb"] - 1) for p, msb in zip(lt, e["lt_msb"])]
    report.check("rps-pocs", key, "RefPicSetLtCurr POCs", expected_lt, actual_lt)


def compare(lines, trace_path, verbose=False):
    """Returns (report, note); report is None when the dump has nothing to compare."""
    units = hd.read_units(lines, KINDS)
    if not units:
        return None, "no H.265 units in the dump"
    pictures, params = model_pictures(units)
    entries = hd.read_trace(trace_path)
    report = hd.Report(verbose)
    compare_params(report, params, entries, verbose)
    traced = [e for e in entries if e.tag == "pic"]
    # Aligned on the POC LSB, so that a wrong POC MSB shows as hdr-poc rather than as misalignment.
    max_lsb = pictures[0].expect["max_lsb"] if pictures else 16
    keys_model = [p.poc & (max_lsb - 1) for p in pictures]
    keys_trace = [((t.one("hevc") or {}).get("CurrPicOrderCntVal", -1)) & (max_lsb - 1) for t in traced]
    matcher = difflib.SequenceMatcher(a=keys_model, b=keys_trace, autojunk=False)
    for tag, a0, a1, b0, b1 in matcher.get_opcodes():
        if tag == "equal":
            for i, j in zip(range(a0, a1), range(b0, b1)):
                compare_picture(report, pictures[i], traced[j])
            continue
        if tag == "replace" and a1 - a0 == b1 - b0:
            for i, j in zip(range(a0, a1), range(b0, b1)):
                compare_picture(report, pictures[i], traced[j])
            continue
        for i in range(a0, a1):
            report.check("picture-count", f"picture {i}", "in the trace", True, False)
        for j in range(b0, b1):
            report.check("picture-count", f"trace picture {j}", "in the model", True, False)
    return report, ""
