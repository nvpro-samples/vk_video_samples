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
        # MSVC is a multi-config generator: the binary lands in demos/Release/, not demos/.
        # Probing both keeps single-config (Ninja/Make) and multi-config builds working.
        if sys.platform == "win32":
            cands = [config.build_dir / "vk_video_encoder" / "demos" / "Release" / "vk-video-enc-test.exe",
                     config.build_dir / "vk_video_encoder" / "demos" / "Debug" / "vk-video-enc-test.exe",
                     config.build_dir / "vk_video_encoder" / "demos" / "vk-video-enc-test.exe"]
        else:
            cands = [config.build_dir / "vk_video_encoder" / "demos" / "vk-video-enc-test"]
        self.encoder = next((c for c in cands if c.exists()), cands[0])

        self._encoder_help_output: Optional[str] = None
    
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
    
    def record_skip(self, name: str, message: str) -> None:
        """Record a skipped test with consistent reporting."""
        print(f"  {YELLOW}○{NC} {name} - {message}")
        self.skipped += 1
        self.results.append(TestResult(name, False, True, 0, message))

    def get_encoder_help_output(self) -> str:
        """Return cached encoder --help output."""
        if self._encoder_help_output is None:
            returncode, stdout, stderr = self.run_command([str(self.encoder), "--help"])
            self._encoder_help_output = (stdout or "") + (stderr or "")
            if returncode != 0 and self.config.verbose:
                print(f"  {YELLOW}Warning:{NC} failed to query encoder help output; AQ capability detection may be inaccurate.")
        return self._encoder_help_output

    def supports_aq_cli(self) -> bool:
        """Detect whether this encoder binary supports AQ CLI controls."""
        help_output = self.get_encoder_help_output()
        return ("--spatialAQStrength" in help_output and
                "--temporalAQStrength" in help_output)

    def run_test(self, name: str, codec: str, input_file: str, width: int, height: int,
                 bpp: int = 8, chroma: str = "420", extra_args: List[str] = None) -> None:
        """Run a single encoder test."""
        # Filter by codec if specified
        if self.config.filter_codec and self.config.filter_codec != codec:
            return
        
        # Check if file exists
        if not self.check_file_exists(input_file):
            self.record_skip(name, "File not found")
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

            aq_supported = self.supports_aq_cli()
            if not aq_supported:
                reason = "AQ CLI not supported by this encoder build"
                print(f"  {YELLOW}○{NC} Skipping AQ tests: {reason}")
                print("    Expected flags: --spatialAQStrength, --temporalAQStrength")

                for codec in ["h264", "h265", "av1"]:
                    if self.config.filter_codec and self.config.filter_codec != codec:
                        continue
                    print(f"\n  {CYAN}{codec.upper()} AQ Tests:{NC}")
                    for desc, _, _ in aq_configs:
                        self.record_skip(f"AQ_{codec}_{desc}", reason)
            else:
                aq_input = cts_video_dir / "720x480_420_8le.yuv"
                if not self.check_file_exists(str(aq_input)):
                    aq_input = cts_video_dir / "352x288_420_8le.yuv"
                    aq_width, aq_height = 352, 288
                else:
                    aq_width, aq_height = 720, 480

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


