#!/usr/bin/env python3
"""
Vulkan Video Samples - Complete Test Suite Runner

Runs decoder and encoder tests on remote host with validation layer support.
Works on both Linux and Windows.

Usage:
    python run_all_video_tests.py --video-dir /path/to/videos [OPTIONS]

Options:
    --video-dir PATH    Directory containing test video files (REQUIRED)
    --decoder           Run decoder tests only
    --encoder           Run encoder tests only
    --validate, -v      Enable Vulkan validation layers
    --aq                Include AQ (Adaptive Quantization) tests
    --verbose           Show detailed output
    --codec CODEC       Only test specific codec (h264, h265, av1, vp9)
    --local             Run locally instead of on remote
    --remote HOST       Remote hostname/IP (default: 127.0.0.1)
    --quick             Quick test mode (fewer frames)
"""

import argparse
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


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


def print_banner(video_dir: Path, ssh_target: str, run_local: bool, 
                 run_decoder: bool, run_encoder: bool, validate: bool, quick: bool):
    """Print the test suite banner."""
    print()
    print(f"{BOLD}╔══════════════════════════════════════════════════════════════════════╗{NC}")
    print(f"{BOLD}║        VULKAN VIDEO SAMPLES - COMPREHENSIVE TEST SUITE              ║{NC}")
    print(f"{BOLD}╚══════════════════════════════════════════════════════════════════════╝{NC}")
    print()
    print(f"  Date:       {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    if run_local:
        print(f"  Target:     {CYAN}localhost{NC}")
    else:
        print(f"  Target:     {CYAN}{ssh_target}{NC}")
    print(f"  Video Dir:  {video_dir}")
    tests = []
    if run_decoder:
        tests.append("Decoder")
    if run_encoder:
        tests.append("Encoder")
    print(f"  Tests:      {' '.join(tests)}")
    print(f"  Validation: {GREEN}Enabled{NC}" if validate else "  Validation: Disabled")
    if quick:
        print(f"  Mode:       {YELLOW}Quick{NC}")
    print()


def check_remote_connectivity(ssh_target: str) -> bool:
    """Check if we can connect to the remote host."""
    try:
        result = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=5", ssh_target, "echo OK"],
            capture_output=True,
            text=True,
            timeout=10
        )
        return result.returncode == 0
    except Exception:
        return False


def check_dir_exists(path: Path, run_local: bool, ssh_target: str) -> bool:
    """Check if directory exists (locally or on remote)."""
    if run_local:
        return path.is_dir()
    else:
        try:
            result = subprocess.run(
                ["ssh", "-o", "ConnectTimeout=5", ssh_target, f"test -d '{path}'"],
                capture_output=True,
                timeout=10
            )
            return result.returncode == 0
        except Exception:
            return False


def run_script(script_path: Path, args: list) -> int:
    """Run a Python script and return its exit code."""
    cmd = [sys.executable, str(script_path)] + args
    try:
        result = subprocess.run(cmd)
        return result.returncode
    except Exception as e:
        print(f"{RED}Error running {script_path.name}: {e}{NC}")
        return 1


