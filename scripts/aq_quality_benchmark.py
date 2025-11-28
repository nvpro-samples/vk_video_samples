#!/usr/bin/env python3
"""
AQ Quality Benchmark Script
===========================

Comprehensive benchmarking tool for Vulkan Video Encoder Adaptive Quantization (AQ).

This script automatically runs the encoder with 4 different AQ configurations,
calculates quality metrics (PSNR, VMAF), and generates comparative reports.

CONFIGURATIONS TESTED:
----------------------
  1. No AQ (Baseline)     - Both spatial and temporal AQ disabled
  2. Spatial AQ Only      - Intra-frame complexity analysis enabled
  3. Temporal AQ Only     - Inter-frame motion analysis enabled
  4. Combined AQ          - Both spatial and temporal AQ enabled

HOW IT WORKS:
-------------
  1. Extracts reference frames from source YUV
  2. Encodes video 4 times with different AQ settings
  3. Decodes each bitstream to raw YUV
  4. Calculates PSNR (Y/U/V/average/min/max) using FFmpeg
  5. Calculates VMAF score using FFmpeg with libvmaf
  6. Generates human-readable (TXT) and machine-readable (JSON) reports

OUTPUT FILES:
-------------
  encoded_no_aq.<ext>         - Baseline encoded bitstream
  encoded_spatial_only.<ext>  - Spatial AQ encoded bitstream
  encoded_temporal_only.<ext> - Temporal AQ encoded bitstream
  encoded_combined.<ext>      - Combined AQ encoded bitstream
  benchmark_report.txt        - Human-readable summary report with command lines
  benchmark_results.json      - Machine-readable JSON with command lines
  commands_no_aq.txt          - Encode/decode/PSNR/VMAF commands for baseline
  commands_spatial_only.txt   - Encode/decode/PSNR/VMAF commands for spatial
  commands_temporal_only.txt  - Encode/decode/PSNR/VMAF commands for temporal
  commands_combined.txt       - Encode/decode/PSNR/VMAF commands for combined
  aq_dump_<config>/           - AQ library dumps (per configuration)

PREREQUISITES:
--------------
  - FFmpeg with libvmaf support (for quality metrics)
  - Vulkan Video Encoder built with AQ library
  - Sufficient disk space for encoded files and temporary YUV

USAGE EXAMPLES:
---------------
  # Basic H.264 benchmark
  python3 aq_quality_benchmark.py \\
      --input video.yuv --width 1920 --height 1080 \\
      --codec h264 --num-frames 64 \\
      --output-dir /tmp/benchmark

  # HEVC with specific bitrate
  python3 aq_quality_benchmark.py \\
      --input video.yuv --width 1920 --height 1080 \\
      --codec hevc --num-frames 64 \\
      --rate-control-mode vbr --average-bitrate 2000000 \\
      --output-dir /tmp/benchmark

  # Fast benchmark (skip VMAF)
  python3 aq_quality_benchmark.py \\
      --input video.yuv --width 1920 --height 1080 \\
      --codec h264 --num-frames 64 --skip-vmaf \\
      --output-dir /tmp/benchmark

INTERPRETING RESULTS:
---------------------
  Quality Thresholds:
    PSNR: Excellent >40dB, Good 35-40dB, Fair 30-35dB, Poor <30dB
    VMAF: Excellent >90, Good 80-90, Fair 70-80, Poor <70

  Expected AQ Impact:
    - Size reduction: 20-40% at constrained bitrates
    - PSNR improvement: +2-7 dB depending on content
    - VMAF improvement: +1-5 points depending on content

Author: NVIDIA Corporation
Date: January 2026
"""

import argparse
import subprocess
import sys
import os
import json
import re
import tempfile
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Optional, Tuple
import platform


def detect_platform():
    """Detect the current platform."""
    return platform.system().lower()


def get_executable_path(vulkan_samples_root: Path, build_type: str) -> Path:
    """Get the encoder executable path based on platform and build type."""
    system = detect_platform()
    
    if system == "windows":
        if build_type == "Release":
            return vulkan_samples_root / "build-release" / "vk_video_encoder" / "test" / "Release" / "vulkan-video-enc-test.exe"
        else:
            return vulkan_samples_root / "build" / "vk_video_encoder" / "test" / "Debug" / "vulkan-video-enc-test.exe"
    else:
        if build_type == "Release":
            return vulkan_samples_root / "build-release" / "vk_video_encoder" / "test" / "vulkan-video-enc-test"
        else:
            return vulkan_samples_root / "build" / "vk_video_encoder" / "test" / "vulkan-video-enc-test"


def get_library_path(vulkan_samples_root: Path, build_type: str) -> Path:
    """Get the library path for LD_LIBRARY_PATH."""
    if build_type == "Release":
        return vulkan_samples_root / "build-release" / "lib"
    else:
        return vulkan_samples_root / "build" / "lib"


def normalize_codec(codec: str) -> str:
    """Normalize codec name to encoder format."""
    codec_map = {
        "h264": "avc",
        "avc": "avc",
        "h265": "hevc",
        "hevc": "hevc",
        "av1": "av1"
    }
    return codec_map.get(codec.lower(), codec.lower())


