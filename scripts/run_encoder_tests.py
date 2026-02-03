#!/usr/bin/env python3
"""
Vulkan Video Encoder Test Runner

Tests encoder functionality for all supported codecs (H.264, H.265, AV1)
with optional Adaptive Quantization (AQ) testing.
Works on both Linux and Windows.

Usage:
    python run_encoder_tests.py --video-dir /path/to/videos [OPTIONS]

Options:
    --video-dir PATH    Directory containing test video files (REQUIRED)
    --validate, -v      Enable Vulkan validation layers
    --verbose           Show detailed output
    --aq                Include Adaptive Quantization (AQ) tests
    --codec CODEC       Only test specific codec (h264, h265, av1)
    --local             Run locally instead of on remote
    --remote HOST       Remote hostname/IP (default: 127.0.0.1)
"""

import argparse
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple


# ANSI color codes (disabled on Windows unless using Windows Terminal)
def supports_color() -> bool:
    """Check if terminal supports ANSI colors."""
    if sys.platform == "win32":
        return os.environ.get("WT_SESSION") is not None or os.environ.get("TERM_PROGRAM") == "vscode"
    return hasattr(sys.stdout, 'isatty') and sys.stdout.isatty()


if supports_color():
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    CYAN = '\033[0;36m'
    BOLD = '\033[1m'
    NC = '\033[0m'
else:
    RED = GREEN = YELLOW = CYAN = BOLD = NC = ''


@dataclass
class TestConfig:
    """Test configuration."""
    video_dir: Path
    build_dir: Path
    output_dir: Path
    remote_host: str = "127.0.0.1"
    remote_user: str = ""
    run_local: bool = False
    validate: bool = False
    verbose: bool = False
    filter_codec: str = ""
    enable_aq: bool = False
    max_frames: int = 30


@dataclass
class TestResult:
    """Result of a single test."""
    name: str
    passed: bool
    skipped: bool
    duration: float
    message: str = ""


