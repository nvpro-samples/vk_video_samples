#!/usr/bin/env python3
"""
Vulkan Video Decoder Roundtrip Test

Decodes all encoded bitstreams in a directory using vulkan-video-dec-test.
Validates that every file produced by the encoder profile tests can be decoded.

Bitstream filenames follow: {input_base}_{codec}_{profile}.{ext}
  e.g. 1920x1080_420_8le_h264_nvidia_high_quality_p4.264

Usage:
    python run_decoder_roundtrip.py /tmp/vulkan_encoder_profile_tests [OPTIONS]
    python run_decoder_roundtrip.py /tmp/vulkan_encoder_profile_tests --filter h265
    python run_decoder_roundtrip.py /tmp/vulkan_encoder_profile_tests --filter nvidia_lossless
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Tuple


def supports_color() -> bool:
    if sys.platform == "win32":
        return os.environ.get("WT_SESSION") is not None
    return hasattr(sys.stdout, "isatty") and sys.stdout.isatty()


if supports_color():
    RED, GREEN, YELLOW, CYAN, BOLD, NC = (
        "\033[0;31m", "\033[0;32m", "\033[1;33m", "\033[0;36m", "\033[1m", "\033[0m")
else:
    RED = GREEN = YELLOW = CYAN = BOLD = NC = ""

# Bitstream extensions → codec name
EXT_CODEC = {
    ".264": "H.264",
    ".265": "H.265",
    ".ivf": "AV1",
    ".bin": "unknown",
}


def discover_bitstreams(directory: Path, filt: str = "") -> List[Path]:
    """Find all bitstream files, optionally filtered."""
    exts = set(EXT_CODEC.keys())
    files = []
    for f in sorted(directory.iterdir()):
        if f.is_file() and f.suffix in exts and f.stat().st_size > 0:
            if filt and filt.lower() not in f.name.lower():
                continue
            files.append(f)
    return files


def run_decode(decoder: str, bitstream: Path, timeout_sec: int = 60,
               verbose: bool = False) -> Tuple[bool, float, str]:
    """Run decoder on one bitstream. Returns (success, duration, message)."""
    cmd = [decoder, "-i", str(bitstream)]

    if verbose:
        print(f"    {CYAN}{' '.join(cmd)}{NC}")

    start = time.time()
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout_sec)
        duration = time.time() - start
        output = result.stdout + result.stderr

        if result.returncode == 0:
            # Check for decode errors in output
            lower = output.lower()
            if "error" in lower and "validation" not in lower:
                return False, duration, "Decode error in output"
            return True, duration, ""
        else:
            msg = result.stderr.strip()[:200] or result.stdout.strip()[:200] or "Unknown error"
            return False, duration, msg

    except subprocess.TimeoutExpired:
        return False, timeout_sec, "Timeout"
    except Exception as exc:
        return False, 0, str(exc)


def main():
    parser = argparse.ArgumentParser(
        description="Decode all bitstreams in a directory with vulkan-video-dec-test")
    parser.add_argument("directory", type=Path,
                        help="Directory containing encoded bitstreams (.264, .265, .ivf)")
    parser.add_argument("--decoder", type=str, default=None,
                        help="Path to vulkan-video-dec-test (auto-detected from project root)")
    parser.add_argument("--filter", type=str, default="",
                        help="Only decode files matching pattern (e.g. 'h265', 'lossless', 'av1')")
    parser.add_argument("--timeout", type=int, default=60,
                        help="Timeout per decode in seconds (default: 60)")
    parser.add_argument("--verbose", action="store_true",
                        help="Show decode commands")
    args = parser.parse_args()

    # Find decoder
    if args.decoder:
        decoder = args.decoder
    else:
        script_dir = Path(__file__).parent
        project_root = script_dir.parent
        decoder = str(project_root / "build" / "vk_video_decoder" / "test" / "vulkan-video-dec-test")

    if not os.access(decoder, os.X_OK):
        print(f"{RED}Error: Decoder not found: {decoder}{NC}")
        return 1

    if not args.directory.is_dir():
        print(f"{RED}Error: Not a directory: {args.directory}{NC}")
        return 1

    bitstreams = discover_bitstreams(args.directory, args.filter)
    if not bitstreams:
        print(f"{YELLOW}No bitstream files found in {args.directory}{NC}")
        if args.filter:
            print(f"  (filter: '{args.filter}')")
        return 0

    print(f"\n{BOLD}{'=' * 55}{NC}")
    print(f"{BOLD} Decoder Roundtrip Test{NC}")
    print(f"{BOLD}{'=' * 55}{NC}")
    print(f"Directory: {args.directory}")
    print(f"Decoder:   {decoder}")
    print(f"Files:     {len(bitstreams)}")
    if args.filter:
        print(f"Filter:    {args.filter}")
    print()

    passed = failed = 0

    for bs in bitstreams:
        codec = EXT_CODEC.get(bs.suffix, "?")
        ok, duration, msg = run_decode(decoder, bs, args.timeout, args.verbose)

        if ok:
            print(f"  {GREEN}✓{NC} {bs.name:<60s} {codec:<6s} {duration:.2f}s")
            passed += 1
        else:
            print(f"  {RED}✗{NC} {bs.name:<60s} {codec:<6s} {duration:.2f}s")
            if msg:
                print(f"    {msg}")
            failed += 1

    print(f"\n{BOLD}{'=' * 55}{NC}")
    print(f"Results: {GREEN}{passed} passed{NC}, {RED}{failed} failed{NC} / {passed + failed} total")

    if failed == 0:
        print(f"{GREEN}{BOLD}All decodes passed!{NC}")
    else:
        print(f"{RED}{BOLD}Some decodes failed.{NC}")

    return 1 if failed > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
