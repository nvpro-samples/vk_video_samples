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
"""DPB slot oracle of the adapter slot trace.

Usage: slot_oracle.py --trace T [-v]

Replays, over a vk-video-adapter-trace trace, the DPB slot states the Vulkan specification defines
(videocoding.adoc "DPB Slot States", and each codec's reference picture setup rule in
h264_decode.adoc, h265_decode.adoc, av1_decode.adoc and vp9_decode.adoc), and checks every decode
against them. A slot is activated by a decode whose setup the codec requests (H.264
flags.is_reference, H.265 flags.IsReference, AV1 and VP9 a non-zero refresh_frame_flags) and is
otherwise invalidated by it; a new video session starts with every slot inactive. A picture
resource is the picture the tool names with res, and for VP9 also its coded extent. Categories:

  begin-<role>-inactive  VUID-vkCmdBeginVideoCodingKHR-slotIndex-07239: a begin-coding entry names
                         a slot that is not active
  begin-<role>-mismatch  VUID-vkCmdBeginVideoCodingKHR-pPictureResource-07265: a begin-coding entry's
                         resource is not one its active slot holds
                         <role> is setup (the setup picture's entry) or ref
  begin-duplicate        VUID-VkVideoBeginCodingInfoKHR-pPictureResource-07238: a resource twice in
                         the begin-coding list
  decode-duplicate       VUID-vkCmdDecodeVideoKHR-pDecodeInfo-07264: a resource twice among the
                         reference slots
  decode-unbound         VUID-vkCmdDecodeVideoKHR-pDecodeInfo-07151: a reference's resource is not
                         bound to its slot by the begin-coding list
  decode-use-count       VUID-vkCmdDecodeVideoKHR-dpbFrameUseCount-07176, dpbTopFieldUseCount-07177,
                         dpbBottomFieldUseCount-07178: a slot referenced twice as one frame or field
  decode-field           VUID-vkCmdDecodeVideoKHR-pDecodeInfo-07266, 07267, 07268 (H.264): the slot
                         holds no frame, top field or bottom field with the reference's resource
                         (a frame holds both its fields)
  setup-missing          VUID-vkCmdDecodeVideoKHR-pDecodeInfo-08376
  setup-unbound          VUID-vkCmdDecodeVideoKHR-pDecodeInfo-07149: the setup resource is not bound
  setup-structure        VUID-vkCmdDecodeVideoKHR-pDecodeInfo-07261, 07262, 07263 (H.264): the setup
                         reference information is not the frame or field the picture is
  slot-range             VUID-VkVideoBeginCodingInfoKHR-slotIndex-04856,
                         VUID-vkCmdDecodeVideoKHR-pDecodeInfo-07170, slotIndex-07256: a slot index at
                         or above the session's maxDpbSlots
  names-unlisted         VUID-vkCmdDecodeVideoKHR-referenceNameSlotIndices-09262 (AV1), 10809 (VP9):
                         a reference name's slot is not among the reference slots
  ref-unnamed            VUID-vkCmdDecodeVideoKHR-slotIndex-09263 (AV1), 10810 (VP9): a reference
                         slot no reference name uses
  names-missing          an AV1 inter or switch frame, or a VP9 inter frame, with a reference name
                         that names no slot: such a frame predicts from every reference it names
                         (AV1 7.20, VP9 6.2), so the decode lacks a reference
  h265-list              h265_decode.adoc: a RefPicSetStCurrBefore, RefPicSetStCurrAfter or
                         RefPicSetLtCurr entry names a slot that is not among the reference slots
  nonexisting            any rule above broken by the entry of an H.264 reference flagged
                         is_non_existing, or a resource it shares with another entry
  ref-info               a reference's Std reference information differs from the information its
                         picture was set up with: H.264 FrameNum of a short-term reference and the
                         field order counts, H.265 PicOrderCntVal, every AV1 field. A picture with
                         memory_management_control_operation 5 has FrameNum 0 and rebased field
                         order counts after its decode (H.264 8.2.1), which is not a mismatch.
  reserve-failed         the frame buffer found no free picture

Exit status: 0 no mismatch, 1 a mismatch, 2 not comparable.
"""
import argparse
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import header_dump as hd  # noqa: E402

NO_REFERENCE = 0xff  # STD_VIDEO_H265_NO_REFERENCE_PICTURE


class Slot:
    """The picture references a DPB slot holds: a frame, or a top and a bottom field (H.264). Each
    is (resource, Std reference information it was set up with)."""

    def __init__(self):
        self.frame = None
        self.top = None
        self.bottom = None

    def active(self):
        return any((self.frame, self.top, self.bottom))

    def resources(self):
        return {ref[0] for ref in (self.frame, self.top, self.bottom) if ref}