def get_file_extension(codec: str) -> str:
    """Get the appropriate file extension for a codec."""
    ext_map = {
        "avc": ".264",
        "h264": ".264",
        "hevc": ".265",
        "h265": ".265",
        "av1": ".ivf"
    }
    return ext_map.get(codec.lower(), ".bin")


class AQConfiguration:
    """Represents an AQ configuration to test."""
    
    def __init__(self, name: str, spatial_aq: float, temporal_aq: float, description: str):
        self.name = name
        self.spatial_aq = spatial_aq
        self.temporal_aq = temporal_aq
        self.description = description
    
    def __repr__(self):
        return f"AQConfig({self.name}, spatial={self.spatial_aq}, temporal={self.temporal_aq})"


# Define the 4 AQ configurations to test
AQ_CONFIGURATIONS = [
    AQConfiguration("no_aq", -2.0, -2.0, "No AQ (Baseline)"),
    AQConfiguration("spatial_only", 0.0, -2.0, "Spatial AQ Only"),
    AQConfiguration("temporal_only", -2.0, 0.0, "Temporal AQ Only"),
    AQConfiguration("combined", 0.0, 0.0, "Combined (Spatial + Temporal)"),
]


class EncoderResult:
    """Stores results from an encoding run."""
    
    def __init__(self, config: AQConfiguration):
        self.config = config
        self.output_file: Optional[Path] = None
        self.file_size: int = 0
        self.encode_success: bool = False
        self.encode_time: float = 0.0
        self.psnr_y: float = 0.0
        self.psnr_u: float = 0.0
        self.psnr_v: float = 0.0
        self.psnr_avg: float = 0.0
        self.psnr_min: float = 0.0
        self.psnr_max: float = 0.0
        self.vmaf_score: float = 0.0
        self.error_message: str = ""
        self.encode_command: str = ""      # Command line used for encoding
        self.psnr_command: str = ""        # Command line used for PSNR calculation
        self.vmaf_command: str = ""        # Command line used for VMAF calculation
        self.decode_command: str = ""      # Command line used for decoding
        self.aq_dump_dir: Optional[Path] = None  # AQ dump directory for this config


def run_encoder(
    args,
    config: AQConfiguration,
    output_dir: Path,
    verbose: bool = False
) -> EncoderResult:
    """Run the encoder with a specific AQ configuration."""
    
    result = EncoderResult(config)
    
    # Determine output file path
    ext = get_file_extension(args.codec)
    output_file = output_dir / f"encoded_{config.name}{ext}"
    result.output_file = output_file
    
    # Get executable and library paths
    exe_path = get_executable_path(args.vulkan_samples_root, args.build_type)
    lib_path = get_library_path(args.vulkan_samples_root, args.build_type)
    
    if not exe_path.exists():
        result.error_message = f"Encoder not found: {exe_path}"
        return result
    
    # Build command
    cmd = [
        str(exe_path),
        "-i", str(args.input),
        "-o", str(output_file),
        "-c", normalize_codec(args.codec),
        "--inputWidth", str(args.width),
        "--inputHeight", str(args.height),
        "--encodeWidth", str(args.encode_width or args.width),
        "--encodeHeight", str(args.encode_height or args.height),
        "--spatialAQStrength", str(config.spatial_aq),
        "--temporalAQStrength", str(config.temporal_aq),
    ]
    
    # Add optional parameters
    if args.num_frames:
        cmd.extend(["--numFrames", str(args.num_frames)])
    if args.start_frame:
        cmd.extend(["--startFrame", str(args.start_frame)])
    if args.rate_control_mode:
        cmd.extend(["--rateControlMode", args.rate_control_mode])
    if args.average_bitrate:
        cmd.extend(["--averageBitrate", str(args.average_bitrate)])
    if args.gop_frame_count:
        cmd.extend(["--gopFrameCount", str(args.gop_frame_count)])
    if args.idr_period:
        cmd.extend(["--idrPeriod", str(args.idr_period)])
    if args.consecutive_b_frame_count is not None:
        cmd.extend(["--consecutiveBFrameCount", str(args.consecutive_b_frame_count)])
    if args.chroma_subsampling:
        cmd.extend(["--inputChromaSubsampling", str(args.chroma_subsampling)])
    if args.bit_depth:
        cmd.extend(["--inputBpp", str(args.bit_depth)])
    if args.quality_level is not None:
        cmd.extend(["--qualityLevel", str(args.quality_level)])
    if args.usage_hints:
        cmd.extend(["--usageHints", args.usage_hints])
    if args.content_hints:
        cmd.extend(["--contentHints", args.content_hints])
    if args.tuning_mode:
        cmd.extend(["--tuningMode", args.tuning_mode])
    
    # Set AQ dump directory - use explicit aq_dump_dir base if provided, else output_dir
    if hasattr(args, 'aq_dump_dir') and args.aq_dump_dir:
        aq_dump_dir = args.aq_dump_dir / f"aq_dump_{config.name}"
    else:
        aq_dump_dir = output_dir / f"aq_dump_{config.name}"
    aq_dump_dir.mkdir(parents=True, exist_ok=True)
    cmd.extend(["--aqDumpDir", str(aq_dump_dir)])
    result.aq_dump_dir = aq_dump_dir
    
    # Setup environment
    env = os.environ.copy()
    system = detect_platform()
    if system == "windows":
        env["PATH"] = f"{lib_path};{env.get('PATH', '')}"
    else:
        env["LD_LIBRARY_PATH"] = f"{lib_path}:{env.get('LD_LIBRARY_PATH', '')}"
    
    # Store the command line
    result.encode_command = ' '.join(cmd)
    
    if verbose:
        print(f"  Command: {result.encode_command}")
    
    # Run encoder
    import time
    start_time = time.time()
    
    try:
        proc = subprocess.run(
            cmd,
            env=env,
            capture_output=True,
            text=True,
            timeout=600  # 10 minute timeout
        )
        
        result.encode_time = time.time() - start_time
        
        if proc.returncode == 0 and output_file.exists():
            result.encode_success = True
            result.file_size = output_file.stat().st_size
        else:
            result.error_message = proc.stderr[:500] if proc.stderr else "Unknown error"
            
    except subprocess.TimeoutExpired:
        result.error_message = "Encoding timed out (>600s)"
    except Exception as e:
        result.error_message = str(e)
    
    return result


