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
  rps-missing     H.265: the reference picture set names a POC no reference picture has
  rps-slots       H.265: the encoder's RPS slots are not the pictures of the reference picture set
  slot-reuse      the setup slot holds a picture that stays a reference
  slot-range      a slot at or above the DPB size

Exit status: 0 no mismatch, 1 a mismatch, 2 not comparable.
"""
import argparse
import sys

H264_TYPES = {"IDR": 3, "I": 2, "P": 0, "B": 1}  # StdVideoH264PictureType
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
    def __init__(self, pic, kind, poc, slot, frame_num=None):
        self.pic = pic
        self.kind = kind
        self.poc = poc
        self.slot = slot
        self.frame_num = frame_num


def compare_list(report, key, name, actual, expected):
    """The encoder's list (pictures) against the initial list cut to the encoder's active count."""
    if len(actual) > len(expected):
        report.fail(name, key, f"{actual} is longer than the initial list {expected}")
    elif actual != expected[:len(actual)]:
        report.fail(name, key, f"{actual} is not the start of the initial list {expected}")


class H264Model:
    def __init__(self, config, report):
        self.report = report
        self.dpb_size = config["dpbSize"]
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
        setup = entry.one("setup")["slot"]
        if setup >= self.dpb_size:
            self.report.fail("slot-range", key, f"setup slot {setup}, DPB size {self.dpb_size}")
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
        self.dpb_size = config["dpbSize"]
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

        setup = entry.one("setup")["slot"]
        if setup >= self.dpb_size:
            self.report.fail("slot-range", key, f"setup slot {setup}, DPB size {self.dpb_size}")
        for p in self.refs:
            if p.slot == setup:
                self.report.fail("slot-reuse", key, f"setup slot {setup} holds reference picture {p.pic}")
        self.refs = [p for p in self.refs if p.slot != setup]
        if f["ref"]:
            self.refs.append(Picture(f["input"], kind, poc, setup))


def compare(trace_path, verbose=False):
    """Returns (report, note); report is None when the trace has nothing to check."""
    entries = read_trace(trace_path)
    if not entries or entries[0].tag != "encoder-dpb-trace":
        return None, "not an encoder DPB trace"
    config = next((e.fields for e in entries if e.tag == "config"), None)
    if config is None:
        return None, "no config line"
    report = Report(verbose)
    model = H264Model(config, report) if config["codec"] == "h264" else H265Model(config, report)
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
