#!/usr/bin/env python3
"""
Vulkan Video Decoder Test Runner

Tests decoder functionality for all supported codecs (H.264, H.265, AV1, VP9).
Works on both Linux and Windows.

Usage:
    python run_decoder_tests.py --video-dir /path/to/videos [OPTIONS]

Options:
    --video-dir PATH    Directory containing test video files (REQUIRED)
    --validate, -v      Enable Vulkan validation layers
    --verbose           Show detailed output
    --codec CODEC       Only test specific codec (h264, h265, av1, vp9)
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
    max_frames: int = 30


@dataclass
class TestResult:
    """Result of a single test."""
    name: str
    passed: bool
    skipped: bool
    duration: float
    message: str = ""


class DecoderTestRunner:
    """Runs decoder tests."""
    
    def __init__(self, config: TestConfig):
        self.config = config
        self.results: List[TestResult] = []
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        
        # Determine decoder path
        if sys.platform == "win32":
            self.decoder = config.build_dir / "vk_video_decoder" / "demos" / "vk-video-dec-test.exe"
        else:
            self.decoder = config.build_dir / "vk_video_decoder" / "demos" / "vk-video-dec-test"
    
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
    
    def run_test(self, name: str, codec: str, input_file: str, description: str) -> None:
        """Run a single decoder test."""
        # Filter by codec if specified
        if self.config.filter_codec and self.config.filter_codec != codec:
            return
        
        # Check if file exists
        if not self.check_file_exists(input_file):
            print(f"  {YELLOW}○{NC} {name} - File not found")
            self.skipped += 1
            self.results.append(TestResult(name, False, True, 0, "File not found"))
            return
        
        output_file = self.config.output_dir / f"{name}.yuv"
        
        # Build command
        cmd = [
            str(self.decoder),
            "-i", input_file,
            "--noPresent",
            "--codec", codec,
            "-c", str(self.config.max_frames),
            "-o", str(output_file)
        ]
        
        if self.config.validate:
            cmd.append("-v")
        
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
                if self.config.verbose:
                    for line in output.split('\n'):
                        if 'validation' in line.lower():
                            print(f"    {line[:100]}")
            else:
                print(f"  {GREEN}✓{NC} {name} ({duration:.2f}s)")
                self.passed += 1
                self.results.append(TestResult(name, True, False, duration))
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
        """Run all decoder tests."""
        video_dir = self.config.video_dir
        cts_dir = video_dir / "cts"
        cts_video_dir = cts_dir / "video"
        
        self.print_header("Vulkan Video Decoder Tests")
        if not self.config.run_local:
            print(f"Target Remote: {CYAN}{self.get_ssh_target()}{NC}")
        print(f"Decoder: {self.decoder}")
        print(f"Video Dir: {video_dir}")
        print(f"Output Dir: {self.config.output_dir}")
        print(f"Max Frames: {self.config.max_frames}")
        print(f"Validation: {GREEN}Enabled{NC}" if self.config.validate else "Validation: Disabled")
        
        # H.264/AVC Tests
        self.print_header("H.264/AVC Decoder Tests")
        h264_tests = [
            ("H264_clip_a", "h264", cts_dir / "clip-a.h264", "H.264 CTS clip-a"),
            ("H264_clip_b", "h264", cts_dir / "clip-b.h264", "H.264 CTS clip-b"),
            ("H264_clip_c", "h264", cts_dir / "clip-c.h264", "H.264 CTS clip-c"),
            ("H264_4k_ibp", "h264", cts_dir / "4k_26_ibp_main.h264", "H.264 4K IBP"),
            ("H264_field", "h264", cts_dir / "avc-720x480-field.h264", "H.264 field coding"),
            ("H264_paff", "h264", cts_dir / "avc-1440x1080-paff.h264", "H.264 PAFF"),
            ("H264_akiyo", "h264", video_dir / "akiyo_176x144_30p_1_0.264", "H.264 Akiyo"),
            ("H264_jellyfish_4k", "h264", video_dir / "jellyfish-250-mbps-4k-uhd-h264.h264", "H.264 Jellyfish 4K"),
            ("H264_jellyfish_mkv", "h264", video_dir / "jellyfish-100-mbps-hd-h264.mkv", "H.264 Jellyfish MKV"),
            ("H264_golden_flower", "h264", video_dir / "golden_flower_h264_720_30p_7M.mp4", "H.264 Golden Flower MP4"),
        ]
        for name, codec, path, desc in h264_tests:
            self.run_test(name, codec, str(path), desc)
        
        # H.265/HEVC Tests
        self.print_header("H.265/HEVC Decoder Tests")
        h265_tests = [
            ("HEVC_clip_d", "h265", cts_dir / "clip-d.h265", "H.265 CTS clip-d"),
            ("HEVC_slist_a", "h265", cts_video_dir / "hevc-itu-slist-a.h265", "H.265 scaling list A"),
            ("HEVC_slist_b", "h265", cts_video_dir / "hevc-itu-slist-b.h265", "H.265 scaling list B"),
            ("HEVC_jellyfish_gob", "h265", cts_dir / "jellyfish-250-mbps-4k-uhd-GOB-IPB13.h265", "H.265 Jellyfish GOB"),
            ("HEVC_jellyfish_hd", "h265", video_dir / "jellyfish-110-mbps-hd-hevc.h265", "H.265 Jellyfish HD"),
            ("HEVC_jellyfish_4k_10bit", "h265", video_dir / "jellyfish-400-mbps-4k-uhd-hevc-10bit.h265", "H.265 4K 10-bit"),
            ("HEVC_jellyfish_mkv", "h265", video_dir / "jellyfish-100-mbps-hd-hevc.mkv", "H.265 Jellyfish MKV"),
        ]
        for name, codec, path, desc in h265_tests:
            self.run_test(name, codec, str(path), desc)
        
        # AV1 Tests
        self.print_header("AV1 Decoder Tests")
        av1_tests = [
            ("AV1_basic_8", "av1", cts_dir / "basic-8.ivf", "AV1 Basic 8-bit"),
            ("AV1_cdef_8", "av1", cts_dir / "cdef-8.ivf", "AV1 CDEF 8-bit"),
            ("AV1_fkf_8", "av1", cts_dir / "forward-key-frame-8.ivf", "AV1 Forward key frame 8-bit"),
            ("AV1_gm_8", "av1", cts_dir / "global-motion-8.ivf", "AV1 Global motion 8-bit"),
            ("AV1_lf_8", "av1", cts_dir / "loop-filter-8.ivf", "AV1 Loop filter 8-bit"),
            ("AV1_basic_10", "av1", cts_dir / "basic-10.ivf", "AV1 Basic 10-bit"),
            ("AV1_cdef_10", "av1", cts_dir / "cdef-10.ivf", "AV1 CDEF 10-bit"),
            ("AV1_allintra", "av1", cts_dir / "av1-1-b8-02-allintra.ivf", "AV1 All intra"),
            ("AV1_filmgrain", "av1", cts_dir / "av1-1-b8-23-film_grain-50.ivf", "AV1 Film grain"),
            ("AV1_176x144_basic_8", "av1", cts_video_dir / "av1-176x144-main-basic-8.ivf", "AV1 176x144 basic 8-bit"),
        ]
        for name, codec, path, desc in av1_tests:
            self.run_test(name, codec, str(path), desc)
        
        # VP9 Tests
        self.print_header("VP9 Decoder Tests")
        vp9_tests = [
            ("VP9_big_buck_bunny", "vp9", video_dir / "Big_Buck_Bunny_1080_10s_30MB.webm", "VP9 Big Buck Bunny 1080p"),
        ]
        for name, codec, path, desc in vp9_tests:
            self.run_test(name, codec, str(path), desc)
        
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
        description="Vulkan Video Decoder Test Runner",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    
    parser.add_argument("--video-dir", type=Path, required=True,
                        help="Directory containing test video files (REQUIRED)")
    parser.add_argument("--validate", "-v", action="store_true",
                        help="Enable Vulkan validation layers")
    parser.add_argument("--verbose", action="store_true",
                        help="Show detailed output")
    parser.add_argument("--codec", type=str, default="",
                        help="Only test specific codec (h264, h265, av1, vp9)")
    parser.add_argument("--local", action="store_true",
                        help="Run locally instead of on remote")
    parser.add_argument("--remote", type=str, default="127.0.0.1",
                        help="Remote hostname/IP (default: 127.0.0.1)")
    parser.add_argument("--remote-user", type=str, default="",
                        help="Remote username (default: current user)")
    parser.add_argument("--max-frames", type=int, default=30,
                        help="Maximum frames to decode per test (default: 30)")
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="Build directory (default: auto-detect)")
    parser.add_argument("--output-dir", type=Path, default=Path("/tmp/vulkan_decoder_tests"),
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
        max_frames=args.max_frames
    )
    
    # Check remote connectivity if not local
    if not config.run_local:
        runner = DecoderTestRunner(config)
        if not runner.check_dir_exists(str(args.video_dir)):
            print(f"{RED}Error: Video directory does not exist on remote: {args.video_dir}{NC}")
            print(f"Note: Checking on remote host {runner.get_ssh_target()}")
            return 1
    
    runner = DecoderTestRunner(config)
    success = runner.run_all_tests()
    
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
