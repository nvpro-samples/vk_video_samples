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
"""VP9 header model of the parser trace test.

Reads each frame's uncompressed header (VP9 6.2) from ffmpeg's trace_headers dump and checks the
parser's trace against it, keeping the state the VP9 specification carries between frames: the
loop filter deltas and the segmentation features (reset by setup_past_independence), the color
config of the last key or intra-only frame, and the frame size of each reference slot for
frame_size_with_refs(). Categories:
  hdr          the Std picture information: profile, frame type, contexts, refresh_frame_flags,
               ref_frame_sign_bias_mask, interpolation filter, quantizer, tiles, and the flags
  hdr-intra    the inter-frame fields of an intra frame, which carries none: 0
               (ref_frame_sign_bias_mask, allow_high_precision_mv, interpolation_filter)
  hdr-size     FrameWidth, FrameHeight, renderWidth, renderHeight, ref_frame_idx, FrameIsIntra,
               compressedHeaderSize
  color        BitDepth, subsampling, color_space, color_range
  lf           the loop filter level, sharpness and deltas
  lf-update    loop_filter_delta_update and the update masks, which only this frame's syntax sets
  seg          the segmentation flags, probabilities, FeatureEnabled and FeatureData
  picture-count  a picture in the trace or the model but not both
A show_existing_frame frame decodes nothing: the parser displays the frame and makes no picture.
"""
import difflib

import header_dump as hd

DEMUXER = "ivf"
KINDS = ("Frame",)
TITLE = "VP9 syntax elements read by vp9_header_model.py"
LITERAL_TO_TYPE = [1, 0, 2, 3]  # EIGHTTAP_SMOOTH, EIGHTTAP, EIGHTTAP_SHARP, BILINEAR
SWITCHABLE = 4
DEFAULT_REF_DELTAS = [1, 0, -1, -1]


class State:
    def __init__(self):
        self.ref_deltas = list(DEFAULT_REF_DELTAS)
        self.mode_deltas = [0, 0]
        self.feature_enabled = [[0] * 4 for _ in range(8)]
        self.feature_data = [[0] * 4 for _ in range(8)]
        self.color = {"BitDepth": 8, "subsampling_x": 1, "subsampling_y": 1, "color_space": 0, "color_range": 0}
        self.slot_size = [(0, 0)] * 8
        self.last_size = None
        self.last_show_frame = False

    def setup_past_independence(self):
        self.ref_deltas = list(DEFAULT_REF_DELTAS)
        self.mode_deltas = [0, 0]
        self.feature_enabled = [[0] * 4 for _ in range(8)]
        self.feature_data = [[0] * 4 for _ in range(8)]


def delta_q(f, name):
    if not f.get(f"{name}.delta_coded", 0):
        return 0
    for key, value in f.items():
        if key.startswith(name + ".") and not key.endswith(".delta_coded"):
            return value
    return 0


