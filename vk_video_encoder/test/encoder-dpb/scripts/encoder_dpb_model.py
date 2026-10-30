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
"""Reference picture model of the encoder DPB trace.

Usage: encoder_dpb_model.py --trace T [-v]

Replays over a vk-video-encoder-dpb-trace trace the reference picture marking and list
initialization the codec specifies, from the picture types, POCs and frame numbers the trace
records, and checks the encoder's decisions against them:

  H.264: the sliding window (8.2.5.3) with max_num_ref_frames, an IDR emptying the DPB, and the
         initial reference lists (8.2.4.2): P by descending PicNum, B list 0 by descending POC
         below the current picture then ascending above it, list 1 the other way round, with the
         first two entries of list 1 swapped when it has more than one and equals list 0.
  H.265: the reference picture set of the picture (7.4.8, 8.3.2) marks every other picture unused,
         and the initial lists (8.3.4) cycle StCurrBefore, StCurrAfter, LtCurr (list 1: StCurrAfter,
         StCurrBefore, LtCurr) to the active count.
  AV1:   the eight reference buffers: refresh_frame_flags stores the frame in the buffers it names
         (all of them for a shown key frame), showing an existing key frame loads it into every
         buffer (7.20, 7.21), and an inter frame's reference names take their frames from the
         buffers ref_frame_idx names.

The encoder chooses each list's active count (num_ref_idx_lX_active_minus1, with the override flag
when it is not the PPS default), and uses no list modification, so each list it writes must be
the initial list cut to that count; an H.264 list longer than the initial list names no picture.

Categories:
  list0, list1    the encoder's list is not the start of the initial list, or (H.264) is longer
  ref-unknown     a reference slot holds no picture the model holds as a reference, or holds
                  another one
  ref-poc         the reference's PicOrderCnt (H.264) or PicOrderCntVal (H.265) is not its picture's
  ref-frame-num   H.264: the reference's FrameNum is not its picture's frame_num
  ref-pic-type    the reference's primary_pic_type (H.264) or pic_type (H.265) is not its picture's
  ref-unused      H.265: a reference flagged unused_for_reference
  setup-info      the setup slot's reference information is not the picture's: H.264 PicOrderCnt,
                  FrameNum and primary_pic_type; H.265 PicOrderCntVal, pic_type, and
                  unused_for_reference for a picture that is not a reference; AV1 frame_type,
                  OrderHint and RefFrameId
  rps-missing     H.265: the reference picture set names a POC no reference picture has
  rps-slots       H.265: the encoder's RPS slots are not the pictures of the reference picture set
  slot-reuse      the setup slot holds a picture that stays a reference
  slot-range      a slot at or above the DPB size, or at or above the device's maxDpbSlots
  AV1:
  ref-name-slot   a reference name's slot is not the slot of the frame in the buffer its
                  ref_frame_idx names, or that buffer holds no frame
  ref-order-hint  ref_order_hint[i] is not the order hint of the frame in buffer i
  names-unlisted  a reference name's slot is not among the slots the encode binds
  ref-unnamed     a slot the encode binds that no reference name uses
  ref-info        a reference's frame_type, OrderHint or RefFrameId is not its frame's
  refresh         a shown key frame that does not refresh every buffer
  show-existing   show_existing_frame shows a buffer that holds no frame, or another frame
  ref-count       an inter frame binds more references than the session holds active, or names
                  none, more than the prediction mode the DPB plans for its type takes (of
                  group 1, of group 2, or in all), or a name outside that mode's name mask. The
                  DPB plans bidirectional compound for a B picture, else unidirectional compound
                  from group 1, else single reference; a primary reference frame used for its
                  CDFs only is bound, not named
  primary-ref     the primary reference frame's name has no slot, or the primary reference frame
                  is used for its CDFs only on a device without
                  VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR (the config
                  line's cdf_only_primary=0)

Exit status: 0 no mismatch, 1 a mismatch, 2 not comparable.
"""
import argparse
import sys

H264_TYPES = {"IDR": 5, "I": 2, "P": 0, "B": 1}  # StdVideoH264PictureType
H265_TYPES = {"IDR": 3, "I": 2, "P": 0, "B": 1}  # StdVideoH265PictureType


def parse_value(text):
    if text.startswith("["):
        inner = text[1:-1]
        return [int(v) for v in inner.split(",")] if inner else []
    try:
        return int(text, 0)
    except ValueError:
        return text


class Entry:
    def __init__(self, tag, fields):
        self.tag = tag
        self.fields = fields
        self.sub = []  # (tag, fields) in order

    def one(self, tag):
        for sub_tag, fields in self.sub:
            if sub_tag == tag:
                return fields
        return None

    def all(self, tag):
        return [fields for sub_tag, fields in self.sub if sub_tag == tag]


