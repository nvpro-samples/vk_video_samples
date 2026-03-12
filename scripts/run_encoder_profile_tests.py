#!/usr/bin/env python3
"""
Vulkan Video Encoder Profile Test Runner

Discovers and runs all JSON encoder profiles against vk-video-enc-test.
Profiles are organized under vk_video_encoder/json_config/:
  - Generic profiles: json_config/*.json (e.g. encoder_config.default.json)
  - IHV-specific profiles: json_config/<ihv>/*.json (e.g. nvidia/high_quality_p4.json)

Input YUV files follow the naming convention: {W}x{H}_{subsampling}_{bitdepth}.yuv
  - e.g. 720x480_420_8le.yuv, 1920x1080_420_10le.yuv

To generate YUV test assets at all required resolutions and formats:
  ThreadedRenderingVk_Standalone/scripts/generate_encoder_yuv.sh --all --output-dir /data/misc/VideoClips/ycbcr

Usage:
    python run_encoder_profile_tests.py --video-dir /data/misc/VideoClips/ycbcr [OPTIONS]

    # Run only NVIDIA profiles
    python run_encoder_profile_tests.py --video-dir /data/misc/VideoClips/ycbcr --profile-filter nvidia

    # Run only a specific profile
    python run_encoder_profile_tests.py --video-dir /data/misc/VideoClips/ycbcr --profile-filter nvidia/high_quality_p4
"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple


def supports_color() -> bool:
    """Check whether ANSI colors are supported."""
    if sys.platform == "win32":
        return os.environ.get("WT_SESSION") is not None or os.environ.get("TERM_PROGRAM") == "vscode"
    return hasattr(sys.stdout, "isatty") and sys.stdout.isatty()


if supports_color():
    RED = "\033[0;31m"
    GREEN = "\033[0;32m"
    YELLOW = "\033[1;33m"
    CYAN = "\033[0;36m"
    BOLD = "\033[1m"
    NC = "\033[0m"
else:
    RED = GREEN = YELLOW = CYAN = BOLD = NC = ""


@dataclass
class TestConfig:
    """Test configuration."""

    video_dir: Path
    profile_dir: Path
    build_dir: Path
    output_dir: Path
    remote_host: str = "127.0.0.1"
    remote_user: str = ""
    run_local: bool = False
    validate: bool = False
    verbose: bool = False
    filter_codec: str = ""
    profile_filter: str = ""
    max_frames: int = 30
    input_file: Optional[Path] = None
    input_width: int = 0
    input_height: int = 0
    input_bpp: int = 8
    input_chroma: str = "420"
    max_supported_quality_preset: int = 4


@dataclass
class TestResult:
    """Result of one profile test."""

    name: str
    passed: bool
    skipped: bool
    duration: float
    message: str = ""


class EncoderProfileTestRunner:
    """Runs profile-based encoder tests."""

    def __init__(self, config: TestConfig):
        self.config = config
        self.results: List[TestResult] = []
        self.passed = 0
        self.failed = 0
        self.skipped = 0

        if sys.platform == "win32":
            self.encoder = config.build_dir / "vk_video_encoder" / "demos" / "vk-video-enc-test.exe"
        else:
            self.encoder = config.build_dir / "vk_video_encoder" / "demos" / "vk-video-enc-test"

    def get_ssh_target(self) -> str:
        """Get SSH target."""
        user = self.config.remote_user or os.environ.get("USER", os.environ.get("USERNAME", "root"))
        return f"{user}@{self.config.remote_host}"

    def run_command(self, cmd: List[str], env: Optional[dict] = None) -> Tuple[int, str, str]:
        """Run command locally or on remote host."""
        if self.config.run_local:
            try:
                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=300,
                    env=env or os.environ,
                )
                return result.returncode, result.stdout, result.stderr
            except subprocess.TimeoutExpired:
                return -1, "", "Timeout"
            except Exception as exc:
                return -1, "", str(exc)

        ssh_target = self.get_ssh_target()
        cmd_str = " ".join(shlex.quote(arg) for arg in cmd)
        if env:
            env_prefix = " ".join(f"{k}={shlex.quote(v)}" for k, v in env.items() if k.startswith("VK_"))
            if env_prefix:
                cmd_str = f"{env_prefix} {cmd_str}"

        try:
            result = subprocess.run(
                ["ssh", ssh_target, cmd_str],
                capture_output=True,
                text=True,
                timeout=300,
            )
            return result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return -1, "", "Timeout"
        except Exception as exc:
            return -1, "", str(exc)

    def check_file_exists(self, path: str) -> bool:
        """Check file existence locally or remotely."""
        if self.config.run_local:
            return Path(path).is_file()
        result = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=5", self.get_ssh_target(), f"test -f '{path}'"],
            capture_output=True,
        )
        return result.returncode == 0

    def check_file_nonempty(self, path: str) -> bool:
        """Check whether file exists and is non-empty."""
        if self.config.run_local:
            p = Path(path)
            return p.is_file() and p.stat().st_size > 0
        result = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=5", self.get_ssh_target(), f"test -s '{path}'"],
            capture_output=True,
        )
        return result.returncode == 0

    def check_dir_exists(self, path: str) -> bool:
        """Check directory existence locally or remotely."""
        if self.config.run_local:
            return Path(path).is_dir()
        result = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=5", self.get_ssh_target(), f"test -d '{path}'"],
            capture_output=True,
        )
        return result.returncode == 0

    def check_executable_exists(self, path: str) -> bool:
        """Check if executable exists locally or remotely."""
        if self.config.run_local:
            p = Path(path)
            return p.is_file() and os.access(p, os.X_OK)
        result = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=5", self.get_ssh_target(), f"test -x '{path}'"],
            capture_output=True,
        )
        return result.returncode == 0

    def print_header(self, title: str) -> None:
        """Print section header."""
        print()
        print(f"{BOLD}{'=' * 50}{NC}")
        print(f"{BOLD}{title}{NC}")
        print(f"{BOLD}{'=' * 50}{NC}")

    @staticmethod
    def normalize_codec(codec_value: str) -> str:
        """Normalize codec aliases."""
        c = (codec_value or "").strip().lower()
        if c in ("h264", "264", "avc"):
            return "h264"
        if c in ("h265", "hevc", "265"):
            return "h265"
        if c == "av1":
            return "av1"
        return ""

    def discover_profiles(self) -> List[Path]:
        """Discover profile JSON files.

        Profile directory layout:
            json_config/                    ← --profile-dir points here
            ├── encoder_config.default.json ← generic profiles (no IHV prefix)
            ├── nvidia/                     ← IHV subdirectory
            │   ├── high_quality_p4.json
            │   └── ...
            └── intel/                      ← another IHV (future)

        --profile-filter matches against the relative path including IHV prefix,
        e.g. "nvidia/high_quality" or "low_latency".
        """
        if not self.config.profile_dir.is_dir():
            return []

        # Collect generic profiles (top-level *.json, excluding schema/example)
        profiles = []
        for p in sorted(self.config.profile_dir.glob("*.json")):
            if p.stem in ("encoder_config.schema", "encoder_config.example"):
                continue
            profiles.append(p)

        # Collect IHV subdirectory profiles (e.g. nvidia/*.json, intel/*.json)
        for subdir in sorted(self.config.profile_dir.iterdir()):
            if subdir.is_dir() and not subdir.name.startswith("."):
                profiles.extend(sorted(subdir.glob("*.json")))

        # Apply filter against relative path (e.g. "nvidia/high_quality" or "p4")
        if self.config.profile_filter:
            filt = self.config.profile_filter.lower()
            profiles = [
                p for p in profiles
                if filt in str(p.relative_to(self.config.profile_dir)).lower()
            ]
        return profiles

    # Regex: {W}x{H}_{subsampling}_{bitdepth}.yuv — same convention as generate_encoder_yuv.sh
    _YUV_FILENAME_RE = re.compile(
        r'^(\d+)x(\d+)_(\d{3})_(\d+)le(?:_packed)?\.yuv$'
    )

    @staticmethod
    def parse_yuv_filename(filename: str) -> Optional[Tuple[int, int, int, str]]:
        """Parse YUV filename into (width, height, bpp, chroma) or None.

        Examples:
            720x480_420_8le.yuv   → (720, 480, 8, "420")
            1920x1080_420_10le.yuv → (1920, 1080, 10, "420")
            352x288_444_10le_packed.yuv → (352, 288, 10, "444")
        """
        m = EncoderProfileTestRunner._YUV_FILENAME_RE.match(filename)
        if not m:
            return None
        w, h = int(m.group(1)), int(m.group(2))
        chroma = m.group(3)    # "420", "422", "444"
        bpp = int(m.group(4))  # 8, 10, 12
        return w, h, bpp, chroma

    def discover_yuv_files(self) -> List[Tuple[str, int, int, int, str]]:
        """Discover all YUV files in video_dir by parsing filenames.

        Returns list of (path, width, height, bpp, chroma) sorted by preference:
        largest resolution first, then 420 before 422/444, then 8-bit before 10-bit.
        """
        results = []
        # List .yuv files — locally or remotely
        if self.config.run_local:
            yuv_files = sorted(self.config.video_dir.glob("*.yuv"))
            for f in yuv_files:
                parsed = self.parse_yuv_filename(f.name)
                if parsed:
                    w, h, bpp, chroma = parsed
                    results.append((str(f), w, h, bpp, chroma))
        else:
            rc, stdout, _ = self.run_command(["ls", str(self.config.video_dir / "*.yuv")])
            if rc == 0:
                for line in stdout.strip().split("\n"):
                    line = line.strip()
                    if not line:
                        continue
                    parsed = self.parse_yuv_filename(Path(line).name)
                    if parsed:
                        w, h, bpp, chroma = parsed
                        results.append((line, w, h, bpp, chroma))

        # Sort: prefer larger resolution, then 420, then lower bpp
        chroma_order = {"420": 0, "422": 1, "444": 2}
        results.sort(key=lambda x: (-x[1] * x[2], chroma_order.get(x[4], 9), x[3]))
        return results

    def resolve_input_for_codec(self, codec: str) -> Optional[Tuple[str, int, int, int, str]]:
        """Resolve input YUV file and format for a codec.

        Priority:
        1. Explicit --input-file (user override)
        2. Auto-detect from video_dir by parsing filenames
           - Prefers 720x480 or larger, 420, 8-bit for broadest codec compatibility
        """
        if self.config.input_file is not None:
            return (
                str(self.config.input_file),
                self.config.input_width,
                self.config.input_height,
                self.config.input_bpp,
                self.config.input_chroma,
            )

        if not hasattr(self, '_discovered_yuv'):
            self._discovered_yuv = self.discover_yuv_files()

        if not self._discovered_yuv:
            return None

        # For now, pick the best match: prefer 420 8-bit at decent resolution
        # (broadest codec support — H.264 baseline only supports 420 8-bit)
        for path, w, h, bpp, chroma in self._discovered_yuv:
            if chroma == "420" and bpp == 8:
                return path, w, h, bpp, chroma

        # Fallback: first available file
        path, w, h, bpp, chroma = self._discovered_yuv[0]
        return path, w, h, bpp, chroma

    # All codecs the encoder supports
    ALL_CODECS = ["h264", "h265", "av1"]

    def run_profile_test(self, profile_path: Path) -> None:
        """Run one profile against all target codecs.

        CLI args override JSON values, so we use the JSON profile as a base config
        and override -c (codec) from the command line for each target codec.
        """
        # Display name includes IHV prefix: "nvidia/high_quality_p4" or "encoder_config.default"
        try:
            rel = profile_path.relative_to(self.config.profile_dir)
            profile_name = str(rel.with_suffix(""))
        except ValueError:
            profile_name = profile_path.stem

        try:
            with profile_path.open("r", encoding="utf-8") as fh:
                profile_data = json.load(fh)
        except Exception as exc:
            print(f"  {RED}✗{NC} {profile_name} - Invalid JSON")
            self.failed += 1
            self.results.append(TestResult(profile_name, False, False, 0.0, f"Invalid JSON: {exc}"))
            return

        quality_preset = profile_data.get("qualityPreset")
        if quality_preset is not None:
            try:
                quality_preset = int(quality_preset)
            except (TypeError, ValueError):
                quality_preset = None

        if quality_preset is not None and quality_preset > self.config.max_supported_quality_preset:
            print(
                f"  {YELLOW}○{NC} {profile_name} - Unsupported qualityPreset={quality_preset} "
                f"(max supported: {self.config.max_supported_quality_preset})"
            )
            self.skipped += 1
            self.results.append(
                TestResult(
                    profile_name,
                    False,
                    True,
                    0.0,
                    f"Unsupported qualityPreset={quality_preset} (max={self.config.max_supported_quality_preset})",
                )
            )
            return

        # Determine which codecs to run: --codec filters to one, otherwise all 3
        if self.config.filter_codec:
            target_codecs = [self.config.filter_codec]
        else:
            target_codecs = self.ALL_CODECS

        for codec in target_codecs:
            self._run_profile_for_codec(profile_path, profile_name, profile_data, codec)

    def _run_profile_for_codec(self, profile_path: Path, profile_name: str,
                                profile_data: dict, codec: str) -> None:
        """Run one profile + codec combination."""
        test_name = f"{profile_name}/{codec}"

        input_info = self.resolve_input_for_codec(codec)
        if input_info is None:
            print(f"  {YELLOW}○{NC} {test_name} - No input YUV found")
            self.skipped += 1
            self.results.append(TestResult(test_name, False, True, 0.0, "No input YUV found"))
            return

        input_file, width, height, bpp, chroma = input_info
        ext_map = {"h264": ".264", "h265": ".265", "av1": ".ivf"}
        codec_arg_map = {"h264": "avc", "h265": "hevc", "av1": "av1"}
        codec_arg = codec_arg_map.get(codec, codec)

        # Build descriptive output filename:
        #   {input_base}_{codec}_{profile_flat}{ext}
        # e.g. 1920x1080_420_8le_h265_nvidia_high_quality_p4.265
        input_base = Path(input_file).stem
        profile_flat = profile_name.replace("/", "_").replace("\\", "_")
        ext = ext_map.get(codec, ".bin")
        output_file = self.config.output_dir / f"{input_base}_{codec}_{profile_flat}{ext}"

        cmd = [
            str(self.encoder),
            "--encoderConfig",
            str(profile_path),
            "-c",
            codec_arg,
            "-i",
            input_file,
            "--inputWidth",
            str(width),
            "--inputHeight",
            str(height),
            "--inputChromaSubsampling",
            chroma,
            "--numFrames",
            str(self.config.max_frames),
            "-o",
            str(output_file),
        ]
        if bpp != 8:
            cmd.extend(["--inputBpp", str(bpp)])

        env = os.environ.copy()
        if self.config.validate:
            env["VK_LOADER_LAYERS_ENABLE"] = "*validation"
            env["VK_VALIDATION_VALIDATE_SYNC"] = "true"

        if self.config.verbose:
            print(f"\n  {CYAN}Profile: {test_name}{NC}")
            print(f"  {CYAN}Input:   {input_base} ({width}x{height}, {chroma}, {bpp}-bit){NC}")
            print(f"  {CYAN}Output:  {output_file.name}{NC}")
            print(f"  {CYAN}Command: {' '.join(cmd)}{NC}")

        start_time = time.time()
        returncode, stdout, stderr = self.run_command(cmd, env)
        duration = time.time() - start_time
        output = stdout + stderr

        if returncode == 0:
            if "validation error" in output.lower():
                print(f"  {RED}✗{NC} {test_name} - Validation errors ({duration:.2f}s)")
                self.failed += 1
                self.results.append(TestResult(test_name, False, False, duration, "Validation errors"))
            elif not self.check_file_nonempty(str(output_file)):
                print(f"  {RED}✗{NC} {test_name} - No output bitstream ({duration:.2f}s)")
                self.failed += 1
                self.results.append(TestResult(test_name, False, False, duration, "Output bitstream missing"))
            else:
                n = self.config.max_frames
                per_frame_ms = (duration / n * 1000) if n > 0 else 0
                enc_fps = n / duration if duration > 0 else 0
                print(f"  {GREEN}✓{NC} {test_name} ({duration:.2f}s, "
                      f"{per_frame_ms:.1f} ms/frame, {enc_fps:.1f} enc-fps)")
                self.passed += 1
                self.results.append(TestResult(test_name, True, False, duration))
        else:
            print(f"  {RED}✗{NC} {test_name} - Failed ({duration:.2f}s)")
            self.failed += 1
            message = stderr.strip()[:300] if stderr.strip() else output.strip()[:300]
            self.results.append(TestResult(test_name, False, False, duration, message or "Unknown error"))
            if self.config.verbose and message:
                print(f"    {message}")

    def run_all_tests(self) -> bool:
        """Run all profile tests."""
        self.print_header("Vulkan Video Encoder JSON Profile Tests")
        if not self.config.run_local:
            print(f"Target Remote: {CYAN}{self.get_ssh_target()}{NC}")
        print(f"Encoder: {self.encoder}")
        print(f"Video Dir: {self.config.video_dir}")
        print(f"Profile Dir: {self.config.profile_dir}")
        print(f"Output Dir: {self.config.output_dir}")
        print(f"Max Frames: {self.config.max_frames}")
        print(f"Validation: {GREEN}Enabled{NC}" if self.config.validate else "Validation: Disabled")
        print(f"Max Supported qualityPreset: {self.config.max_supported_quality_preset}")
        if self.config.filter_codec:
            print(f"Codec Filter: {self.config.filter_codec}")
        if self.config.profile_filter:
            print(f"Profile Filter: {self.config.profile_filter}")

        if not self.check_executable_exists(str(self.encoder)):
            print(f"{RED}Error: Encoder not found at {self.encoder}{NC}")
            print("Please build the project first: cd build && cmake .. && make")
            return False

        if not self.check_dir_exists(str(self.config.video_dir)):
            print(f"{RED}Error: Video directory does not exist: {self.config.video_dir}{NC}")
            if not self.config.run_local:
                print(f"Note: checked on remote host {self.get_ssh_target()}")
            return False

        profiles = self.discover_profiles()
        if not profiles:
            print(f"{RED}Error: No JSON profiles found in {self.config.profile_dir}{NC}")
            return False

        if self.config.run_local:
            self.config.output_dir.mkdir(parents=True, exist_ok=True)
        else:
            rc, _, err = self.run_command(["mkdir", "-p", str(self.config.output_dir)])
            if rc != 0:
                print(f"{RED}Error: Could not create output dir on remote: {err}{NC}")
                return False

        self.print_header("Running Profiles")
        for profile in profiles:
            self.run_profile_test(profile)

        self.print_header("Test Summary")
        total = self.passed + self.failed + self.skipped
        print()
        print(f"Total Profiles: {total}")
        print(f"  {GREEN}✓ Passed:{NC}    {self.passed}")
        print(f"  {RED}✗ Failed:{NC}    {self.failed}")
        print(f"  {YELLOW}○ Skipped:{NC}   {self.skipped}")
        print()

        passed_results = [r for r in self.results if r.passed and r.duration > 0]
        if passed_results:
            n = self.config.max_frames
            self.print_header("Encode Timing Report")
            print(f"{'Profile':<50} {'Total(s)':>9} {'ms/frame':>9} {'enc-fps':>9}")
            print("-" * 80)
            for r in passed_results:
                per_frame = (r.duration / n * 1000) if n > 0 else 0
                fps = n / r.duration if r.duration > 0 else 0
                print(f"{r.name:<50} {r.duration:>9.2f} {per_frame:>9.1f} {fps:>9.1f}")
            print()

        if self.failed == 0 and self.passed > 0:
            print(f"{GREEN}{BOLD}All profile tests passed!{NC}")
            return True

        if self.failed > 0:
            print(f"{RED}{BOLD}Some profile tests failed.{NC}")
        else:
            print(f"{YELLOW}{BOLD}All profiles skipped (no input or unsupported).{NC}")
        return False


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Vulkan Video Encoder NVIDIA JSON profile test runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--video-dir",
        type=Path,
        required=True,
        help="Directory containing YUV test videos (required)",
    )
    parser.add_argument(
        "--profile-dir",
        type=Path,
        default=None,
        help="JSON config base directory (default: vk_video_encoder/json_config). "
             "Scans top-level for generic profiles and IHV subdirs (nvidia/, intel/, etc.)",
    )
    parser.add_argument("--validate", "-v", action="store_true", help="Enable Vulkan validation layers")
    parser.add_argument("--verbose", action="store_true", help="Verbose output")
    parser.add_argument("--codec", type=str, default="", help="Only run specific codec profiles (h264, h265, av1)")
    parser.add_argument("--profile-filter", type=str, default="", help="Only run profiles containing this substring")
    parser.add_argument("--local", action="store_true", help="Run locally instead of over SSH")
    parser.add_argument("--remote", type=str, default="127.0.0.1", help="Remote host/IP")
    parser.add_argument("--remote-user", type=str, default="", help="Remote username")
    parser.add_argument("--max-frames", type=int, default=30, help="Frames to encode per profile")
    parser.add_argument("--build-dir", type=Path, default=None, help="Build directory (default: auto-detect)")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("/tmp/vulkan_encoder_profile_tests"),
        help="Output directory for profile bitstreams",
    )
    parser.add_argument("--input-file", type=Path, default=None, help="Custom input YUV file")
    parser.add_argument("--input-width", type=int, default=0, help="Custom input width (required with --input-file)")
    parser.add_argument("--input-height", type=int, default=0, help="Custom input height (required with --input-file)")
    parser.add_argument("--input-bpp", type=int, default=8, help="Custom input bit depth (default: 8)")
    parser.add_argument("--input-chroma", type=str, default="420", help="Custom chroma subsampling (default: 420)")
    parser.add_argument(
        "--max-supported-quality-preset",
        type=int,
        default=4,
        help="Maximum qualityPreset supported by current driver (default: 4)",
    )
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    if args.build_dir is None:
        args.build_dir = project_root / "build"
    if args.profile_dir is None:
        args.profile_dir = project_root / "vk_video_encoder" / "json_config"

    filter_codec = EncoderProfileTestRunner.normalize_codec(args.codec) if args.codec else ""
    if args.codec and not filter_codec:
        print(f"{RED}Error: Unsupported codec filter: {args.codec}{NC}")
        return 1

    if args.input_file is not None:
        if args.input_width <= 0 or args.input_height <= 0:
            print(f"{RED}Error: --input-width and --input-height are required with --input-file{NC}")
            return 1
        if args.input_bpp <= 0:
            print(f"{RED}Error: --input-bpp must be > 0{NC}")
            return 1

    if args.local:
        args.output_dir.mkdir(parents=True, exist_ok=True)

    config = TestConfig(
        video_dir=args.video_dir,
        profile_dir=args.profile_dir,
        build_dir=args.build_dir,
        output_dir=args.output_dir,
        remote_host=args.remote,
        remote_user=args.remote_user,
        run_local=args.local,
        validate=args.validate,
        verbose=args.verbose,
        filter_codec=filter_codec,
        profile_filter=args.profile_filter,
        max_frames=args.max_frames,
        input_file=args.input_file,
        input_width=args.input_width,
        input_height=args.input_height,
        input_bpp=args.input_bpp,
        input_chroma=args.input_chroma,
        max_supported_quality_preset=args.max_supported_quality_preset,
    )

    runner = EncoderProfileTestRunner(config)
    success = runner.run_all_tests()
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