def word(value):
    """A one-word value: the trace reader reads a word as a set of flags."""
    return next(iter(value)) if isinstance(value, set) and len(value) == 1 else value


def resource(fields):
    """A resource's identity: the picture, and for VP9 its coded extent."""
    if "width" in fields:
        return (fields["res"], fields["width"], fields["height"])
    return fields["res"]


def setup_requested(codec, info):
    flags = info.get("flags", set())
    if codec == "h264":
        return "ref" in flags
    if codec == "h265":
        return "isref" in flags
    return info.get("refresh", 0) != 0


def h264_kinds(std):
    """The active reference pictures an H.264 reference slot adds: a frame, or its fields."""
    flags = std.get("flags", set())
    kinds = [kind for kind in ("top", "bottom") if kind in flags]
    return kinds or ["frame"]


def std_fields(fields):
    return {k: v for k, v in fields.items() if k not in ("slot", "res", "role", "width", "height")}


def mmco5_poc(kind, poc):
    """The field order counts of a picture after its memory_management_control_operation 5:
    tempPicOrderCnt, the picture's order count, is subtracted from them (H.264 8.2.1)."""
    if kind == "frame":
        temp = min(poc)
        return [poc[0] - temp, poc[1] - temp]
    return [0, 0]


def compare_info(codec, report, key, kind, std, stored):
    """ref-info: the reference's Std information against the one its picture was set up with."""
    if stored is None:
        return
    if codec == "h264":
        poc, set_up = std.get("poc", [0, 0]), stored.get("poc", [0, 0])
        short_term = "lt" not in std.get("flags", set())
        if short_term and std.get("frame_num") == 0 and stored.get("frame_num") != 0:
            set_up = mmco5_poc(kind, set_up)
        elif short_term:
            report.check("ref-info", key, "frame_num", stored.get("frame_num"), std.get("frame_num"))
        if kind in ("frame", "top"):
            report.check("ref-info", key, f"{kind} PicOrderCnt[0]", set_up[0], poc[0])
        if kind in ("frame", "bottom"):
            report.check("ref-info", key, f"{kind} PicOrderCnt[1]", set_up[1], poc[1])
    elif codec == "h265":
        report.check("ref-info", key, "PicOrderCntVal", stored.get("poc"), std.get("poc"))
    elif codec == "av1":
        for name in sorted(set(std) | set(stored)):
            report.check("ref-info", key, name, stored.get(name), std.get(name))