class EncoderTestRunner:
    """Runs encoder tests."""
    
    def __init__(self, config: TestConfig):
        self.config = config
        self.results: List[TestResult] = []
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        
        # Determine encoder path
        if sys.platform == "win32":
            self.encoder = config.build_dir / "vk_video_encoder" / "demos" / "vk-video-enc-test.exe"
        else:
            self.encoder = config.build_dir / "vk_video_encoder" / "demos" / "vk-video-enc-test"
    
    def get_ssh_target(self) -> str:
        """Get SSH target string."""
        user = self.config.remote_user or os.environ.get("USER", os.environ.get("USERNAME", "root"))
        return f"{user}@{self.config.remote_host}"
    
    def run_command(self, cmd: List[str], env: Optional[dict] = None) -> Tuple[int, str, str]:
        """Run command locally or on remote."""
        if self.config.run_local:
            try:
                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=300,
                    env=env or os.environ
                )
                return result.returncode, result.stdout, result.stderr
            except subprocess.TimeoutExpired:
                return -1, "", "Timeout"
            except Exception as e:
                return -1, "", str(e)
        else:
            # Run via SSH
            ssh_target = self.get_ssh_target()
            cmd_str = " ".join(f"'{c}'" for c in cmd)
            
            # Add environment variables for validation
            if env:
                env_prefix = " ".join(f"{k}='{v}'" for k, v in env.items() if k.startswith("VK_"))
                cmd_str = f"{env_prefix} {cmd_str}"
            
            try:
                result = subprocess.run(
                    ["ssh", ssh_target, cmd_str],
                    capture_output=True,
                    text=True,
                    timeout=300
                )
                return result.returncode, result.stdout, result.stderr
            except subprocess.TimeoutExpired:
                return -1, "", "Timeout"
            except Exception as e:
                return -1, "", str(e)
    
    def check_file_exists(self, path: str) -> bool:
        """Check if file exists (locally or on remote)."""
        if self.config.run_local:
            return Path(path).exists()
        else:
            ssh_target = self.get_ssh_target()
            result = subprocess.run(
                ["ssh", "-o", "ConnectTimeout=5", ssh_target, f"test -f '{path}'"],
                capture_output=True
            )
            return result.returncode == 0
    
    def check_dir_exists(self, path: str) -> bool:
        """Check if directory exists (locally or on remote)."""
        if self.config.run_local:
            return Path(path).is_dir()
        else:
            ssh_target = self.get_ssh_target()
            result = subprocess.run(
                ["ssh", "-o", "ConnectTimeout=5", ssh_target, f"test -d '{path}'"],
                capture_output=True
            )
            return result.returncode == 0
    
    def run_test(self, name: str, codec: str, input_file: str, width: int, height: int,
                 bpp: int = 8, chroma: str = "420", extra_args: List[str] = None) -> None:
        """Run a single encoder test."""
        # Filter by codec if specified
        if self.config.filter_codec and self.config.filter_codec != codec:
            return
        
        # Check if file exists
        if not self.check_file_exists(input_file):
            print(f"  {YELLOW}○{NC} {name} - File not found")
            self.skipped += 1
            self.results.append(TestResult(name, False, True, 0, "File not found"))
            return
        
        # Determine output extension
        ext_map = {"h264": ".264", "h265": ".265", "av1": ".ivf"}
        ext = ext_map.get(codec, ".bin")
        output_file = self.config.output_dir / f"{name}{ext}"
        
        # Map codec name for encoder
        codec_arg = "hevc" if codec == "h265" else codec
        
        # Build command
        cmd = [
            str(self.encoder),
            "-i", input_file,
            "-c", codec_arg,
            "--inputWidth", str(width),
            "--inputHeight", str(height),
            "--inputChromaSubsampling", chroma,
            "--numFrames", str(self.config.max_frames),
            "-o", str(output_file)
        ]
        
        if bpp != 8:
            cmd.extend(["--inputBpp", str(bpp)])
        
        if extra_args:
            cmd.extend(extra_args)
        
        # Set up environment for validation
        env = os.environ.copy()
        if self.config.validate:
            env["VK_LOADER_LAYERS_ENABLE"] = "*validation"
            env["VK_VALIDATION_VALIDATE_SYNC"] = "true"
        
        if self.config.verbose:
            print(f"\n  {CYAN}Command: {' '.join(cmd)}{NC}")
        
        start_time = time.time()
        returncode, stdout, stderr = self.run_command(cmd, env)
        duration = time.time() - start_time
        
        output = stdout + stderr
        
        # Check result
        if returncode == 0:
            # Check for validation errors
            if "validation error" in output.lower():
                print(f"  {RED}✗{NC} {name} - Validation errors ({duration:.2f}s)")
                self.failed += 1
                self.results.append(TestResult(name, False, False, duration, "Validation errors"))
            elif "Done processing" in output or returncode == 0:
                print(f"  {GREEN}✓{NC} {name} ({duration:.2f}s)")
                self.passed += 1
                self.results.append(TestResult(name, True, False, duration))
            else:
                print(f"  {RED}✗{NC} {name} - Unknown error ({duration:.2f}s)")
                self.failed += 1
                self.results.append(TestResult(name, False, False, duration, "Unknown error"))
        else:
            print(f"  {RED}✗{NC} {name} - Failed ({duration:.2f}s)")
            self.failed += 1
            self.results.append(TestResult(name, False, False, duration, stderr[:200] if stderr else "Unknown error"))
            if self.config.verbose:
                print(f"    {stderr[:200] if stderr else output[:200]}")
    
    def print_header(self, title: str) -> None:
        """Print section header."""
        print()
        print(f"{BOLD}{'=' * 50}{NC}")
        print(f"{BOLD}{title}{NC}")
        print(f"{BOLD}{'=' * 50}{NC}")
    
    def run_all_tests(self) -> bool:
        """Run all encoder tests."""
        video_dir = self.config.video_dir
        cts_video_dir = video_dir / "cts" / "video"
        
        self.print_header("Vulkan Video Encoder Tests")
        if not self.config.run_local:
            print(f"Target Remote: {CYAN}{self.get_ssh_target()}{NC}")
        print(f"Encoder: {self.encoder}")
        print(f"Video Dir: {video_dir}")
        print(f"Output Dir: {self.config.output_dir}")
        print(f"Max Frames: {self.config.max_frames}")
        print(f"Validation: {GREEN}Enabled{NC}" if self.config.validate else "Validation: Disabled")
        print(f"AQ Tests: {GREEN}Enabled{NC}" if self.config.enable_aq else "AQ Tests: Disabled")
        
        # H.264/AVC Encoder Tests
        self.print_header("H.264/AVC Encoder Tests")
        h264_tests = [
            ("H264_176x144_8bit", "h264", cts_video_dir / "176x144_420_8le.yuv", 176, 144, 8),
            ("H264_352x288_8bit", "h264", cts_video_dir / "352x288_420_8le.yuv", 352, 288, 8),
            ("H264_720x480_8bit", "h264", cts_video_dir / "720x480_420_8le.yuv", 720, 480, 8),
            ("H264_1920x1080_8bit", "h264", cts_video_dir / "1920x1080_420_8le.yuv", 1920, 1080, 8),
            ("H264_gop_8", "h264", cts_video_dir / "352x288_420_8le.yuv", 352, 288, 8,
             ["--gopFrameCount", "8", "--consecutiveBFrameCount", "0"]),
            ("H264_gop_16_b3", "h264", cts_video_dir / "352x288_420_8le.yuv", 352, 288, 8,
             ["--gopFrameCount", "16", "--consecutiveBFrameCount", "3"]),
            ("H264_rc_cbr", "h264", cts_video_dir / "720x480_420_8le.yuv", 720, 480, 8,
             ["--rateControlMode", "cbr", "--averageBitrate", "2000000"]),
        ]
        for test in h264_tests:
            name, codec, path, w, h, bpp = test[:6]
            extra = test[6] if len(test) > 6 else None
            self.run_test(name, codec, str(path), w, h, bpp, "420", extra)
        
        # H.265/HEVC Encoder Tests
        self.print_header("H.265/HEVC Encoder Tests")
        h265_tests = [
            ("HEVC_176x144_8bit", "h265", cts_video_dir / "176x144_420_8le.yuv", 176, 144, 8),
            ("HEVC_352x288_8bit", "h265", cts_video_dir / "352x288_420_8le.yuv", 352, 288, 8),
            ("HEVC_720x480_8bit", "h265", cts_video_dir / "720x480_420_8le.yuv", 720, 480, 8),
            ("HEVC_1920x1080_8bit", "h265", cts_video_dir / "1920x1080_420_8le.yuv", 1920, 1080, 8),
            ("HEVC_352x288_10bit", "h265", cts_video_dir / "352x288_420_10le.yuv", 352, 288, 10),
            ("HEVC_720x480_10bit", "h265", cts_video_dir / "720x480_420_10le.yuv", 720, 480, 10),
            ("HEVC_gop_8", "h265", cts_video_dir / "352x288_420_8le.yuv", 352, 288, 8,
             ["--gopFrameCount", "8", "--consecutiveBFrameCount", "0"]),
            ("HEVC_rc_cbr", "h265", cts_video_dir / "720x480_420_8le.yuv", 720, 480, 8,
             ["--rateControlMode", "cbr", "--averageBitrate", "2000000"]),
        ]
        for test in h265_tests:
            name, codec, path, w, h, bpp = test[:6]
            extra = test[6] if len(test) > 6 else None
            self.run_test(name, codec, str(path), w, h, bpp, "420", extra)
        
        # AV1 Encoder Tests
        self.print_header("AV1 Encoder Tests")
        av1_tests = [
            ("AV1_176x144_8bit", "av1", cts_video_dir / "176x144_420_8le.yuv", 176, 144, 8),
            ("AV1_352x288_8bit", "av1", cts_video_dir / "352x288_420_8le.yuv", 352, 288, 8),
            ("AV1_720x480_8bit", "av1", cts_video_dir / "720x480_420_8le.yuv", 720, 480, 8),
            ("AV1_1920x1080_8bit", "av1", cts_video_dir / "1920x1080_420_8le.yuv", 1920, 1080, 8),
            ("AV1_352x288_10bit", "av1", cts_video_dir / "352x288_420_10le.yuv", 352, 288, 10),
            ("AV1_gop_8", "av1", cts_video_dir / "352x288_420_8le.yuv", 352, 288, 8,
             ["--gopFrameCount", "8", "--consecutiveBFrameCount", "0"]),
        ]
        for test in av1_tests:
            name, codec, path, w, h, bpp = test[:6]
            extra = test[6] if len(test) > 6 else None
            self.run_test(name, codec, str(path), w, h, bpp, "420", extra)
        
        # AQ Tests
        if self.config.enable_aq:
            self.print_header("Adaptive Quantization (AQ) Tests")
            
            aq_input = cts_video_dir / "720x480_420_8le.yuv"
            if not self.check_file_exists(str(aq_input)):
                aq_input = cts_video_dir / "352x288_420_8le.yuv"
                aq_width, aq_height = 352, 288
            else:
                aq_width, aq_height = 720, 480
            
            aq_configs = [
                ("spatial_default", 0.0, -2.0),
                ("spatial_0.5", 0.5, -2.0),
                ("spatial_max", 1.0, -2.0),
                ("temporal_default", -2.0, 0.0),
                ("temporal_0.5", -2.0, 0.5),
                ("temporal_max", -2.0, 1.0),
                ("combined_default", 0.0, 0.0),
                ("combined_medium", 0.5, 0.5),
                ("combined_max", 1.0, 1.0),
            ]
            
            for codec in ["h264", "h265", "av1"]:
                if self.config.filter_codec and self.config.filter_codec != codec:
                    continue
                print(f"\n  {CYAN}{codec.upper()} AQ Tests:{NC}")
                for desc, spatial, temporal in aq_configs:
                    name = f"AQ_{codec}_{desc}"
                    extra = [
                        "--spatialAQStrength", str(spatial),
                        "--temporalAQStrength", str(temporal)
                    ]
                    self.run_test(name, codec, str(aq_input), aq_width, aq_height, 8, "420", extra)
        
        # Summary
        self.print_header("Test Summary")
        total = self.passed + self.failed + self.skipped
        print()
        print(f"Total Tests:    {total}")
        print(f"  {GREEN}✓ Passed:{NC}    {self.passed}")
        print(f"  {RED}✗ Failed:{NC}    {self.failed}")
        print(f"  {YELLOW}○ Skipped:{NC}   {self.skipped}")
        print()
        
        if self.failed == 0 and self.passed > 0:
            print(f"{GREEN}{BOLD}All tests passed!{NC}")
            return True
        else:
            print(f"{RED}{BOLD}Some tests failed.{NC}")
            return False