def read_trace(path):
    entries = []
    with open(path) as trace:
        for line in trace:
            if not line.strip():
                continue
            tokens = line.split()
            fields = {}
            for token in tokens[1:]:
                key, _, value = token.partition("=")
                fields[key] = parse_value(value)
            if line.startswith("  ") and entries:
                entries[-1].sub.append((tokens[0], fields))
            else:
                entries.append(Entry(tokens[0], fields))
    return entries


def flags(fields):
    value = fields.get("flags", "-")
    return set() if value == "-" else set(str(value).split("|"))


class Report:
    def __init__(self, verbose):
        self.categories = {}
        self.verbose = verbose

    def fail(self, category, key, message):
        entry = self.categories.setdefault(category, {"pictures": [], "first": None})
        if key not in entry["pictures"]:
            entry["pictures"].append(key)
        if entry["first"] is None:
            entry["first"] = f"{key}: {message}"
        if self.verbose:
            print(f"  {key} {category}: {message}")


class Picture:
    def __init__(self, pic, kind, poc, slot, frame_num=None, frame_id=None):
        self.pic = pic
        self.kind = kind
        self.poc = poc
        self.slot = slot
        self.frame_num = frame_num
        self.frame_id = frame_id


def compare_list(report, key, name, actual, expected):
    """The encoder's list (pictures) against the initial list cut to the encoder's active count."""
    if len(actual) > len(expected):
        report.fail(name, key, f"{actual} is longer than the initial list {expected}")
    elif actual != expected[:len(actual)]:
        report.fail(name, key, f"{actual} is not the start of the initial list {expected}")


class H264Model:
    def __init__(self, config, report):
        self.report = report
        self.dpb_size = min(config["dpbSize"], config["max_dpb_slots"])
        self.max_num_ref_frames = config["max_num_ref_frames"]
        self.max_frame_num = 1 << config["log2_max_frame_num"]
        self.refs = []  # short-term reference pictures, oldest first

    def picture(self, entry):
        f = entry.fields
        key = f"picture {f['index']}"
        h264 = entry.one("h264")
        kind, poc, frame_num = f["type"], h264["PicOrderCnt"], h264["frame_num"]
        if kind == "IDR":
            self.refs = []
        by_slot = {p.slot: p for p in self.refs}

        # The initial lists over the references held before this picture.
        lists = entry.one("lists")
        if kind in ("P", "B"):
            def pic_num(p):
                return p.frame_num if p.frame_num <= frame_num else p.frame_num - self.max_frame_num
            if kind == "P":
                l0 = [p.pic for p in sorted(self.refs, key=pic_num, reverse=True)]
                l1 = []
            else:
                before = sorted((p for p in self.refs if p.poc < poc), key=lambda p: p.poc, reverse=True)
                after = sorted((p for p in self.refs if p.poc > poc), key=lambda p: p.poc)
                l0 = [p.pic for p in before + after]
                l1 = [p.pic for p in after + before]
                if len(l1) > 1 and l1 == l0:
                    l1[0], l1[1] = l1[1], l1[0]
            for name, slots, expected in (("list0", lists["l0"], l0), ("list1", lists["l1"], l1)):
                actual = [by_slot[s].pic if s in by_slot else f"slot{s}" for s in slots]
                compare_list(self.report, key, name, actual, expected)

        for ref in entry.all("ref"):
            held = by_slot.get(ref["slot"])
            if held is None or held.pic != ref["pic"]:
                self.report.fail("ref-unknown", key, f"slot {ref['slot']} (picture {ref['pic']}) is not a reference")
                continue
            if ref["PicOrderCnt"] != held.poc:
                self.report.fail("ref-poc", key, f"picture {held.pic} PicOrderCnt {ref['PicOrderCnt']}, not {held.poc}")
            if "lt" not in flags(ref) and ref["FrameNum"] != held.frame_num:
                self.report.fail("ref-frame-num", key, f"picture {held.pic} FrameNum {ref['FrameNum']}, not {held.frame_num}")
            if ref["primary_pic_type"] != H264_TYPES[held.kind]:
                self.report.fail("ref-pic-type", key,
                                 f"picture {held.pic} primary_pic_type {ref['primary_pic_type']}, not {H264_TYPES[held.kind]}")

        # The sliding window, then the current picture.
        setup_info = entry.one("setup")
        setup = setup_info["slot"]
        if setup >= self.dpb_size:
            self.report.fail("slot-range", key, f"setup slot {setup}, DPB size {self.dpb_size}")
        elif (setup_info["PicOrderCnt"], setup_info["FrameNum"], setup_info["primary_pic_type"]) != (poc, frame_num, H264_TYPES[kind]):
            self.report.fail("setup-info", key,
                             f"PicOrderCnt {setup_info['PicOrderCnt']} FrameNum {setup_info['FrameNum']} primary_pic_type "
                             f"{setup_info['primary_pic_type']}, not {poc} {frame_num} {H264_TYPES[kind]}")
        if f["ref"]:
            if len(self.refs) >= self.max_num_ref_frames:
                oldest = min(self.refs, key=lambda p: p.frame_num if p.frame_num <= frame_num else p.frame_num - self.max_frame_num)
                self.refs.remove(oldest)
        for p in self.refs:
            if p.slot == setup:
                self.report.fail("slot-reuse", key, f"setup slot {setup} holds reference picture {p.pic}")
        if f["ref"]:
            self.refs.append(Picture(f["input"], kind, poc, setup, frame_num))


