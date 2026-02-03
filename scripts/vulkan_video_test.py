#!/usr/bin/env python3
"""
Vulkan Video Samples - Comprehensive Test Suite

This script provides comprehensive testing for the Vulkan Video decoder and encoder,
including validation layer support and adaptive quantization (AQ) testing.

Supported codecs:
    - H.264/AVC (decode and encode)
    - H.265/HEVC (decode and encode)
    - AV1 (decode and encode)
    - VP9 (decode only)

Usage:
    python3 vulkan_video_test.py --test decoder
    python3 vulkan_video_test.py --test encoder
    python3 vulkan_video_test.py --test all
    python3 vulkan_video_test.py --test decoder --validate
    python3 vulkan_video_test.py --test encoder --aq

Author: Vulkan Video Samples Project
Date: 2026-02-03
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import Dict, List, Optional, Tuple


# ============================================================================
# Configuration
# ============================================================================

class Codec(Enum):
    H264 = "h264"
    H265 = "h265"
    AV1 = "av1"
    VP9 = "vp9"


class TestResult(Enum):
    PASSED = "PASSED"
    FAILED = "FAILED"
    SKIPPED = "SKIPPED"
    ERROR = "ERROR"


@dataclass
class TestConfig:
    """Configuration for the test suite."""
    project_root: Path = Path("/data/nvidia/android-extra/video-apps/vulkan-video-samples")
    build_dir: Path = Path("/data/nvidia/android-extra/video-apps/vulkan-video-samples/build")
    video_clips_dir: Optional[Path] = None  # Must be specified
    output_dir: Path = Path("/tmp/vulkan_video_tests")
    
    # Remote host configuration (default: loopback/localhost)
    remote_host: str = "127.0.0.1"
    remote_user: str = ""  # Will default to current user
    run_local: bool = False
    
    # Executables
    decoder_exe: Path = field(default_factory=lambda: Path("vk_video_decoder/demos/vk-video-dec-test"))
    encoder_exe: Path = field(default_factory=lambda: Path("vk_video_encoder/demos/vk-video-enc-test"))
    decoder_test_exe: Path = field(default_factory=lambda: Path("vk_video_decoder/test/vulkan-video-dec-test"))
    encoder_test_exe: Path = field(default_factory=lambda: Path("vk_video_encoder/test/vulkan-video-enc-test"))
    
    # Test options
    enable_validation: bool = False
    enable_aq: bool = False
    verbose: bool = False
    max_frames: int = 30
    timeout: int = 300  # seconds
    
    def get_ssh_target(self) -> str:
        """Get SSH target string (user@host)."""
        user = self.remote_user or os.environ.get("USER", "root")
        return f"{user}@{self.remote_host}"
    
    def get_decoder_path(self, use_test: bool = True) -> Path:
        """Get the full path to the decoder executable."""
        exe = self.decoder_test_exe if use_test else self.decoder_exe
        return self.build_dir / exe
    
    def get_encoder_path(self, use_test: bool = True) -> Path:
        """Get the full path to the encoder executable."""
        exe = self.encoder_test_exe if use_test else self.encoder_exe
        return self.build_dir / exe


@dataclass
class TestCase:
    """Represents a single test case."""
    name: str
    codec: Codec
    input_file: Path
    description: str
    width: int = 0
    height: int = 0
    bitdepth: int = 8
    chroma: str = "420"
    num_frames: int = 0
    expected_success: bool = True
    extra_args: List[str] = field(default_factory=list)
    
    
@dataclass
class TestResult_:
    """Result of a single test case."""
    test_case: TestCase
    status: TestResult
    duration: float
    output: str
    error: str = ""
    validation_errors: int = 0
    frames_processed: int = 0


# ============================================================================
# Test Case Definitions
# ============================================================================

def get_decoder_test_cases(config: TestConfig) -> List[TestCase]:
    """Get all decoder test cases."""
    test_cases = []
    video_dir = config.video_clips_dir
    cts_dir = video_dir / "cts"
    cts_video_dir = cts_dir / "video"
    
    # H.264/AVC test cases
    h264_files = [
        # CTS content
        ("clip-a.h264", "H.264 CTS clip-a", cts_dir),
        ("clip-b.h264", "H.264 CTS clip-b", cts_dir),
        ("clip-c.h264", "H.264 CTS clip-c", cts_dir),
        ("avc-720x480-field.h264", "H.264 720x480 field coding", cts_dir),
        ("avc-1440x1080-paff.h264", "H.264 1440x1080 PAFF", cts_dir),
        ("4k_26_ibp_main.h264", "H.264 4K IBP main profile", cts_dir),
        # Main content
        ("akiyo_176x144_30p_1_0.264", "H.264 Akiyo 176x144", video_dir),
        ("jellyfish-250-mbps-4k-uhd-h264.h264", "H.264 Jellyfish 4K UHD", video_dir),
        ("MR4_TANDBERG_C.264", "H.264 Tandberg test", video_dir),
        # Container formats
        ("jellyfish-100-mbps-hd-h264.mkv", "H.264 Jellyfish HD (MKV)", video_dir),
        ("golden_flower_h264_720_30p_7M.mp4", "H.264 Golden Flower (MP4)", video_dir),
        ("fantastic4_h264_bp_1080_30_20M.mp4", "H.264 Fantastic4 1080p (MP4)", video_dir),
    ]
    
    for filename, desc, base_dir in h264_files:
        filepath = base_dir / filename
        if filepath.exists():
            test_cases.append(TestCase(
                name=f"DEC_H264_{Path(filename).stem}",
                codec=Codec.H264,
                input_file=filepath,
                description=desc
            ))
    
    # H.265/HEVC test cases
    h265_files = [
        # CTS content
        ("clip-d.h265", "H.265 CTS clip-d", cts_dir),
        ("hevc-itu-slist-a.h265", "H.265 ITU scaling list A", cts_video_dir),
        ("hevc-itu-slist-b.h265", "H.265 ITU scaling list B", cts_video_dir),
        ("jellyfish-250-mbps-4k-uhd-GOB-IPB13.h265", "H.265 Jellyfish 4K GOB", cts_video_dir),
        # Main content
        ("jellyfish-110-mbps-hd-hevc.h265", "H.265 Jellyfish HD", video_dir),
        ("jellyfish-400-mbps-4k-uhd-hevc-10bit.h265", "H.265 Jellyfish 4K 10-bit", video_dir),
        ("4K_1440p.hevc", "H.265 4K 1440p", video_dir),
        # Container formats
        ("jellyfish-100-mbps-hd-hevc.mkv", "H.265 Jellyfish HD (MKV)", video_dir),
        ("jellyfish-110-mbps-hd-hevc.mkv", "H.265 Jellyfish HD 110Mbps (MKV)", video_dir),
        ("hevc_bt2020_10bit.mp4", "H.265 BT.2020 10-bit (MP4)", video_dir),
        ("video-h265.mkv", "H.265 Video (MKV)", video_dir),
    ]
    
    for filename, desc, base_dir in h265_files:
        filepath = base_dir / filename
        if filepath.exists():
            test_cases.append(TestCase(
                name=f"DEC_HEVC_{Path(filename).stem}",
                codec=Codec.H265,
                input_file=filepath,
                description=desc
            ))
    
    # AV1 test cases
    av1_files = [
        # CTS 8-bit content
        ("basic-8.ivf", "AV1 Basic 8-bit", cts_dir),
        ("cdef-8.ivf", "AV1 CDEF 8-bit", cts_dir),
        ("forward-key-frame-8.ivf", "AV1 Forward key frame 8-bit", cts_dir),
        ("global-motion-8.ivf", "AV1 Global motion 8-bit", cts_dir),
        ("loop-filter-8.ivf", "AV1 Loop filter 8-bit", cts_dir),
        ("lossless-8.ivf", "AV1 Lossless 8-bit", cts_dir),
        ("order-hint-8.ivf", "AV1 Order hint 8-bit", cts_dir),
        # CTS 10-bit content
        ("basic-10.ivf", "AV1 Basic 10-bit", cts_dir),
        ("cdef-10.ivf", "AV1 CDEF 10-bit", cts_dir),
        ("forward-key-frame-10.ivf", "AV1 Forward key frame 10-bit", cts_dir),
        ("global-motion-10.ivf", "AV1 Global motion 10-bit", cts_dir),
        ("loop-filter-10.ivf", "AV1 Loop filter 10-bit", cts_dir),
        ("lossless-10.ivf", "AV1 Lossless 10-bit", cts_dir),
        ("order-hint-10.ivf", "AV1 Order hint 10-bit", cts_dir),
        # CTS special content
        ("av1-1-b8-02-allintra.ivf", "AV1 All intra", cts_dir),
        ("av1-1-b8-04-cdfupdate.ivf", "AV1 CDF update", cts_dir),
        ("av1-1-b8-05-mv.ivf", "AV1 Motion vectors", cts_dir),
        ("av1-1-b8-06-mfmv.ivf", "AV1 MFMV", cts_dir),
        ("av1-1-b8-23-film_grain-50.ivf", "AV1 Film grain", cts_dir),
        # SVC content
        ("av1-1-b8-22-svc-L1T2.ivf", "AV1 SVC L1T2", cts_dir),
        ("av1-1-b8-22-svc-L2T1.ivf", "AV1 SVC L2T1", cts_dir),
        ("av1-1-b8-22-svc-L2T2.ivf", "AV1 SVC L2T2", cts_dir),
        # CTS video directory
        ("av1-176x144-main-basic-8.ivf", "AV1 176x144 basic 8-bit", cts_video_dir),
        ("av1-176x144-main-basic-10.ivf", "AV1 176x144 basic 10-bit", cts_video_dir),
        ("av1-352x288-main-allintra-8.ivf", "AV1 352x288 all intra", cts_video_dir),
        ("av1-352x288-main-filmgrain-8.ivf", "AV1 352x288 film grain", cts_video_dir),
        ("av1-1920x1080-main-superres-8.ivf", "AV1 1080p superres", cts_video_dir),
        ("av1-1920x1080-intrabc-extreme-dv-8.ivf", "AV1 1080p intrabc extreme", cts_video_dir),
    ]
    
    for filename, desc, base_dir in av1_files:
        filepath = base_dir / filename
        if filepath.exists():
            test_cases.append(TestCase(
                name=f"DEC_AV1_{Path(filename).stem.replace('-', '_')}",
                codec=Codec.AV1,
                input_file=filepath,
                description=desc
            ))
    
    # Add AV1 content from av1-test-content directory
    av1_content_dir = video_dir / "av1-test-content" / "av1_input" / "cts1"
    if av1_content_dir.exists():
        for ivf_file in av1_content_dir.glob("*.ivf"):
            test_cases.append(TestCase(
                name=f"DEC_AV1_cts1_{ivf_file.stem.replace('-', '_')}",
                codec=Codec.AV1,
                input_file=ivf_file,
                description=f"AV1 CTS1 {ivf_file.stem}"
            ))
    
    # VP9 test cases
    # WebM files that might contain VP9
    vp9_files = [
        # Known VP9 content
        ("Big_Buck_Bunny_1080_10s_30MB.webm", "VP9 Big Buck Bunny 1080p", video_dir),
        # Add VP9 WebM files if available
        ("sample_vp9.webm", "VP9 sample WebM", video_dir),
        ("vp9_sample.ivf", "VP9 sample IVF", video_dir),
    ]
    
    for filename, desc, base_dir in vp9_files:
        filepath = base_dir / filename
        if filepath.exists():
            test_cases.append(TestCase(
                name=f"DEC_VP9_{Path(filename).stem.replace('-', '_')}",
                codec=Codec.VP9,
                input_file=filepath,
                description=desc
            ))
    
    # Auto-discover VP9 files from various directories
    vp9_patterns = ["*vp9*.ivf", "*vp9*.webm", "vp9*.ivf", "vp9*.webm"]
    search_dirs = [video_dir, cts_dir, cts_video_dir]
    
    for search_dir in search_dirs:
        if search_dir.exists():
            for pattern in vp9_patterns:
                for vp9_file in search_dir.glob(pattern):
                    test_cases.append(TestCase(
                        name=f"DEC_VP9_{vp9_file.stem.replace('-', '_')}",
                        codec=Codec.VP9,
                        input_file=vp9_file,
                        description=f"VP9 {vp9_file.stem}"
                    ))
    
    return test_cases


def get_encoder_test_cases(config: TestConfig) -> List[TestCase]:
    """Get all encoder test cases."""
    test_cases = []
    video_dir = config.video_clips_dir
    cts_video_dir = video_dir / "cts" / "video"
    
    # YUV input files for encoding
    yuv_files = [
        # CTS video content - 8-bit
        ("352x288_420_8le.yuv", 352, 288, 8, "420"),
        ("176x144_420_8le.yuv", 176, 144, 8, "420"),
        ("720x480_420_8le.yuv", 720, 480, 8, "420"),
        ("128x128_420_8le.yuv", 128, 128, 8, "420"),
        ("1920x1080_420_8le.yuv", 1920, 1080, 8, "420"),
        ("3840x2160_420_8le.yuv", 3840, 2160, 8, "420"),
        # CTS video content - 10-bit
        ("352x288_420_10le.yuv", 352, 288, 10, "420"),
        ("176x144_420_10le.yuv", 176, 144, 10, "420"),
        ("720x480_420_10le.yuv", 720, 480, 10, "420"),
        ("128x128_420_10le.yuv", 128, 128, 10, "420"),
        ("1920x1080_420_10le.yuv", 1920, 1080, 10, "420"),
        ("3840x2160_420_10le.yuv", 3840, 2160, 10, "420"),
    ]
    
    # Main video clips YUV files
    main_yuv_files = [
        ("352x288_420_8le.yuv", 352, 288, 8, "420"),
        ("BasketballDrive_1920x1080_50.yuv", 1920, 1080, 8, "420"),
    ]
    
    # 10-bit test content
    test_10bit_dir = video_dir / "test_content_10bit"
    tenbit_files = [
        ("jellyfish_1920x1080_10bit_420_100frames.yuv", 1920, 1080, 10, "420"),
        ("jellyfish_1920x1080_10bit_422_100frames.yuv", 1920, 1080, 10, "422"),
        ("jellyfish_1920x1080_10bit_444_100frames.yuv", 1920, 1080, 10, "444"),
        ("jellyfish_3840x2160_10bit_420_100frames.yuv", 3840, 2160, 10, "420"),
    ]
    
    codecs = [Codec.H264, Codec.H265, Codec.AV1]
    
    # Add CTS video YUV files for all codecs
    for yuv_file, width, height, bpp, chroma in yuv_files:
        filepath = cts_video_dir / yuv_file
        if not filepath.exists():
            continue
            
        for codec in codecs:
            # Skip 10-bit for H.264 (not commonly supported)
            if bpp == 10 and codec == Codec.H264:
                continue
            # Skip 4K for smaller tests
            if width > 1920 and height > 1080:
                continue
                
            test_cases.append(TestCase(
                name=f"ENC_{codec.value.upper()}_{width}x{height}_{bpp}bit",
                codec=codec,
                input_file=filepath,
                description=f"{codec.value.upper()} encode {width}x{height} {bpp}-bit",
                width=width,
                height=height,
                bitdepth=bpp,
                chroma=chroma,
                num_frames=min(config.max_frames, 30)
            ))
    
    # Add main YUV files
    for yuv_file, width, height, bpp, chroma in main_yuv_files:
        filepath = video_dir / yuv_file
        if not filepath.exists():
            continue
            
        for codec in codecs:
            test_cases.append(TestCase(
                name=f"ENC_{codec.value.upper()}_{Path(yuv_file).stem}",
                codec=codec,
                input_file=filepath,
                description=f"{codec.value.upper()} encode {yuv_file}",
                width=width,
                height=height,
                bitdepth=bpp,
                chroma=chroma,
                num_frames=min(config.max_frames, 30)
            ))
    
    # Add 10-bit test content
    if test_10bit_dir.exists():
        for yuv_file, width, height, bpp, chroma in tenbit_files:
            filepath = test_10bit_dir / yuv_file
            if not filepath.exists():
                continue
                
            for codec in [Codec.H265, Codec.AV1]:  # Skip H.264 for 10-bit
                # Skip non-420 for now (limited support)
                if chroma != "420":
                    continue
                    
                test_cases.append(TestCase(
                    name=f"ENC_{codec.value.upper()}_10bit_{Path(yuv_file).stem}",
                    codec=codec,
                    input_file=filepath,
                    description=f"{codec.value.upper()} encode 10-bit {yuv_file}",
                    width=width,
                    height=height,
                    bitdepth=bpp,
                    chroma=chroma,
                    num_frames=min(config.max_frames, 30)
                ))
    
    return test_cases


def get_aq_test_cases(config: TestConfig) -> List[TestCase]:
    """Get AQ-specific encoder test cases."""
    test_cases = []
    video_dir = config.video_clips_dir
    cts_video_dir = video_dir / "cts" / "video"
    
    # Use 1080p content for AQ testing
    yuv_1080p = cts_video_dir / "1920x1080_420_8le.yuv"
    yuv_720p = cts_video_dir / "720x480_420_8le.yuv"
    
    # AQ strength combinations to test
    aq_configs = [
        # (spatial_aq, temporal_aq, description)
        (0.0, -2.0, "spatial_only_default"),
        (0.5, -2.0, "spatial_only_0.5"),
        (1.0, -2.0, "spatial_only_max"),
        (-2.0, 0.0, "temporal_only_default"),
        (-2.0, 0.5, "temporal_only_0.5"),
        (-2.0, 1.0, "temporal_only_max"),
        (0.0, 0.0, "combined_default"),
        (0.5, 0.5, "combined_medium"),
        (1.0, 1.0, "combined_max"),
    ]
    
    for codec in [Codec.H264, Codec.H265, Codec.AV1]:
        for spatial_aq, temporal_aq, desc in aq_configs:
            yuv_file = yuv_720p if yuv_720p.exists() else yuv_1080p
            if not yuv_file.exists():
                continue
                
            width = 720 if "720" in str(yuv_file) else 1920
            height = 480 if "480" in str(yuv_file) else 1080
            
            test_cases.append(TestCase(
                name=f"ENC_AQ_{codec.value.upper()}_{desc}",
                codec=codec,
                input_file=yuv_file,
                description=f"{codec.value.upper()} AQ {desc} (spatial={spatial_aq}, temporal={temporal_aq})",
                width=width,
                height=height,
                bitdepth=8,
                chroma="420",
                num_frames=min(config.max_frames, 32),
                extra_args=[
                    "--spatialAQStrength", str(spatial_aq),
                    "--temporalAQStrength", str(temporal_aq),
                ]
            ))
    
    return test_cases


# ============================================================================
# Test Runner
# ============================================================================

class TestRunner:
    """Runs test cases and collects results."""
    
    def __init__(self, config: TestConfig):
        self.config = config
        self.results: List[TestResult_] = []
        
        # Create output directory (on VM or locally)
        self._run_command(f"mkdir -p '{self.config.output_dir}'", shell=True)
        
        # Verify VM connectivity if not running locally
        if not config.run_local:
            try:
                result = subprocess.run(
                    ["ssh", "-o", "ConnectTimeout=5", config.get_ssh_target(), "echo 'Remote OK'"],
                    capture_output=True, text=True, timeout=10
                )
                if result.returncode != 0:
                    print(f"Warning: Cannot connect to remote at {config.get_ssh_target()}")
            except Exception as e:
                print(f"Warning: Remote connectivity check failed: {e}")
    
    def _run_command(self, cmd: List[str] | str, shell: bool = False) -> subprocess.CompletedProcess:
        """Run command locally or on VM via SSH."""
        if self.config.run_local:
            if shell:
                return subprocess.run(cmd, shell=True, capture_output=True, text=True)
            return subprocess.run(cmd, capture_output=True, text=True)
        else:
            # Run via SSH
            ssh_target = self.config.get_ssh_target()
            if isinstance(cmd, list):
                cmd_str = " ".join(f"'{c}'" for c in cmd)
            else:
                cmd_str = cmd
            return subprocess.run(
                ["ssh", ssh_target, cmd_str],
                capture_output=True, text=True
            )
        
    def _build_decoder_command(self, test_case: TestCase) -> List[str]:
        """Build decoder command line."""
        decoder = self.config.get_decoder_path()
        cmd = [str(decoder)]
        
        # Input file
        cmd.extend(["-i", str(test_case.input_file)])
        
        # Headless mode (no presentation)
        cmd.append("--noPresent")
        
        # Force codec if specified
        if test_case.codec == Codec.H264:
            cmd.extend(["--codec", "h264"])
        elif test_case.codec == Codec.H265:
            cmd.extend(["--codec", "h265"])
        elif test_case.codec == Codec.AV1:
            cmd.extend(["--codec", "av1"])
        elif test_case.codec == Codec.VP9:
            cmd.extend(["--codec", "vp9"])
        
        # Max frames
        if self.config.max_frames > 0:
            cmd.extend(["-c", str(self.config.max_frames)])
        
        # Validation
        if self.config.enable_validation:
            cmd.append("-v")
        
        # Output file for verification
        output_file = self.config.output_dir / f"{test_case.name}.yuv"
        cmd.extend(["-o", str(output_file)])
        
        return cmd
    
    def _build_encoder_command(self, test_case: TestCase) -> List[str]:
        """Build encoder command line."""
        encoder = self.config.get_encoder_path()
        cmd = [str(encoder)]
        
        # Input file
        cmd.extend(["-i", str(test_case.input_file)])
        
        # Codec
        if test_case.codec == Codec.H264:
            cmd.extend(["-c", "h264"])
            output_ext = ".264"
        elif test_case.codec == Codec.H265:
            cmd.extend(["-c", "hevc"])
            output_ext = ".265"
        elif test_case.codec == Codec.AV1:
            cmd.extend(["-c", "av1"])
            output_ext = ".ivf"
        else:
            output_ext = ".bin"
        
        # Dimensions
        cmd.extend(["--inputWidth", str(test_case.width)])
        cmd.extend(["--inputHeight", str(test_case.height)])
        
        # Bit depth
        if test_case.bitdepth != 8:
            cmd.extend(["--inputBpp", str(test_case.bitdepth)])
        
        # Chroma subsampling
        cmd.extend(["--inputChromaSubsampling", test_case.chroma])
        
        # Number of frames
        if test_case.num_frames > 0:
            cmd.extend(["--numFrames", str(test_case.num_frames)])
        
        # Output file
        output_file = self.config.output_dir / f"{test_case.name}{output_ext}"
        cmd.extend(["-o", str(output_file)])
        
        # Extra args (like AQ parameters)
        cmd.extend(test_case.extra_args)
        
        return cmd
    
    def _get_validation_env(self) -> Dict[str, str]:
        """Get environment variables for validation layers."""
        env = os.environ.copy()
        if self.config.enable_validation:
            env["VK_LOADER_LAYERS_ENABLE"] = "*validation"
            env["VK_VALIDATION_VALIDATE_SYNC"] = "true"
            env["VK_VALIDATION_THREAD_SAFETY"] = "true"
        return env
    
    def _count_validation_errors(self, output: str) -> int:
        """Count validation errors in output."""
        error_patterns = [
            "VALIDATION ERROR",
            "Validation Error",
            "VK_ERROR_",
            "ERROR:",
        ]
        count = 0
        for line in output.split('\n'):
            for pattern in error_patterns:
                if pattern in line:
                    count += 1
                    break
        return count
    
    def run_test(self, test_case: TestCase, test_type: str) -> TestResult_:
        """Run a single test case."""
        print(f"  Running: {test_case.name}", end="", flush=True)
        
        # Build command
        if test_type == "decoder":
            cmd = self._build_decoder_command(test_case)
        else:
            cmd = self._build_encoder_command(test_case)
        
        # Setup environment variables for validation
        env_prefix = ""
        if self.config.enable_validation:
            env_prefix = "VK_LOADER_LAYERS_ENABLE='*validation' VK_VALIDATION_VALIDATE_SYNC=true "
        
        # Build full command string
        cmd_str = env_prefix + " ".join(f"'{c}'" for c in cmd)
        
        # Run the test
        start_time = time.time()
        try:
            if self.config.run_local:
                # Run locally with environment
                env = self._get_validation_env()
                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=self.config.timeout,
                    env=env
                )
            else:
                # Run on VM via SSH
                ssh_target = self.config.get_ssh_target()
                result = subprocess.run(
                    ["ssh", ssh_target, cmd_str],
                    capture_output=True,
                    text=True,
                    timeout=self.config.timeout
                )
            
            duration = time.time() - start_time
            
            output = result.stdout + result.stderr
            validation_errors = self._count_validation_errors(output)
            
            # Determine status
            if result.returncode == 0:
                if validation_errors > 0 and self.config.enable_validation:
                    status = TestResult.FAILED
                else:
                    status = TestResult.PASSED
            else:
                status = TestResult.FAILED
            
            test_result = TestResult_(
                test_case=test_case,
                status=status,
                duration=duration,
                output=output,
                validation_errors=validation_errors
            )
            
        except subprocess.TimeoutExpired:
            duration = time.time() - start_time
            test_result = TestResult_(
                test_case=test_case,
                status=TestResult.ERROR,
                duration=duration,
                output="",
                error=f"Timeout after {self.config.timeout}s"
            )
        except FileNotFoundError as e:
            duration = time.time() - start_time
            test_result = TestResult_(
                test_case=test_case,
                status=TestResult.SKIPPED,
                duration=duration,
                output="",
                error=f"Executable not found: {e}"
            )
        except Exception as e:
            duration = time.time() - start_time
            test_result = TestResult_(
                test_case=test_case,
                status=TestResult.ERROR,
                duration=duration,
                output="",
                error=str(e)
            )
        
        # Print result
        status_icons = {
            TestResult.PASSED: "✓",
            TestResult.FAILED: "✗",
            TestResult.SKIPPED: "○",
            TestResult.ERROR: "!",
        }
        print(f" [{status_icons[test_result.status]}] ({test_result.duration:.2f}s)")
        
        if test_result.status != TestResult.PASSED and self.config.verbose:
            if test_result.error:
                print(f"    Error: {test_result.error}")
            if test_result.validation_errors > 0:
                print(f"    Validation errors: {test_result.validation_errors}")
        
        return test_result
    
    def run_decoder_tests(self) -> List[TestResult_]:
        """Run all decoder tests."""
        print("\n" + "=" * 70)
        print("DECODER TESTS")
        print("=" * 70)
        
        test_cases = get_decoder_test_cases(self.config)
        print(f"Found {len(test_cases)} decoder test cases\n")
        
        results = []
        for codec in [Codec.H264, Codec.H265, Codec.AV1, Codec.VP9]:
            codec_tests = [tc for tc in test_cases if tc.codec == codec]
            if codec_tests:
                print(f"\n{codec.value.upper()} Decoder Tests ({len(codec_tests)} tests):")
                print("-" * 50)
                for tc in codec_tests:
                    result = self.run_test(tc, "decoder")
                    results.append(result)
        
        self.results.extend(results)
        return results
    
    def run_encoder_tests(self, include_aq: bool = False) -> List[TestResult_]:
        """Run all encoder tests."""
        print("\n" + "=" * 70)
        print("ENCODER TESTS")
        print("=" * 70)
        
        test_cases = get_encoder_test_cases(self.config)
        if include_aq:
            test_cases.extend(get_aq_test_cases(self.config))
        
        print(f"Found {len(test_cases)} encoder test cases\n")
        
        results = []
        for codec in [Codec.H264, Codec.H265, Codec.AV1]:
            codec_tests = [tc for tc in test_cases if tc.codec == codec]
            if codec_tests:
                print(f"\n{codec.value.upper()} Encoder Tests ({len(codec_tests)} tests):")
                print("-" * 50)
                for tc in codec_tests:
                    result = self.run_test(tc, "encoder")
                    results.append(result)
        
        self.results.extend(results)
        return results
    
    def print_summary(self):
        """Print test summary."""
        print("\n" + "=" * 70)
        print("TEST SUMMARY")
        print("=" * 70)
        
        total = len(self.results)
        passed = len([r for r in self.results if r.status == TestResult.PASSED])
        failed = len([r for r in self.results if r.status == TestResult.FAILED])
        skipped = len([r for r in self.results if r.status == TestResult.SKIPPED])
        errors = len([r for r in self.results if r.status == TestResult.ERROR])
        total_validation_errors = sum(r.validation_errors for r in self.results)
        
        print(f"\nTotal Tests:     {total}")
        print(f"  ✓ Passed:      {passed}")
        print(f"  ✗ Failed:      {failed}")
        print(f"  ○ Skipped:     {skipped}")
        print(f"  ! Errors:      {errors}")
        print(f"\nValidation Errors: {total_validation_errors}")
        
        total_duration = sum(r.duration for r in self.results)
        print(f"\nTotal Duration:  {total_duration:.2f}s")
        
        # Print failed tests
        failed_results = [r for r in self.results if r.status in [TestResult.FAILED, TestResult.ERROR]]
        if failed_results:
            print("\n" + "-" * 50)
            print("Failed/Error Tests:")
            for r in failed_results:
                print(f"  - {r.test_case.name}: {r.error or 'See output'}")
        
        print("\n" + "=" * 70)
        
        return passed == total and total > 0
    
    def save_report(self, filename: str = "test_report.json"):
        """Save test results to JSON file."""
        report = {
            "timestamp": datetime.now().isoformat(),
            "config": {
                "validation_enabled": self.config.enable_validation,
                "aq_enabled": self.config.enable_aq,
                "max_frames": self.config.max_frames,
            },
            "summary": {
                "total": len(self.results),
                "passed": len([r for r in self.results if r.status == TestResult.PASSED]),
                "failed": len([r for r in self.results if r.status == TestResult.FAILED]),
                "skipped": len([r for r in self.results if r.status == TestResult.SKIPPED]),
                "errors": len([r for r in self.results if r.status == TestResult.ERROR]),
            },
            "results": [
                {
                    "name": r.test_case.name,
                    "codec": r.test_case.codec.value,
                    "description": r.test_case.description,
                    "status": r.status.value,
                    "duration": r.duration,
                    "validation_errors": r.validation_errors,
                    "error": r.error,
                }
                for r in self.results
            ]
        }
        
        report_path = self.config.output_dir / filename
        with open(report_path, 'w') as f:
            json.dump(report, f, indent=2)
        
        print(f"\nReport saved to: {report_path}")


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Vulkan Video Samples - Comprehensive Test Suite",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    # Run all decoder tests (video-dir is required)
    python3 vulkan_video_test.py --video-dir /data/videos --test decoder
    
    # Run all encoder tests with validation layers
    python3 vulkan_video_test.py --video-dir /data/videos --test encoder --validate
    
    # Run encoder tests with AQ testing
    python3 vulkan_video_test.py --video-dir /data/videos --test encoder --aq
    
    # Run all tests on a remote host
    python3 vulkan_video_test.py --video-dir /data/videos --remote 192.168.122.216
    
    # Run quick tests (limited frames)
    python3 vulkan_video_test.py --video-dir /data/videos --test all --max-frames 10
"""
    )
    
    parser.add_argument(
        "--test",
        choices=["decoder", "encoder", "all"],
        default="all",
        help="Which tests to run (default: all)"
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="Enable Vulkan validation layers"
    )
    parser.add_argument(
        "--aq",
        action="store_true",
        help="Include AQ (Adaptive Quantization) tests for encoder"
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Show verbose output"
    )
    parser.add_argument(
        "--max-frames",
        type=int,
        default=30,
        help="Maximum frames to process per test (default: 30)"
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=300,
        help="Timeout per test in seconds (default: 300)"
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("/tmp/vulkan_video_tests"),
        help="Output directory for test artifacts"
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("/data/nvidia/android-extra/video-apps/vulkan-video-samples/build"),
        help="Build directory containing executables"
    )
    parser.add_argument(
        "--video-dir",
        type=Path,
        required=True,
        help="Directory containing test video clips (REQUIRED)"
    )
    parser.add_argument(
        "--local",
        action="store_true",
        help="Run tests locally instead of on remote"
    )
    parser.add_argument(
        "--remote",
        type=str,
        default="127.0.0.1",
        help="Remote hostname/IP (default: 127.0.0.1)"
    )
    parser.add_argument(
        "--remote-user",
        type=str,
        default="",
        help="Remote username (default: current user)"
    )
    
    args = parser.parse_args()
    
    # Create config
    config = TestConfig(
        build_dir=args.build_dir,
        video_clips_dir=args.video_dir,
        output_dir=args.output_dir,
        remote_host=args.remote,
        remote_user=args.remote_user,
        run_local=args.local,
        enable_validation=args.validate,
        enable_aq=args.aq,
        verbose=args.verbose,
        max_frames=args.max_frames,
        timeout=args.timeout,
    )
    
    # Verify video directory exists
    if config.video_clips_dir is None:
        print("Error: --video-dir is required")
        return 1
    
    if not config.video_clips_dir.exists():
        print(f"Error: Video directory does not exist: {config.video_clips_dir}")
        return 1
    
    # Verify executables exist
    decoder_path = config.get_decoder_path()
    encoder_path = config.get_encoder_path()
    
    if not decoder_path.exists():
        print(f"Warning: Decoder not found at {decoder_path}")
    if not encoder_path.exists():
        print(f"Warning: Encoder not found at {encoder_path}")
    
    # Create and run test suite
    print("=" * 70)
    print("VULKAN VIDEO SAMPLES - TEST SUITE")
    print("=" * 70)
    print(f"Date: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    if config.run_local:
        print(f"Target: Local execution")
    else:
        print(f"Target Remote: {config.get_ssh_target()}")
    print(f"Validation: {'Enabled' if config.enable_validation else 'Disabled'}")
    print(f"AQ Tests: {'Enabled' if config.enable_aq else 'Disabled'}")
    print(f"Max Frames: {config.max_frames}")
    print(f"Output Dir: {config.output_dir}")
    
    runner = TestRunner(config)
    
    if args.test in ["decoder", "all"]:
        runner.run_decoder_tests()
    
    if args.test in ["encoder", "all"]:
        runner.run_encoder_tests(include_aq=config.enable_aq)
    
    # Print summary
    all_passed = runner.print_summary()
    
    # Save report
    runner.save_report()
    
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
