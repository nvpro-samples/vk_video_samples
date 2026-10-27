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

The suite file (suites/<codec>.json) names its "codec" and lists streams. An entry has:
  name            a label
  path            the stream, relative to the suite file's directory, or with "root" relative to the
                  directory that environment variable names; an entry whose variable is unset is
                  skipped
  glob            in place of path: one pattern or a list of patterns, every matching file under the
                  root
  decode_samples  in place of path: tests/decode_samples.json, relative to the suite file's
                  directory; every sample of the suite's codec whose file exists under the root
  format          bytestream (H.264, H.265), ivf, obu (AV1 low overhead) or annexb (AV1); a
                  decode_samples entry takes it from the file extension
  golden          the recorded trace, relative to the suite file's directory (optional)
  md5             for a glob or decode_samples entry: the file, relative to the suite file's
                  directory, that lists the md5 of each stream's trace
  headers         AV1: a compact header dump (av1_reference_model.py --save-headers), relative to the
                  suite file's directory. Without it the dump is made with ffmpeg from the IVF the
                  tool writes, and the model check is skipped when ffmpeg is missing.
  known           AV1: the model mismatch categories the stream shows, each one a defect named in the
                  suite's "defects" table

Checks per stream: the tool exits 0; the trace equals the golden, or its md5 equals the recorded
one; for AV1, the set of model mismatch categories equals "known". An unlisted category is a new
defect or a regression. A listed category that no longer appears means its fix landed, and the
entry has to go with that fix. A stream an md5 list names but the root does not hold is reported
and does not fail. An AV1 glob entry without md5 is reported per model category and never fails.

--mint records the goldens, the md5 lists and the header dumps of entries that name one, from
this run, and prints each AV1 stream's categories for the "known" lists. Exit status: 0 every check
held, 1 a check failed, 2 nothing ran.
"""
import argparse
import difflib
import glob
import hashlib
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

FORMAT_BY_EXTENSION = {".h264": "bytestream", ".264": "bytestream", ".h265": "bytestream", ".265": "bytestream",
                       ".ivf": "ivf", ".obu": "annexb"}


def run_tool(tool, codec, fmt, stream, trace, ivf=None):
    cmd = [tool, "--codec", codec, "--format", fmt, "-i", stream, "-o", trace]
    if ivf:
        cmd += ["--write-ivf", ivf]
    try:
        result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors="replace",
                                timeout=300)
    except subprocess.TimeoutExpired:
        return "timeout"
    return None if result.returncode == 0 else f"exit status {result.returncode}"


def file_md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def read_md5_list(path):
    listed = {}
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                digest, name = line.rstrip("\n").split("  ", 1)
                listed[name] = digest
    return listed


def write_md5_list(path, digests):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        for name in sorted(digests):
            f.write(f"{digests[name]}  {name}\n")


def header_lines(entry, suite_dir, tool, stream, work, ffmpeg, mint):
    """Returns (lines, note); lines is None when no dump is available."""
    if entry.get("headers") and not mint:
        with open(os.path.join(suite_dir, entry["headers"])) as dump:
            return dump.read().splitlines(), ""
    if not shutil.which(ffmpeg):
        return None, "ffmpeg not found"
    ivf = os.path.join(work, "stream.ivf")
    failure = run_tool(tool, "av1", entry["format"], stream, os.devnull, ivf)
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
    codec = suite["codec"]
    trace = os.path.join(work, name + ".trace")
    failure = run_tool(args.tool, codec, entry["format"], stream, trace)
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
    if codec != "av1":
        return failures
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


def entry_files(entry, suite, suite_dir, base):
    """Returns [(name, path, format)] for a glob or decode_samples entry."""
    if "decode_samples" in entry:
        with open(os.path.join(suite_dir, entry["decode_samples"])) as f:
            samples = json.load(f)["samples"]
        files = []
        for sample in samples:
            path = os.path.join(base, sample["source_filepath"])
            if sample["codec"] == suite["codec"] and os.path.exists(path):
                fmt = FORMAT_BY_EXTENSION.get(os.path.splitext(path)[1].lower())
                files.append((sample["source_filepath"], path, fmt))
        return files
    patterns = entry["glob"] if isinstance(entry["glob"], list) else [entry["glob"]]
    paths = sorted({p for pattern in patterns for p in glob.glob(os.path.join(base, pattern))})
    return [(os.path.relpath(p, base), p, entry["format"]) for p in paths]


def check_md5_set(entry, suite, suite_dir, files, args, work):
    """Checks every stream's trace md5 against the entry's list. Returns failure messages."""
    md5_path = os.path.join(suite_dir, entry["md5"])
    listed = read_md5_list(md5_path)
    digests = {}
    failures = []
    keep = os.path.join(args.out, entry["name"]) if args.out else None
    for name, path, fmt in files:
        trace = os.path.join(keep, name + ".trace") if keep else os.path.join(work, "set.trace")
        os.makedirs(os.path.dirname(trace), exist_ok=True)
        failure = run_tool(args.tool, suite["codec"], fmt, path, trace)
        if failure:
            failures.append(f"{name}: tool {failure}")
            continue
        digests[name] = file_md5(trace)
    if args.mint:
        write_md5_list(md5_path, digests)
        return failures
    for name in sorted(digests):
        if name not in listed:
            failures.append(f"{name}: not in {entry['md5']}")
        elif listed[name] != digests[name]:
            failures.append(f"{name}: trace differs from the recorded md5")
    missing = sorted(set(listed) - {name for name, _, _ in files})
    print(f"    {entry['name']}: {len(files)} streams, {len(digests)} traced, {len(missing)} listed but not present")
    return failures


def report_glob(entry, base, args, work):
    files = sorted(glob.glob(os.path.join(base, entry["glob"])))
    totals = {}
    tool_failures = 0
    not_comparable = 0
    for stream in files:
        trace = os.path.join(work, "glob.trace")
        if run_tool(args.tool, "av1", entry["format"], stream, trace):
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
        if ("glob" in entry or "decode_samples" in entry) and "md5" in entry:
            files = entry_files(entry, suite, suite_dir, base)
            ran += 1
            failures = check_md5_set(entry, suite, suite_dir, files, args, work)
            if failures:
                failed += 1
                print(f"FAIL {entry['name']}")
                for message in failures[:24]:
                    print(f"    {message}")
                if len(failures) > 24:
                    print(f"    ... {len(failures) - 24} more")
            else:
                print(f"PASS {entry['name']}")
            continue
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
    print(f"{ran} entries checked, {failed} failed")
    if ran == 0:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