def rps_pocs(rps, poc):
    """The POCs of a short-term RPS (7.4.8): (StCurrBefore, StCurrAfter, StFoll)."""
    before, after, foll = [], [], []
    delta = 0
    for i, minus1 in enumerate(rps["delta_poc_s0_minus1"]):
        delta -= minus1 + 1
        (before if (rps["used_s0"] >> i) & 1 else foll).append(poc + delta)
    delta = 0
    for i, minus1 in enumerate(rps["delta_poc_s1_minus1"]):
        delta += minus1 + 1
        (after if (rps["used_s1"] >> i) & 1 else foll).append(poc + delta)
    return before, after, foll


class H265Model:
    def __init__(self, config, report):
        self.report = report
        self.dpb_size = min(config["dpbSize"], config["max_dpb_slots"])
        self.refs = []

    def picture(self, entry):
        f = entry.fields
        key = f"picture {f['index']}"
        kind, poc = f["type"], entry.one("h265")["PicOrderCntVal"]
        rps = entry.one("rps")
        if kind == "IDR":
            self.refs = []
            before, after, foll = [], [], []
        elif rps is None:
            self.report.fail("rps-missing", key, "a non-IDR picture without a reference picture set")
            before, after, foll = [], [], []
        else:
            before, after, foll = rps_pocs(rps, poc)
        by_poc = {p.poc: p for p in self.refs}
        for name, pocs in (("before", before), ("after", after), ("foll", foll)):
            for value in pocs:
                if value not in by_poc:
                    self.report.fail("rps-missing", key, f"{name} POC {value} is not a reference picture")
        # 8.3.2: every picture the RPS does not name is marked unused.
        kept = set(before + after + foll)
        self.refs = [p for p in self.refs if p.poc in kept]
        by_slot = {p.slot: p for p in self.refs}

        slots = entry.one("rps-slots")
        if slots is not None:
            for name, pocs in (("stCurrBefore", before), ("stCurrAfter", after), ("stFoll", foll)):
                actual = sorted(s for s in slots[name] if s >= 0)
                expected = sorted(by_poc[v].slot for v in pocs if v in by_poc)
                if actual != expected:
                    self.report.fail("rps-slots", key, f"{name} slots {actual}, not {expected}")

        lists = entry.one("lists")
        if lists is not None:
            curr0 = [by_poc[v].pic for v in before + after if v in by_poc]
            curr1 = [by_poc[v].pic for v in after + before if v in by_poc]
            for name, slots_, curr in (("list0", lists["l0"], curr0), ("list1", lists["l1"], curr1)):
                actual = [by_slot[s].pic if s in by_slot else f"slot{s}" for s in slots_]
                # 8.3.4: the initial list repeats the current references up to the active count.
                expected = [curr[i % len(curr)] for i in range(len(actual))] if curr else []
                compare_list(self.report, key, name, actual, expected)

        for ref in entry.all("ref"):
            held = by_slot.get(ref["slot"])
            if held is None or held.pic != ref["pic"]:
                self.report.fail("ref-unknown", key, f"slot {ref['slot']} (picture {ref['pic']}) is not a reference")
                continue
            if ref["PicOrderCntVal"] != held.poc:
                self.report.fail("ref-poc", key, f"picture {held.pic} PicOrderCntVal {ref['PicOrderCntVal']}, not {held.poc}")
            if ref["pic_type"] != H265_TYPES[held.kind]:
                self.report.fail("ref-pic-type", key, f"picture {held.pic} pic_type {ref['pic_type']}, not {H265_TYPES[held.kind]}")
            if "unused" in flags(ref):
                self.report.fail("ref-unused", key, f"picture {held.pic} is flagged unused_for_reference")

        setup_info = entry.one("setup")
        setup = setup_info["slot"]
        if setup >= self.dpb_size:
            self.report.fail("slot-range", key, f"setup slot {setup}, DPB size {self.dpb_size}")
        elif ((setup_info["PicOrderCntVal"], setup_info["pic_type"]) != (poc, H265_TYPES[kind]) or
              ("unused" in flags(setup_info)) == bool(f["ref"])):
            self.report.fail("setup-info", key,
                             f"PicOrderCntVal {setup_info['PicOrderCntVal']} pic_type {setup_info['pic_type']} flags "
                             f"{setup_info.get('flags', '-')}, not {poc} {H265_TYPES[kind]} of a picture with ref={f['ref']}")
        for p in self.refs:
            if p.slot == setup:
                self.report.fail("slot-reuse", key, f"setup slot {setup} holds reference picture {p.pic}")
        self.refs = [p for p in self.refs if p.slot != setup]
        if f["ref"]:
            self.refs.append(Picture(f["input"], kind, poc, setup))


