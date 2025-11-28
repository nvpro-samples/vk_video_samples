#!/usr/bin/env python3
"""
Cross-platform script to run Vulkan Video Encoder with Adaptive Quantization (AQ) support.

This script works on both Windows and Linux and automatically constructs the correct
command-line arguments based on the platform.

Usage:
    Linux:
        python3 run_aq_encoder.py --codec av1 --input video.yuv --width 1920 --height 1080 \
            --num-frames 32 --spatial-aq 0.5 --temporal-aq 0.5 --output encoded.ivf

    Windows:
        python run_aq_encoder.py --codec av1 --input video.yuv --width 1920 --height 1080 ^
            --num-frames 32 --spatial-aq 0.5 --temporal-aq 0.5 --output encoded.ivf
"""

import argparse
import sys
import os
import subprocess
import platform
from pathlib import Path


def detect_platform():
    """Detect the current platform."""
    return platform.system().lower()


def get_executable_path(vulkan_samples_root, build_type="Debug"):
    """
    Get the encoder executable path based on platform and build type.
    
    Args:
        vulkan_samples_root: Root directory of vulkan-video-samples
        build_type: Build type (Debug or Release)
    
    Returns:
        Path to the encoder executable
    """
    system = detect_platform()
    
    if system == "windows":
        if build_type == "Release":
            exe_path = Path(vulkan_samples_root) / "build-release" / "vk_video_encoder" / "test" / "Release" / "vulkan-video-enc-test.exe"
        else:
            exe_path = Path(vulkan_samples_root) / "build" / "vk_video_encoder" / "test" / "Debug" / "vulkan-video-enc-test.exe"
    else:  # Linux
        if build_type == "Release":
            exe_path = Path(vulkan_samples_root) / "build-release" / "vk_video_encoder" / "test" / "vulkan-video-enc-test"
        else:
            exe_path = Path(vulkan_samples_root) / "build" / "vk_video_encoder" / "test" / "vulkan-video-enc-test"
    
    return exe_path


def get_library_path(vulkan_samples_root, build_type="Debug"):
    """
    Get the library path for LD_LIBRARY_PATH (Linux) or PATH (Windows).
    
    Args:
        vulkan_samples_root: Root directory of vulkan-video-samples
        build_type: Build type (Debug or Release)
    
    Returns:
        Path to the library directory
    """
    system = detect_platform()
    
    if system == "windows":
        if build_type == "Release":
            lib_path = Path(vulkan_samples_root) / "build-release" / "lib"
        else:
            lib_path = Path(vulkan_samples_root) / "build" / "lib"
    else:  # Linux
        if build_type == "Release":
            lib_path = Path(vulkan_samples_root) / "build-release" / "lib"
        else:
            lib_path = Path(vulkan_samples_root) / "build" / "lib"
    
    return lib_path


def normalize_codec(codec):
    """Normalize codec name to encoder format."""
    codec_lower = codec.lower()
    codec_map = {
        "h264": "avc",
        "avc": "avc",
        "h265": "hevc",
        "hevc": "hevc",
        "av1": "av1"
    }
    return codec_map.get(codec_lower, codec_lower)


def build_command(args):
    """
    Build the encoder command based on arguments and platform.
    
    Args:
        args: Parsed command-line arguments
    
    Returns:
        List of command arguments
    """
    system = detect_platform()
    
    # Get executable path
    exe_path = get_executable_path(args.vulkan_samples_root, args.build_type)
    
    if not exe_path.exists():
        raise FileNotFoundError(f"Encoder executable not found at: {exe_path}")
    
    # Build command
    cmd = [str(exe_path)]
    
    # Input file
    cmd.extend(["-i", str(args.input)])
    
    # Output file
    cmd.extend(["-o", str(args.output)])
    
    # Codec
    codec = normalize_codec(args.codec)
    cmd.extend(["-c", codec])
    
    # Input dimensions
    cmd.extend(["--inputWidth", str(args.width)])
    cmd.extend(["--inputHeight", str(args.height)])
    
    # Encode dimensions (default to input dimensions if not specified)
    encode_width = args.encode_width if args.encode_width else args.width
    encode_height = args.encode_height if args.encode_height else args.height
    cmd.extend(["--encodeWidth", str(encode_width)])
    cmd.extend(["--encodeHeight", str(encode_height)])
    
    # Frame parameters
    if args.num_frames:
        cmd.extend(["--numFrames", str(args.num_frames)])
    if args.start_frame:
        cmd.extend(["--startFrame", str(args.start_frame)])
    
    # Rate control
    if args.rate_control_mode:
        cmd.extend(["--rateControlMode", args.rate_control_mode])
    if args.average_bitrate:
        cmd.extend(["--averageBitrate", str(args.average_bitrate)])
    
    # GOP structure
    if args.gop_frame_count:
        cmd.extend(["--gopFrameCount", str(args.gop_frame_count)])
    if args.idr_period:
        cmd.extend(["--idrPeriod", str(args.idr_period)])
    if args.consecutive_b_frame_count:
        cmd.extend(["--consecutiveBFrameCount", str(args.consecutive_b_frame_count)])
    
    # Chroma subsampling
    if args.chroma_subsampling:
        cmd.extend(["--inputChromaSubsampling", str(args.chroma_subsampling)])
    
    # AQ parameters
    if args.spatial_aq is not None:
        cmd.extend(["--spatialAQStrength", str(args.spatial_aq)])
    if args.temporal_aq is not None:
        cmd.extend(["--temporalAQStrength", str(args.temporal_aq)])
    
    # Quality and tuning
    if args.quality_level is not None:
        cmd.extend(["--qualityLevel", str(args.quality_level)])
    if args.usage_hints:
        cmd.extend(["--usageHints", args.usage_hints])
    if args.content_hints:
        cmd.extend(["--contentHints", args.content_hints])
    if args.tuning_mode:
        cmd.extend(["--tuningMode", args.tuning_mode])
    
    # AQ dump directory
    if args.aq_dump_dir:
        cmd.extend(["--aqDumpDir", str(args.aq_dump_dir)])
    
    return cmd