def calculate_psnr(
    decoded_yuv: Path,
    reference_yuv: Path,
    width: int,
    height: int,
    verbose: bool = False
) -> Tuple[Tuple[float, float, float, float, float, float], str]:
    """Calculate PSNR using ffmpeg. Returns ((y, u, v, avg, min, max), command_line)."""
    
    cmd = [
        "ffmpeg",
        "-s", f"{width}x{height}",
        "-pix_fmt", "yuv420p",
        "-i", str(decoded_yuv),
        "-s", f"{width}x{height}",
        "-pix_fmt", "yuv420p",
        "-i", str(reference_yuv),
        "-lavfi", "[0:v][1:v]psnr",
        "-f", "null", "-"
    ]
    
    cmd_str = ' '.join(cmd)
    
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        output = proc.stderr
        
        # Parse PSNR output: PSNR y:XX.XX u:XX.XX v:XX.XX average:XX.XX min:XX.XX max:XX.XX
        match = re.search(
            r'PSNR y:([0-9.]+) u:([0-9.]+) v:([0-9.]+) average:([0-9.]+) min:([0-9.]+) max:([0-9.]+)',
            output
        )
        
        if match:
            return (
                (
                    float(match.group(1)),  # y
                    float(match.group(2)),  # u
                    float(match.group(3)),  # v
                    float(match.group(4)),  # avg
                    float(match.group(5)),  # min
                    float(match.group(6)),  # max
                ),
                cmd_str
            )
    except Exception as e:
        if verbose:
            print(f"    PSNR calculation failed: {e}")
    
    return ((0.0, 0.0, 0.0, 0.0, 0.0, 0.0), cmd_str)


def calculate_vmaf(
    decoded_yuv: Path,
    reference_yuv: Path,
    width: int,
    height: int,
    verbose: bool = False
) -> Tuple[float, str]:
    """Calculate VMAF using ffmpeg. Returns (VMAF score, command_line)."""
    
    cmd = [
        "ffmpeg",
        "-s", f"{width}x{height}",
        "-pix_fmt", "yuv420p",
        "-i", str(decoded_yuv),
        "-s", f"{width}x{height}",
        "-pix_fmt", "yuv420p",
        "-i", str(reference_yuv),
        "-lavfi", "[0:v][1:v]libvmaf",
        "-f", "null", "-"
    ]
    
    cmd_str = ' '.join(cmd)
    
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        output = proc.stderr
        
        # Parse VMAF output: VMAF score: XX.XXXXXX
        match = re.search(r'VMAF score:\s*([0-9.]+)', output)
        
        if match:
            return (float(match.group(1)), cmd_str)
    except Exception as e:
        if verbose:
            print(f"    VMAF calculation failed: {e}")
    
    return (0.0, cmd_str)


def decode_to_yuv(
    encoded_file: Path,
    output_yuv: Path,
    verbose: bool = False
) -> Tuple[bool, str]:
    """Decode an encoded file to raw YUV using ffmpeg. Returns (success, command_line)."""
    
    cmd = [
        "ffmpeg", "-y",
        "-i", str(encoded_file),
        "-f", "rawvideo",
        "-pix_fmt", "yuv420p",
        str(output_yuv)
    ]
    
    cmd_str = ' '.join(cmd)
    
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        return (proc.returncode == 0 and output_yuv.exists(), cmd_str)
    except Exception as e:
        if verbose:
            print(f"    Decode failed: {e}")
        return (False, cmd_str)


