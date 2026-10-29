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
"""H.264 header model of the parser trace test.

Reads an H.264 stream's syntax elements from ffmpeg's trace_headers dump and checks the parser's
trace against them, in categories:
  sps, pps         the fields of each parameter set, in order, against their syntax elements (the
                   Std level enum for level_idc)
  sps-flags, pps-flags  the flags of each parameter set; sps-constraint-flags the SPS constraint_set
                   flags
  hdr-poc          the picture's field order counts (H.264 8.2.1, POC types 0, 1 and 2, with memory
                   management control operation 5)
  hdr-slice        frame_num, field_pic_flag, bottom_field_flag and the PPS id
  hdr-flags        ref_pic_flag (nal_ref_idc not 0) and intra_pic_flag (every slice I or SI)
  picture-count    a picture in the trace or the model but not both (alignment is by frame_num and
                   picture structure)
  params-count     a parameter set in the trace or the dump but not both
A picture is a frame or a field. Redundant slices (redundant_pic_cnt above 0), auxiliary pictures
and pictures whose PPS uses slice groups are left out: the parser decodes none of them. The
parser leaves the rest of VkParserH264PictureData's slice and PPS copies (MbaffFrameFlag, the
active reference counts, the weighted prediction and QP fields) at 0, unused by the Vulkan path,
so they are not compared.
"""
import difflib

import header_dump as hd

DEMUXER = "h264"
KINDS = ("Sequence Parameter Set", "Picture Parameter Set", "Slice Header", "End of Sequence", "End of Stream")
TITLE = "H.264 syntax elements read by h264_header_model.py"
LEVELS = {10: 0, 11: 1, 12: 2, 13: 3, 20: 4, 21: 5, 22: 6, 30: 7, 31: 8, 32: 9, 40: 10, 41: 11, 42: 12, 50: 13, 51: 14,
          52: 15, 60: 16, 61: 17, 62: 18}
PARAM_TAG = {"Sequence Parameter Set": "sps", "Picture Parameter Set": "pps"}
SLICE_FIELDS = {"nal_ref_idc", "nal_unit_type", "first_mb_in_slice", "slice_type", "pic_parameter_set_id", "frame_num",
                "field_pic_flag", "bottom_field_flag", "idr_pic_id", "pic_order_cnt_lsb", "delta_pic_order_cnt_bottom",
                "delta_pic_order_cnt[0]", "delta_pic_order_cnt[1]", "redundant_pic_cnt",
                "memory_management_control_operation"}

SPS_FIELDS = ["seq_parameter_set_id", "profile_idc", "bit_depth_luma_minus8", "bit_depth_chroma_minus8",
              "log2_max_frame_num_minus4", "pic_order_cnt_type", "offset_for_non_ref_pic", "offset_for_top_to_bottom_field",
              "log2_max_pic_order_cnt_lsb_minus4", "num_ref_frames_in_pic_order_cnt_cycle", "max_num_ref_frames",
              "pic_width_in_mbs_minus1", "pic_height_in_map_units_minus1", "frame_crop_left_offset",
              "frame_crop_right_offset", "frame_crop_top_offset", "frame_crop_bottom_offset"]
# Std flag name: syntax element name.
SPS_FLAGS = {"constraint_set0_flag": "constraint_set0_flag", "constraint_set1_flag": "constraint_set1_flag",
             "constraint_set2_flag": "constraint_set2_flag", "constraint_set3_flag": "constraint_set3_flag",
             "constraint_set4_flag": "constraint_set4_flag", "constraint_set5_flag": "constraint_set5_flag",
             "direct_8x8_inference_flag": "direct_8x8_inference_flag",
             "mb_adaptive_frame_field_flag": "mb_adaptive_frame_field_flag", "frame_mbs_only_flag": "frame_mbs_only_flag",
             "delta_pic_order_always_zero_flag": "delta_pic_order_always_zero_flag",
             "separate_colour_plane_flag": "separate_colour_plane_flag",
             "gaps_in_frame_num_value_allowed_flag": "gaps_in_frame_num_allowed_flag",
             "qpprime_y_zero_transform_bypass_flag": "qpprime_y_zero_transform_bypass_flag",
             "frame_cropping_flag": "frame_cropping_flag", "seq_scaling_matrix_present_flag": "seq_scaling_matrix_present_flag",
             "vui_parameters_present_flag": "vui_parameters_present_flag"}
