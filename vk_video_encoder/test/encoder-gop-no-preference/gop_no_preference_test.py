#!/usr/bin/env python3
"""
An encode on a device that states no preferred GOP length.

VK_LAYER_VVS_gop_no_preference, built next to this script, makes every encode
quality level state preferredGopFrameCount 0, "no preference", which a device
may state. The encoder takes the GOP length from that preference when none is
asked for, and the GOP structure divides by the length from the second picture
on, so the encoder must take the codec's default length instead.

For each codec the encoder runs under the layer with no GOP length asked for,
and passes when the layer printed its line for the codec (the device stated no
preference), the encoder exited 0 with a stream, and it reports the codec's
default GOP length. A codec the device does not encode (the encoder's exit
status 69) is skipped; every codec skipped is a skip (77), and a failure with
VVS_REQUIRE_GPU set.

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

LAYER_NAME = "VK_LAYER_VVS_gop_no_preference"
ENCODER_EXIT_UNSUPPORTED = 69
EXIT_SKIP = 77
WIDTH = 320
HEIGHT = 240
NUM_FRAMES = 8

# (codec option, the layer's name for it, output extension, the GOP length the
# codec takes when the device states none: GOP_LENGTH_DEFAULT of
# EncoderConfigH264 and EncoderConfigAV1, DEFAULT_GOP_FRAME_COUNT for H.265)
CODECS = (
    ("h264", "H.264", "264", 30),
    ("h265", "H.265", "265", 16),
    ("av1", "AV1", "ivf", 60),
)


def write_input(path):
    """NUM_FRAMES 8-bit 4:2:0 pictures of a luma ramp that moves each frame."""
    chroma = bytes([128]) * (WIDTH * HEIGHT // 2)
    with open(path, "wb") as f:
        for frame in range(NUM_FRAMES):
            luma = bytes((x + y + 4 * frame) & 0xFF
                         for y in range(HEIGHT) for x in range(WIDTH))
            f.write(luma)
            f.write(chroma)


def layer_environment(layer_dir):
    """The environment with the layer's manifest directory and the layer
    ahead of any the caller names."""
    env = dict(os.environ)
    for name, value, sep in (("VK_ADD_LAYER_PATH", layer_dir, os.pathsep),
                             ("VK_INSTANCE_LAYERS", LAYER_NAME, ":")):
        env[name] = value + (sep + env[name] if env.get(name) else "")
    return env


def judge(run, codec, output):
    """The verdict on one codec's encode: (verdict, detail), verdict PASS,
    FAIL or SKIP."""
    label, default_gop = codec[1], codec[3]
    if run.returncode == ENCODER_EXIT_UNSUPPORTED:
        return "SKIP", "the device does not encode it (exit 69)"
    marker = f"[{LAYER_NAME}] {label} preferredGopFrameCount"
    stated = [line for line in run.stderr.splitlines() if line.startswith(marker)]
    gop = re.search(r"GOP frame count: (\d+)", run.stdout + run.stderr)
    failure = None
    if run.returncode < 0:
        failure = f"the encoder was killed by signal {-run.returncode}"
    elif run.returncode != 0:
        failure = f"the encoder exited {run.returncode}: {run.stderr.strip()[-400:]}"
    elif not stated:
        failure = f"the layer did not state the {label} preference: no '{marker}' line"
    elif not os.path.exists(output) or os.path.getsize(output) == 0:
        failure = "the encoder exited 0 with no stream"
    elif gop is None:
        failure = "the encoder did not report its GOP length"
    elif int(gop.group(1)) != default_gop:
        failure = f"GOP length {gop.group(1)}, want the {label} default {default_gop}"
    if failure is not None:
        return "FAIL", failure
    return "PASS", f"{stated[0][len(marker):].strip()}, GOP length {default_gop}"


def run_codec(args, codec, env, input_path):
    """Encode one codec under the layer with no GOP length asked for, and
    judge it."""
    output = os.path.join(args.output_dir, f"gop_no_preference.{codec[2]}")
    if os.path.exists(output):
        os.remove(output)
    cmd = [args.encoder, "-i", input_path, "-o", output, "-c", codec[0],
           "--inputWidth", str(WIDTH), "--inputHeight", str(HEIGHT),
           "--inputBpp", "8", "--numFrames", str(NUM_FRAMES), "--verbose"]
    if args.device_id:
        cmd += ["--deviceID", args.device_id]
    try:
        run = subprocess.run(cmd, env=env, capture_output=True, text=True,
                             errors="replace", timeout=120, check=False)
    except subprocess.TimeoutExpired:
        return "FAIL", "the encoder did not finish in 120 s"
    log = os.path.join(args.output_dir, f"gop_no_preference.{codec[0]}.log")
    with open(log, "w", encoding="utf-8") as f:
        f.write(f"{' '.join(cmd)}\nexit status {run.returncode}\n"
                f"--- stdout\n{run.stdout}--- stderr\n{run.stderr}")
    verdict, detail = judge(run, codec, output)
    return verdict, (f"{detail} (log: {log})" if verdict == "FAIL" else detail)


def main():
    """Run each codec and return the exit status."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--encoder", required=True, help="vulkan-video-enc-test")
    parser.add_argument("--layer-dir", required=True,
                        help="the directory holding the layer's manifest")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--device-id",
                        help="the encoder's --deviceID (hex); by default the first device "
                             "that encodes the codec")
    args = parser.parse_args()

    if not os.path.exists(args.encoder):
        print(f"SKIP: no encoder at {args.encoder}")
        return EXIT_SKIP
    os.makedirs(args.output_dir, exist_ok=True)
    input_path = os.path.join(args.output_dir, f"ramp_{WIDTH}x{HEIGHT}.yuv")
    write_input(input_path)
    env = layer_environment(args.layer_dir)

    verdicts = []
    for codec in CODECS:
        verdict, detail = run_codec(args, codec, env, input_path)
        print(f"{verdict:4}  {codec[1]}: {detail}")
        verdicts.append(verdict)

    if "FAIL" in verdicts:
        return 1
    if "PASS" not in verdicts:
        return 1 if os.environ.get("VVS_REQUIRE_GPU") == "1" else EXIT_SKIP
    return 0


if __name__ == "__main__":
    sys.exit(main())