# ---------------------------------------------------------------------------
# Encode format matrix (Suite A of
# design/Blackwell_Encode_Format_Verification_Procedure_2026-08-15.md)
#
# 20 cells: 19 PASS + p010_badshift XFAIL(ok). Any other outcome is a failure.
#
# THE GATE IS CONTENT, NOT EXIT CODE. rc == 0 and a correct pix_fmt are both passed
# by a completely wrong picture - a staging copy that fills a quarter of each scanline,
# or a 10-bit path that saturates every sample to white, still produces a decodable
# stream of the right size and pixel format. Each cell therefore compares the decoded
# luma average against the same statistic measured on the source, within 2%.
#
# cell = (name, src_pixfmt, subsampling, planes, bpp, want_pixfmt, extra_args,
#         expect_src, expect_profile, xfail, codec)
# ---------------------------------------------------------------------------
FORMAT_CELLS = [
    # A1 - packed 4:4:4 (new on Blackwell)
    ("ayuv",           "vuya",        444, 1,  8, "yuv444p",     [], "", "", False, "hevc"),
    ("y410",           "xv30le",      444, 1, 10, "yuv444p10le", [], "", "", False, "hevc"),
    # A2 - 8-bit planar / semi-planar regression
    ("nv12",           "nv12",        420, 2,  8, "yuv420p",     [], "", "", False, "hevc"),
    ("nv24",           "nv24",        444, 2,  8, "yuv444p",     [], "", "", False, "hevc"),
    ("yuv444p",        "yuv444p",     444, 3,  8, "yuv444p",     [], "", "", False, "hevc"),
    # A3 - 4:2:2 semi-planar (new)
    ("nv16",           "nv16",        422, 2,  8, "yuv422p",     [], "", "", False, "hevc"),
    ("p210",           "p210le",      422, 2, 10, "yuv422p10le", [], "", "", False, "hevc"),
    # A4 - which encode-source format was chosen. A packed and a 2-plane 4:4:4 source
    # both yield a yuv444p bitstream with identical luma, so the content gate ALONE
    # cannot tell a working --preferPackedYcbcr from one that silently did nothing.
    ("p444_planar_8",  "nv24",        444, 2,  8, "yuv444p",     [],
     "planar/semi-planar", "", False, "hevc"),
    ("p444_packed_8",  "nv24",        444, 2,  8, "yuv444p",     ["--preferPackedYcbcr"],
     "AYUV", "", False, "hevc"),
    ("p444_planar_10", "p410le",      444, 2, 10, "yuv444p10le", [],
     "planar/semi-planar", "", False, "hevc"),
    ("p444_packed_10", "p410le",      444, 2, 10, "yuv444p10le", ["--preferPackedYcbcr"],
     "Y410", "", False, "hevc"),
    # A5 - H.264 coded profile. H.264 cannot code chroma_format_idc == 2 below High
    # 4:2:2, so the profile IDC has to change with the chroma format; a downgrade to
    # High would still produce a decodable stream. NOTE: profile 122 covers 4:2:2 at
    # BOTH 8 and 10 bit - there is no separate 10-bit 4:2:2 IDC, so h264_422_10 also
    # expects "High 4:2:2". For that cell pix_fmt is the load-bearing check.
    ("h264_420",       "nv12",        420, 2,  8, "yuv420p",     [], "", "High", False, "h264"),
    ("h264_422",       "nv16",        422, 2,  8, "yuv422p",     [], "", "High 4:2:2", False, "h264"),
    ("h264_444",       "nv24",        444, 2,  8, "yuv444p",     [], "", "High 4:4:4 Predictive", False, "h264"),
    ("h264_420_10",    "p010le",      420, 2, 10, "yuv420p10le", [], "", "High 10", False, "h264"),
    ("h264_422_10",    "p210le",      422, 2, 10, "yuv422p10le", [], "", "High 4:2:2", False, "h264"),
    # A6 - 10-bit sample alignment, probed from the data (DetectInputMsbShift):
    # right-aligned yuv420p10le needs shift 6, left-aligned p010le needs 0, and
    # neither caller says so.
    ("yuv420p10",      "yuv420p10le", 420, 3, 10, "yuv420p10le", [], "", "", False, "hevc"),
    ("p010_auto",      "p010le",      420, 2, 10, "yuv420p10le", [], "", "", False, "hevc"),
    ("p010_s0",        "p010le",      420, 2, 10, "yuv420p10le", ["--msbShift", "0"], "", "", False, "hevc"),
    # NEGATIVE CONTROL: force the wrong shift on left-aligned data; it must saturate
    # and FAIL the content gate. This is the only cell proving the gate can still
    # bite - if it ever XPASSes, every PASS above is worthless.
    ("p010_badshift",  "p010le",      420, 2, 10, "yuv420p10le", ["--msbShift", "6"], "", "", True, "hevc"),
]