PPS_FIELDS = ["seq_parameter_set_id", "pic_parameter_set_id", "num_ref_idx_l0_default_active_minus1",
              "num_ref_idx_l1_default_active_minus1", "weighted_bipred_idc", "pic_init_qp_minus26", "pic_init_qs_minus26",
              "chroma_qp_index_offset"]
PPS_FLAGS = ["transform_8x8_mode_flag", "redundant_pic_cnt_present_flag", "constrained_intra_pred_flag",
             "deblocking_filter_control_present_flag", "weighted_pred_flag", "bottom_field_pic_order_in_frame_present_flag",
             "entropy_coding_mode_flag", "pic_scaling_matrix_present_flag"]


def keep(unit):
    """The fields of a unit the model reads, for the compact dump."""
    if unit.kind == "Slice Header":
        return [f for f in unit.fields if f[1] in SLICE_FIELDS]
    if unit.kind == "Sequence Parameter Set":
        vui = unit.position("vui_parameters_present_flag")
        return unit.fields if vui is None else unit.fields[:vui + 1]
    return [f for f in unit.fields if not f[1].startswith(("scaling_list", "delta_scale", "pic_scaling_list_present_flag["))]


def unit_fields(unit):
    return {name: value for _, name, value in unit.fields}


def new_picture(prev, cur):
    """H.264 7.4.1.2.4: whether the slice cur starts a new picture after the slice prev."""
    if prev is None:
        return True
    for name in ("frame_num", "pic_parameter_set_id", "field_pic_flag", "bottom_field_flag", "pic_order_cnt_lsb",
                 "delta_pic_order_cnt_bottom", "delta_pic_order_cnt[0]", "delta_pic_order_cnt[1]"):
        if prev.get(name, 0) != cur.get(name, 0):
            return True
    if (prev["nal_ref_idc"] == 0) != (cur["nal_ref_idc"] == 0):
        return True
    if (prev["nal_unit_type"] == 5) != (cur["nal_unit_type"] == 5):
        return True
    return cur["nal_unit_type"] == 5 and prev.get("idr_pic_id", 0) != cur.get("idr_pic_id", 0)


class PocState:
    """The state of H.264 8.2.1 between pictures."""

    def __init__(self):
        self.prev_msb = 0
        self.prev_lsb = 0
        self.prev_frame_num = 0
        self.prev_frame_num_offset = 0
        self.prev_ref_mmco5 = False
        self.prev_ref_bottom = False
        self.prev_ref_top_after_mmco5 = 0
        self.prev_mmco5 = False