class AV1Model:
    KEY, INTER, INTRA_ONLY, SWITCH = 0, 1, 2, 3

    def __init__(self, config, report):
        self.report = report
        self.dpb_size = min(config["dpbSize"], config["max_dpb_slots"])
        self.buffers = [None] * 8  # (Picture, frame_type) in each reference buffer
        self.max_active = config["max_active_refs"]
        self.single, self.unidir, self.bidir = config["single"], config["unidir"], config["bidir"]
        self.cdf_only_primary = config.get("cdf_only_primary", 1)

    def plan(self, kind):
        """(name mask, group 1 count, group 2 count, count) of the prediction mode the DPB plans
        for a P or B picture."""
        if kind == "B" and self.bidir[0] > 0:
            return self.bidir[3], self.bidir[1], self.bidir[2], self.bidir[0]
        if self.unidir[0] > 0:
            return self.unidir[2] & 0xf, self.unidir[1], 0, self.unidir[0]
        return self.single[1], 4, 3, self.single[0]

    def count(self, key, kind, av1, bound):
        if len(bound) > self.max_active:
            self.report.fail("ref-count", key, f"{len(bound)} references bound, {self.max_active} active")
        cdf_only = av1["primary_ref_frame"] if av1["cdf_only"] else None
        named = [i for i, slot in enumerate(av1["names"]) if slot >= 0 and i != cdf_only]
        mask, group1, group2, total = self.plan(kind)
        in1, in2 = [i for i in named if i < 4], [i for i in named if i >= 4]
        if not named:
            self.report.fail("ref-count", key, f"a {kind} picture names no reference")
        elif len(in1) > group1 or len(in2) > group2 or len(named) > total:
            self.report.fail("ref-count", key, f"a {kind} picture names {len(in1)} + {len(in2)} references, "
                                               f"its prediction mode takes {group1} + {group2}, {total} in all")
        outside = [i for i in named if not (mask >> i) & 1]
        if outside:
            self.report.fail("ref-count", key, f"a {kind} picture names {outside}, outside the name mask {mask:#x}")

    def picture(self, entry):
        f = entry.fields
        key = f"picture {f['index']}"
        av1 = entry.one("av1")
        if f["show_existing"]:
            buffer = entry.one("shown")["buffer"]
            held = self.buffers[buffer] if 0 <= buffer < 8 else None
            if held is None:
                self.report.fail("show-existing", key, f"buffer {buffer} holds no frame")
            elif held.pic != f["input"]:
                self.report.fail("show-existing", key, f"buffer {buffer} holds picture {held.pic}, not {f['input']}")
            elif held.kind == self.KEY:
                self.buffers = [held] * 8
            return
        frame_type = av1["frame_type"]
        names = av1["names"]
        refs = entry.all("ref")
        bound = [ref["slot"] for ref in refs]
        if frame_type in (self.INTER, self.SWITCH):
            for i, buffer in enumerate(av1["ref_frame_idx"]):
                held = self.buffers[buffer] if 0 <= buffer < 8 else None
                if held is None:
                    self.report.fail("ref-name-slot", key, f"reference name {i} takes buffer {buffer}, which holds no frame")
                elif names[i] >= 0 and names[i] != held.slot:
                    self.report.fail("ref-name-slot", key, f"reference name {i} uses slot {names[i]}, buffer {buffer} "
                                                           f"holds picture {held.pic} in slot {held.slot}")
            for i, hint in enumerate(av1["ref_order_hint"]):
                held = self.buffers[i]
                if held is not None and hint != held.poc % 256:
                    self.report.fail("ref-order-hint", key, f"ref_order_hint[{i}] {hint}, buffer {i} holds order hint {held.poc % 256}")
        for i, slot in enumerate(names):
            if slot >= 0 and slot not in bound:
                self.report.fail("names-unlisted", key, f"reference name {i} uses slot {slot}, which the encode does not bind")
        if frame_type in (self.INTER, self.SWITCH) and f["type"] in ("P", "B"):
            self.count(key, f["type"], av1, bound)
        primary = av1["primary_ref_frame"]
        if primary < len(names):
            if names[primary] < 0:
                self.report.fail("primary-ref", key, f"primary_ref_frame {primary} names no slot")
            elif av1["cdf_only"] and not self.cdf_only_primary:
                self.report.fail("primary-ref", key, f"primary_ref_frame {primary} is used for its CDFs only, "
                                                     "which the device does not take")
        for slot in bound:
            if slot not in names:
                self.report.fail("ref-unnamed", key, f"slot {slot} is bound and no reference name uses it")
        by_slot = {p.slot: p for p in self.buffers if p is not None}
        for ref in refs:
            held = by_slot.get(ref["slot"])
            if held is None or held.pic != ref["pic"]:
                self.report.fail("ref-unknown", key, f"slot {ref['slot']} (picture {ref['pic']}) is in no reference buffer")
                continue
            if (ref["frame_type"], ref["OrderHint"], ref["RefFrameId"]) != (held.kind, held.poc % 256, held.frame_id):
                self.report.fail("ref-info", key, f"picture {held.pic} frame_type {ref['frame_type']} OrderHint {ref['OrderHint']} "
                                                  f"RefFrameId {ref['RefFrameId']}, not {held.kind} {held.poc % 256} {held.frame_id}")

        setup_info = entry.one("setup")
        setup = setup_info["slot"]
        frame_id = av1["current_frame_id"]
        if setup >= self.dpb_size:
            self.report.fail("slot-range", key, f"setup slot {setup}, DPB size {self.dpb_size}")
        elif (setup_info["frame_type"], setup_info["OrderHint"], setup_info["RefFrameId"]) != (frame_type, f["poc"] % 256, frame_id):
            self.report.fail("setup-info", key,
                             f"frame_type {setup_info['frame_type']} OrderHint {setup_info['OrderHint']} RefFrameId "
                             f"{setup_info['RefFrameId']}, not {frame_type} {f['poc'] % 256} {frame_id}")
        refresh = av1["refresh"]
        if frame_type == self.KEY and "show_frame" in flags(av1) and refresh != 0xff:
            self.report.fail("refresh", key, f"a shown key frame refreshes {refresh:#x}")
        current = Picture(f["input"], frame_type, f["poc"], setup, frame_id=frame_id)
        self.buffers = [current if (refresh >> i) & 1 else held for i, held in enumerate(self.buffers)]
        for i, held in enumerate(self.buffers):
            if held is not None and held is not current and held.slot == setup:
                self.report.fail("slot-reuse", key, f"setup slot {setup} holds picture {held.pic}, still in buffer {i}")


