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
"""Runs the parser trace suite or the adapter slot trace suite.

Usage: parser_trace_suite.py [--mode parser|adapter] --tool T --suite S [--out DIR] [--mint] [--ffmpeg F] [-v]

--mode parser (the default) runs vk-video-parser-trace and checks its traces against the header
models; --mode adapter runs vk-video-adapter-trace and checks its traces with slot_oracle.py, which
needs no ffmpeg. Both read the same suite files: adapter mode uses an entry's adapter_golden,
adapter_md5 and adapter_known in place of golden, md5 and known.

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
  headers         a compact dump of the stream's headers (av1_reference_model.py --save-headers,
                  header_check.py --save-headers), relative to the suite file's directory. Without it
                  the dump is made with ffmpeg, from the stream or, for AV1, from the IVF the tool
                  writes, and the model check is skipped when ffmpeg is missing.
  known           the model mismatch categories the stream shows, each one a defect named in the
                  suite's "defects" table
  adapter_golden, adapter_md5, adapter_known
                  the same for adapter mode, the categories being the slot oracle's. On an md5 entry,
                  adapter_known lists the categories its streams show together, named in the suite's
                  "adapter_defects" table
  adapter_args    arguments added to vk-video-adapter-trace's, such as --refuse-chroma 400; an entry
                  that has them runs in adapter mode only
  parser_args     arguments added to vk-video-parser-trace's, such as --fail-decode 3; an entry that
                  has them runs in parser mode only
  mode            parser or adapter: the one mode the entry runs in

Checks per stream: the tool exits 0; the trace equals the golden, or its md5 equals the recorded
one; the set of model mismatch categories equals "known". The model is av1_reference_model.py for
AV1 and h264_header_model.py, h265_header_model.py or vp9_header_model.py for the others. An
unlisted category is a new defect or a regression. A listed category that no longer appears means
its fix landed, and the entry has to go with that fix. A stream an md5 list names but the root does
not hold is reported and does not fail. In adapter mode an md5 entry is also held to its
adapter_known as a whole: a category a stream of the set shows that the list does not name fails,
and so does a listed category no stream shows. A glob entry without md5 is reported per category
and never fails.

--mint records the goldens, the md5 lists and the header dumps of entries that name one, from
this run, and prints each stream's categories for the "known" lists. Exit status: 0 every check
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
import header_check  # noqa: E402
import header_dump  # noqa: E402
import slot_oracle  # noqa: E402

# The suite entry keys each mode reads.
KEYS = {
    "parser": {"golden": "golden", "md5": "md5", "known": "known", "defects": "defects"},
    "adapter": {"golden": "adapter_golden", "md5": "adapter_md5", "known": "adapter_known",
                "defects": "adapter_defects"},
}

FORMAT_BY_EXTENSION = {".h264": "bytestream", ".264": "bytestream", ".h265": "bytestream", ".265": "bytestream",
                       ".ivf": "ivf", ".obu": "annexb"}


def run_tool(tool, codec, fmt, stream, trace, ivf=None, extra=()):
    cmd = [tool, "--codec", codec, "--format", fmt, "-i", stream, "-o", trace] + list(extra)
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


def header_lines(codec, entry, suite_dir, tool, stream, work, ffmpeg, mint):
    """Returns (lines, note); lines is None when no dump is available."""
    if entry.get("headers") and not mint:
        with open(os.path.join(suite_dir, entry["headers"])) as dump:
            return dump.read().splitlines(), ""
    if not shutil.which(ffmpeg):
        return None, "ffmpeg not found"
    if codec == "av1":
        ivf = os.path.join(work, "stream.ivf")
        failure = run_tool(tool, "av1", entry["format"], stream, os.devnull, ivf)
        if failure:
            return None, f"tool {failure} writing the IVF"
        rc, lines = model.run_ffmpeg(ffmpeg, ivf)
    else:
        rc, lines = header_dump.run_ffmpeg(ffmpeg, stream, header_check.MODELS[codec].DEMUXER)
    if rc != 0:
        return None, f"ffmpeg exited {rc}"
    if mint and entry.get("headers"):
        with open(os.path.join(suite_dir, entry["headers"]), "w") as out:
            if codec == "av1":
                out.write(model.compact_headers(lines))
            else:
                header_model = header_check.MODELS[codec]
                out.write(header_dump.compact(lines, header_model.KINDS, header_model.TITLE,
                                              getattr(header_model, "keep", None)))
    return lines, ""


def model_categories(codec, lines, trace):
    """Returns ({category: (pictures, first)}, note); the dict is None when not comparable."""
    if codec != "av1":
        return header_check.categories(header_check.MODELS[codec], lines, trace)
    frames, scalable = model.model_frames(lines)
    if scalable:
        return None, "scalable stream"
    if not frames:
        return None, "no frame headers in the dump"
    report = model.compare(frames, model.trace_pictures(trace), False)
    return {name: (len(e["pictures"]), e["first"]) for name, e in report.categories.items()}, ""


def check_stream(entry, suite, suite_dir, stream, args, work):
    """Returns a list of failure messages."""
    keys = KEYS[args.mode]
    name = entry["name"]
    codec = suite["codec"]
    trace = os.path.join(work, name + ".trace")
    extra = entry.get("adapter_args" if args.mode == "adapter" else "parser_args", [])
    failure = run_tool(args.tool, codec, entry["format"], stream, trace, extra=extra)
    if failure:
        return [f"tool {failure}"]
    failures = []
    if entry.get(keys["golden"]):
        golden = os.path.join(suite_dir, entry[keys["golden"]])
        if args.mint:
            shutil.copyfile(trace, golden)
        elif not os.path.exists(golden):
            failures.append(f"no golden {entry[keys['golden']]}")
        else:
            with open(golden) as g, open(trace) as t:
                expected, actual = g.read().splitlines(), t.read().splitlines()
            if expected != actual:
                diff = list(difflib.unified_diff(expected, actual, "golden", "trace", n=0, lineterm=""))
                failures.append("trace differs from the golden:\n      " + "\n      ".join(diff[:24]))
    if args.mode == "adapter":
        found, note = slot_oracle.categories(trace)
    else:
        lines, note = header_lines(codec, entry, suite_dir, args.tool, stream, work, args.ffmpeg, args.mint)
        if lines is None:
            print(f"    {name}: model not checked ({note})")
            return failures
        found, note = model_categories(codec, lines, trace)
    if found is None:
        print(f"    {name}: model not comparable ({note})")
        return failures
    if args.mint or args.verbose:
        print(f"    {name}: categories {sorted(found)}")
    known = set(entry.get(keys["known"], []))
    defects = suite.get(keys["defects"], {})
    for cat in sorted(set(found) - known):
        failures.append(f"{'oracle' if args.mode == 'adapter' else 'model'} mismatch {cat} in {found[cat][0]} pictures, "
                        f"first {found[cat][1]}")
    for cat in sorted(known - set(found)):
        failures.append(f"{cat} no longer mismatches; remove it from known ({defects.get(cat, 'no defect named')})")
    return failures


def entry_files(entry, suite, suite_dir, base):
    """Returns [(name, path, format)] for a glob or decode_samples entry."""
    if "decode_samples" in entry:
        with open(os.path.join(suite_dir, entry["decode_samples"])) as f:
            samples = json.load(f)["samples"]
        files = []
        listed = set()
        for sample in samples:
            path = os.path.join(base, sample["source_filepath"])
            # A file several tests decode is listed once per test.
            if sample["source_filepath"] in listed:
                continue
            listed.add(sample["source_filepath"])
            if sample["codec"] == suite["codec"] and os.path.exists(path):
                fmt = FORMAT_BY_EXTENSION.get(os.path.splitext(path)[1].lower())
                files.append((sample["source_filepath"], path, fmt))
        return files
    patterns = entry["glob"] if isinstance(entry["glob"], list) else [entry["glob"]]
    paths = sorted({p for pattern in patterns for p in glob.glob(os.path.join(base, pattern))})
    return [(os.path.relpath(p, base), p, entry["format"]) for p in paths]


def check_md5_set(entry, suite, suite_dir, files, args, work):
    """Checks every stream's trace md5 against the entry's list, and in adapter mode the categories
    of the set against its adapter_known. Returns failure messages."""
    keys = KEYS[args.mode]
    md5_path = os.path.join(suite_dir, entry[keys["md5"]])
    listed = read_md5_list(md5_path)
    digests = {}
    failures = []
    shown = {}  # adapter mode: category -> the streams that show it
    keep = os.path.join(args.out, entry["name"]) if args.out else None
    for name, path, fmt in files:
        trace = os.path.join(keep, name + ".trace") if keep else os.path.join(work, "set.trace")
        os.makedirs(os.path.dirname(trace), exist_ok=True)
        failure = run_tool(args.tool, suite["codec"], fmt, path, trace)
        if failure:
            failures.append(f"{name}: tool {failure}")
            continue
        digests[name] = file_md5(trace)
        if args.mode == "adapter":
            found, _ = slot_oracle.categories(trace)
            for cat in found or {}:
                shown.setdefault(cat, []).append(name)
    if args.mode == "adapter":
        if args.mint or args.verbose:
            for cat, names in sorted(shown.items()):
                print(f"    {entry['name']}: {cat} on {len(names)} streams, first {names[0]}")
        if not args.mint:
            known = set(entry.get(keys["known"], []))
            defects = suite.get(keys["defects"], {})
            for cat in sorted(set(shown) - known):
                failures.append(f"oracle category {cat} on {len(shown[cat])} streams, first {shown[cat][0]}")
            for cat in sorted(known - set(shown)):
                failures.append(f"{cat} no longer appears; remove it from {keys['known']} "
                                f"({defects.get(cat, 'no defect named')})")
    if args.mint:
        write_md5_list(md5_path, digests)
        return failures
    for name in sorted(digests):
        if name not in listed:
            failures.append(f"{name}: not in {entry[keys['md5']]}")
        elif listed[name] != digests[name]:
            failures.append(f"{name}: trace differs from the recorded md5")
    missing = sorted(set(listed) - {name for name, _, _ in files})
    print(f"    {entry['name']}: {len(files)} streams, {len(digests)} traced, {len(missing)} listed but not present")
    return failures


def report_glob(entry, suite, base, args, work):
    files = sorted(glob.glob(os.path.join(base, entry["glob"])))
    totals = {}
    tool_failures = 0
    not_comparable = 0
    for stream in files:
        trace = os.path.join(work, "glob.trace")
        if run_tool(args.tool, suite["codec"], entry["format"], stream, trace):
            tool_failures += 1
            continue
        if args.mode == "adapter":
            found = slot_oracle.categories(trace)[0]
        else:
            lines, _ = header_lines("av1", entry, base, args.tool, stream, work, args.ffmpeg, False)
            found = model_categories("av1", lines, trace)[0] if lines is not None else None
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
    parser.add_argument("--mode", choices=sorted(KEYS), default="parser")
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
        only = "adapter" if "adapter_args" in entry else "parser" if "parser_args" in entry else args.mode
        if entry.get("mode", only) != args.mode:
            continue
        base = suite_dir
        if entry.get("root"):
            if entry["root"] not in os.environ:
                print(f"SKIP {entry['name']}: ${entry['root']} is not set")
                continue
            base = os.environ[entry["root"]]
        if ("glob" in entry or "decode_samples" in entry) and KEYS[args.mode]["md5"] in entry:
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
        if "decode_samples" in entry:
            print(f"SKIP {entry['name']}: no {KEYS[args.mode]['md5']} list")
            continue
        if "glob" in entry:
            print(f"REPORT {entry['name']}")
            report_glob(entry, suite, base, args, work)
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