def model_frames(units):
    state = State()
    frames = []
    for unit in units:
        f = {name: value for _, name, value in unit.fields}
        if "frame_marker" not in f:
            continue
        if f.get("show_existing_frame", 0):
            state.last_show_frame = True
            continue
        profile = f.get("profile_low_bit", 0) + 2 * f.get("profile_high_bit", 0)
        key_frame = f.get("frame_type", 0) == 0
        show_frame = f.get("show_frame", 0)
        error_res = f.get("error_resilient_mode", 0)
        intra_only = 0 if key_frame else f.get("intra_only", 0)
        intra = key_frame or intra_only
        e = {}
        if key_frame or intra_only:
            if key_frame or profile > 0:
                depth = 8 if profile < 2 else (12 if f.get("ten_or_twelve_bit", 0) else 10)
                cs = f.get("color_space", 0)
                if cs != 7:  # CS_RGB
                    ssx = f.get("subsampling_x", 1) if profile in (1, 3) else 1
                    ssy = f.get("subsampling_y", 1) if profile in (1, 3) else 1
                    rng = f.get("color_range", 0)
                else:
                    ssx = ssy = 0
                    rng = 1
                state.color = {"BitDepth": depth, "subsampling_x": ssx, "subsampling_y": ssy, "color_space": cs,
                               "color_range": rng}
            else:
                state.color = {"BitDepth": 8, "subsampling_x": 1, "subsampling_y": 1, "color_space": 1, "color_range": 0}
        refresh = 0xFF if key_frame else f.get("refresh_frame_flags", 0)
        ref_idx = [f.get(f"ref_frame_idx[{i}]", 0) for i in range(3)] if not intra else None
        if intra or "frame_width_minus_1" in f:
            width, height = f.get("frame_width_minus_1", 0) + 1, f.get("frame_height_minus_1", 0) + 1
        else:
            found = next((i for i in range(3) if f.get(f"found_ref[{i}]", 0)), None)
            width, height = state.slot_size[ref_idx[found]] if found is not None else (0, 0)
        if f.get("render_and_frame_size_different", 0):
            render = (f.get("render_width_minus_1", 0) + 1, f.get("render_height_minus_1", 0) + 1)
        else:
            render = (width, height)
        if intra or error_res:
            state.setup_past_independence()
        if f.get("loop_filter_delta_enabled", 0) and f.get("loop_filter_delta_update", 0):
            for i in range(4):
                if f.get(f"update_ref_delta[{i}]", 0):
                    state.ref_deltas[i] = f.get(f"loop_filter_ref_deltas[{i}]", 0)
            for i in range(2):
                if f.get(f"update_mode_delta[{i}]", 0):
                    state.mode_deltas[i] = f.get(f"loop_filter_mode_deltas[{i}]", 0)
        seg_enabled = f.get("segmentation_enabled", 0)
        seg = {"flags": set()}
        if seg_enabled:
            for name in ("segmentation_update_map", "segmentation_temporal_update", "segmentation_update_data",
                         "segmentation_abs_or_delta_update"):
                if f.get(name, 0):
                    seg["flags"].add(name)
            if f.get("segmentation_update_map", 0):
                seg["segmentation_tree_probs"] = [
                    f.get(f"segmentation_tree_probs[{i}].prob", 255) if f.get(f"segmentation_tree_probs[{i}].prob_coded", 0)
                    else 255 for i in range(7)]
                seg["segmentation_pred_prob"] = [
                    f.get(f"segmentation_pred_prob[{i}].prob", 255) if f.get(f"segmentation_pred_prob[{i}].prob_coded", 0)
                    else 255 for i in range(3)]
            if f.get("segmentation_update_data", 0):
                for i in range(8):
                    for j in range(4):
                        enabled = f.get(f"feature_enabled[{i}][{j}]", 0)
                        value = f.get(f"feature_value[{i}][{j}]", 0)
                        if f.get(f"feature_sign[{i}][{j}]", 0):
                            value = -value
                        state.feature_enabled[i][j] = enabled
                        state.feature_data[i][j] = value if enabled else 0
        # Inter frames take the previous frame's motion vectors when it was shown and has this size, and
        # error resilience is off (as ffmpeg's vp9.c use_last_frame_mvs); intra frames take none.
        use_prev_mvs = int(not intra and state.last_size == (width, height) and not error_res and state.last_show_frame)
        e["std"] = {
            "profile": profile, "frame_type": f.get("frame_type", 0), "frame_context_idx": f.get("frame_context_idx", 0),
            "reset_frame_context": f.get("reset_frame_context", 0) if not error_res else 0,
            "refresh_frame_flags": refresh,
            "ref_frame_sign_bias_mask": sum(f.get(f"ref_frame_sign_bias[{i}]", 0) << i for i in range(1, 4)) if not intra else 0,
            "base_q_idx": f.get("base_q_idx", 0),
            "delta_q_y_dc": delta_q(f, "delta_q_y_dc"), "delta_q_uv_dc": delta_q(f, "delta_q_uv_dc"),
            "delta_q_uv_ac": delta_q(f, "delta_q_uv_ac"),
            "tile_cols_log2": f.get("tile_cols_log2", 0), "tile_rows_log2": f.get("tile_rows_log2", 0),
        }
        if not intra:
            e["std"]["interpolation_filter"] = (SWITCHABLE if f.get("is_filter_switchable", 0)
                                                else LITERAL_TO_TYPE[f.get("raw_interpolation_filter_type", 0)])
        flags = {"show_frame": show_frame, "error_resilient_mode": error_res, "intra_only": intra_only,
                 "allow_high_precision_mv": f.get("allow_high_precision_mv", 0) if not intra else 0,
                 "refresh_frame_context": f.get("refresh_frame_context", 0) if not error_res else 0,
                 "frame_parallel_decoding_mode": f.get("frame_parallel_decoding_mode", 0) if not error_res else 1,
                 "segmentation_enabled": seg_enabled, "UsePrevFrameMvs": use_prev_mvs}
        e["flags"] = flags
        e["size"] = {"FrameWidth": width, "FrameHeight": height, "renderWidth": render[0], "renderHeight": render[1],
                     "FrameIsIntra": int(bool(intra)), "compressedHeaderSize": f.get("header_size_in_bytes", 0)}
        if ref_idx is not None:
            e["size"]["ref_frame_idx"] = ref_idx
        e["color"] = dict(state.color)
        e["lf"] = {"loop_filter_level": f.get("loop_filter_level", 0),
                   "loop_filter_sharpness": f.get("loop_filter_sharpness", 0),
                   "loop_filter_ref_deltas": list(state.ref_deltas), "loop_filter_mode_deltas": list(state.mode_deltas),
                   "loop_filter_delta_enabled": f.get("loop_filter_delta_enabled", 0),
                   "loop_filter_delta_update": f.get("loop_filter_delta_update", 0)}
        delta_update = f.get("loop_filter_delta_enabled", 0) and f.get("loop_filter_delta_update", 0)
        e["lf"]["update_ref_delta"] = sum(f.get(f"update_ref_delta[{i}]", 0) << i for i in range(4)) if delta_update else 0
        e["lf"]["update_mode_delta"] = sum(f.get(f"update_mode_delta[{i}]", 0) << i for i in range(2)) if delta_update else 0
        e["seg"] = seg
        if seg_enabled:
            e["seg"]["FeatureEnabled"] = [sum(state.feature_enabled[i][j] << j for j in range(4)) for i in range(8)]
            e["seg"]["FeatureData"] = [state.feature_data[i][j] for i in range(8) for j in range(4)]
        e["key"] = (f.get("frame_type", 0), intra_only, refresh, f.get("base_q_idx", 0))
        frames.append(e)
        for i in range(8):
            if refresh & (1 << i):
                state.slot_size[i] = (width, height)
        state.last_size = (width, height)
        state.last_show_frame = bool(show_frame)
    return frames