def extract_reference_frames(
    input_yuv: Path,
    output_yuv: Path,
    width: int,
    height: int,
    num_frames: int
) -> bool:
    """Extract reference frames from source YUV."""
    
    frame_size = width * height * 3 // 2  # YUV 4:2:0
    total_bytes = frame_size * num_frames
    
    try:
        with open(input_yuv, 'rb') as f_in:
            data = f_in.read(total_bytes)
        
        with open(output_yuv, 'wb') as f_out:
            f_out.write(data)
        
        return output_yuv.exists() and output_yuv.stat().st_size == total_bytes
    except Exception:
        return False


def calculate_quality_metrics(
    result: EncoderResult,
    reference_yuv: Path,
    width: int,
    height: int,
    work_dir: Path,
    skip_psnr: bool = False,
    skip_vmaf: bool = False,
    verbose: bool = False
) -> None:
    """Calculate PSNR and VMAF for an encoding result."""
    
    if not result.encode_success or not result.output_file.exists():
        return
    
    # Decode to YUV
    decoded_yuv = work_dir / f"decoded_{result.config.name}.yuv"
    
    if verbose:
        print(f"    Decoding {result.output_file.name}...")
    
    decode_success, decode_cmd = decode_to_yuv(result.output_file, decoded_yuv, verbose)
    result.decode_command = decode_cmd
    
    if not decode_success:
        result.error_message = "Failed to decode for quality analysis"
        return
    
    # Calculate PSNR
    if not skip_psnr:
        if verbose:
            print(f"    Calculating PSNR...")
        
        psnr, psnr_cmd = calculate_psnr(decoded_yuv, reference_yuv, width, height, verbose)
        result.psnr_command = psnr_cmd
        result.psnr_y = psnr[0]
        result.psnr_u = psnr[1]
        result.psnr_v = psnr[2]
        result.psnr_avg = psnr[3]
        result.psnr_min = psnr[4]
        result.psnr_max = psnr[5]
    
    # Calculate VMAF
    if not skip_vmaf:
        if verbose:
            print(f"    Calculating VMAF...")
        
        vmaf_score, vmaf_cmd = calculate_vmaf(decoded_yuv, reference_yuv, width, height, verbose)
        result.vmaf_command = vmaf_cmd
        result.vmaf_score = vmaf_score
    
    # Cleanup decoded file
    if decoded_yuv.exists():
        decoded_yuv.unlink()


