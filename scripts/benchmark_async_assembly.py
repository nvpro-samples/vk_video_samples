#!/usr/bin/env python3
"""
Async Assembly Performance Benchmark

Measures encoding throughput with async vs sync bitstream assembly
across all supported codecs, resolutions, bit depths, and chroma formats.
Works on both Linux and Windows (MINGW64).

Usage:
    python benchmark_async_assembly.py --video-dir /path/to/yuv [OPTIONS]

    # Linux VM (local):
    python scripts/benchmark_async_assembly.py --video-dir /data/misc/VideoClips/ycbcr --local

    # Remote (SSH):
    python scripts/benchmark_async_assembly.py --video-dir /data/misc/VideoClips/ycbcr --remote 192.168.122.216

    # Windows (MINGW64, local):
    python scripts/benchmark_async_assembly.py --video-dir /n/data/misc/VideoClips/ycbcr --local
"""

import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import List, Optional, Tuple


def supports_color() -> bool:
    if sys.platform == "win32":
        return os.environ.get("WT_SESSION") is not None or "MINGW" in os.environ.get("MSYSTEM", "")
    return hasattr(sys.stdout, 'isatty') and sys.stdout.isatty()


if supports_color():
    GREEN = '\033[0;32m'
    RED = '\033[0;31m'
    YELLOW = '\033[1;33m'
    CYAN = '\033[0;36m'
    BOLD = '\033[1m'
    NC = '\033[0m'
else:
    GREEN = RED = YELLOW = CYAN = BOLD = NC = ''


@dataclass
class BenchResult:
    tag: str
    codec: str
    width: int
    height: int
    bpp: int
    chroma: str
    frames: int
    async_ms: int = 0
    sync_ms: int = 0
    async_fps: float = 0.0
    sync_fps: float = 0.0
    speedup_pct: float = 0.0
    async_md5: str = ""
    sync_md5: str = ""
    md5_match: bool = False
    skipped: bool = False
    skip_reason: str = ""
    error: str = ""


