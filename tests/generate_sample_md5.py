#!/usr/bin/env python3
"""
Generate MD5 checksum for a video sample using ffmpeg.
This is useful for adding expected_output_md5 values to sample JSON files.
"""

import argparse
import subprocess
import sys
from pathlib import Path


def calculate_md5(input_file: str) -> str:
    """Calculate MD5 of video file using ffmpeg.

    Args:
        input_file: Path to the video file

    Returns:
        MD5 hash string, or None if calculation failed
    """
    input_path = Path(input_file)

    if not input_path.exists():
        print(f"Error: File not found: {input_file}", file=sys.stderr)
        return None

    print(f"Calculating MD5 for: {input_file}")

    try:
        result = subprocess.run(
            ["ffmpeg", "-i", str(input_path), "-f", "md5", "-"],
            capture_output=True,
            text=True,
            timeout=120,
            check=False
        )

        # Parse MD5 from output (format: "MD5=<hash>")
        output = result.stdout.strip()
        if "MD5=" in output:
            md5_hash = output.split("MD5=")[-1].strip()
            return md5_hash

        print("Error: Could not parse MD5 from ffmpeg output",
              file=sys.stderr)
        return None

    except subprocess.TimeoutExpired:
        print("Error: Timeout expired while calculating MD5",
              file=sys.stderr)
        return None
    except FileNotFoundError:
        print("Error: ffmpeg not found. Please install ffmpeg.",
              file=sys.stderr)
        return None
    except OSError as e:
        print(f"Error: {e}", file=sys.stderr)
        return None


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Generate MD5 checksum for video samples using ffmpeg",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Calculate MD5 for a single file
  python3 generate_sample_md5.py video.h264

  # Calculate MD5 for multiple files
  python3 generate_sample_md5.py video1.h264 video2.h265 video3.ivf

  # Use with decode_samples.json
  python3 generate_sample_md5.py resources/video/avc/clip-a.h264
        """
    )

    parser.add_argument(
        "files",
        nargs="+",
        help="Video file(s) to calculate MD5 for"
    )

    args = parser.parse_args()

    failed = []
    results = []

    for file in args.files:
        md5_hash = calculate_md5(file)
        if md5_hash:
            results.append((file, md5_hash))
            print(f"✓ MD5: {md5_hash}\n")
        else:
            failed.append(file)

    # Print summary
    if results:
        print("=" * 70)
        print("RESULTS")
        print("=" * 70)
        for file, md5_hash in results:
            print(f"{Path(file).name}:")
            print(f"  \"expected_output_md5\": \"{md5_hash}\"")

    if failed:
        print("\n" + "=" * 70)
        print("FAILED")
        print("=" * 70)
        for file in failed:
            print(f"✗ {file}")
        return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
