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
"""What the H.264, H.265 and VP9 header models share: ffmpeg's trace_headers dump, its compact form,
the parser trace, and the mismatch report.

A unit is one header section of the dump (a parameter set, a slice header, a frame header) with its
syntax elements in bitstream order, each as (bit position, name, value). Names keep their array
indices (delta_poc_s0_minus1[0]); syntax that repeats without an index, as the reference picture
sets of an SPS do, repeats in order. ffmpeg prints the parameter sets of a raw stream's extradata
before the first packet; those copies are left out, so every unit is one the parser reads.
"""
import re
import subprocess

# The bits can be empty: ffmpeg prints a value it derives from several elements, as VP9 tile_cols_log2,
# with none.
FIELD = re.compile(r"^\[trace_headers @ 0x[0-9a-f]+\] (\d+)\s+(\S+)\s+(?:[01]+\s+)?= (-?\d+)$")
HEADING = re.compile(r"^\[trace_headers @ 0x[0-9a-f]+\] (\S.*)$")
COMPACT_FIELD = re.compile(r"^(\d+) (\S+) (-?\d+)$")
COMPACT_HEADING = re.compile(r"^== (.+)$")


class Unit:
    def __init__(self, packet, kind):
        self.packet = packet
        self.kind = kind
        self.fields = []  # (bit position, name, value), in bitstream order

    def get(self, name, default=None):
        for _, field, value in self.fields:
            if field == name:
                return value
        return default

    def has(self, name):
        return any(field == name for _, field, _ in self.fields)

    def position(self, name, start=0):
        """The index in fields of the first element called name at or after start, or None."""
        for i in range(start, len(self.fields)):
            if self.fields[i][1] == name:
                return i
        return None


def run_ffmpeg(ffmpeg, stream, demuxer):
    cmd = [ffmpeg, "-hide_banner", "-nostdin", "-f", demuxer, "-i", stream, "-c", "copy", "-copyinkf", "-bsf:v",
           "trace_headers", "-f", "null", "-"]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return result.returncode, result.stdout.splitlines()


def read_units(lines, kinds):
    """Returns the units of the kinds named, from an ffmpeg dump or its compact form."""
    units = []
    packet = -1
    unit = None
    for line in lines:
        m = FIELD.match(line) or COMPACT_FIELD.match(line)
        if m:
            if unit is not None:
                unit.fields.append((int(m.group(1)), m.group(2), int(m.group(3))))
            continue
        m = HEADING.match(line) or COMPACT_HEADING.match(line)
        if not m:
            continue
        text = m.group(1)
        if unit is not None and text.startswith(unit.kind + ":"):
            continue  # a summary line inside the unit, as VP9's "Frame:  size 64x64 ..."
        unit = None
        if text.startswith("Packet"):
            packet += 1
        elif text in kinds and packet >= 0:
            unit = Unit(packet, text)
            units.append(unit)
    return units


def compact(lines, kinds, title, keep=None):
    """The packets and units of the kinds named, in the form read_units() reads. keep(unit), when
    given, returns the fields of the unit to write."""
    out = [f"# {title}", "# from ffmpeg's trace_headers dump (-c copy -copyinkf -bsf:v trace_headers -f null -)"]
    packet = -1
    for unit in read_units(lines, kinds):
        while packet < unit.packet:
            out.append("== Packet")
            packet += 1
        out.append(f"== {unit.kind}")
        fields = keep(unit) if keep else unit.fields
        out.extend(f"{position} {name} {value}" for position, name, value in fields)
    total = sum(1 for line in lines if HEADING.match(line) and HEADING.match(line).group(1).startswith("Packet"))
    while packet < total - 1:
        out.append("== Packet")
        packet += 1
    return "\n".join(out) + "\n"


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


class TraceEntry:
    """A line of the trace with the indented lines under it, by tag; a tag that repeats (dpb, strps)
    keeps its lines in a list."""

    def __init__(self, tag, fields):
        self.tag = tag
        self.fields = fields
        self.sub = {}

    def one(self, tag):
        lines = self.sub.get(tag, [])
        return lines[0] if lines else None


def read_trace(path):
    """Returns the trace's top-level entries in order."""
    entries = []
    with open(path) as trace:
        for line in trace:
            if not line.strip():
                continue
            tag, fields = parse_line(line)
            if line.startswith("  ") and entries:
                entries[-1].sub.setdefault(tag, []).append(fields)
            else:
                entries.append(TraceEntry(tag, fields))
    return entries


# ---- the report --------------------------------------------------------------------------------

class Report:
    """Mismatches by category: for each, the keys (a parameter set or a picture) and the first one."""

    def __init__(self, verbose=False):
        self.categories = {}
        self.verbose = verbose

    def check(self, category, key, field, expected, actual):
        if expected == actual:
            return
        entry = self.categories.setdefault(category, {"pictures": [], "first": None})
        if key not in entry["pictures"]:
            entry["pictures"].append(key)
        if entry["first"] is None:
            entry["first"] = f"{key}: {field} expected {expected} got {actual}"
        if self.verbose:
            print(f"  {key} {category}: {field} expected {expected} got {actual}")

    def fail(self, category, key, message):
        """Records a broken rule, which has no expected value."""
        entry = self.categories.setdefault(category, {"pictures": [], "first": None})
        if key not in entry["pictures"]:
            entry["pictures"].append(key)
        if entry["first"] is None:
            entry["first"] = f"{key}: {message}"
        if self.verbose:
            print(f"  {key} {category}: {message}")