def compare_picture(report, index, e, entry):
    key = f"picture {index}"
    std = entry.one("std") or {}
    vp9 = entry.one("vp9") or {}
    color = entry.one("color") or {}
    lf = entry.one("lf") or {}
    seg = entry.one("seg") or {}
    intra = e["size"]["FrameIsIntra"]
    for name, value in e["std"].items():
        category = "hdr-intra" if intra and name in ("ref_frame_sign_bias_mask", "interpolation_filter") else "hdr"
        report.check(category, key, name, value, std.get(name))
    for name, value in e["flags"].items():
        category = "hdr-intra" if intra and name == "allow_high_precision_mv" else "hdr"
        report.check(category, key, name, value, int(name in std.get("flags", set())))
    for name, value in e["size"].items():
        report.check("hdr-size", key, name, value, vp9.get(name))
    for name, value in e["color"].items():
        actual = int("color_range" in color.get("flags", set())) if name == "color_range" else color.get(name)
        report.check("color", key, name, value, actual)
    for name in ("loop_filter_level", "loop_filter_sharpness", "loop_filter_ref_deltas", "loop_filter_mode_deltas"):
        report.check("lf", key, name, e["lf"][name], lf.get(name))
    for name in ("update_ref_delta", "update_mode_delta"):
        report.check("lf-update", key, name, e["lf"][name], lf.get(name))
    report.check("lf", key, "loop_filter_delta_enabled", e["lf"]["loop_filter_delta_enabled"],
                 int("loop_filter_delta_enabled" in lf.get("flags", set())))
    report.check("lf-update", key, "loop_filter_delta_update", e["lf"]["loop_filter_delta_update"],
                 int("loop_filter_delta_update" in lf.get("flags", set())))
    if e["flags"]["segmentation_enabled"]:
        report.check("seg", key, "flags", e["seg"]["flags"], seg.get("flags", set()))
        for name in ("segmentation_tree_probs", "segmentation_pred_prob", "FeatureEnabled", "FeatureData"):
            if name in e["seg"]:
                report.check("seg", key, name, e["seg"][name], seg.get(name))


def compare(lines, trace_path, verbose=False):
    """Returns (report, note); report is None when the dump has nothing to compare."""
    units = hd.read_units(lines, KINDS)
    if not units:
        return None, "no VP9 frames in the dump"
    frames = model_frames(units)
    entries = hd.read_trace(trace_path)
    report = hd.Report(verbose)
    traced = [t for t in entries if t.tag == "pic"]

    def trace_key(t):
        std = t.one("std") or {}
        return (std.get("frame_type"), int("intra_only" in std.get("flags", set())), std.get("refresh_frame_flags"),
                std.get("base_q_idx"))

    matcher = difflib.SequenceMatcher(a=[e["key"] for e in frames], b=[trace_key(t) for t in traced], autojunk=False)
    for tag, a0, a1, b0, b1 in matcher.get_opcodes():
        if tag == "equal" or (tag == "replace" and a1 - a0 == b1 - b0):
            for i, j in zip(range(a0, a1), range(b0, b1)):
                compare_picture(report, i, frames[i], traced[j])
            continue
        for i in range(a0, a1):
            report.check("picture-count", f"picture {i}", "in the trace", True, False)
        for j in range(b0, b1):
            report.check("picture-count", f"trace picture {j}", "in the model", True, False)
    return report, ""