def compare(trace_path, verbose=False):
    """Returns (report, note); report is None when the trace has nothing to check."""
    entries = read_trace(trace_path)
    if not entries or entries[0].tag != "encoder-dpb-trace":
        return None, "not an encoder DPB trace"
    config = next((e.fields for e in entries if e.tag == "config"), None)
    if config is None:
        return None, "no config line"
    config = dict(config, max_dpb_slots=entries[0].fields["max_dpb_slots"])
    report = Report(verbose)
    models = {"h264": H264Model, "h265": H265Model, "av1": AV1Model}
    model = models[config["codec"]](config, report)
    pictures = 0
    for entry in entries:
        if entry.tag == "pic":
            model.picture(entry)
            pictures += 1
    if pictures == 0:
        return None, "no pictures"
    return report, ""


def categories(trace_path, verbose=False):
    """Returns ({category: (pictures, first)}, note); the dict is None when not comparable."""
    try:
        report, note = compare(trace_path, verbose)
    except (ValueError, KeyError, IndexError, TypeError) as error:
        return None, f"model error: {error!r}"
    if report is None:
        return None, note
    return {name: (len(e["pictures"]), e["first"]) for name, e in report.categories.items()}, ""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--trace", required=True)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    found, note = categories(args.trace, args.verbose)
    if found is None:
        print(f"not comparable: {note}")
        return 2
    for name, (count, first) in sorted(found.items()):
        print(f"{name:16s} {count:5d}  first: {first}")
    if not found:
        print("no mismatch")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
