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
"""Runs the encoder DPB suite.

Usage: encoder_dpb_suite.py --tool T --suite S [--out DIR] [--mint] [-v]

The suite file (suites/<codec>.json) names its "codec", a "defects" table, and "configs", each
with:
  name     a label
  args     the vk-video-encoder-dpb-trace arguments after --codec
  golden   the recorded trace, relative to the suite file's directory, or in its place
  md5      the md5 of the trace, for a long configuration whose trace is not worth committing
  known    the model mismatch categories the configuration shows, each one a defect the
           "defects" table names
  debug    why the configuration stops at an assert in a build with assertions; when the tool
           reports one (--asserts), the configuration is reported as skipped

Checks per configuration: the tool exits 0; the trace equals the golden, or its md5 the recorded one; the set of
encoder_dpb_model.py categories equals "known". An unlisted category is a new defect or a
regression; a listed one that no longer appears means its fix landed, and the entry has to go
with that fix. --mint records the goldens and md5s from this run and prints each configuration's
categories. Exit status: 0 every check held, 1 a check failed, 2 nothing ran.
"""
import argparse
import difflib
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import encoder_dpb_model  # noqa: E402


def run_tool(tool, codec, args, trace):
    cmd = [tool, "--codec", codec] + args + ["-o", trace]
    try:
        result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors="replace",
                                timeout=120)
    except subprocess.TimeoutExpired:
        return "timeout"
    return None if result.returncode == 0 else f"exit status {result.returncode}"


def check_config(config, suite, suite_dir, args, work):
    """Returns a list of failure messages."""
    trace = os.path.join(work, config["name"] + ".trace")
    failure = run_tool(args.tool, suite["codec"], config["args"], trace)
    if failure:
        return [f"tool {failure}"]
    failures = []
    if "md5" in config:
        with open(trace, "rb") as t:
            digest = hashlib.md5(t.read()).hexdigest()
        if args.mint:
            config["md5"] = digest
        elif digest != config["md5"]:
            failures.append(f"trace md5 {digest}, recorded {config['md5']}")
        golden = None
    else:
        golden = os.path.join(suite_dir, config["golden"])
    if golden is None:
        pass
    elif args.mint:
        os.makedirs(os.path.dirname(golden), exist_ok=True)
        shutil.copyfile(trace, golden)
    elif not os.path.exists(golden):
        failures.append(f"no golden {config['golden']}")
    else:
        with open(golden) as g, open(trace) as t:
            expected, actual = g.read().splitlines(), t.read().splitlines()
        if expected != actual:
            diff = list(difflib.unified_diff(expected, actual, "golden", "trace", n=0, lineterm=""))
            failures.append("trace differs from the golden:\n      " + "\n      ".join(diff[:24]))
    found, note = encoder_dpb_model.categories(trace)
    if found is None:
        failures.append(f"model not comparable ({note})")
        return failures
    if args.mint or args.verbose:
        print(f"    {config['name']}: categories {sorted(found)}")
    known = set(config.get("known", []))
    defects = suite.get("defects", {})
    for cat in sorted(set(found) - known):
        failures.append(f"model mismatch {cat} in {found[cat][0]} pictures, first {found[cat][1]}")
    for cat in sorted(known - set(found)):
        failures.append(f"{cat} no longer mismatches; remove it from known ({defects.get(cat, 'no defect named')})")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--suite", required=True)
    parser.add_argument("--out", help="keep the traces here")
    parser.add_argument("--mint", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    suite_dir = os.path.dirname(os.path.abspath(args.suite))
    with open(args.suite) as f:
        suite = json.load(f)
    work = args.out or tempfile.mkdtemp(prefix="encoder-dpb-")
    os.makedirs(work, exist_ok=True)

    asserts = subprocess.run([args.tool, "--asserts"], stdout=subprocess.PIPE, text=True).stdout.strip() == "asserts=1"
    ran = 0
    failed = 0
    for config in suite["configs"]:
        if asserts and config.get("debug"):
            print(f"SKIP {config['name']}: {config['debug']}")
            continue
        ran += 1
        failures = check_config(config, suite, suite_dir, args, work)
        if failures:
            failed += 1
            print(f"FAIL {config['name']}")
            for message in failures:
                print(f"    {message}")
        else:
            print(f"PASS {config['name']}")
    if args.mint:
        with open(args.suite, "w") as f:
            json.dump(suite, f, indent=2)
            f.write("\n")
    if not args.out:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{ran} configurations checked, {failed} failed")
    if ran == 0:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