class FormatMatrixRunner:
    """Suite A - encode format matrix. Identical cells on Linux and Windows."""

    WIDTH, HEIGHT = 352, 288

    def __init__(self, config: "TestConfig", encoder: Path, device_uuid: str = ""):
        self.config = config
        self.encoder = encoder
        self.device_uuid = device_uuid
        self.results: List[TestResult] = []
        self.ffmpeg, self.ffprobe = self._resolve_ffmpeg()

    def _resolve_ffmpeg(self) -> Tuple[str, str]:
        """ffmpeg/ffprobe are on PATH on Linux. On Windows they are NOT - they ship
        beside the runtime DLLs, and that directory must also be ON PATH or the
        encoder cannot resolve avutil-*.dll."""
        if sys.platform != "win32":
            return "ffmpeg", "ffprobe"
        root = Path(__file__).resolve().parent.parent
        bundled = root / "vk_video_decoder" / "bin" / "libs" / "ffmpeg" / "win64" / "bin"
        if (bundled / "ffmpeg.exe").exists():
            os.environ["PATH"] = str(bundled) + os.pathsep + os.environ.get("PATH", "")
            return str(bundled / "ffmpeg.exe"), str(bundled / "ffprobe.exe")
        return "ffmpeg", "ffprobe"

    def _yavg(self, args: List[str]) -> Optional[float]:
        """Mean luma via ffmpeg signalstats.

        Deliberately NOT ffprobe's movie= filter: movie= needs the drive-letter colon
        escaped on Windows, and that escaping does not survive shell quoting - it
        reports "Failed to avformat_open_input 'C'" and yields an empty value on EVERY
        cell, which reads as total failure and is pure harness.

        Takes the FIRST YAVG line only: metadata=mode=print emits one per frame, and
        keeping them all turns the value into a multi-line string that silently fails
        every downstream comparison.
        """
        cmd = [self.ffmpeg, "-v", "error"] + args + [
            "-vf", "signalstats,metadata=mode=print:file=-", "-f", "null", "-"]
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=180).stdout
        except (subprocess.SubprocessError, OSError):
            return None
        for line in out.splitlines():
            if "YAVG" in line:
                try:
                    return float(line.rsplit("=", 1)[1].strip())
                except (ValueError, IndexError):
                    return None
        return None

    def _probe(self, path: Path, entry: str) -> str:
        cmd = [self.ffprobe, "-v", "error", "-select_streams", "v:0",
               "-show_entries", f"stream={entry}", "-of", "csv=p=0", str(path)]
        try:
            return subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=120).stdout.strip()
        except (subprocess.SubprocessError, OSError):
            return ""

    def run_cell(self, cell) -> TestResult:
        (name, src_fmt, sub, planes, bpp, want, extra,
         expect_src, expect_profile, xfail, codec) = cell
        outdir = Path(self.config.output_dir) / "formats"
        outdir.mkdir(parents=True, exist_ok=True)
        raw, out = outdir / f"in_{name}.raw", outdir / f"out_{name}.265"
        log = outdir / f"log_{name}.txt"
        for f in (raw, out):
            f.unlink(missing_ok=True)
        t0 = time.time()

        gen = [self.ffmpeg, "-v", "error", "-f", "lavfi", "-i",
               f"testsrc=size={self.WIDTH}x{self.HEIGHT}:rate=1:duration=4",
               "-pix_fmt", src_fmt, "-f", "rawvideo", str(raw), "-y"]
        subprocess.run(gen, capture_output=True, timeout=180)
        if not raw.exists() or raw.stat().st_size == 0:
            return TestResult(name, False, False, time.time() - t0, "no source generated")

        srcavg = self._yavg(["-f", "rawvideo", "-pix_fmt", src_fmt,
                             "-s", f"{self.WIDTH}x{self.HEIGHT}", "-i", str(raw),
                             "-frames:v", "1"])

        cmd = [str(self.encoder), "-i", str(raw),
               "--inputWidth", str(self.WIDTH), "--inputHeight", str(self.HEIGHT),
               "--inputChromaSubsampling", str(sub), "--inputNumPlanes", str(planes),
               "--inputBpp", str(bpp), "--verbose", "-c", codec,
               "--numFrames", "2", "-o", str(out)] + list(extra)
        # These formats are Blackwell-only: a run that lands on a non-Blackwell GPU
        # fabricates "not supported". The bare 36-char UUID is required - parseUuid
        # rejects the G/P/U of nvidia-smi's "GPU-" prefix.
        if self.device_uuid:
            cmd += ["--deviceUuid", self.device_uuid]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            rc, blob = proc.returncode, (proc.stdout or "") + (proc.stderr or "")
        except subprocess.SubprocessError as exc:
            rc, blob = 1, str(exc)
        log.write_text(blob, errors="replace")

        size = out.stat().st_size if out.exists() else 0
        got = self._probe(out, "pix_fmt") if size else "none"
        encavg = self._yavg(["-i", str(out)]) if size else None

        reasons = []
        if rc != 0:
            reasons.append(f"rc={rc}")
        if size == 0:
            reasons.append("empty bitstream")
        if got != want:
            reasons.append(f"pix_fmt={got or 'none'} want {want}")
        if srcavg and encavg:
            ratio = encavg / srcavg
            if not 0.98 < ratio < 1.02:
                reasons.append(f"content {encavg:.3f} vs src {srcavg:.3f}")
        else:
            reasons.append("no luma statistic")
        if expect_src and f"({expect_src})" not in blob:
            reasons.append(f"encode-source != {expect_src}")
        if expect_profile:
            prof = self._probe(out, "profile") if size else ""
            if prof != expect_profile:
                reasons.append(f"profile={prof or 'none'} want {expect_profile}")

        failed = bool(reasons)
        dur = time.time() - t0
        if xfail:
            # An XFAIL that fails for the WRONG reason proves nothing, so record why.
            if failed:
                return TestResult(name, True, False, dur, "XFAIL(ok): " + "; ".join(reasons))
            return TestResult(name, False, False, dur, "XPASS(gate-broken!)")
        return TestResult(name, not failed, False, dur, "; ".join(reasons))

    def run_all(self) -> bool:
        print(f"\n{BOLD}Encode format matrix - {len(FORMAT_CELLS)} cells{NC}")
        print(f"  encoder: {self.encoder}")
        print(f"  ffmpeg : {self.ffmpeg}")
        ok = True
        for cell in FORMAT_CELLS:
            res = self.run_cell(cell)
            self.results.append(res)
            tag = f"{GREEN}PASS{NC}" if res.passed else f"{RED}FAIL{NC}"
            if res.passed and res.message.startswith("XFAIL"):
                tag = f"{YELLOW}XFAIL{NC}"
            print(f"  {res.name:<16} {tag}  {res.message}")
            ok &= res.passed
        n_pass = sum(1 for r in self.results if r.passed and not r.message.startswith("XFAIL"))
        n_xfail = sum(1 for r in self.results if r.passed and r.message.startswith("XFAIL"))
        n_fail = sum(1 for r in self.results if not r.passed)
        print(f"\n  TOTAL: PASS={n_pass} XFAIL={n_xfail} FAIL={n_fail}"
              f"   (expected 19 / 1 / 0)")
        return ok