def picture_poc(state, sps, s, mmco5):
    """Returns (TopFieldOrderCnt, BottomFieldOrderCnt) of the picture whose first slice is s, and
    updates the state."""
    idr = s["nal_unit_type"] == 5
    field = s.get("field_pic_flag", 0)
    bottom = s.get("bottom_field_flag", 0)
    max_frame_num = 1 << (sps["log2_max_frame_num_minus4"] + 4)
    frame_num = s["frame_num"]
    top = bot = 0
    poc_type = sps["pic_order_cnt_type"]
    if poc_type == 0:
        max_lsb = 1 << (sps["log2_max_pic_order_cnt_lsb_minus4"] + 4)
        if idr:
            prev_msb = prev_lsb = 0
        elif state.prev_ref_mmco5:
            prev_msb = 0
            prev_lsb = 0 if state.prev_ref_bottom else state.prev_ref_top_after_mmco5
        else:
            prev_msb, prev_lsb = state.prev_msb, state.prev_lsb
        lsb = s.get("pic_order_cnt_lsb", 0)
        if lsb < prev_lsb and prev_lsb - lsb >= max_lsb // 2:
            msb = prev_msb + max_lsb
        elif lsb > prev_lsb and lsb - prev_lsb > max_lsb // 2:
            msb = prev_msb - max_lsb
        else:
            msb = prev_msb
        if not (field and bottom):
            top = msb + lsb
            bot = top + s.get("delta_pic_order_cnt_bottom", 0) if not field else top
        else:
            bot = msb + lsb
            top = bot
        if s["nal_ref_idc"] != 0:
            state.prev_msb, state.prev_lsb = msb, lsb
    else:
        if idr:
            frame_num_offset = 0
        else:
            prev_offset = 0 if state.prev_mmco5 else state.prev_frame_num_offset
            frame_num_offset = prev_offset + max_frame_num if state.prev_frame_num > frame_num else prev_offset
        if poc_type == 1:
            cycle = sps.get("num_ref_frames_in_pic_order_cnt_cycle", 0)
            abs_frame_num = frame_num_offset + frame_num if cycle != 0 else 0
            if s["nal_ref_idc"] == 0 and abs_frame_num > 0:
                abs_frame_num -= 1
            expected = 0
            if abs_frame_num > 0:
                deltas = [sps.get(f"offset_for_ref_frame[{i}]", 0) for i in range(cycle)]
                cycle_count = (abs_frame_num - 1) // cycle
                in_cycle = (abs_frame_num - 1) % cycle
                expected = cycle_count * sum(deltas) + sum(deltas[:in_cycle + 1])
            if s["nal_ref_idc"] == 0:
                expected += sps.get("offset_for_non_ref_pic", 0)
            d0 = s.get("delta_pic_order_cnt[0]", 0)
            d1 = s.get("delta_pic_order_cnt[1]", 0)
            if not field:
                top = expected + d0
                bot = top + sps.get("offset_for_top_to_bottom_field", 0) + d1
            elif not bottom:
                top = bot = expected + d0
            else:
                top = bot = expected + sps.get("offset_for_top_to_bottom_field", 0) + d0
        else:
            if idr:
                temp = 0
            elif s["nal_ref_idc"] == 0:
                temp = 2 * (frame_num_offset + frame_num) - 1
            else:
                temp = 2 * (frame_num_offset + frame_num)
            top = bot = temp
        state.prev_frame_num_offset = frame_num_offset
    state.prev_frame_num = frame_num
    state.prev_mmco5 = mmco5
    if s["nal_ref_idc"] != 0:
        state.prev_ref_mmco5 = mmco5
        state.prev_ref_bottom = bool(field and bottom)
        if mmco5:
            # After the picture, its order counts are made relative to the smaller one (8.2.1).
            temp = min(top, bot) if not field else (top if not bottom else bot)
            state.prev_ref_top_after_mmco5 = top - temp
            if poc_type == 0:
                state.prev_msb, state.prev_lsb = 0, top - temp
    if mmco5:
        state.prev_frame_num = 0
        state.prev_frame_num_offset = 0
    return top, bot


class Picture:
    def __init__(self, index):
        self.index = index
        self.expect = {}
        self.key = None


def model_pictures(units):
    sps, pps = {}, {}
    params = []
    pictures = []
    state = PocState()
    slices = []  # slices of the current picture

    def finish():
        if not slices:
            return
        s = slices[0]
        p = pps.get(s["pic_parameter_set_id"])
        q = sps.get(p["seq_parameter_set_id"]) if p else None
        if q is None:
            return
        mmco5 = any(op == 5 for sl in slices for op in sl["mmco"])
        top, bot = picture_poc(state, q, s, mmco5)
        pic = Picture(len(pictures))
        e = pic.expect
        field = s.get("field_pic_flag", 0)
        bottom = s.get("bottom_field_flag", 0)
        e["frame_num"] = s["frame_num"]
        e["field_pic_flag"] = field
        e["bottom_field_flag"] = bottom
        e["pic_parameter_set_id"] = s["pic_parameter_set_id"]
        e["poc"] = (top, bot, field, bottom)
        e["ref_pic_flag"] = int(s["nal_ref_idc"] != 0)
        e["intra_pic_flag"] = int(all(sl["slice_type"] % 5 in (2, 4) for sl in slices))
        pic.key = (s["frame_num"], field, bottom)
        pictures.append(pic)
        slices.clear()

    prev = None
    for unit in units:
        if unit.kind in PARAM_TAG:
            params.append(unit)
            fields = unit_fields(unit)
            if unit.kind == "Sequence Parameter Set":
                fields["offsets"] = [v for _, n, v in unit.fields if n.startswith("offset_for_ref_frame[")]
                sps[fields["seq_parameter_set_id"]] = fields
            else:
                pps[fields["pic_parameter_set_id"]] = fields
            continue
        if unit.kind != "Slice Header":
            continue
        s = unit_fields(unit)
        # Redundant slices and auxiliary pictures (nal_unit_type 19) are not decoded, nor is a
        # picture whose PPS uses slice groups: the parser marks it non-existing (FMO is unsupported).
        if s.get("redundant_pic_cnt", 0) > 0 or s["nal_unit_type"] == 19:
            continue
        p = pps.get(s.get("pic_parameter_set_id"))
        if p is not None and p.get("num_slice_groups_minus1", 0) > 0:
            continue
        s["mmco"] = [v for _, n, v in unit.fields if n == "memory_management_control_operation"]
        if new_picture(prev, s):
            finish()
        slices.append(s)
        prev = s
    finish()
    return pictures, params