def setup_environment(args):
    """
    Setup environment variables for library loading.
    
    Args:
        args: Parsed command-line arguments
    
    Returns:
        Dictionary of environment variables
    """
    system = detect_platform()
    lib_path = get_library_path(args.vulkan_samples_root, args.build_type)
    
    env = os.environ.copy()
    
    if system == "windows":
        # Add to PATH on Windows
        current_path = env.get("PATH", "")
        env["PATH"] = f"{lib_path};{current_path}" if current_path else str(lib_path)
    else:
        # Add to LD_LIBRARY_PATH on Linux
        current_ld_path = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = f"{lib_path}:{current_ld_path}" if current_ld_path else str(lib_path)
    
    return env


def main():
    parser = argparse.ArgumentParser(
        description="Run Vulkan Video Encoder with Adaptive Quantization (AQ) support",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # AV1 encoding with combined AQ
  python3 run_aq_encoder.py --codec av1 --input video.yuv --width 1920 --height 1080 \\
      --num-frames 32 --spatial-aq 0.5 --temporal-aq 0.5 --output encoded.ivf \\
      --aq-dump-dir /tmp/aq_dump

  # H.264 encoding with spatial AQ only (temporal disabled with -2.0)
  python3 run_aq_encoder.py --codec h264 --input video.yuv --width 1920 --height 1080 \\
      --num-frames 32 --spatial-aq 0.0 --temporal-aq -2.0 --output encoded.264

  # HEVC encoding with temporal AQ only (spatial disabled with -2.0)
  python3 run_aq_encoder.py --codec hevc --input video.yuv --width 1920 --height 1080 \\
      --num-frames 32 --spatial-aq -2.0 --temporal-aq 0.0 --output encoded.265
        """
    )
    
    # Required arguments
    parser.add_argument(
        "--codec",
        required=True,
        choices=["avc", "h264", "hevc", "h265", "av1"],
        help="Codec type: avc/h264, hevc/h265, or av1"
    )
    parser.add_argument(
        "--input",
        required=True,
        type=Path,
        help="Input YUV file path"
    )
    parser.add_argument(
        "--width",
        required=True,
        type=int,
        help="Input video width in pixels"
    )
    parser.add_argument(
        "--height",
        required=True,
        type=int,
        help="Input video height in pixels"
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="Output encoded file path"
    )
    
    # AQ parameters
    parser.add_argument(
        "--spatial-aq",
        type=float,
        default=-2.0,
        help="Spatial AQ strength [-1.0, 1.0], 0.0=default, <-1.0=disabled (default: -2.0)"
    )
    parser.add_argument(
        "--temporal-aq",
        type=float,
        default=-2.0,
        help="Temporal AQ strength [-1.0, 1.0], 0.0=default, <-1.0=disabled (default: -2.0)"
    )
    parser.add_argument(
        "--aq-dump-dir",
        type=Path,
        help="Directory for AQ dump files (default: ./aqDump)"
    )
    
    # Frame parameters
    parser.add_argument(
        "--num-frames",
        type=int,
        help="Number of frames to encode"
    )
    parser.add_argument(
        "--start-frame",
        type=int,
        default=0,
        help="Start frame number (default: 0)"
    )
    
    # Encode dimensions
    parser.add_argument(
        "--encode-width",
        type=int,
        help="Encoded width (default: same as input width)"
    )
    parser.add_argument(
        "--encode-height",
        type=int,
        help="Encoded height (default: same as input height)"
    )
    
    # Rate control
    parser.add_argument(
        "--rate-control-mode",
        choices=["default", "disabled", "cbr", "vbr"],
        default="vbr",
        help="Rate control mode (default: vbr)"
    )
    parser.add_argument(
        "--average-bitrate",
        type=int,
        help="Average bitrate in bits/sec (5000000=5Mbps for 1080p, 15000000=15Mbps for 4K)"
    )
    
    # GOP structure
    parser.add_argument(
        "--gop-frame-count",
        type=int,
        default=16,
        help="Number of frames in GOP (default: 16)"
    )
    parser.add_argument(
        "--idr-period",
        type=int,
        default=4294967295,
        help="IDR period (default: 4294967295)"
    )
    parser.add_argument(
        "--consecutive-b-frame-count",
        type=int,
        default=3,
        help="Number of consecutive B frames (default: 3)"
    )
    
    # Video parameters
    parser.add_argument(
        "--chroma-subsampling",
        choices=["400", "420", "422", "444"],
        default="420",
        help="Chroma subsampling (default: 420)"
    )
    parser.add_argument(
        "--quality-level",
        type=int,
        default=4,
        help="Quality level (default: 4)"
    )
    parser.add_argument(
        "--usage-hints",
        choices=["default", "transcoding", "streaming", "recording", "conferencing"],
        default="transcoding",
        help="Usage hints (default: transcoding)"
    )
    parser.add_argument(
        "--content-hints",
        choices=["default", "camera", "desktop", "rendered"],
        default="default",
        help="Content hints (default: default)"
    )
    parser.add_argument(
        "--tuning-mode",
        choices=["default", "highquality", "lowlatency", "ultralowlatency", "lossless"],
        default="default",
        help="Tuning mode (default: default)"
    )
    
    # Build configuration
    parser.add_argument(
        "--vulkan-samples-root",
        type=Path,
        default=Path.cwd(),
        help="Root directory of vulkan-video-samples (default: current directory)"
    )
    parser.add_argument(
        "--build-type",
        choices=["Debug", "Release"],
        default="Debug",
        help="Build type: Debug or Release (default: Debug)"
    )
    
    # Execution options
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the command without executing it"
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print verbose output"
    )
    
    args = parser.parse_args()
    
    # Validate AQ parameters - valid range is [-1.0, 1.0], values < -1.0 are disabled
    if args.spatial_aq > 1.0:
        parser.error("--spatial-aq must be <= 1.0 (use < -1.0 to disable)")
    if args.temporal_aq > 1.0:
        parser.error("--temporal-aq must be <= 1.0 (use < -1.0 to disable)")
    
    # Validate input file exists
    if not args.input.exists():
        parser.error(f"Input file not found: {args.input}")
    
    # Create output directory if it doesn't exist
    args.output.parent.mkdir(parents=True, exist_ok=True)
    
    # Create AQ dump directory if specified
    if args.aq_dump_dir:
        args.aq_dump_dir.mkdir(parents=True, exist_ok=True)
    
    try:
        # Build command
        cmd = build_command(args)
        
        if args.verbose or args.dry_run:
            print("Platform:", detect_platform())
            print("Command:", " ".join(cmd))
            print()
        
        if args.dry_run:
            print("Dry run mode - command would be:")
            env = setup_environment(args)
            if detect_platform() == "windows":
                print(f"  PATH={env.get('PATH', '')}")
            else:
                print(f"  LD_LIBRARY_PATH={env.get('LD_LIBRARY_PATH', '')}")
            return 0
        
        # Setup environment
        env = setup_environment(args)
        
        # Run command
        if args.verbose:
            print("Running encoder...")
            print(f"Environment: {env.get('LD_LIBRARY_PATH' if detect_platform() != 'windows' else 'PATH', '')}")
        
        result = subprocess.run(cmd, env=env, check=False)
        
        if result.returncode == 0:
            print(f"Encoding completed successfully. Output: {args.output}")
            return 0
        else:
            print(f"Encoding failed with return code: {result.returncode}", file=sys.stderr)
            return result.returncode
            
    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        print("\nPlease ensure:")
        print(f"  1. Vulkan Video Samples is built in {args.build_type} mode")
        print(f"  2. The encoder executable exists at the expected location")
        print(f"  3. --vulkan-samples-root points to the correct directory")
        return 1
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