def generate_report(
    results: List[EncoderResult],
    args,
    output_dir: Path
) -> str:
    """Generate a comprehensive quality report."""
    
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    
    # Find baseline for comparison
    baseline = next((r for r in results if r.config.name == "no_aq"), None)
    baseline_size = baseline.file_size if baseline else 0
    baseline_psnr = baseline.psnr_avg if baseline else 0
    baseline_vmaf = baseline.vmaf_score if baseline else 0
    
    report = []
    report.append("=" * 80)
    report.append("           AQ QUALITY BENCHMARK REPORT")
    report.append("=" * 80)
    report.append("")
    report.append(f"Generated: {timestamp}")
    report.append("")
    
    # Test configuration
    report.append("## Test Configuration")
    report.append("")
    report.append(f"  Input File:     {args.input}")
    report.append(f"  Resolution:     {args.width}x{args.height}")
    report.append(f"  Codec:          {args.codec.upper()}")
    report.append(f"  Frames:         {args.num_frames or 'all'}")
    report.append(f"  Rate Control:   {args.rate_control_mode or 'default'}")
    report.append(f"  Bitrate:        {args.average_bitrate or 'default'} kbps")
    report.append(f"  GOP Size:       {args.gop_frame_count or 'default'}")
    report.append(f"  B-Frames:       {args.consecutive_b_frame_count if args.consecutive_b_frame_count is not None else 'default'}")
    report.append("")
    
    # Results summary table
    report.append("## Results Summary")
    report.append("")
    report.append("```")
    report.append("+----------------------+------------+----------+----------+----------+----------+")
    report.append("| Configuration        | Size (KB)  | vs Base  | PSNR     | VMAF     | vs Base  |")
    report.append("+----------------------+------------+----------+----------+----------+----------+")
    
    for r in results:
        size_kb = r.file_size / 1024
        
        if baseline_size > 0:
            size_diff = ((r.file_size - baseline_size) / baseline_size) * 100
            size_diff_str = f"{size_diff:+.1f}%"
        else:
            size_diff_str = "N/A"
        
        if baseline_vmaf > 0:
            vmaf_diff = r.vmaf_score - baseline_vmaf
            vmaf_diff_str = f"{vmaf_diff:+.2f}"
        else:
            vmaf_diff_str = "N/A"
        
        if r.encode_success:
            report.append(
                f"| {r.config.description:<20} | {size_kb:>10.1f} | {size_diff_str:>8} | "
                f"{r.psnr_avg:>8.2f} | {r.vmaf_score:>8.2f} | {vmaf_diff_str:>8} |"
            )
        else:
            report.append(
                f"| {r.config.description:<20} | {'FAILED':>10} | {'N/A':>8} | "
                f"{'N/A':>8} | {'N/A':>8} | {'N/A':>8} |"
            )
    
    report.append("+----------------------+------------+----------+----------+----------+----------+")
    report.append("```")
    report.append("")
    
    # Detailed PSNR breakdown
    report.append("## Detailed PSNR Breakdown")
    report.append("")
    report.append("```")
    report.append("+----------------------+----------+----------+----------+----------+----------+----------+")
    report.append("| Configuration        | PSNR Y   | PSNR U   | PSNR V   | Average  | Min      | Max      |")
    report.append("+----------------------+----------+----------+----------+----------+----------+----------+")
    
    for r in results:
        if r.encode_success:
            report.append(
                f"| {r.config.description:<20} | {r.psnr_y:>8.2f} | {r.psnr_u:>8.2f} | "
                f"{r.psnr_v:>8.2f} | {r.psnr_avg:>8.2f} | {r.psnr_min:>8.2f} | {r.psnr_max:>8.2f} |"
            )
        else:
            report.append(
                f"| {r.config.description:<20} | {'FAILED':>8} | {'N/A':>8} | "
                f"{'N/A':>8} | {'N/A':>8} | {'N/A':>8} | {'N/A':>8} |"
            )
    
    report.append("+----------------------+----------+----------+----------+----------+----------+----------+")
    report.append("```")
    report.append("")
    
    # File information
    report.append("## Output Files")
    report.append("")
    for r in results:
        if r.encode_success and r.output_file:
            report.append(f"  {r.config.description}:")
            report.append(f"    File: {r.output_file}")
            report.append(f"    Size: {r.file_size:,} bytes ({r.file_size/1024:.1f} KB)")
            report.append(f"    Encode time: {r.encode_time:.2f}s")
        else:
            report.append(f"  {r.config.description}: FAILED - {r.error_message}")
        report.append("")
    
    # Analysis
    report.append("## Analysis")
    report.append("")
    
    # Find best configuration
    successful = [r for r in results if r.encode_success]
    if successful:
        best_vmaf = max(successful, key=lambda r: r.vmaf_score)
        best_psnr = max(successful, key=lambda r: r.psnr_avg)
        smallest = min(successful, key=lambda r: r.file_size)
        
        report.append(f"  Best VMAF:      {best_vmaf.config.description} ({best_vmaf.vmaf_score:.2f})")
        report.append(f"  Best PSNR:      {best_psnr.config.description} ({best_psnr.psnr_avg:.2f} dB)")
        report.append(f"  Smallest Size:  {smallest.config.description} ({smallest.file_size/1024:.1f} KB)")
        report.append("")
        
        # AQ improvements
        if baseline and baseline.encode_success:
            report.append("  AQ Improvements vs Baseline:")
            for r in results:
                if r.config.name != "no_aq" and r.encode_success:
                    size_diff = ((r.file_size - baseline.file_size) / baseline.file_size) * 100
                    psnr_diff = r.psnr_avg - baseline.psnr_avg
                    vmaf_diff = r.vmaf_score - baseline.vmaf_score
                    report.append(
                        f"    {r.config.description}: "
                        f"Size {size_diff:+.1f}%, PSNR {psnr_diff:+.2f} dB, VMAF {vmaf_diff:+.2f}"
                    )
    
    report.append("")
    
    # Command lines section
    report.append("## Command Lines Used")
    report.append("")
    report.append("All command lines are also saved to individual `commands_<config>.txt` files.")
    report.append("")
    
    for r in results:
        report.append(f"### {r.config.description}")
        report.append("")
        if r.encode_command:
            report.append("**Encode:**")
            report.append("```")
            report.append(r.encode_command)
            report.append("```")
            report.append("")
        if r.decode_command:
            report.append("**Decode:**")
            report.append("```")
            report.append(r.decode_command)
            report.append("```")
            report.append("")
        if r.psnr_command:
            report.append("**PSNR:**")
            report.append("```")
            report.append(r.psnr_command)
            report.append("```")
            report.append("")
        if r.vmaf_command:
            report.append("**VMAF:**")
            report.append("```")
            report.append(r.vmaf_command)
            report.append("```")
            report.append("")
        if r.aq_dump_dir:
            report.append(f"**AQ Dump Dir:** `{r.aq_dump_dir}`")
            report.append("")
    
    report.append("=" * 80)
    report.append("                         END OF REPORT")
    report.append("=" * 80)
    
    return "\n".join(report)