def compare_params(report, params, entries):
    trace = {tag: [e for e in entries if e.tag == tag] for tag in ("sps", "pps")}
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
        if tag == "sps":
            for name in SPS_FIELDS:
                report.check("sps", key, name, unit.get(name, 0), fields.get(name))
            report.check("sps", key, "chroma_format_idc", unit.get("chroma_format_idc", 1), fields.get("chroma_format_idc"))
            report.check("sps", key, "level_idc", LEVELS.get(unit.get("level_idc"), "unknown level"), fields.get("level_idc"))
            offsets = [v for _, n_, v in unit.fields if n_.startswith("offset_for_ref_frame[")]
            if unit.get("pic_order_cnt_type", 0) == 1:
                report.check("sps", key, "offset_for_ref_frame", offsets, fields.get("offset_for_ref_frame", []))
            for std, syntax in SPS_FLAGS.items():
                expected = unit.get(syntax, 1 if syntax == "frame_mbs_only_flag" else 0)
                category = "sps-constraint-flags" if std.startswith("constraint_set") else "sps-flags"
                report.check(category, key, std, expected, int(std in flags))
        else:
            for name in PPS_FIELDS:
                report.check("pps", key, name, unit.get(name, 0), fields.get(name))
            report.check("pps", key, "second_chroma_qp_index_offset",
                         unit.get("second_chroma_qp_index_offset", unit.get("chroma_qp_index_offset", 0)),
                         fields.get("second_chroma_qp_index_offset"))
            for name in PPS_FLAGS:
                report.check("pps-flags", key, name, unit.get(name, 0), int(name in flags))
    for tag, items in trace.items():
        for n in range(counts[tag], len(items)):
            report.check("params-count", f"{tag} {n}", "present in the dump", True, False)


def compare_picture(report, pic, entry):
    key = f"picture {pic.index}"
    e = pic.expect
    h = entry.one("h264") or {}
    top, bot, field, bottom = e["poc"]
    poc = h.get("CurrFieldOrderCnt", [None, None])
    if not field:
        report.check("hdr-poc", key, "CurrFieldOrderCnt", [top, bot], poc)
    elif not bottom:
        report.check("hdr-poc", key, "CurrFieldOrderCnt[0]", top, poc[0])
    else:
        report.check("hdr-poc", key, "CurrFieldOrderCnt[1]", bot, poc[1])
    for name in ("frame_num", "field_pic_flag", "bottom_field_flag", "pic_parameter_set_id"):
        report.check("hdr-slice", key, name, e[name], h.get(name))
    for name in ("ref_pic_flag", "intra_pic_flag"):
        report.check("hdr-flags", key, name, e[name], entry.fields.get(name))


def compare(lines, trace_path, verbose=False):
    """Returns (report, note); report is None when the dump has nothing to compare."""
    units = hd.read_units(lines, KINDS)
    if not units:
        return None, "no H.264 units in the dump"
    pictures, params = model_pictures(units)
    entries = hd.read_trace(trace_path)
    report = hd.Report(verbose)
    compare_params(report, params, entries)
    traced = [t for t in entries if t.tag == "pic"]
    keys_model = [p.key for p in pictures]
    keys_trace = [((t.one("h264") or {}).get("frame_num"), (t.one("h264") or {}).get("field_pic_flag"),
                   (t.one("h264") or {}).get("bottom_field_flag")) for t in traced]
    matcher = difflib.SequenceMatcher(a=keys_model, b=keys_trace, autojunk=False)
    for tag, a0, a1, b0, b1 in matcher.get_opcodes():
        if tag == "equal" or (tag == "replace" and a1 - a0 == b1 - b0):
            for i, j in zip(range(a0, a1), range(b0, b1)):
                compare_picture(report, pictures[i], traced[j])
            continue
        for i in range(a0, a1):
            report.check("picture-count", f"picture {i}", "in the trace", True, False)
        for j in range(b0, b1):
            report.check("picture-count", f"trace picture {j}", "in the model", True, False)
    return report, ""
