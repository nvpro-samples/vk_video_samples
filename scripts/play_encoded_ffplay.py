#!/usr/bin/env python3
"""
Play encoded bitstreams with ffplay for visual verification.

Discovers .264, .265, .ivf files in a directory and plays them sequentially
with ffplay. Press Q to advance to the next file, Ctrl+C to stop.

Usage:
    python play_encoded_ffplay.py /tmp/vulkan_encoder_profile_tests
    python play_encoded_ffplay.py /tmp/vulkan_encoder_profile_tests --filter h265
    python play_encoded_ffplay.py /tmp/vulkan_encoder_profile_tests --filter lossless
    python play_encoded_ffplay.py --play /tmp/vulkan_encoder_profile_tests/file.264
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import List

EXT_CODEC = {
    ".264": "H.264",
    ".265": "H.265",
    ".ivf": "AV1",
}


def discover_bitstreams(directory: Path, filt: str = "") -> List[Path]:
    """Find all bitstream files, optionally filtered."""
    files = []
    for f in sorted(directory.iterdir()):
        if f.is_file() and f.suffix in EXT_CODEC and f.stat().st_size > 0:
            if filt and filt.lower() not in f.name.lower():
                continue
            files.append(f)
    return files


def play_file(filepath: Path, loop: bool = False):
    """Play a bitstream with ffplay. Blocks until user closes (press Q)."""
    codec = EXT_CODEC.get(filepath.suffix, "?")
    title = f"{filepath.name} [{codec}]"

    cmd = ["ffplay", "-window_title", title]
    if loop:
        cmd += ["-loop", "0"]
    cmd += ["-i", str(filepath)]

    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(
        description="Play encoded bitstreams with ffplay for visual verification")
    parser.add_argument("directory", nargs="?", type=Path, default=None,
                        help="Directory containing encoded bitstreams (.264, .265, .ivf)")
    parser.add_argument("--play", type=Path, default=None,
                        help="Play a single file (loops)")
    parser.add_argument("--filter", type=str, default="",
                        help="Only play files matching pattern (e.g. 'h265', 'lossless', 'av1')")
    args = parser.parse_args()

    if args.play:
        if not args.play.is_file():
            print(f"Error: File not found: {args.play}", file=sys.stderr)
            return 1
        codec = EXT_CODEC.get(args.play.suffix, "?")
        print(f"Playing: {args.play.name} [{codec}] (loops, press Q to quit)")
        play_file(args.play, loop=True)
        return 0

    if args.directory is None:
        parser.print_help()
        return 1

    if not args.directory.is_dir():
        print(f"Error: Not a directory: {args.directory}", file=sys.stderr)
        return 1

    bitstreams = discover_bitstreams(args.directory, args.filter)
    if not bitstreams:
        print(f"No bitstream files found in {args.directory}")
        if args.filter:
            print(f"  (filter: '{args.filter}')")
        return 0

    print(f"Found {len(bitstreams)} bitstreams in {args.directory}")
    if args.filter:
        print(f"Filter: {args.filter}")
    print("Press Q in ffplay window to advance to next file, Ctrl+C to stop.\n")

    for i, bs in enumerate(bitstreams, 1):
        codec = EXT_CODEC.get(bs.suffix, "?")
        print(f"  [{i}/{len(bitstreams)}] {bs.name} [{codec}]")
        try:
            play_file(bs)
        except KeyboardInterrupt:
            print("\n  Interrupted.")
            break

    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
