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
"""Runs the parser trace suite.

Usage: parser_trace_suite.py --tool T --suite S [--out DIR] [--mint] [--ffmpeg F] [-v]

The suite file (suites/av1.json) lists streams. An entry has:
  name     a label
  path     the stream, relative to the suite file's directory, or with "root" relative to the
           directory that environment variable names; an entry whose variable is unset is skipped
  glob     in place of path: every matching file under the root, checked in report mode
  format   ivf, obu (low overhead) or annexb
  golden   the recorded trace, relative to the suite file's directory (optional)
  headers  a compact header dump (av1_reference_model.py --save-headers), relative to the suite
           file's directory. Without it the dump is made with ffmpeg from the IVF the tool writes,
           and the model check is skipped when ffmpeg is missing.
  known    the model mismatch categories the stream shows, each one a defect named in the suite's
           "defects" table

Checks per stream: the tool exits 0; the trace equals the golden; the set of model mismatch
categories equals "known". An unlisted category is a new defect or a regression. A listed category
that no longer appears means its fix landed, and the entry has to go with that fix.
A glob entry is reported per category and never fails the run.

--mint records the goldens, and the header dumps of entries that name one, from this run, and
prints each stream's categories for the "known" lists. Exit status: 0 every check held, 1 a check
failed, 2 nothing ran.
"""
import argparse
import difflib
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

# The model sits next to this script in the source tree; importing it must not leave a
# __pycache__ directory there.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import av1_reference_model as model  # noqa: E402


def run_tool(tool, fmt, stream, trace, ivf=None):
    cmd = [tool, "--codec", "av1", "--format", fmt, "-i", stream, "-o", trace]
    if ivf:
        cmd += ["--write-ivf", ivf]
    try:
        result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors="replace",
                                timeout=300)
    except subprocess.TimeoutExpired:
        return "timeout"
    return None if result.returncode == 0 else f"exit status {result.returncode}"


def header_lines(entry, suite_dir, tool, stream, work, ffmpeg, mint):
    """Returns (lines, note); lines is None when no dump is available."""
    if entry.get("headers") and not mint:
        with open(os.path.join(suite_dir, entry["headers"])) as dump:
            return dump.read().splitlines(), ""
    if not shutil.which(ffmpeg):
        return None, "ffmpeg not found"
    ivf = os.path.join(work, "stream.ivf")
    failure = run_tool(tool, entry["format"], stream, os.devnull, ivf)
    if failure:
        return None, f"tool {failure} writing the IVF"
    rc, lines = model.run_ffmpeg(ffmpeg, ivf)
    if rc != 0:
        return None, f"ffmpeg exited {rc}"
    if mint and entry.get("headers"):
        with open(os.path.join(suite_dir, entry["headers"]), "w") as out:
            out.write(model.compact_headers(lines))
    return lines, ""


def model_categories(lines, trace):
    """Returns ({category: (pictures, first)}, note); the dict is None when not comparable."""
    frames, scalable = model.model_frames(lines)
    if scalable:
        return None, "scalable stream"
    if not frames:
        return None, "no frame headers in the dump"
    report = model.compare(frames, model.trace_pictures(trace), False)
    return {name: (len(e["pictures"]), e["first"]) for name, e in report.categories.items()}, ""


def check_stream(entry, suite, suite_dir, stream, args, work):
    """Returns a list of failure messages."""
    name = entry["name"]
    trace = os.path.join(work, name + ".trace")
    failure = run_tool(args.tool, entry["format"], stream, trace)
    if failure:
        return [f"tool {failure}"]
    failures = []
    if entry.get("golden"):
        golden = os.path.join(suite_dir, entry["golden"])
        if args.mint:
            shutil.copyfile(trace, golden)
        elif not os.path.exists(golden):
            failures.append(f"no golden {entry['golden']}")
        else:
            with open(golden) as g, open(trace) as t:
                expected, actual = g.read().splitlines(), t.read().splitlines()
            if expected != actual:
                diff = list(difflib.unified_diff(expected, actual, "golden", "trace", n=0, lineterm=""))
                failures.append("trace differs from the golden:\n      " + "\n      ".join(diff[:24]))
    lines, note = header_lines(entry, suite_dir, args.tool, stream, work, args.ffmpeg, args.mint)
    if lines is None:
        print(f"    {name}: model not checked ({note})")
        return failures
    found, note = model_categories(lines, trace)
    if found is None:
        print(f"    {name}: model not comparable ({note})")
        return failures
    if args.mint or args.verbose:
        print(f"    {name}: categories {sorted(found)}")
    known = set(entry.get("known", []))
    defects = suite.get("defects", {})
    for cat in sorted(set(found) - known):
        failures.append(f"model mismatch {cat} in {found[cat][0]} pictures, first {found[cat][1]}")
    for cat in sorted(known - set(found)):
        failures.append(f"{cat} no longer mismatches; remove it from known ({defects.get(cat, 'no defect named')})")
    return failures


def report_glob(entry, base, args, work):
    files = sorted(glob.glob(os.path.join(base, entry["glob"])))
    totals = {}
    tool_failures = 0
    not_comparable = 0
    for stream in files:
        trace = os.path.join(work, "glob.trace")
        if run_tool(args.tool, entry["format"], stream, trace):
            tool_failures += 1
            continue
        lines, _ = header_lines(entry, base, args.tool, stream, work, args.ffmpeg, False)
        found = model_categories(lines, trace)[0] if lines is not None else None
        if found is None:
            not_comparable += 1
            continue
        for cat, (pictures, _) in found.items():
            streams, total = totals.get(cat, (0, 0))
            totals[cat] = (streams + 1, total + pictures)
    print(f"    {entry['name']}: {len(files)} streams, tool failures {tool_failures}, "
          f"not comparable {not_comparable}")
    for cat, (streams, pictures) in sorted(totals.items()):
        print(f"      {cat:40s} streams {streams:5d} pictures {pictures:7d}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--suite", required=True)
    parser.add_argument("--out", help="keep the traces here")
    parser.add_argument("--mint", action="store_true")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    suite_dir = os.path.dirname(os.path.abspath(args.suite))
    with open(args.suite) as f:
        suite = json.load(f)
    work = args.out or tempfile.mkdtemp(prefix="parser-trace-")
    os.makedirs(work, exist_ok=True)

    ran = 0
    failed = 0
    for entry in suite["streams"]:
        base = suite_dir
        if entry.get("root"):
            if entry["root"] not in os.environ:
                print(f"SKIP {entry['name']}: ${entry['root']} is not set")
                continue
            base = os.environ[entry["root"]]
        if "glob" in entry:
            print(f"REPORT {entry['name']}")
            report_glob(entry, base, args, work)
            continue
        stream = os.path.join(base, entry["path"])
        if not os.path.exists(stream):
            print(f"FAIL {entry['name']}: {stream} does not exist")
            failed += 1
            continue
        ran += 1
        failures = check_stream(entry, suite, suite_dir, stream, args, work)
        if failures:
            failed += 1
            print(f"FAIL {entry['name']}")
            for message in failures:
                print(f"    {message}")
        else:
            print(f"PASS {entry['name']}")
    if not args.out:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{ran} streams checked, {failed} failed")
    if ran == 0:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