def main():
    parser = argparse.ArgumentParser(
        description="Vulkan Video Encoder Test Runner",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    
    parser.add_argument("--video-dir", type=Path, required=True,
                        help="Directory containing test video files (REQUIRED)")
    parser.add_argument("--validate", "-v", action="store_true",
                        help="Enable Vulkan validation layers")
    parser.add_argument("--verbose", action="store_true",
                        help="Show detailed output")
    parser.add_argument("--aq", action="store_true",
                        help="Include Adaptive Quantization (AQ) tests")
    parser.add_argument("--codec", type=str, default="",
                        help="Only test specific codec (h264, h265, av1)")
    parser.add_argument("--local", action="store_true",
                        help="Run locally instead of on remote")
    parser.add_argument("--remote", type=str, default="127.0.0.1",
                        help="Remote hostname/IP (default: 127.0.0.1)")
    parser.add_argument("--remote-user", type=str, default="",
                        help="Remote username (default: current user)")
    parser.add_argument("--max-frames", type=int, default=30,
                        help="Maximum frames to encode per test (default: 30)")
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="Build directory (default: auto-detect)")
    parser.add_argument("--output-dir", type=Path, default=Path("/tmp/vulkan_encoder_tests"),
                        help="Output directory for test artifacts")
    
    args = parser.parse_args()
    
    # Auto-detect build directory
    if args.build_dir is None:
        script_dir = Path(__file__).parent
        project_root = script_dir.parent
        args.build_dir = project_root / "build"
    
    # Validate video directory
    if not args.video_dir.exists():
        print(f"{RED}Error: Video directory does not exist: {args.video_dir}{NC}")
        return 1
    
    # Create output directory
    args.output_dir.mkdir(parents=True, exist_ok=True)
    
    config = TestConfig(
        video_dir=args.video_dir,
        build_dir=args.build_dir,
        output_dir=args.output_dir,
        remote_host=args.remote,
        remote_user=args.remote_user,
        run_local=args.local,
        validate=args.validate,
        verbose=args.verbose,
        filter_codec=args.codec,
        enable_aq=args.aq,
        max_frames=args.max_frames
    )
    
    # Check remote connectivity if not local
    if not config.run_local:
        runner = EncoderTestRunner(config)
        if not runner.check_dir_exists(str(args.video_dir)):
            print(f"{RED}Error: Video directory does not exist on remote: {args.video_dir}{NC}")
            print(f"Note: Checking on remote host {runner.get_ssh_target()}")
            return 1
    
    runner = EncoderTestRunner(config)
    success = runner.run_all_tests()
    
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