class AsyncAssemblyBenchmark:
    def __init__(self, args):
        self.video_dir = Path(args.video_dir)
        self.build_dir = Path(args.build_dir) if args.build_dir else self._find_build_dir()
        self.output_dir = Path(args.output_dir)
        self.remote_host = args.remote
        self.remote_user = args.remote_user or os.environ.get("USER", os.environ.get("USERNAME", ""))
        self.run_local = args.local
        self.frames = args.frames
        self.iterations = args.iterations
        self.results: List[BenchResult] = []

        if hasattr(args, 'encoder') and args.encoder:
            self.encoder = Path(args.encoder)
        else:
            self.encoder = self._find_encoder()

    def _find_encoder(self) -> Path:
        is_win = sys.platform == "win32" or "MINGW" in os.environ.get("MSYSTEM", "")
        exe = "vk-video-enc-test.exe" if is_win else "vk-video-enc-test"
        candidates = [
            self.build_dir / "bin" / exe,                                    # installed (Windows)
            self.build_dir / exe,                                            # direct path
            self.build_dir / "vk_video_encoder" / "demos" / exe,            # Linux build tree
            self.build_dir / "vk_video_encoder" / "demos" / "Debug" / exe,  # MSVC Debug
            self.build_dir / "vk_video_encoder" / "demos" / "Release" / exe,
        ]
        for c in candidates:
            if c.exists():
                return c
        return self.build_dir / "vk_video_encoder" / "demos" / exe

    def _find_build_dir(self) -> Path:
        script_dir = Path(__file__).resolve().parent
        project_root = script_dir.parent
        for candidate in [project_root / "build", project_root / "build" / "install" / "Debug",
                          project_root / "build" / "install" / "Release"]:
            if (candidate / "vk_video_encoder").exists():
                return candidate
        return project_root / "build"

    def _ssh_target(self) -> str:
        return f"{self.remote_user}@{self.remote_host}"

    @staticmethod
    def _to_native_path(p: str) -> str:
        """Convert MINGW64 /x/... paths to X:\\... for cmd.exe/subprocess."""
        if len(p) >= 3 and p[0] == '/' and p[2] == '/' and p[1].isalpha():
            return p[1].upper() + ':' + p[2:].replace('/', '\\')
        return p

    def _run(self, cmd_str: str, timeout: int = 300, cwd: str = None) -> Tuple[int, str]:
        if self.run_local:
            try:
                r = subprocess.run(cmd_str, shell=True, capture_output=True, text=True,
                                   timeout=timeout, cwd=cwd)
                return r.returncode, r.stdout + r.stderr
            except subprocess.TimeoutExpired:
                return -1, "TIMEOUT"
        else:
            try:
                r = subprocess.run(["ssh", self._ssh_target(), cmd_str],
                                   capture_output=True, text=True, timeout=timeout)
                return r.returncode, r.stdout + r.stderr
            except subprocess.TimeoutExpired:
                return -1, "TIMEOUT"

    def _file_exists(self, path: str) -> bool:
        rc, _ = self._run(f"test -f '{path}' && echo YES || echo NO", timeout=10)
        return "YES" in _

    def _md5(self, path: str) -> str:
        if self.run_local:
            try:
                h = hashlib.md5()
                with open(path, "rb") as f:
                    for chunk in iter(lambda: f.read(1 << 20), b""):
                        h.update(chunk)
                return h.hexdigest()
            except Exception:
                return ""
        else:
            rc, out = self._run(f"md5sum '{path}' 2>/dev/null | cut -d' ' -f1", timeout=30)
            return out.strip() if rc == 0 else ""

    def _run_encode(self, input_file: str, output_file: str, codec: str,
                    width: int, height: int, bpp: int, chroma: str,
                    sync: bool = False) -> Tuple[int, float]:
        codec_arg = "hevc" if codec == "h265" else codec
        is_win = sys.platform == "win32" or "MINGW" in os.environ.get("MSYSTEM", "")

        if self.run_local and is_win:
            enc_native = self._to_native_path(str(self.encoder.resolve()))
            inp_native = self._to_native_path(str(Path(input_file).resolve()))
            out_native = self._to_native_path(str(Path(output_file).resolve()))
            cmd_list = [enc_native, "-i", inp_native, "-c", codec_arg,
                        "--inputWidth", str(width), "--inputHeight", str(height),
                        "--inputChromaSubsampling", chroma,
                        "--numFrames", str(self.frames), "-o", out_native]
            if bpp != 8:
                cmd_list.extend(["--inputBpp", str(bpp)])
            if sync:
                cmd_list.append("--syncAssembly")

            t0 = time.monotonic()
            try:
                r = subprocess.run(cmd_list, capture_output=True, text=True, timeout=600)
                output = r.stdout + r.stderr
                rc = r.returncode
            except subprocess.TimeoutExpired:
                rc, output = -1, "TIMEOUT"
            elapsed_ms = int((time.monotonic() - t0) * 1000)
        else:
            enc_str = str(self.encoder)
            cmd = (f'"{enc_str}" -i "{input_file}" -c {codec_arg} '
                   f"--inputWidth {width} --inputHeight {height} "
                   f"--inputChromaSubsampling {chroma} "
                   f'--numFrames {self.frames} -o "{output_file}"')
            if bpp != 8:
                cmd += f" --inputBpp {bpp}"
            if sync:
                cmd += " --syncAssembly"

            t0 = time.monotonic()
            rc, output = self._run(cmd, timeout=600)
            elapsed_ms = int((time.monotonic() - t0) * 1000)

        if rc != 0:
            if "not supported" in output.lower() or "FORMAT_NOT_SUPPORTED" in output or "-1000023003" in output:
                return -2, elapsed_ms  # HW unsupported
            return -1, elapsed_ms  # real failure

        if "Done processing" not in output:
            return -1, elapsed_ms

        return 0, elapsed_ms

    def bench_one(self, tag: str, codec: str, input_rel: str,
                  width: int, height: int, bpp: int = 8, chroma: str = "420") -> BenchResult:
        res = BenchResult(tag=tag, codec=codec, width=width, height=height,
                          bpp=bpp, chroma=chroma, frames=self.frames)

        input_file = str(self.video_dir / input_rel)
        if not self._file_exists(input_file):
            res.skipped = True
            res.skip_reason = "no file"
            return res

        async_times = []
        sync_times = []
        async_out = str(self.output_dir / f"{tag}_async.out")
        sync_out = str(self.output_dir / f"{tag}_sync.out")

        for i in range(self.iterations):
            rc, ms = self._run_encode(input_file, async_out, codec, width, height, bpp, chroma, sync=False)
            if rc == -2:
                res.skipped = True
                res.skip_reason = "HW unsupported"
                return res
            if rc != 0:
                res.error = f"async encode failed (iter {i})"
                return res
            async_times.append(ms)

            rc, ms = self._run_encode(input_file, sync_out, codec, width, height, bpp, chroma, sync=True)
            if rc != 0:
                res.error = f"sync encode failed (iter {i})"
                return res
            sync_times.append(ms)

        res.async_ms = sum(async_times) // len(async_times)
        res.sync_ms = sum(sync_times) // len(sync_times)
        res.async_fps = self.frames * 1000.0 / res.async_ms if res.async_ms > 0 else 0
        res.sync_fps = self.frames * 1000.0 / res.sync_ms if res.sync_ms > 0 else 0
        res.speedup_pct = ((res.sync_ms - res.async_ms) * 100.0 / res.sync_ms) if res.sync_ms > 0 else 0

        res.async_md5 = self._md5(async_out)
        res.sync_md5 = self._md5(sync_out)
        res.md5_match = (res.async_md5 == res.sync_md5 and res.async_md5 != "")

        return res

    def run_all(self):
        self._run(f"mkdir -p '{self.output_dir}'", timeout=10)

        tests = [
            # 420 8-bit
            ("h264_352x288_420_8",    "h264", "352x288_420_8le.yuv",    352,  288,  8, "420"),
            ("h264_1080p_420_8",      "h264", "1920x1080_420_8le.yuv",  1920, 1080, 8, "420"),
            ("h265_352x288_420_8",    "h265", "352x288_420_8le.yuv",    352,  288,  8, "420"),
            ("h265_1080p_420_8",      "h265", "1920x1080_420_8le.yuv",  1920, 1080, 8, "420"),
            ("av1_352x288_420_8",     "av1",  "352x288_420_8le.yuv",    352,  288,  8, "420"),
            ("av1_1080p_420_8",       "av1",  "1920x1080_420_8le.yuv",  1920, 1080, 8, "420"),
            # 420 10-bit
            ("h265_352x288_420_10",   "h265", "352x288_420_10le.yuv",   352,  288,  10, "420"),
            ("h265_1080p_420_10",     "h265", "1920x1080_420_10le.yuv", 1920, 1080, 10, "420"),
            ("av1_352x288_420_10",    "av1",  "352x288_420_10le.yuv",   352,  288,  10, "420"),
            ("av1_1080p_420_10",      "av1",  "1920x1080_420_10le.yuv", 1920, 1080, 10, "420"),
            # 444 8-bit
            ("h264_352x288_444_8",    "h264", "352x288_444_8le.yuv",    352,  288,  8, "444"),
            ("h265_352x288_444_8",    "h265", "352x288_444_8le.yuv",    352,  288,  8, "444"),
            # 444 10-bit
            ("h265_720p_444_10",      "h265", "1280x720_444_10le.yuv",  1280, 720,  10, "444"),
            # 422 10-bit
            ("h265_1080p_422_10",     "h265", "1920x1080_422_10le.yuv", 1920, 1080, 10, "422"),
        ]

        hostname = platform.node()
        print(f"\n{BOLD}{'=' * 80}{NC}")
        print(f"{BOLD} Async Assembly Performance Benchmark{NC}")
        print(f"{BOLD}{'=' * 80}{NC}")
        print(f"  Host:       {hostname}")
        print(f"  Encoder:    {self.encoder}")
        print(f"  Video dir:  {self.video_dir}")
        print(f"  Frames:     {self.frames}")
        print(f"  Iterations: {self.iterations} (averaged)")
        print(f"  Mode:       {'local' if self.run_local else f'remote ({self._ssh_target()})'}")
        print()

        print(f"  {'Test':<28s}  {'Async':>10s}  {'Sync':>10s}  {'Speedup':>8s}  {'MD5':>5s}")
        print(f"  {'-'*28}  {'-'*10}  {'-'*10}  {'-'*8}  {'-'*5}")

        for tag, codec, yuv, w, h, bpp, chroma in tests:
            res = self.bench_one(tag, codec, yuv, w, h, bpp, chroma)
            self.results.append(res)

            if res.skipped:
                print(f"  {YELLOW}{tag:<28s}  {'SKIP':>10s}  {res.skip_reason}{NC}")
            elif res.error:
                print(f"  {RED}{tag:<28s}  {'FAIL':>10s}  {res.error}{NC}")
            else:
                a = f"{res.async_fps:6.1f} fps"
                s = f"{res.sync_fps:6.1f} fps"
                sp = f"{res.speedup_pct:+5.1f}%"
                md5 = f"{GREEN}OK{NC}" if res.md5_match else f"{RED}!!{NC}"
                color = GREEN if res.speedup_pct > 1 else (RED if res.speedup_pct < -5 else NC)
                print(f"  {tag:<28s}  {a:>10s}  {s:>10s}  {color}{sp:>8s}{NC}  {md5}")

        # Summary
        passed = [r for r in self.results if not r.skipped and not r.error]
        skipped = [r for r in self.results if r.skipped]
        failed = [r for r in self.results if r.error]
        mismatched = [r for r in passed if not r.md5_match]

        print(f"\n{BOLD}{'=' * 80}{NC}")
        print(f"  Benchmarked: {len(passed)}   Skipped: {len(skipped)}   Failed: {len(failed)}   MD5 mismatch: {len(mismatched)}")

        if passed:
            hd_results = [r for r in passed if r.width >= 1280]
            if hd_results:
                avg_speedup = sum(r.speedup_pct for r in hd_results) / len(hd_results)
                print(f"  Avg speedup (HD+): {GREEN}{avg_speedup:+.1f}%{NC}")

        # Write JSON report
        report_path = self.output_dir / "benchmark_results.json"
        report = {
            "host": hostname,
            "platform": sys.platform,
            "frames": self.frames,
            "iterations": self.iterations,
            "results": [asdict(r) for r in self.results],
        }
        try:
            if self.run_local:
                self.output_dir.mkdir(parents=True, exist_ok=True)
                with open(report_path, "w") as f:
                    json.dump(report, f, indent=2)
                print(f"  JSON report: {report_path}")
            else:
                json_str = json.dumps(report, indent=2)
                self._run(f"cat > '{report_path}' << 'JSONEOF'\n{json_str}\nJSONEOF", timeout=10)
                print(f"  JSON report: {report_path}")
        except Exception as e:
            print(f"  (could not write JSON report: {e})")

        print()
        return len(failed) == 0 and len(mismatched) == 0


def main():
    parser = argparse.ArgumentParser(description="Async Assembly Performance Benchmark")
    parser.add_argument("--video-dir", type=str, required=True, help="Directory with YUV test files")
    parser.add_argument("--build-dir", type=str, default=None, help="Build directory (auto-detect)")
    parser.add_argument("--encoder", type=str, default=None, help="Direct path to vk-video-enc-test binary")
    parser.add_argument("--output-dir", type=str, default="/tmp/async_assembly_bench",
                        help="Output directory for artifacts")
    parser.add_argument("--frames", type=int, default=300, help="Frames per test (default: 300)")
    parser.add_argument("--iterations", type=int, default=3, help="Iterations to average (default: 3)")
    parser.add_argument("--local", action="store_true", help="Run locally")
    parser.add_argument("--remote", type=str, default="127.0.0.1", help="Remote host")
    parser.add_argument("--remote-user", type=str, default="", help="Remote SSH user")
    args = parser.parse_args()

    bench = AsyncAssemblyBenchmark(args)
    ok = bench.run_all()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
