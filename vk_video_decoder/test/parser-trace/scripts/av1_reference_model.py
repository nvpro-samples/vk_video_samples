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
"""Checks an AV1 trace from vk-video-parser-trace against the AV1 specification.

Usage:
  av1_reference_model.py --trace T --headers H [--json OUT] [-v]
  av1_reference_model.py --trace T --ivf I [--ffmpeg F] [--save-headers H] [--json OUT] [-v]
  av1_reference_model.py --ivf I --save-headers H        (only write the compact dump)

H is ffmpeg's trace_headers dump (ffmpeg -f ivf -i I -c copy -copyinkf -bsf:v trace_headers -f null -)
of I, the IVF file vk-video-parser-trace --write-ivf wrote for the run that produced T. The dump
has to come from that IVF: it holds exactly the temporal units the parser was given, and ffmpeg's
own OBU and Annex B demuxers drop temporal units of some streams. -copyinkf keeps the packets
before the first one ffmpeg flags as a key frame. Only syntax elements are read from the dump;
--save-headers writes a compact dump with just the packets, the header units and the fields the
model reads, which is what the committed streams carry. From
them this script replays the state that Vulkan's AV1 picture and reference information is defined
by: AV1 5.9.2 (uncompressed_header, including OrderHints and RefFrameSignBias), 5.9.14
(segmentation_params), 7.8 (set_frame_refs), 7.20 (reference frame update) and 7.21 (reference
frame loading). Vulkan defines each StdVideoDecodeAV1ReferenceInfo member by these sections (the
"Std Reference Information" list of the AV1 decode chapter): a reference's information is the
information of the frame stored in that buffer.

Every decoded picture of the trace is compared with the model. Mismatches are counted per category:
  hdr-*      the picture's own header fields; hdr-quant, hdr-loop-filter, hdr-tx-mode, hdr-mode-flags and
             hdr-film-grain compare syntax read late in the header with ffmpeg's reading of it, so a
             misread earlier in the header that shifts the bits shows there
  hdr-frame-size  UpscaledWidth, FrameWidth and FrameHeight (5.9.5 to 5.9.8, with frame_size_with_refs)
  tile-*     the tile information Vulkan's StdVideoAV1TileInfo carries (5.9.15, 6.8.14), and the tile
             offsets and sizes: every tile lies inside the picture's data
  seg-*      the picture's segmentation flags and its effective FeatureEnabled and FeatureData
  setup-*    StdVideoDecodeAV1ReferenceInfo of the picture itself (the setup slot)
  ref-*      StdVideoDecodeAV1ReferenceInfo of each buffer the picture references
Values the specification leaves undefined are not compared: RefFrameSignBias of intra frames,
element 0 (INTRA_FRAME) of SavedOrderHints, and the segmentation update flags of a frame without
segmentation. Buffers whose content the headers do not determine (a
frame replaced after an error-resilient ref_order_hint mismatch) are not compared either.

Exit status: 0 no mismatch, 1 mismatches, 3 not comparable (no headers, or a scalable stream whose
other layers the parser skips).
"""
import argparse
import copy
import json
import re
import subprocess
import sys

KEY_FRAME, INTER_FRAME, INTRA_ONLY_FRAME, SWITCH_FRAME = 0, 1, 2, 3
PRIMARY_REF_NONE = 7
NUM_REF_FRAMES = 8
REFS_PER_FRAME = 7
LAST_FRAME, LAST2_FRAME, LAST3_FRAME, GOLDEN_FRAME, BWDREF_FRAME, ALTREF2_FRAME, ALTREF_FRAME = range(1, 8)
SEG_FEATURE_MAX = [255, 63, 63, 63, 63, 7, 0, 0]
SEG_FEATURE_SIGNED = [1, 1, 1, 1, 1, 0, 0, 0]

HEADING = re.compile(r"^\[trace_headers @ [0-9a-fx]+\] (\D.*?)\s*$")
FIELD = re.compile(r"^\[trace_headers @ [0-9a-fx]+\]\s+\d+\s+(\S+)\s+[01]+\s+=\s+(-?\d+)\s*$")
COMPACT_HEADING = re.compile(r"^== (.+?)\s*$")
COMPACT_FIELD = re.compile(r"^(\S+) (-?\d+)$")
UNITS = ("Sequence Header", "Frame Header", "Redundant Frame Header")