def save_command_lines(
    results: List[EncoderResult],
    output_dir: Path
) -> None:
    """Save command lines to individual files for each configuration."""
    
    for r in results:
        # Create command line file for this configuration
        cmd_file = output_dir / f"commands_{r.config.name}.txt"
        
        with open(cmd_file, 'w') as f:
            f.write(f"# Command lines for {r.config.description}\n")
            f.write(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write("#" + "=" * 78 + "\n\n")
            
            f.write("## ENCODE COMMAND\n")
            f.write(f"# Encodes input to {r.config.description} configuration\n")
            f.write(f"{r.encode_command}\n\n")
            
            if r.decode_command:
                f.write("## DECODE COMMAND\n")
                f.write("# Decodes encoded bitstream to raw YUV for quality analysis\n")
                f.write(f"{r.decode_command}\n\n")
            
            if r.psnr_command:
                f.write("## PSNR CALCULATION COMMAND\n")
                f.write("# Calculates PSNR between decoded and reference YUV\n")
                f.write(f"{r.psnr_command}\n\n")
            
            if r.vmaf_command:
                f.write("## VMAF CALCULATION COMMAND\n")
                f.write("# Calculates VMAF score between decoded and reference YUV\n")
                f.write(f"{r.vmaf_command}\n\n")
            
            if r.aq_dump_dir:
                f.write(f"## AQ DUMP DIRECTORY\n")
                f.write(f"# AQ library output location\n")
                f.write(f"{r.aq_dump_dir}\n")


def save_json_results(
    results: List[EncoderResult],
    args,
    output_file: Path
) -> None:
    """Save results as JSON for programmatic analysis."""
    
    data = {
        "timestamp": datetime.now().isoformat(),
        "configuration": {
            "input": str(args.input),
            "width": args.width,
            "height": args.height,
            "codec": args.codec,
            "num_frames": args.num_frames,
            "bitrate": args.average_bitrate,
            "gop_size": args.gop_frame_count,
            "b_frames": args.consecutive_b_frame_count,
        },
        "results": []
    }
    
    for r in results:
        data["results"].append({
            "config_name": r.config.name,
            "description": r.config.description,
            "spatial_aq": r.config.spatial_aq,
            "temporal_aq": r.config.temporal_aq,
            "success": r.encode_success,
            "file_size": r.file_size,
            "encode_time": r.encode_time,
            "psnr": {
                "y": r.psnr_y,
                "u": r.psnr_u,
                "v": r.psnr_v,
                "average": r.psnr_avg,
                "min": r.psnr_min,
                "max": r.psnr_max,
            },
            "vmaf": r.vmaf_score,
            "error": r.error_message if not r.encode_success else None,
            "output_file": str(r.output_file) if r.output_file else None,
            "aq_dump_dir": str(r.aq_dump_dir) if r.aq_dump_dir else None,
            "commands": {
                "encode": r.encode_command,
                "decode": r.decode_command,
                "psnr": r.psnr_command,
                "vmaf": r.vmaf_command,
            },
        })
    
    with open(output_file, 'w') as f:
        json.dump(data, f, indent=2)


def main():
    parser = argparse.ArgumentParser(
        description="""
AQ Quality Benchmark - Comprehensive Adaptive Quantization Testing Tool

This script runs the Vulkan Video Encoder with 4 different AQ configurations
and generates quality comparison reports with file sizes, PSNR, and VMAF metrics.

CONFIGURATIONS TESTED:
  1. No AQ (Baseline)  - spatial=-2.0, temporal=-2.0 (both disabled)
  2. Spatial AQ Only   - spatial=0.0,  temporal=-2.0 (intra-frame analysis)
  3. Temporal AQ Only  - spatial=-2.0, temporal=0.0  (inter-frame analysis)
  4. Combined AQ       - spatial=0.0,  temporal=0.0  (both enabled)

OUTPUT FILES GENERATED:
  encoded_no_aq.<ext>         Baseline encoded bitstream
  encoded_spatial_only.<ext>  Spatial AQ encoded bitstream
  encoded_temporal_only.<ext> Temporal AQ encoded bitstream
  encoded_combined.<ext>      Combined AQ encoded bitstream
  benchmark_report.txt        Human-readable report with all command lines
  benchmark_results.json      Machine-readable JSON with command lines
  commands_<config>.txt       Encode/decode/PSNR/VMAF commands per config
  aq_dump_<config>/           AQ library dumps (QP maps) per configuration

PREREQUISITES:
  - FFmpeg with libvmaf support (verify: ffmpeg -filters | grep vmaf)
  - Vulkan Video Encoder built with AQ library
        """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
EXAMPLES:
---------

  Basic H.264 benchmark (1080p, 64 frames):
    python3 aq_quality_benchmark.py \\
        --input video.yuv --width 1920 --height 1080 \\
        --codec h264 --num-frames 64 \\
        --output-dir /tmp/benchmark \\
        --vulkan-samples-root /path/to/vulkan-video-samples

  HEVC with specific bitrate (2 Mbps):
    python3 aq_quality_benchmark.py \\
        --input video.yuv --width 1920 --height 1080 \\
        --codec hevc --num-frames 64 \\
        --rate-control-mode vbr --average-bitrate 2000000 \\
        --output-dir /tmp/benchmark \\
        --vulkan-samples-root /path/to/vulkan-video-samples

  4K AV1 benchmark with custom AQ strength (15 Mbps for 4K):
    python3 aq_quality_benchmark.py \\
        --input video.yuv --width 3840 --height 2160 \\
        --codec av1 --num-frames 60 \\
        --average-bitrate 15000000 \\
        --spatial-aq-strength 0.5 --temporal-aq-strength 0.3 \\
        --output-dir /tmp/benchmark \\
        --vulkan-samples-root /path/to/vulkan-video-samples

  Fast benchmark (skip slow VMAF calculation):
    python3 aq_quality_benchmark.py \\
        --input video.yuv --width 1920 --height 1080 \\
        --codec h264 --num-frames 64 --skip-vmaf \\
        --output-dir /tmp/benchmark \\
        --vulkan-samples-root /path/to/vulkan-video-samples

  10-bit HEVC benchmark:
    python3 aq_quality_benchmark.py \\
        --input video_10bit.yuv --width 1920 --height 1080 \\
        --codec hevc --bit-depth 10 --num-frames 64 \\
        --output-dir /tmp/benchmark \\
        --vulkan-samples-root /path/to/vulkan-video-samples

INTERPRETING RESULTS:
---------------------

  Quality Thresholds:
    PSNR: Excellent >40dB | Good 35-40dB | Fair 30-35dB | Poor <30dB
    VMAF: Excellent >90   | Good 80-90   | Fair 70-80   | Poor <70

  Expected AQ Benefits:
    - Size reduction: 20-40% at constrained bitrates
    - PSNR improvement: +2-7 dB depending on content
    - VMAF improvement: +1-5 points depending on content

  Content Sensitivity:
    - High motion: Temporal AQ provides larger benefits
    - Complex textures: Spatial AQ provides larger benefits
    - Mixed content: Combined mode usually performs best
    - High bitrates (>5 Mbps): AQ effects are less pronounced

For more information, see README-AQ.md in the vulkan-video-samples repository.
        """
    )
    
    # Required arguments
    required = parser.add_argument_group('Required Arguments')
    required.add_argument("--input", required=True, type=Path, 
                          help="Path to input YUV file (raw planar format)")
    required.add_argument("--width", required=True, type=int, 
                          help="Input video width in pixels (e.g., 1920)")
    required.add_argument("--height", required=True, type=int, 
                          help="Input video height in pixels (e.g., 1080)")
    required.add_argument("--codec", required=True, choices=["avc", "h264", "hevc", "h265", "av1"],
                          help="Video codec: avc/h264, hevc/h265, or av1")
    required.add_argument("--output-dir", required=True, type=Path, 
                          help="Output directory for encoded files and reports")
    
    # Video parameters
    video = parser.add_argument_group('Video Parameters')
    video.add_argument("--num-frames", type=int, 
                       help="Number of frames to encode (default: all frames in input)")
    video.add_argument("--start-frame", type=int, default=0, 
                       help="Starting frame number in input file (default: 0)")
    video.add_argument("--encode-width", type=int, 
                       help="Encoded video width, for scaling (default: same as input)")
    video.add_argument("--encode-height", type=int, 
                       help="Encoded video height, for scaling (default: same as input)")
    video.add_argument("--chroma-subsampling", choices=["400", "420", "422", "444"], default="420",
                       help="Chroma subsampling format (default: 420)")
    video.add_argument("--bit-depth", type=int, choices=[8, 10], default=8, 
                       help="Video bit depth: 8 or 10 bits per sample (default: 8)")
    
    # Rate control
    rate_ctrl = parser.add_argument_group('Rate Control')
    rate_ctrl.add_argument("--rate-control-mode", choices=["default", "disabled", "cbr", "vbr"],
                           default="vbr", 
                           help="Rate control mode: cbr (constant), vbr (variable) (default: vbr)")
    rate_ctrl.add_argument("--average-bitrate", type=int, 
                           help="Target average bitrate in bits/sec. "
                                "Recommended: 2000000-8000000 for 1080p, 10000000-25000000 for 4K")
    
    # GOP structure
    gop = parser.add_argument_group('GOP Structure')
    gop.add_argument("--gop-frame-count", type=int, default=16, 
                     help="Number of frames in Group of Pictures (default: 16)")
    gop.add_argument("--idr-period", type=int, default=4294967295, 
                     help="IDR period - frames between IDR frames (default: max)")
    gop.add_argument("--consecutive-b-frame-count", type=int, default=3, 
                     help="Number of consecutive B-frames between references (default: 3)")
    
    # Quality parameters
    quality = parser.add_argument_group('Quality Settings')
    quality.add_argument("--quality-level", type=int, default=4, 
                         help="Encoder quality level 0-7, higher=better (default: 4)")
    quality.add_argument("--usage-hints", choices=["default", "transcoding", "streaming", "recording"],
                         default="transcoding", 
                         help="Usage hint for encoder optimization (default: transcoding)")
    quality.add_argument("--content-hints", choices=["default", "camera", "desktop", "rendered"],
                         default="default", 
                         help="Content type hint (default: default)")
    quality.add_argument("--tuning-mode", choices=["default", "highquality", "lowlatency", "lossless"],
                         default="default", 
                         help="Encoder tuning mode (default: default)")
    
    # Build configuration
    build = parser.add_argument_group('Build Configuration')
    build.add_argument("--vulkan-samples-root", type=Path, default=Path.cwd(),
                       help="Path to vulkan-video-samples root directory")
    build.add_argument("--build-type", choices=["Debug", "Release"], default="Release",
                       help="Build type to use: Debug or Release (default: Release)")
    
    # Benchmark options
    bench = parser.add_argument_group('Benchmark Options')
    bench.add_argument("--spatial-aq-strength", type=float, default=0.0,
                       help="Spatial AQ strength [-1.0 to 1.0] for spatial/combined modes. "
                            "0.0=default strength, positive=stronger, negative=weaker (default: 0.0)")
    bench.add_argument("--temporal-aq-strength", type=float, default=0.0,
                       help="Temporal AQ strength [-1.0 to 1.0] for temporal/combined modes. "
                            "0.0=default strength, positive=stronger, negative=weaker (default: 0.0)")
    bench.add_argument("--aq-dump-dir", type=Path,
                       help="Base directory for AQ library dumps. If not specified, uses "
                            "<output-dir>/aq_dump_<config>/ for each configuration")
    bench.add_argument("--skip-vmaf", action="store_true", 
                       help="Skip VMAF calculation for faster benchmarks (PSNR only)")
    bench.add_argument("--skip-psnr", action="store_true", 
                       help="Skip PSNR calculation (not recommended)")
    bench.add_argument("--verbose", action="store_true", 
                       help="Enable verbose output with detailed progress")
    
    args = parser.parse_args()
    
    # Validate input
    if not args.input.exists():
        print(f"Error: Input file not found: {args.input}", file=sys.stderr)
        return 1
    
    # Create output directory
    args.output_dir.mkdir(parents=True, exist_ok=True)
    
    # Update AQ configurations with custom strengths
    aq_configs = [
        AQConfiguration("no_aq", -2.0, -2.0, "No AQ (Baseline)"),
        AQConfiguration("spatial_only", args.spatial_aq_strength, -2.0, "Spatial AQ Only"),
        AQConfiguration("temporal_only", -2.0, args.temporal_aq_strength, "Temporal AQ Only"),
        AQConfiguration("combined", args.spatial_aq_strength, args.temporal_aq_strength, "Combined (Spatial + Temporal)"),
    ]
    
    print("=" * 60)
    print("        AQ QUALITY BENCHMARK")
    print("=" * 60)
    print()
    print(f"Input:       {args.input}")
    print(f"Resolution:  {args.width}x{args.height}")
    print(f"Codec:       {args.codec.upper()}")
    print(f"Frames:      {args.num_frames or 'all'}")
    print(f"Bitrate:     {args.average_bitrate or 'default'} kbps")
    print(f"Output:      {args.output_dir}")
    print()
    
    # Prepare reference YUV
    work_dir = args.output_dir / "work"
    work_dir.mkdir(exist_ok=True)
    
    num_frames = args.num_frames or 64  # Default to 64 frames
    reference_yuv = work_dir / "reference.yuv"
    
    print("Preparing reference frames...")
    if not extract_reference_frames(args.input, reference_yuv, args.width, args.height, num_frames):
        print("Error: Failed to extract reference frames", file=sys.stderr)
        return 1
    print(f"  Extracted {num_frames} frames to {reference_yuv}")
    print()
    
    # Run encodings
    results = []
    
    for i, config in enumerate(aq_configs, 1):
        print(f"[{i}/4] Encoding: {config.description}")
        print(f"       Spatial AQ: {config.spatial_aq}, Temporal AQ: {config.temporal_aq}")
        
        result = run_encoder(args, config, args.output_dir, args.verbose)
        
        if result.encode_success:
            print(f"       ✓ Success: {result.file_size:,} bytes in {result.encode_time:.1f}s")
            
            # Calculate quality metrics
            if not args.skip_psnr or not args.skip_vmaf:
                print("       Calculating quality metrics...")
                calculate_quality_metrics(
                    result, reference_yuv, args.width, args.height, work_dir,
                    skip_psnr=args.skip_psnr, skip_vmaf=args.skip_vmaf, verbose=args.verbose
                )
                
                if result.psnr_avg > 0:
                    print(f"       PSNR: {result.psnr_avg:.2f} dB")
                if result.vmaf_score > 0:
                    print(f"       VMAF: {result.vmaf_score:.2f}")
        else:
            print(f"       ✗ Failed: {result.error_message}")
        
        results.append(result)
        print()
    
    # Generate report
    print("Generating report...")
    report = generate_report(results, args, args.output_dir)
    
    # Save report
    report_file = args.output_dir / "benchmark_report.txt"
    with open(report_file, 'w') as f:
        f.write(report)
    print(f"  Saved: {report_file}")
    
    # Save JSON results
    json_file = args.output_dir / "benchmark_results.json"
    save_json_results(results, args, json_file)
    print(f"  Saved: {json_file}")
    
    # Save command lines to individual files
    save_command_lines(results, args.output_dir)
    print(f"  Saved: commands_*.txt (one per configuration)")
    
    # Print report to console
    print()
    print(report)
    
    # Cleanup work directory
    if reference_yuv.exists():
        reference_yuv.unlink()
    if work_dir.exists():
        try:
            work_dir.rmdir()
        except:
            pass
    
    return 0


if __name__ == "__main__":
    sys.exit(main())

