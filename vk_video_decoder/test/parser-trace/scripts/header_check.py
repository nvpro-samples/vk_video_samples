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
"""Checks one H.264, H.265 or VP9 parser trace against the stream's headers.

Usage: header_check.py --codec h264|h265|vp9 --trace T (--stream S | --headers H) [--save-headers H]
                       [--ffmpeg F] [-v]

With --stream, ffmpeg dumps the stream's headers; --headers reads a compact dump instead, and
--save-headers writes one. Prints each mismatch category with its count and first mismatch; -v
prints every mismatch. Exit status: 0 no mismatch, 1 a mismatch, 2 not comparable.
"""
import argparse
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import h264_header_model  # noqa: E402
import h265_header_model  # noqa: E402
import header_dump  # noqa: E402
import vp9_header_model  # noqa: E402

MODELS = {"h264": h264_header_model, "h265": h265_header_model, "vp9": vp9_header_model}


def header_lines(model, stream, headers, ffmpeg):
    """Returns (lines, note); lines is None when no dump is available."""
    if headers:
        with open(headers) as dump:
            return dump.read().splitlines(), ""
    rc, lines = header_dump.run_ffmpeg(ffmpeg, stream, model.DEMUXER)
    if rc != 0:
        return None, f"ffmpeg exited {rc}"
    return lines, ""


def categories(model, lines, trace, verbose=False):
    """Returns ({category: (keys, first)}, note); the dict is None when not comparable."""
    try:
        report, note = model.compare(lines, trace, verbose)
    except (ValueError, KeyError, IndexError, TypeError) as error:
        return None, f"model error: {error}"
    if report is None:
        return None, note
    return {name: (len(e["pictures"]), e["first"]) for name, e in report.categories.items()}, ""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--codec", required=True, choices=sorted(MODELS))
    parser.add_argument("--trace", required=True)
    parser.add_argument("--stream")
    parser.add_argument("--headers")
    parser.add_argument("--save-headers")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    model = MODELS[args.codec]
    lines, note = header_lines(model, args.stream, args.headers, args.ffmpeg)
    if lines is None:
        print(f"not comparable: {note}")
        return 2
    if args.save_headers:
        with open(args.save_headers, "w") as out:
            out.write(header_dump.compact(lines, model.KINDS, model.TITLE, getattr(model, "keep", None)))
    found, note = categories(model, lines, args.trace, args.verbose)
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