# The syntax elements the model reads; a compact dump (--save-headers) keeps only these.
MODEL_FIELDS = {
    "reduced_still_picture_header", "enable_order_hint", "order_hint_bits_minus_1", "operating_points_cnt_minus_1",
    "show_existing_frame", "frame_to_show_map_idx", "frame_type", "show_frame", "error_resilient_mode",
    "disable_cdf_update", "order_hint", "primary_ref_frame", "refresh_frame_flags", "frame_refs_short_signaling",
    "last_frame_idx", "golden_frame_idx", "disable_frame_end_update_cdf", "segmentation_enabled",
    "segmentation_update_map", "segmentation_temporal_update", "segmentation_update_data",
    "base_q_idx", "diff_uv_delta", "delta_q_y_dc.delta_q", "delta_q_u_dc.delta_q",
    "delta_q_u_ac.delta_q", "delta_q_v_dc.delta_q", "delta_q_v_ac.delta_q", "tx_mode", "reference_select",
    "skip_mode_present", "reduced_tx_set", "allow_warped_motion", "is_motion_mode_switchable",
    "allow_high_precision_mv", "apply_grain", "grain_seed", "update_grain", "num_y_points",
    "max_frame_width_minus_1", "max_frame_height_minus_1", "enable_superres", "use_128x128_superblock",
    "frame_size_override_flag", "frame_width_minus_1", "frame_height_minus_1", "use_superres", "coded_denom",
    "uniform_tile_spacing_flag", "tile_cols_log2", "tile_rows_log2", "context_update_tile_id",
    "tile_size_bytes_minus1",
}
MODEL_FIELD_ARRAYS = ("ref_order_hint[", "ref_frame_idx[", "feature_enabled[", "feature_value[", "loop_filter_level[",
                      "point_y_value[", "found_ref[", "width_in_sbs_minus_1[", "height_in_sbs_minus_1[")
SUPERRES_NUM, SUPERRES_DENOM_MIN = 8, 9


def clip3(lo, hi, value):
    return lo if value < lo else hi if value > hi else value


# ---- ffmpeg trace_headers --------------------------------------------------------------------

def read_header_units(lines):
    """Yields (packet index, kind, fields) for every OBU header section of the dump. Reads ffmpeg's
    trace_headers output and the compact form compact_headers() writes."""
    packet = -1
    unit = None
    for line in lines:
        m = FIELD.match(line) or COMPACT_FIELD.match(line)
        if m:
            if unit is not None:
                unit[2][m.group(1)] = int(m.group(2))
            continue
        m = HEADING.match(line) or COMPACT_HEADING.match(line)
        if not m:
            continue
        text = m.group(1)
        if unit is not None:
            yield unit
            unit = None
        if text.startswith("Packet"):
            packet += 1
        elif text in UNITS:
            unit = (packet, text, {})
    if unit is not None:
        yield unit


def compact_headers(lines):
    """The packets and header units of an ffmpeg dump, with only the fields the model reads."""
    out = ["# AV1 syntax elements read by av1_reference_model.py, from ffmpeg's trace_headers dump",
           "# (ffmpeg -f ivf -i <ivf> -c copy -copyinkf -bsf:v trace_headers -f null -)"]
    packet = -1
    for unit_packet, kind, fields in read_header_units(lines):
        while packet < unit_packet:
            out.append("== Packet")
            packet += 1
        out.append(f"== {kind}")
        out.extend(f"{name} {value}" for name, value in fields.items()
                   if name in MODEL_FIELDS or name.startswith(MODEL_FIELD_ARRAYS))
    # Packets after the last header unit carry no header, but they still count.
    total = sum(1 for line in lines if HEADING.match(line) and HEADING.match(line).group(1).startswith("Packet"))
    while packet < total - 1:
        out.append("== Packet")
        packet += 1
    return "\n".join(out) + "\n"


def run_ffmpeg(ffmpeg, ivf):
    cmd = [ffmpeg, "-hide_banner", "-nostdin", "-f", "ivf", "-i", ivf, "-c", "copy", "-copyinkf", "-bsf:v",
           "trace_headers", "-f", "null", "-"]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return result.returncode, result.stdout.splitlines()


# ---- the model ---------------------------------------------------------------------------------

class Buffer:
    """The saved state of one reference buffer (7.20)."""

    def __init__(self):
        self.known = True          # the headers determine this buffer's content
        self.frame_type = KEY_FRAME
        self.order_hint = 0
        self.saved_order_hints = [0] * NUM_REF_FRAMES
        self.sign_bias = None      # RefFrameSignBias bitmask; None for intra frames (undefined)
        self.segmentation_enabled = 0
        self.disable_frame_end_update_cdf = 0
        self.feature_enabled = [[0] * 8 for _ in range(8)]
        self.feature_data = [[0] * 8 for _ in range(8)]
        self.written = False       # a frame has been stored here
        self.upscaled_width = 0    # RefUpscaledWidth
        self.frame_height = 0      # RefFrameHeight

    def copy(self):
        return copy.deepcopy(self)


