#!/usr/bin/env python3
"""
The video session parameters objects a decode keeps alive.

The parser gives the decoder every parameter set it parses, and a set sent
again, as before every key frame of an MP4 input, makes a new session
parameters object. A decode must keep only those its latest sets and its
pictures in flight use, not every object a long stream makes.

VK_LAYER_VVS_session_parameters_count, built next to this script, counts the
video session parameters objects of each device and prints, when the device is
destroyed, how many were created, the most alive at once and how many are still
alive. For H.264, H.265 and AV1 the script encodes 300 pictures with a key
frame, and its parameter sets, every third one (ffmpeg's libx264, libx265 and
libaom-av1), decodes each under the layer, and passes when the decoder exits 0,
at most PEAK_LIMIT objects are alive at once and none is alive when the device
is destroyed. A codec the device does not decode (the decoder's exit status 69)
or that ffmpeg cannot encode is skipped; every codec skipped is a skip (77),
and a failure with VVS_REQUIRE_GPU set.

Copyright 2026 NVIDIA Corporation.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import argparse
import os
import re
import subprocess
import sys

LAYER_NAME = "VK_LAYER_VVS_session_parameters_count"
DECODER_EXIT_UNSUPPORTED = 69
EXIT_SKIP = 77
# At least every decode minimum of the GPUs the tests run on: H.265 144x144, AV1 128x128.
WIDTH = 176
HEIGHT = 144
NUM_FRAMES = 300
# Above the objects the pictures a decode holds use, and far below the 100 to 300 objects the
# streams' 100 key frames make.
PEAK_LIMIT = 32

# (the decoder's codec, ffmpeg's encoder options, the stream's file name)
CODECS = (
    ("h264", ["-c:v", "libx264", "-g", "3", "-keyint_min", "3", "-sc_threshold", "0", "-bf", "0",
              "-x264-params", "repeat-headers=1"], "repeated-parameter-sets.h264"),
    ("h265", ["-c:v", "libx265", "-x265-params",
              "keyint=3:min-keyint=3:scenecut=0:bframes=0:repeat-headers=1:log-level=error"],
     "repeated-parameter-sets.h265"),
    ("av1", ["-c:v", "libaom-av1", "-g", "3", "-keyint_min", "3", "-cpu-used", "8", "-b:v", "200k"],
     "repeated-parameter-sets.ivf"),
)

LAYER_LINE = re.compile(r"\[" + LAYER_NAME + r"\] created (\d+), peak live (\d+), "
                        r"live at vkDestroyDevice (-?\d+)")


def encode(codec, path):
    """The stream of a codec, or None when ffmpeg cannot encode it."""
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
           "-i", f"testsrc=size={WIDTH}x{HEIGHT}:rate=30", "-frames:v", str(NUM_FRAMES),
           "-pix_fmt", "yuv420p"] + codec[1] + [path]
    try:
        run = subprocess.run(cmd, capture_output=True, text=True, errors="replace",
                             timeout=300, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if run.returncode != 0 or not os.path.exists(path) or os.path.getsize(path) == 0:
        return None
    return path


def layer_environment(layer_dir):
    """The environment with the layer's manifest directory and the layer
    ahead of any the caller names."""
    env = dict(os.environ)
    for name, value, sep in (("VK_ADD_LAYER_PATH", layer_dir, os.pathsep),
                             ("VK_INSTANCE_LAYERS", LAYER_NAME, ":")):
        env[name] = value + (sep + env[name] if env.get(name) else "")
    return env


def judge(run):
    """The verdict on one decode: (verdict, detail), verdict PASS, FAIL or SKIP."""
    if run.returncode == DECODER_EXIT_UNSUPPORTED:
        return "SKIP", "the device does not decode it (exit 69)"
    counts = LAYER_LINE.findall(run.stderr + run.stdout)
    failure = None
    if run.returncode < 0:
        failure = f"the decoder was killed by signal {-run.returncode}"
    elif run.returncode != 0:
        failure = f"the decoder exited {run.returncode}"
    elif not counts:
        failure = "the layer reported no device"
    else:
        created, peak, live = (int(value) for value in counts[-1])
        if live != 0:
            failure = f"{live} of {created} objects were alive when the device was destroyed"
        elif peak > PEAK_LIMIT:
            failure = f"{peak} of {created} objects were alive at once, more than {PEAK_LIMIT}"
    if failure is not None:
        return "FAIL", failure
    created, peak, _ = counts[-1]
    return "PASS", f"created {created}, at most {peak} alive at once"


def run_codec(args, codec, env):
    """Encode and decode one codec: (verdict, detail)."""
    stream = encode(codec, os.path.join(args.output_dir, codec[2]))
    if stream is None:
        return "SKIP", "ffmpeg cannot encode it"
    cmd = [args.decoder, "-i", stream, "--codec", codec[0], "--noPresent"]
    if args.device_id:
        cmd += ["--deviceID", args.device_id]
    try:
        run = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace",
                             timeout=300, check=False)
    except subprocess.TimeoutExpired:
        return "FAIL", "the decoder did not finish in 300 s"
    log = os.path.join(args.output_dir, f"{codec[2]}.log")
    with open(log, "w", encoding="utf-8") as f:
        f.write(f"{' '.join(cmd)}\nexit status {run.returncode}\n"
                f"--- stdout\n{run.stdout}--- stderr\n{run.stderr}")
    verdict, detail = judge(run)
    return verdict, (f"{detail} (log: {log})" if verdict == "FAIL" else detail)


def main():
    """Run each codec and return the exit status."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--decoder", required=True, help="vulkan-video-dec-test")
    parser.add_argument("--layer-dir", required=True,
                        help="the directory holding the layer's manifest")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--device-id",
                        help="the decoder's --deviceID (hex); by default the first device "
                             "that decodes the codec")
    args = parser.parse_args()

    if not os.path.exists(args.decoder):
        print(f"SKIP: no decoder at {args.decoder}")
        return EXIT_SKIP
    os.makedirs(args.output_dir, exist_ok=True)
    env = layer_environment(args.layer_dir)

    verdicts = []
    for codec in CODECS:
        verdict, detail = run_codec(args, codec, env)
        print(f"{verdict:4}  {codec[0]}: {detail}")
        verdicts.append(verdict)

    if "FAIL" in verdicts:
        return 1
    if "PASS" not in verdicts:
        return 1 if os.environ.get("VVS_REQUIRE_GPU") == "1" else EXIT_SKIP
    return 0


if __name__ == "__main__":
    sys.exit(main())