def main():
    parser = argparse.ArgumentParser(
        description="Vulkan Video Samples - Complete Test Suite Runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    python run_all_video_tests.py --video-dir /data/videos
    python run_all_video_tests.py --video-dir /data/videos --validate
    python run_all_video_tests.py --video-dir /data/videos --encoder --aq
    python run_all_video_tests.py --video-dir /data/videos --remote 192.168.122.216
    python run_all_video_tests.py --video-dir /data/videos --quick --validate --local
"""
    )
    
    parser.add_argument("--video-dir", type=Path, required=True,
                        help="Directory containing test video files (REQUIRED)")
    parser.add_argument("--decoder", action="store_true",
                        help="Run decoder tests only")
    parser.add_argument("--encoder", action="store_true",
                        help="Run encoder tests only")
    parser.add_argument("--validate", "-v", action="store_true",
                        help="Enable Vulkan validation layers")
    parser.add_argument("--aq", action="store_true",
                        help="Include AQ (Adaptive Quantization) tests")
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
    parser.add_argument("--quick", action="store_true",
                        help="Quick test mode (fewer frames)")
    parser.add_argument("--max-frames", type=int, default=None,
                        help="Maximum frames per test (default: 30, or 5 in quick mode)")
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="Build directory (default: auto-detect)")
    
    args = parser.parse_args()
    
    # Determine what to run
    run_decoder = args.decoder or (not args.decoder and not args.encoder)
    run_encoder = args.encoder or (not args.decoder and not args.encoder)
    
    # Set max frames
    max_frames = args.max_frames
    if max_frames is None:
        max_frames = 5 if args.quick else 30
    
    # Get SSH target
    user = args.remote_user or os.environ.get("USER", os.environ.get("USERNAME", "root"))
    ssh_target = f"{user}@{args.remote}"
    
    # Script directory
    script_dir = Path(__file__).parent
    
    # Print banner
    print_banner(args.video_dir, ssh_target, args.local,
                 run_decoder, run_encoder, args.validate, args.quick)
    
    # Check remote connectivity if not local
    if not args.local:
        print("Checking remote connectivity... ", end="", flush=True)
        if check_remote_connectivity(ssh_target):
            print(f"{GREEN}OK{NC}")
        else:
            print(f"{RED}FAILED{NC}")
            print()
            print(f"{RED}Cannot connect to remote at {ssh_target}{NC}")
            print("Options:")
            print("  1. Check remote host is running and accessible")
            print("  2. Use --local to run locally")
            print("  3. Set --remote and --remote-user arguments")
            return 1
    else:
        print("Running locally...")
    
    # Check video directory
    if not check_dir_exists(args.video_dir, args.local, ssh_target):
        print(f"{RED}Error: Video directory does not exist: {args.video_dir}{NC}")
        if not args.local:
            print(f"Note: Checking on remote host {ssh_target}")
        return 1
    
    # Build common arguments
    common_args = [
        "--video-dir", str(args.video_dir),
        "--max-frames", str(max_frames),
    ]
    if args.validate:
        common_args.append("--validate")
    if args.verbose:
        common_args.append("--verbose")
    if args.codec:
        common_args.extend(["--codec", args.codec])
    if args.local:
        common_args.append("--local")
    else:
        common_args.extend(["--remote", args.remote])
        if args.remote_user:
            common_args.extend(["--remote-user", args.remote_user])
    if args.build_dir:
        common_args.extend(["--build-dir", str(args.build_dir)])
    
    start_time = time.time()
    decoder_result = 0
    encoder_result = 0
    
    # Run decoder tests
    if run_decoder:
        print()
        print(f"{BOLD}{'━' * 70}{NC}")
        print(f"{BOLD}                         DECODER TESTS                                {NC}")
        print(f"{BOLD}{'━' * 70}{NC}")
        
        decoder_script = script_dir / "run_decoder_tests.py"
        if decoder_script.exists():
            decoder_result = run_script(decoder_script, common_args)
            if decoder_result == 0:
                print(f"\n{GREEN}Decoder tests completed successfully{NC}")
            else:
                print(f"\n{RED}Decoder tests had failures{NC}")
        else:
            print(f"{RED}Error: Decoder test script not found: {decoder_script}{NC}")
            decoder_result = 1
    
    # Run encoder tests
    if run_encoder:
        print()
        print(f"{BOLD}{'━' * 70}{NC}")
        print(f"{BOLD}                         ENCODER TESTS                                {NC}")
        print(f"{BOLD}{'━' * 70}{NC}")
        
        encoder_script = script_dir / "run_encoder_tests.py"
        encoder_args = common_args.copy()
        if args.aq:
            encoder_args.append("--aq")
        
        if encoder_script.exists():
            encoder_result = run_script(encoder_script, encoder_args)
            if encoder_result == 0:
                print(f"\n{GREEN}Encoder tests completed successfully{NC}")
            else:
                print(f"\n{RED}Encoder tests had failures{NC}")
        else:
            print(f"{RED}Error: Encoder test script not found: {encoder_script}{NC}")
            encoder_result = 1
    
    total_duration = int(time.time() - start_time)
    
    # Final summary
    print()
    print(f"{BOLD}╔══════════════════════════════════════════════════════════════════════╗{NC}")
    print(f"{BOLD}║                         FINAL SUMMARY                                 ║{NC}")
    print(f"{BOLD}╚══════════════════════════════════════════════════════════════════════╝{NC}")
    print()
    print(f"  Total Duration: {total_duration}s")
    print()
    
    if run_decoder:
        if decoder_result == 0:
            print(f"  Decoder Tests: {GREEN}✓ PASSED{NC}")
        else:
            print(f"  Decoder Tests: {RED}✗ FAILED{NC}")
    
    if run_encoder:
        if encoder_result == 0:
            print(f"  Encoder Tests: {GREEN}✓ PASSED{NC}")
        else:
            print(f"  Encoder Tests: {RED}✗ FAILED{NC}")
    
    print()
    
    # Exit with error if any tests failed
    if decoder_result != 0 or encoder_result != 0:
        print(f"{RED}{BOLD}Some tests failed. See above for details.{NC}")
        return 1
    else:
        print(f"{GREEN}{BOLD}All tests passed!{NC}")
        return 0


if __name__ == "__main__":
    sys.exit(main())