class Model:
    def __init__(self):
        self.seq = {}
        self.buffers = [Buffer() for _ in range(NUM_REF_FRAMES)]
        self.ref_order_hint = [0] * NUM_REF_FRAMES      # RefOrderHint[]
        self.order_hints = [0] * NUM_REF_FRAMES         # OrderHints[], kept across frames as in 5.9.2
        self.scalable = False

    def order_hint_bits(self):
        return self.seq.get("order_hint_bits_minus_1", 0) + 1 if self.seq.get("enable_order_hint", 0) else 0

    def get_relative_dist(self, a, b):
        if not self.seq.get("enable_order_hint", 0):
            return 0
        bits = self.order_hint_bits()
        diff = a - b
        m = 1 << (bits - 1)
        return (diff & (m - 1)) - (diff & m)

    def sequence_header(self, fields):
        self.seq = fields
        if fields.get("operating_points_cnt_minus_1", 0) > 0:
            self.scalable = True

    def set_frame_refs(self, order_hint, last_frame_idx, gold_frame_idx):
        """7.8: the reference indices of a frame with frame_refs_short_signaling."""
        ref_frame_idx = [-1] * REFS_PER_FRAME
        ref_frame_idx[LAST_FRAME - LAST_FRAME] = last_frame_idx
        ref_frame_idx[GOLDEN_FRAME - LAST_FRAME] = gold_frame_idx
        used = [False] * NUM_REF_FRAMES
        used[last_frame_idx] = True
        used[gold_frame_idx] = True
        cur_frame_hint = 1 << (self.order_hint_bits() - 1)
        shifted = [cur_frame_hint + self.get_relative_dist(self.ref_order_hint[i], order_hint)
                   for i in range(NUM_REF_FRAMES)]

        def find(backward, latest):
            ref, best = -1, 0
            for i in range(NUM_REF_FRAMES):
                hint = shifted[i]
                if used[i] or (hint >= cur_frame_hint) != backward:
                    continue
                if ref < 0 or (hint >= best if latest else hint < best):
                    ref, best = i, hint
            return ref

        ref = find(True, True)                     # find_latest_backward
        if ref >= 0:
            ref_frame_idx[ALTREF_FRAME - LAST_FRAME] = ref
            used[ref] = True
        ref = find(True, False)                    # find_earliest_backward
        if ref >= 0:
            ref_frame_idx[BWDREF_FRAME - LAST_FRAME] = ref
            used[ref] = True
        ref = find(True, False)
        if ref >= 0:
            ref_frame_idx[ALTREF2_FRAME - LAST_FRAME] = ref
            used[ref] = True
        for ref_frame in (LAST2_FRAME, LAST3_FRAME, BWDREF_FRAME, ALTREF2_FRAME, ALTREF_FRAME):
            if ref_frame_idx[ref_frame - LAST_FRAME] < 0:
                ref = find(False, True)            # find_latest_forward
                if ref >= 0:
                    ref_frame_idx[ref_frame - LAST_FRAME] = ref
                    used[ref] = True
        ref, earliest = -1, 0
        for i in range(NUM_REF_FRAMES):
            hint = shifted[i]
            if ref < 0 or hint < earliest:
                ref, earliest = i, hint
        return [ref if r < 0 else r for r in ref_frame_idx]

    def frame_header(self, f):
        """Returns the expected values of a decoded frame, or None for show_existing_frame."""
        seq = self.seq
        reduced = seq.get("reduced_still_picture_header", 0)
        if not reduced and f.get("show_existing_frame", 0):
            idx = f["frame_to_show_map_idx"]
            shown = self.buffers[idx]
            if shown.frame_type == KEY_FRAME:
                # 7.21 loads the shown frame, then 7.20 stores it in every buffer.
                self.order_hints = list(shown.saved_order_hints)
                for i in range(NUM_REF_FRAMES):
                    self.buffers[i] = shown.copy()
                    self.ref_order_hint[i] = shown.order_hint
            return None

        frame_type = KEY_FRAME if reduced else f["frame_type"]
        show_frame = 1 if reduced else f["show_frame"]
        intra = frame_type in (KEY_FRAME, INTRA_ONLY_FRAME)
        if frame_type == KEY_FRAME and show_frame:
            for i in range(NUM_REF_FRAMES):
                self.ref_order_hint[i] = 0
            for i in range(REFS_PER_FRAME):
                self.order_hints[LAST_FRAME + i] = 0
        disable_cdf_update = f.get("disable_cdf_update", 0)
        if reduced or frame_type == SWITCH_FRAME or (frame_type == KEY_FRAME and show_frame):
            error_resilient_mode = 1
        else:
            error_resilient_mode = f.get("error_resilient_mode", 0)
        order_hint = f.get("order_hint", 0) if self.order_hint_bits() else 0
        if intra or error_resilient_mode:
            primary_ref_frame = PRIMARY_REF_NONE
        else:
            primary_ref_frame = f.get("primary_ref_frame", PRIMARY_REF_NONE)
        if frame_type == SWITCH_FRAME or (frame_type == KEY_FRAME and show_frame):
            refresh = 0xFF
        else:
            refresh = f.get("refresh_frame_flags", 0)

        if (not intra or refresh != 0xFF) and error_resilient_mode and seq.get("enable_order_hint", 0):
            for i in range(NUM_REF_FRAMES):
                hint = f.get(f"ref_order_hint[{i}]")
                if hint is not None and hint != self.ref_order_hint[i]:
                    self.ref_order_hint[i] = hint
                    replaced = Buffer()
                    replaced.known = False
                    replaced.order_hint = hint
                    self.buffers[i] = replaced

        ref_frame_idx = None
        sign_bias = None
        refs_known = True
        if not intra:
            if f.get("frame_refs_short_signaling", 0):
                ref_frame_idx = self.set_frame_refs(order_hint, f["last_frame_idx"], f["golden_frame_idx"])
            else:
                ref_frame_idx = [f[f"ref_frame_idx[{i}]"] for i in range(REFS_PER_FRAME)]
            sign_bias = 0
            for i in range(REFS_PER_FRAME):
                hint = self.ref_order_hint[ref_frame_idx[i]]
                self.order_hints[LAST_FRAME + i] = hint
                if seq.get("enable_order_hint", 0) and self.get_relative_dist(hint, order_hint) > 0:
                    sign_bias |= 1 << (LAST_FRAME + i)
                refs_known = refs_known and self.buffers[ref_frame_idx[i]].known

        upscaled_width, frame_width, frame_height = self.frame_size(f, reduced, frame_type, error_resilient_mode,
                                                                    ref_frame_idx)

        if reduced or disable_cdf_update:
            disable_frame_end_update_cdf = 1
        else:
            disable_frame_end_update_cdf = f.get("disable_frame_end_update_cdf", 0)

        # load_previous() or setup_past_independence(), then segmentation_params() (5.9.14).
        seg_known = True
        if primary_ref_frame == PRIMARY_REF_NONE:
            fe = [[0] * 8 for _ in range(8)]
            fd = [[0] * 8 for _ in range(8)]
        else:
            prev = self.buffers[ref_frame_idx[primary_ref_frame]]
            fe = [list(row) for row in prev.feature_enabled]
            fd = [list(row) for row in prev.feature_data]
            seg_known = prev.known
        segmentation_enabled = f.get("segmentation_enabled", 0)
        update_map = temporal_update = update_data = 0
        if segmentation_enabled:
            if primary_ref_frame == PRIMARY_REF_NONE:
                update_map, temporal_update, update_data = 1, 0, 1
            else:
                update_map = f.get("segmentation_update_map", 0)
                temporal_update = f.get("segmentation_temporal_update", 0) if update_map else 0
                update_data = f.get("segmentation_update_data", 0)
            if update_data:
                seg_known = True
                for i in range(8):
                    for j in range(8):
                        enabled = f.get(f"feature_enabled[{i}][{j}]", 0)
                        value = f.get(f"feature_value[{i}][{j}]", 0) if enabled else 0
                        limit = SEG_FEATURE_MAX[j]
                        fe[i][j] = enabled
                        fd[i][j] = clip3(-limit if SEG_FEATURE_SIGNED[j] else 0, limit, value) if enabled else 0
        else:
            seg_known = True
            fe = [[0] * 8 for _ in range(8)]
            fd = [[0] * 8 for _ in range(8)]

        expected = {
            "frame_type": frame_type,
            "show_frame": show_frame,
            "intra": intra,
            "OrderHint": order_hint,
            "primary_ref_frame": primary_ref_frame,
            "refresh_frame_flags": refresh,
            "ref_frame_idx": ref_frame_idx,
            "OrderHints": list(self.order_hints),
            "refs_known": refs_known,
            "error_resilient_mode": error_resilient_mode,
            "disable_cdf_update": disable_cdf_update,
            "disable_frame_end_update_cdf": disable_frame_end_update_cdf,
            "segmentation_enabled": segmentation_enabled,
            "segmentation_update_map": update_map,
            "segmentation_temporal_update": temporal_update,
            "segmentation_update_data": update_data,
            "seg_known": seg_known,
            "FeatureEnabled": [sum(bit << j for j, bit in enumerate(row)) for row in fe],
            "FeatureData": [v for row in fd for v in row],
            "sign_bias": sign_bias,
            "references": {},
        }
        expected.update(self.header_tail(f, intra))
        expected["frame_size"] = [upscaled_width, frame_width, frame_height]
        expected["tiles"] = self.tile_info(f, frame_width, frame_height)
        if not intra:
            for idx in sorted(set(ref_frame_idx)):
                expected["references"][idx] = self.buffers[idx]

        # 7.20: the reference frame update process.
        stored = Buffer()
        stored.known = refs_known if not intra else True
        stored.frame_type = frame_type
        stored.order_hint = order_hint
        stored.saved_order_hints = list(self.order_hints)
        stored.sign_bias = sign_bias
        stored.segmentation_enabled = segmentation_enabled
        stored.disable_frame_end_update_cdf = disable_frame_end_update_cdf
        stored.feature_enabled = fe
        stored.feature_data = fd
        stored.written = True
        stored.upscaled_width = upscaled_width
        stored.frame_height = frame_height
        for i in range(NUM_REF_FRAMES):
            if refresh & (1 << i):
                self.buffers[i] = stored.copy()
                self.ref_order_hint[i] = order_hint
        return expected


    def frame_size(self, f, reduced, frame_type, error_resilient_mode, ref_frame_idx):
        """Returns (UpscaledWidth, FrameWidth, FrameHeight): frame_size_with_refs() (5.9.7) for an inter
        frame that overrides its size outside error resilient mode, frame_size() (5.9.5) otherwise, then
        superres_params() (5.9.8)."""
        seq = self.seq
        if reduced:
            override = 0
        elif frame_type == SWITCH_FRAME:
            override = 1
        else:
            override = f.get("frame_size_override_flag", 0)
        upscaled = height = None
        if ref_frame_idx is not None and override and not error_resilient_mode:
            for i in range(REFS_PER_FRAME):
                if f.get(f"found_ref[{i}]", 0):
                    buf = self.buffers[ref_frame_idx[i]]
                    upscaled, height = buf.upscaled_width, buf.frame_height
                    break
        if upscaled is None:
            if override:
                upscaled, height = f.get("frame_width_minus_1", 0) + 1, f.get("frame_height_minus_1", 0) + 1
            else:
                upscaled = seq.get("max_frame_width_minus_1", 0) + 1
                height = seq.get("max_frame_height_minus_1", 0) + 1
        denom = SUPERRES_NUM
        if seq.get("enable_superres", 0) and f.get("use_superres", 0):
            denom = f.get("coded_denom", 0) + SUPERRES_DENOM_MIN
        return upscaled, (upscaled * SUPERRES_NUM + denom // 2) // denom, height

    def tile_info(self, f, frame_width, frame_height):
        """tile_info() (5.9.15): the tile layout in superblocks, and MiColStarts and MiRowStarts in mode
        info units (6.8.14), the units Vulkan's pMiColStarts and pMiRowStarts take."""
        mi_cols = 2 * ((frame_width + 7) >> 3)
        mi_rows = 2 * ((frame_height + 7) >> 3)
        sb_shift = 5 if self.seq.get("use_128x128_superblock", 0) else 4
        sb_cols = (mi_cols + (1 << sb_shift) - 1) >> sb_shift
        sb_rows = (mi_rows + (1 << sb_shift) - 1) >> sb_shift

        def starts(log2_name, sizes_name, sb_count):
            if f.get("uniform_tile_spacing_flag", 0):
                log2 = f.get(log2_name, 0)
                size = (sb_count + (1 << log2) - 1) >> log2
                return list(range(0, sb_count, size)), log2
            # A size coded as ns(1), one superblock left, takes no bits and is 0; ffmpeg prints nothing.
            result, start = [], 0
            while start < sb_count:
                start_count = len(result)
                result.append(start)
                start += f.get(f"{sizes_name}[{start_count}]", 0) + 1
            return result, (len(result) - 1).bit_length()     # tile_log2(1, count)

        col_starts, cols_log2 = starts("tile_cols_log2", "width_in_sbs_minus_1", sb_cols)
        row_starts, rows_log2 = starts("tile_rows_log2", "height_in_sbs_minus_1", sb_rows)
        coded = cols_log2 + rows_log2 > 0
        return {
            "TileCols": len(col_starts),
            "TileRows": len(row_starts),
            "MiColStarts": [start << sb_shift for start in col_starts],
            "MiRowStarts": [start << sb_shift for start in row_starts],
            "width_in_sbs_minus_1": [end - start - 1 for start, end in zip(col_starts, col_starts[1:] + [sb_cols])],
            "height_in_sbs_minus_1": [end - start - 1 for start, end in zip(row_starts, row_starts[1:] + [sb_rows])],
            "context_update_tile_id": f.get("context_update_tile_id", 0) if coded else 0,
            # Coded only with more than one tile; a single-tile frame leaves it undefined.
            "tile_size_bytes_minus_1": f.get("tile_size_bytes_minus1") if coded else None,
        }

    def header_tail(self, f, intra):
        """Syntax read after the segmentation parameters, as ffmpeg read it. An element the header does
        not code takes the value the specification infers for it."""
        y_dc = f.get("delta_q_y_dc.delta_q", 0)
        u_dc = f.get("delta_q_u_dc.delta_q", 0)
        u_ac = f.get("delta_q_u_ac.delta_q", 0)
        if f.get("diff_uv_delta", 0):
            v_dc, v_ac = f.get("delta_q_v_dc.delta_q", 0), f.get("delta_q_v_ac.delta_q", 0)
        else:
            v_dc, v_ac = u_dc, u_ac
        tail = {
            "quant": [f.get("base_q_idx", 0), y_dc, u_dc, u_ac, v_dc, v_ac],
            # read_tx_mode: ffmpeg codes TX_MODE_LARGEST 1 and TX_MODE_SELECT 2, and infers ONLY_4X4 0 for a
            # CodedLossless frame, which codes neither loop filter levels nor tx_mode_select.
            "TxMode": f.get("tx_mode", 0),
            "loop_filter_level": [f.get(f"loop_filter_level[{i}]", 0) for i in range(4)],
            "mode_flags": {name: f.get(name, 0) for name in
                           ("reference_select", "skip_mode_present", "reduced_tx_set", "allow_warped_motion")},
            "apply_grain": f.get("apply_grain", 0),
            "grain_seed": f.get("grain_seed"),
            "point_y_value": None,
        }
        if not intra:
            # Coded for inter frames only.
            for name in ("is_motion_mode_switchable", "allow_high_precision_mv"):
                tail["mode_flags"][name] = f.get(name, 0)
        if "num_y_points" in f:
            tail["point_y_value"] = [f.get(f"point_y_value[{i}]", 0) for i in range(f["num_y_points"])]
        return tail


def model_frames(lines):
    """Returns (frames, scalable): the expected values of every decoded frame, keyed by
    (packet index, decoded frame number within the packet)."""
    model = Model()
    frames = {}
    per_packet = {}
    for packet, kind, fields in read_header_units(lines):
        if kind == "Sequence Header":
            model.sequence_header(fields)
        elif kind == "Frame Header":
            expected = model.frame_header(fields)
            if expected is not None:
                n = per_packet.get(packet, 0)
                per_packet[packet] = n + 1
                frames[(packet, n)] = expected
    return frames, model.scalable


# ---- the trace ---------------------------------------------------------------------------------

def parse_value(text):
    if text.startswith("["):
        inner = text[1:-1]
        return [int(v, 0) for v in inner.split(",")] if inner else []
    try:
        return int(text, 0)
    except ValueError:
        return set() if text == "-" else set(text.split("|"))


def parse_line(line):
    tokens = line.split()
    fields = {}
    for token in tokens[1:]:
        key, _, value = token.partition("=")
        fields[key] = parse_value(value)
    return tokens[0], fields


def trace_pictures(path):
    """Returns the trace's pictures keyed by (packet index, picture number within the packet)."""
    pictures = {}
    packet = -1
    count = {}
    current = None
    with open(path) as trace:
        for line in trace:
            if not line.strip():
                continue
            tag, fields = parse_line(line)
            if not line.startswith("  "):
                current = None
            if tag == "pkt":
                packet = fields["index"]
            elif tag == "pic":
                n = count.get(packet, 0)
                count[packet] = n + 1
                current = {"pic": fields, "slots": {}}
                pictures[(packet, n)] = current
            elif current is not None:
                if tag == "slot":
                    current["slots"][fields["i"]] = fields
                else:
                    current[tag] = fields
    return pictures


# ---- comparison --------------------------------------------------------------------------------

class Report:
    def __init__(self, verbose):
        self.categories = {}
        self.verbose = verbose

    def check(self, category, key, field, expected, actual):
        if expected == actual:
            return
        entry = self.categories.setdefault(category, {"pictures": [], "first": None})
        if key not in entry["pictures"]:
            entry["pictures"].append(key)
        if entry["first"] is None:
            entry["first"] = f"packet {key[0]} picture {key[1]}: {field} expected {expected} got {actual}"
        if self.verbose:
            print(f"  packet {key[0]} picture {key[1]} {category}: {field} expected {expected} got {actual}")


def compare_tiles(report, key, exp, pic):
    tiles = pic["tiles"]
    cols, rows = exp["TileCols"], exp["TileRows"]
    report.check("tile-count", key, "TileCols, TileRows", [cols, rows], [tiles["TileCols"], tiles["TileRows"]])
    report.check("tile-count", key, "tileCount", cols * rows, pic["av1"]["tileCount"])
    report.check("tile-sizes", key, "width_in_sbs_minus_1", exp["width_in_sbs_minus_1"],
                 tiles["width_in_sbs_minus_1"][:cols])
    report.check("tile-sizes", key, "height_in_sbs_minus_1", exp["height_in_sbs_minus_1"],
                 tiles["height_in_sbs_minus_1"][:rows])
    report.check("tile-starts", key, "MiColStarts", exp["MiColStarts"], tiles["MiColStarts"][:cols])
    report.check("tile-starts", key, "MiRowStarts", exp["MiRowStarts"], tiles["MiRowStarts"][:rows])
    report.check("tile-context", key, "context_update_tile_id", exp["context_update_tile_id"],
                 tiles["context_update_tile_id"])
    if exp["tile_size_bytes_minus_1"] is not None:
        report.check("tile-context", key, "tile_size_bytes_minus_1", exp["tile_size_bytes_minus_1"],
                     tiles["tile_size_bytes_minus_1"])
    # Every tile the parser hands over lies inside the picture's data, after the one before it.
    end = 0
    for i, (offset, size) in enumerate(zip(pic["av1"]["tileOffsets"], pic["av1"]["tileSizes"])):
        if offset < end or offset + size > pic["pic"]["bitstreamDataLen"]:
            report.check("tile-bounds", key, f"tile {i} [offset, size]", f"within [{end}, {pic['pic']['bitstreamDataLen']}]",
                         [offset, size])
            break
        end = offset + size


def compare(frames, pictures, verbose):
    report = Report(verbose)
    for key in sorted(set(frames) | set(pictures)):
        exp = frames.get(key)
        pic = pictures.get(key)
        if exp is None or pic is None:
            report.check("picture-count", key, "picture", "present" if exp else "absent",
                         "present" if pic else "absent")
            continue
        std = pic["std"]
        flags = std["flags"]
        av1 = pic["av1"]
        report.check("hdr-frame-type", key, "frame_type", exp["frame_type"], std["frame_type"])
        report.check("hdr-order-hint", key, "OrderHint", exp["OrderHint"], std["OrderHint"])
        report.check("hdr-primary-ref-frame", key, "primary_ref_frame", exp["primary_ref_frame"],
                     std["primary_ref_frame"])
        report.check("hdr-refresh-frame-flags", key, "refresh_frame_flags", exp["refresh_frame_flags"],
                     std["refresh_frame_flags"])
        for name in ("error_resilient_mode", "disable_cdf_update", "disable_frame_end_update_cdf"):
            report.check("hdr-flags", key, name, exp[name], int(name in flags))
        if not exp["intra"]:
            report.check("hdr-ref-frame-idx", key, "ref_frame_idx", exp["ref_frame_idx"], av1["ref_frame_idx"])
            if exp["refs_known"]:
                report.check("hdr-order-hints", key, "OrderHints[1..7]", exp["OrderHints"][1:], std["OrderHints"][1:])

        quant = pic["quant"]
        report.check("hdr-quant", key, "base_q_idx and DeltaQ", exp["quant"],
                     [quant[name] for name in ("base_q_idx", "DeltaQYDc", "DeltaQUDc", "DeltaQUAc", "DeltaQVDc",
                                               "DeltaQVAc")])
        report.check("hdr-loop-filter", key, "loop_filter_level", exp["loop_filter_level"],
                     pic["lf"]["loop_filter_level"])
        report.check("hdr-tx-mode", key, "TxMode", exp["TxMode"], std["TxMode"])
        for name, value in exp["mode_flags"].items():
            report.check("hdr-mode-flags", key, name, value, int(name in flags))
        report.check("hdr-film-grain", key, "apply_grain", exp["apply_grain"], int("apply_grain" in flags))
        if exp["apply_grain"]:
            report.check("hdr-film-grain", key, "grain_seed", exp["grain_seed"], pic["fg"]["grain_seed"])
            if exp["point_y_value"] is not None:
                n = len(exp["point_y_value"])
                report.check("hdr-film-grain", key, "point_y_value", exp["point_y_value"],
                             pic["fg"]["point_y_value"][:n] if pic["fg"]["num_y_points"] == n else
                             f"num_y_points {pic['fg']['num_y_points']}")

        report.check("hdr-frame-size", key, "UpscaledWidth, FrameWidth, FrameHeight", exp["frame_size"],
                     [av1["upscaled_width"], av1["frame_width"], av1["frame_height"]])
        compare_tiles(report, key, exp["tiles"], pic)

        report.check("seg-flags", key, "segmentation_enabled", exp["segmentation_enabled"],
                     int("segmentation_enabled" in flags))
        if exp["segmentation_enabled"]:
            # Without segmentation the update flags are absent from the syntax and undefined.
            for name in ("segmentation_update_map", "segmentation_temporal_update", "segmentation_update_data"):
                report.check("seg-flags", key, name, exp[name], int(name in flags))
        if exp["segmentation_enabled"] and exp["seg_known"]:
            report.check("seg-feature-enabled", key, "FeatureEnabled", exp["FeatureEnabled"],
                         pic["seg"]["FeatureEnabled"])
            report.check("seg-feature-data", key, "FeatureData", exp["FeatureData"], pic["seg"]["FeatureData"])

        setup = pic["setup"]
        setup_flags = setup["flags"]
        kind = "intra" if exp["intra"] else "inter"
        report.check("setup-frame-type", key, "setup frame_type", exp["frame_type"], setup["frame_type"])
        report.check("setup-order-hint", key, "setup OrderHint", exp["OrderHint"], setup["OrderHint"])
        if exp["refs_known"]:
            report.check(f"setup-saved-order-hints-{kind}", key, "setup SavedOrderHints[1..7]", exp["OrderHints"][1:],
                         setup["SavedOrderHints"][1:])
        if exp["sign_bias"] is not None and exp["refs_known"]:
            report.check("setup-sign-bias", key, "setup RefFrameSignBias", hex(exp["sign_bias"]),
                         hex(setup["RefFrameSignBias"]))
        report.check("setup-segmentation-enabled", key, "setup segmentation_enabled", exp["segmentation_enabled"],
                     int("segmentation_enabled" in setup_flags))
        report.check("setup-disable-frame-end-update-cdf", key, "setup disable_frame_end_update_cdf",
                     exp["disable_frame_end_update_cdf"], int("disable_frame_end_update_cdf" in setup_flags))

        for idx, buf in exp["references"].items():
            slot = pic["slots"].get(idx)
            if slot is None or not buf.known or not buf.written:
                continue
            where = f"slot {idx}"
            ref_kind = "intra" if buf.frame_type in (KEY_FRAME, INTRA_ONLY_FRAME) else "inter"
            report.check("ref-frame-type", key, f"{where} frame_type", buf.frame_type, slot["frame_type"])
            report.check("ref-order-hint", key, f"{where} OrderHint", buf.order_hint, slot["OrderHint"])
            report.check(f"ref-saved-order-hints-{ref_kind}", key, f"{where} SavedOrderHints[1..7]",
                         buf.saved_order_hints[1:], slot["SavedOrderHints"][1:])
            if buf.sign_bias is not None:
                report.check("ref-sign-bias", key, f"{where} RefFrameSignBias", hex(buf.sign_bias),
                             hex(slot["RefFrameSignBias"]))
            report.check("ref-segmentation-enabled", key, f"{where} segmentation_enabled", buf.segmentation_enabled,
                         int("segmentation_enabled" in slot["flags"]))
            report.check("ref-disable-frame-end-update-cdf", key, f"{where} disable_frame_end_update_cdf",
                         buf.disable_frame_end_update_cdf, int("disable_frame_end_update_cdf" in slot["flags"]))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--trace", help="the trace to check; without it, --ivf and --save-headers only write the dump")
    parser.add_argument("--headers", help="an ffmpeg trace_headers dump, or a compact one from --save-headers")
    parser.add_argument("--ivf")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--save-headers", help="with --ivf, write the compact dump here")
    parser.add_argument("--json")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    result = {"trace": args.trace, "status": "compared", "categories": {}}
    if args.headers:
        with open(args.headers, errors="replace") as dump:
            lines = dump.read().splitlines()
    elif args.ivf:
        rc, lines = run_ffmpeg(args.ffmpeg, args.ivf)
        if rc != 0:
            print(f"ffmpeg exited {rc}", file=sys.stderr)
            return 3
        if args.save_headers:
            with open(args.save_headers, "w") as out:
                out.write(compact_headers(lines))
    else:
        parser.error("give --headers or --ivf")
    if not args.trace:
        if not args.save_headers:
            parser.error("give --trace, or --ivf with --save-headers")
        return 0

    status = 3
    if result["status"] == "compared":
        frames, scalable = model_frames(lines)
        pictures = trace_pictures(args.trace)
        if scalable:
            result.update(status="not-comparable", reason="scalable stream (operating_points_cnt_minus_1 > 0)")
        elif not frames:
            result.update(status="not-comparable", reason="no frame headers in the dump")
        else:
            report = compare(frames, pictures, args.verbose)
            result["pictures"] = len(pictures)
            result["frames"] = len(frames)
            result["categories"] = {name: {"pictures": len(entry["pictures"]), "first": entry["first"]}
                                    for name, entry in sorted(report.categories.items())}
            status = 1 if report.categories else 0

    if result["status"] != "compared":
        print(f"not comparable: {result['reason']}")
    else:
        print(f"pictures {result['pictures']} model frames {result['frames']}")
        for name, entry in result["categories"].items():
            print(f"category {name} {entry['pictures']} first: {entry['first']}")
    if args.json:
        with open(args.json, "w") as out:
            json.dump(result, out, indent=1, sort_keys=True)
    return status


if __name__ == "__main__":
    sys.exit(main())