def main():
    parser = argparse.ArgumentParser(
        description="Vulkan Video Encoder Test Runner",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    
    parser.add_argument("--video-dir", type=Path, default=None,
                        help="Directory containing test video files (required unless --formats)")
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
    parser.add_argument("--formats", action="store_true",
                        help="Run the 20-cell encode FORMAT matrix (packed 4:4:4, 4:2:2, "
                             "10-bit alignment, H.264 profiles). Content-gated; needs no --video-dir.")
    parser.add_argument("--device-uuid", type=str, default="",
                        help="Pin the GPU by UUID. BARE 36-char form, no 'GPU-' prefix. "
                             "Required for the format matrix: those formats are Blackwell-only, "
                             "so landing on an older GPU fabricates 'not supported'.")
    
    args = parser.parse_args()
    
    # Auto-detect build directory
    if args.build_dir is None:
        script_dir = Path(__file__).parent
        project_root = script_dir.parent
        args.build_dir = project_root / "build"
    
    # Validate video directory (the format matrix makes its own sources)
    if not args.formats and args.video_dir is None:
        print(f"{RED}Error: --video-dir is required (or use --formats){NC}")
        return 1
    if args.formats and args.video_dir is None:
        args.video_dir = args.output_dir
    if not args.video_dir.exists() and not args.formats:
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
    
    # The format matrix generates its own sources with ffmpeg and always runs LOCALLY
    # against the installed/loaded driver, so it must short-circuit before the remote
    # connectivity probe below (which would otherwise try to ssh to 127.0.0.1).
    if args.formats:
        fm = FormatMatrixRunner(config, EncoderTestRunner(config).encoder, args.device_uuid)
        if not fm.encoder.exists():
            print(f"{RED}Error: encoder not found: {fm.encoder}{NC}")
            return 1
        if not args.device_uuid:
            print(f"{YELLOW}Warning:{NC} no --device-uuid; the matrix may run on the wrong GPU "
                  f"and report 'not supported' for Blackwell-only formats.")
        return 0 if fm.run_all() else 1

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