class Oracle:
    def __init__(self, codec, report):
        self.codec = codec
        self.report = report
        self.slots = {}
        self.max_dpb_slots = 0

    def slot(self, index):
        return self.slots.setdefault(index, Slot())

    def new_session(self, max_dpb_slots):
        self.slots = {}
        self.max_dpb_slots = max_dpb_slots

    def check_range(self, key, what, index):
        if index >= self.max_dpb_slots:
            self.report.fail("slot-range", key, f"{what} slot {index}, maxDpbSlots {self.max_dpb_slots}")

    def picture(self, entry):
        """Checks one decode, then applies its reference picture setup."""
        key = f"picture {entry.fields['index']}"
        info = (entry.one(self.codec) or {})
        setup = entry.one("setup")
        refs = entry.sub.get("ref", [])
        begins = entry.sub.get("begin", [])
        report = self.report

        def fail(category, message, nonexisting):
            report.fail("nonexisting" if nonexisting else category, key, message)

        # The begin-coding list: the bound reference picture resources and their slots.
        bound = set()
        seen = {}
        ref_index = 0
        for begin in begins:
            res, index = resource(begin), begin["slot"]
            role = word(begin.get("role", "ref"))
            nonexisting = False
            if role == "ref":
                ref = refs[ref_index] if ref_index < len(refs) else {}
                ref_index += 1
                nonexisting = "nonexisting" in ref.get("flags", set())
            if begin["res"] >= 0:
                if res in seen:
                    fail("begin-duplicate", f"resource {res} listed twice (07238)", nonexisting or seen[res])
                seen[res] = seen.get(res, False) or nonexisting
            if index < 0:
                if begin["res"] >= 0:
                    bound.add((res, None))
                continue
            self.check_range(key, "begin-coding", index)
            if begin["res"] < 0:
                self.slots.pop(index, None)  # deactivated by the command
                continue
            bound.add((res, index))
            slot = self.slots.get(index)
            if slot is None or not slot.active():
                fail(f"begin-{role}-inactive", f"slot {index} (resource {res}) is not active (07239)", nonexisting)
            elif res not in slot.resources():
                fail(f"begin-{role}-mismatch",
                     f"slot {index} holds {sorted(slot.resources(), key=str)}, not {res} (07265)", nonexisting)

        # The decode's reference slots.
        seen = {}
        use = {}
        ref_slots = []
        for ref in refs:
            res, index = resource(ref), ref["slot"]
            nonexisting = "nonexisting" in ref.get("flags", set())
            ref_slots.append(index)
            self.check_range(key, "reference", index)
            if res in seen:
                fail("decode-duplicate", f"resource {res} referenced twice (07264)", nonexisting or seen[res])
            seen[res] = seen.get(res, False) or nonexisting
            if (res, index) not in bound:
                fail("decode-unbound", f"resource {res} is not bound to slot {index} (07151)", nonexisting)
            slot = self.slots.get(index, Slot())
            kinds = h264_kinds(ref) if self.codec == "h264" else ["frame"]
            for kind in kinds:
                use[(index, kind)] = use.get((index, kind), 0) + 1
                if use[(index, kind)] == 2:
                    fail("decode-use-count", f"slot {index} referenced twice as {kind}", nonexisting)
                held = getattr(slot, kind) or slot.frame
                if self.codec == "h264" and (held is None or held[0] != res):
                    fail("decode-field", f"slot {index} holds no {kind} with resource {res} (07266-07268)", nonexisting)
                if held is not None and held[0] == res:
                    if self.codec == "vp9":
                        continue
                    compare_info(self.codec, report, key, kind, std_fields(ref), held[1])

        names = info.get("names")
        if names is not None:
            inter = ((self.codec == "av1" and info.get("frame_type") in (1, 3)) or
                     (self.codec == "vp9" and info.get("frame_type") == 1 and
                      "intra_only" not in info.get("flags", set())))
            for i, name in enumerate(names):
                if name >= 0 and name not in ref_slots:
                    report.fail("names-unlisted", key, f"reference name slot {name} is not a reference slot")
                if inter and name < 0:
                    report.fail("names-missing", key, f"reference name {i} of an inter frame has no slot")
            for index in ref_slots:
                if index not in names:
                    report.fail("ref-unnamed", key, f"reference slot {index} has no reference name")
        if self.codec == "h265":
            for name in ("before", "after", "lt"):
                for index in info.get(name, []):
                    if index != NO_REFERENCE and index not in ref_slots:
                        report.fail("h265-list", key, f"{name} names slot {index}, not a reference slot")

        # The setup slot, and the reference picture setup the decode requests.
        if setup is None:
            report.fail("setup-missing", key, "no setup slot")
            return
        res, index = resource(setup), setup["slot"]
        self.check_range(key, "setup", index)
        if res not in {bound_res for bound_res, _ in bound}:
            report.fail("setup-unbound", key, f"setup resource {res} is not bound")
        kind = "frame"
        if self.codec == "h264":
            flags = info.get("flags", set())
            kind = ("bottom" if "bottom" in flags else "top") if "field" in flags else "frame"
            structure = h264_kinds(setup)
            if structure != [kind]:
                report.fail("setup-structure", key, f"a {kind} picture set up as {structure}")
        slot = self.slot(index)
        if setup_requested(self.codec, info):
            if kind == "frame":
                slot.frame, slot.top, slot.bottom = (res, std_fields(setup)), None, None
            else:
                if slot.frame:
                    slot.frame = None
                setattr(slot, kind, (res, std_fields(setup)))
        elif kind == "frame" or slot.frame:
            self.slots.pop(index, None)
        else:
            setattr(slot, kind, None)
            if not slot.active():
                self.slots.pop(index, None)


def compare(trace_path, verbose=False):
    """Returns (report, note); report is None when the trace has nothing to check."""
    entries = hd.read_trace(trace_path)
    if not entries or entries[0].tag != "adapter-trace":
        return None, "not an adapter trace"
    codec = word(entries[0].fields["codec"])
    report = hd.Report(verbose)
    oracle = Oracle(codec, report)
    pictures = 0
    for entry in entries[1:]:
        if entry.tag == "session":
            if word(entry.fields["state"]) == "new":
                oracle.new_session(entry.fields["maxDpbSlots"])
            else:
                oracle.max_dpb_slots = entry.fields["maxDpbSlots"]
        elif entry.tag == "pic":
            oracle.picture(entry)
            pictures += 1
        elif entry.tag == "reserve-failed":
            report.fail("reserve-failed", f"after picture {pictures - 1}", "no free picture")
    if pictures == 0:
        return None, "no pictures"
    return report, ""


def categories(trace_path, verbose=False):
    """Returns ({category: (keys, first)}, note); the dict is None when not comparable."""
    try:
        report, note = compare(trace_path, verbose)
    except (ValueError, KeyError, IndexError, TypeError) as error:
        return None, f"oracle error: {error}"
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
        print(f"{name:24s} {count:5d}  first: {first}")
    if not found:
        print("no mismatch")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
