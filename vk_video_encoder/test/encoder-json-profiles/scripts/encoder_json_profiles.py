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
"""The encoder's JSON profiles through the encoder library's loader.

Usage: encoder_json_profiles.py --tool T --profiles DIR --schema S [-v]

For every *.json file under DIR but the schema:
  - the file validates against the schema: with jsonschema when it is installed, otherwise by the
    schema rules this script reads itself (no key outside "properties", and each key's type,
    enum, minimum, maximum, minLength and maxLength);
  - vk-video-encoder-json-load loads it with result 0, and each of these settings the file names
    reaches the configuration with the file's value: codec, rcMode, averageBitrate, maxBitrate,
    vbvBufferSize, gopLength, bFrameCount, idrPeriod, closedGop, frameRateNum, frameRateDen,
    qualityPreset and tuningMode (default when the file has none). The constant QPs, taken in the
    file's order ("qp" sets the I and P ones), are the file's, -1 and 0 both leaving a QP unset
    (0); a minQp or maxQp is the file's, and takes effect (its marker set) unless it is -1.
A copy of a profile with QP limits (minQp 20, maxQp 40) and "qp" -1 is checked the same way. Then
the loader must refuse (result -1) a copy with a key the schema does not define, one with a
tuningMode no encoder takes, and one with a codec the schema does not define.

Last, encoder command lines with --encoderConfig, through vk-video-encoder-json-load --argv: the
file is the base configuration, which --qualityLevel and --gopFrameCount override whether they
come before it or after it, and --codec selects the codec configuration and its codec over the
file's codec. Without --codec the codec of the last file that names one selects them, and a
command line whose files name no codec, or one the schema does not define, creates none. The H.264
and H.265 rate control states the IDR period the GOP emits: the one given, or 0 when the GOP has
none (no --idrPeriod and no device to prefer one, which is what a device that states no preferred
period leaves).

Exit status: 0 every check held, 1 a check failed, 2 nothing to check.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

CODECS = {"h264": "h264", "264": "h264", "h265": "h265", "hevc": "h265", "265": "h265", "av1": "av1"}
CHECKED = ["codec", "rcMode", "averageBitrate", "maxBitrate", "vbvBufferSize", "gopLength", "bFrameCount", "idrPeriod",
           "closedGop", "frameRateNum", "frameRateDen", "qualityPreset", "tuningMode"]
TYPES = {"string": str, "integer": int, "boolean": bool, "array": list, "object": dict}


def load(tool, paths, work):
    """{path: {field: value}} from the tool's lines. The tool runs in work: the loader creates the
    output file a configuration names (encoder_config.example.json names output.264)."""
    out = subprocess.run([tool] + paths, capture_output=True, text=True, check=True, cwd=work).stdout
    loaded = {}
    for line in out.splitlines():
        fields = dict(token.split("=", 1) for token in line.split())
        loaded[fields["file"]] = fields
    return loaded


def expected(profile, key):
    value = profile[key]
    if key == "codec":
        return CODECS.get(value, "none")
    if key == "closedGop":
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value % (1 << 32))
    return str(value)


def schema_errors(profile, schema):
    try:
        import jsonschema
    except ImportError:
        jsonschema = None
    if jsonschema is not None:
        return [e.message for e in jsonschema.Draft7Validator(schema).iter_errors(profile)]
    errors = []
    properties = schema.get("properties", {})
    for key, value in profile.items():
        rule = properties.get(key)
        if rule is None:
            if schema.get("additionalProperties") is False:
                errors.append(f"{key} is not a schema property")
            continue
        kind = TYPES.get(rule.get("type"))
        if kind is not None and (not isinstance(value, kind) or (kind is int and isinstance(value, bool))):
            errors.append(f"{key} {value!r} is not {rule['type']}")
            continue
        if "enum" in rule and value not in rule["enum"]:
            errors.append(f"{key} {value!r} not in {rule['enum']}")
        if "minimum" in rule and isinstance(value, int) and value < rule["minimum"]:
            errors.append(f"{key} {value} below {rule['minimum']}")
        if "maximum" in rule and isinstance(value, int) and value > rule["maximum"]:
            errors.append(f"{key} {value} above {rule['maximum']}")
        if "minLength" in rule and isinstance(value, str) and len(value) < rule["minLength"]:
            errors.append(f"{key} {value!r} shorter than {rule['minLength']}")
        if "maxLength" in rule and isinstance(value, str) and len(value) > rule["maxLength"]:
            errors.append(f"{key} {value!r} longer than {rule['maxLength']}")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--profiles", required=True)
    parser.add_argument("--schema", required=True)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    schema = json.load(open(args.schema))
    profiles = os.path.abspath(args.profiles)
    paths = sorted(os.path.join(root, name) for root, _, names in os.walk(profiles) for name in names
                   if name.endswith(".json") and os.path.join(root, name) != os.path.abspath(args.schema))
    if not paths:
        print(f"no profiles under {args.profiles}")
        return 2
    failures = []
    with tempfile.TemporaryDirectory() as work:
        tool = os.path.abspath(args.tool)
        loaded = load(tool, paths, work)
        for path in paths:
            check(os.path.relpath(path, profiles), json.load(open(path)), loaded.get(path), schema, failures, args.verbose)

        base = json.load(open(paths[0]))
        accepted = dict(base, minQp=20, maxQp=40, qp=-1)
        refused = {"unknown-key.json": dict(base, tuningMod="lossless"), "bad-tuning-mode.json": dict(base, tuningMode="fastest"),
                   "bad-codec.json": dict(base, codec="vp9")}
        made = []
        for name, content in [("qp-limits.json", accepted)] + list(refused.items()):
            made.append(os.path.join(work, name))
            with open(made[-1], "w") as f:
                json.dump(content, f)
        results = load(tool, made, work)
        check("qp-limits.json", accepted, results.get(made[0]), schema, failures, args.verbose)
        for path in made[1:]:
            if results[path]["rc"] == "0":
                failures.append(f"{os.path.basename(path)}: the loader accepts it")
        check_argv(tool, work, failures)

    for failure in failures:
        print(f"FAIL {failure}")
    print(f"{len(paths)} profiles checked, {len(failures)} failures")
    return 1 if failures else 0


def run_argv(tool, args, work):
    """The configuration vk-video-encoder-json-load --argv creates for an encoder command line
    (with an input extent and an output file in work)."""
    base = ["--inputWidth", "64", "--inputHeight", "64", "-o", os.path.join(work, "argv.out")]
    out = subprocess.run([tool, "--argv"] + base + args, capture_output=True, text=True, check=True, cwd=work).stdout
    line = next((line for line in out.splitlines() if line.startswith("file=argv ")), "file=argv rc=none")
    return dict(token.split("=", 1) for token in line.split())


def check_argv(tool, work, failures):
    """--encoderConfig on an encoder command line: the base configuration, which the options
    override wherever they stand, --codec over the file's codec, and the files' codec without
    --codec."""
    files = {}
    for name, content in [("base-h264.json", {"codec": "h264", "qualityPreset": 3, "gopLength": 120}),
                          ("h265.json", {"codec": "h265", "qualityPreset": 1}), ("av1.json", {"codec": "av1"}),
                          ("no-codec.json", {"qualityPreset": 2}), ("vp9.json", {"codec": "vp9"})]:
        files[name] = os.path.join(work, name)
        with open(files[name], "w") as f:
            json.dump(content, f)
    path = files["base-h264.json"]
    overridden = {"config": "h264", "codec": "h264", "qualityPreset": "2", "gopLength": "60"}
    cases = [
        (["--codec", "h264", "--encoderConfig", path],
         {"config": "h264", "codec": "h264", "qualityPreset": "3", "gopLength": "120"}),
        (["--codec", "h264", "--qualityLevel", "2", "--gopFrameCount", "60", "--encoderConfig", path], overridden),
        (["--codec", "h264", "--encoderConfig", path, "--qualityLevel", "2", "--gopFrameCount", "60"], overridden),
        (["--codec", "av1", "--encoderConfig", path], {"config": "av1", "codec": "av1", "qualityPreset": "3"}),
        (["--encoderConfig", files["h265.json"]], {"config": "h265", "codec": "h265", "qualityPreset": "1"}),
        (["--encoderConfig", path, "--encoderConfig", files["av1.json"]], {"config": "av1", "codec": "av1", "qualityPreset": "3"}),
        (["--encoderConfig", files["no-codec.json"]], None),
        (["--encoderConfig", files["vp9.json"]], None),
        (["--codec", "h264"], {"config": "h264", "idrPeriod": "0", "rcIdrPeriod": "0"}),
        (["--codec", "h265"], {"config": "h265", "idrPeriod": "0", "rcIdrPeriod": "0"}),
        (["--codec", "h264", "--gopFrameCount", "30", "--idrPeriod", "90"],
         {"config": "h264", "rcGopFrameCount": "30", "rcIdrPeriod": "90"}),
        (["--codec", "h265", "--gopFrameCount", "16", "--idrPeriod", "64"],
         {"config": "h265", "rcGopFrameCount": "16", "rcIdrPeriod": "64"}),
    ]
    for args, want in cases:
        got = run_argv(tool, args, work)
        shown = " ".join(os.path.basename(arg) if arg in files.values() else arg for arg in args)
        if want is None:
            if got["rc"] == "0" or got.get("config") != "none":
                failures.append(f"argv {shown}: creates a {got.get('config')} configuration (rc {got['rc']})")
            continue
        if got["rc"] != "0":
            failures.append(f"argv {shown}: no configuration (rc {got['rc']})")
            continue
        for key, value in want.items():
            if got.get(key) != value:
                failures.append(f"argv {shown}: {key} is {got.get(key)}, not {value}")


def check(name, profile, fields, schema, failures, verbose):
    """The checks of one file the loader must accept."""
    for error in schema_errors(profile, schema):
        failures.append(f"{name}: schema: {error}")
    if fields is None or fields["rc"] != "0":
        failures.append(f"{name}: the loader refuses it")
        return
    for key in CHECKED:
        want = expected(profile, key) if key in profile else ("default" if key == "tuningMode" else None)
        if want is not None and fields[key] != want:
            failures.append(f"{name}: {key} loads as {fields[key]}, the file has {want}")
    qps = {"constQpI": 0, "constQpP": 0, "constQpB": 0}
    named = False
    for key, value in profile.items():
        if key in qps:
            qps[key] = max(value, 0)
            named = True
        elif key == "qp":
            qps["constQpI"] = qps["constQpP"] = max(value, 0)
            named = True
    for key, want in qps.items():
        if named and fields[key] != str(want):
            failures.append(f"{name}: {key} loads as {fields[key]}, the file gives {want}")
    for key in ("minQp", "maxQp"):
        if key in profile:
            want = (str(profile[key]), "1" if profile[key] >= 0 else "0")
            if (fields[key], fields[key + "Set"]) != want:
                failures.append(f"{name}: {key} loads as {fields[key]} (set {fields[key + 'Set']}), the file gives {want[0]} (set {want[1]})")
    if verbose:
        print(f"  {name}: tuningMode={fields['tuningMode']} rcMode={fields['rcMode']}")


if __name__ == "__main__":
    sys.exit(main())
