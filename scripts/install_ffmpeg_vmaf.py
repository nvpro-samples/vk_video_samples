#!/usr/bin/env python3
"""
FFmpeg + VMAF Installation Script
=================================

Installs FFmpeg with PSNR and VMAF support for quality benchmarking.

This script will:
1. Check if FFmpeg is already installed with VMAF support
2. If not, try to install via package manager
3. If VMAF not available in packages, build libvmaf from source
4. Build FFmpeg from source with libvmaf support

Supported platforms:
- Ubuntu/Debian (apt)
- RHEL/CentOS/Fedora (dnf/yum)

Usage:
    python3 install_ffmpeg_vmaf.py [--prefix /usr/local] [--jobs 4]

Options:
    --prefix    Installation prefix (default: /usr/local)
    --jobs      Number of parallel build jobs (default: number of CPUs)
    --force     Force rebuild even if FFmpeg with VMAF is already installed
    --skip-vmaf Skip VMAF installation (PSNR only)

Author: NVIDIA Corporation
Date: January 2026
"""

import argparse
import subprocess
import sys
import os
import shutil
import tempfile
from pathlib import Path
import multiprocessing


def run_cmd(cmd, check=True, capture_output=False, env=None, cwd=None):
    """Run a command and return the result."""
    print(f"  $ {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    
    if isinstance(cmd, str):
        result = subprocess.run(cmd, shell=True, check=check, 
                               capture_output=capture_output, text=True, 
                               env=env, cwd=cwd)
    else:
        result = subprocess.run(cmd, check=check, capture_output=capture_output, 
                               text=True, env=env, cwd=cwd)
    return result


def check_command_exists(cmd):
    """Check if a command exists in PATH."""
    return shutil.which(cmd) is not None


def check_ffmpeg_has_vmaf():
    """Check if FFmpeg is installed and has VMAF support."""
    if not check_command_exists("ffmpeg"):
        return False, "FFmpeg not installed"
    
    try:
        result = subprocess.run(
            ["ffmpeg", "-filters"],
            capture_output=True, text=True, timeout=10
        )
        if "libvmaf" in result.stdout or "libvmaf" in result.stderr:
            return True, "FFmpeg with VMAF support is installed"
        else:
            return False, "FFmpeg installed but without VMAF support"
    except Exception as e:
        return False, f"Error checking FFmpeg: {e}"


def check_ffmpeg_has_psnr():
    """Check if FFmpeg has PSNR support."""
    if not check_command_exists("ffmpeg"):
        return False
    
    try:
        result = subprocess.run(
            ["ffmpeg", "-filters"],
            capture_output=True, text=True, timeout=10
        )
        return "psnr" in result.stdout or "psnr" in result.stderr
    except:
        return False


def detect_package_manager():
    """Detect the system's package manager."""
    if check_command_exists("apt-get"):
        return "apt"
    elif check_command_exists("dnf"):
        return "dnf"
    elif check_command_exists("yum"):
        return "yum"
    elif check_command_exists("pacman"):
        return "pacman"
    elif check_command_exists("brew"):
        return "brew"
    return None


def install_build_dependencies(pkg_manager):
    """Install build dependencies for FFmpeg and libvmaf."""
    print("\n📦 Installing build dependencies...")
    
    if pkg_manager == "apt":
        deps = [
            "build-essential", "cmake", "git", "nasm", "yasm",
            "pkg-config", "meson", "ninja-build",
            "libx264-dev", "libx265-dev", "libvpx-dev",
            "libfdk-aac-dev", "libmp3lame-dev", "libopus-dev",
            "libass-dev", "libfreetype6-dev", "libgnutls28-dev",
            "libsdl2-dev", "libtool", "libva-dev", "libvdpau-dev",
            "libvorbis-dev", "libxcb1-dev", "libxcb-shm0-dev",
            "libxcb-xfixes0-dev", "texinfo", "zlib1g-dev",
            "python3-pip", "python3-setuptools", "python3-wheel",
            "doxygen"
        ]
        run_cmd(["sudo", "apt-get", "update"])
        run_cmd(["sudo", "apt-get", "install", "-y"] + deps, check=False)
        
    elif pkg_manager in ["dnf", "yum"]:
        deps = [
            "gcc", "gcc-c++", "cmake", "git", "nasm", "yasm",
            "pkgconfig", "meson", "ninja-build",
            "x264-devel", "x265-devel", "libvpx-devel",
            "fdk-aac-devel", "lame-devel", "opus-devel",
            "libass-devel", "freetype-devel", "gnutls-devel",
            "SDL2-devel", "libtool", "libva-devel", "libvdpau-devel",
            "libvorbis-devel", "libxcb-devel",
            "texinfo", "zlib-devel",
            "python3-pip", "python3-setuptools",
            "doxygen"
        ]
        run_cmd(["sudo", pkg_manager, "install", "-y"] + deps, check=False)
        
    elif pkg_manager == "brew":
        deps = [
            "cmake", "git", "nasm", "yasm", "pkg-config", "meson", "ninja",
            "x264", "x265", "libvpx", "fdk-aac", "lame", "opus",
            "libass", "freetype", "gnutls", "sdl2", "libtool",
            "libvorbis", "texinfo", "doxygen"
        ]
        for dep in deps:
            run_cmd(["brew", "install", dep], check=False)
    
    print("  ✓ Build dependencies installed")


def build_libvmaf(prefix, jobs):
    """Build and install libvmaf from source."""
    print("\n🔨 Building libvmaf from source...")
    
    vmaf_version = "3.0.0"
    vmaf_url = f"https://github.com/Netflix/vmaf/archive/refs/tags/v{vmaf_version}.tar.gz"
    
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        
        # Download libvmaf
        print(f"  Downloading libvmaf v{vmaf_version}...")
        tarball = tmpdir / f"vmaf-{vmaf_version}.tar.gz"
        run_cmd(["wget", "-O", str(tarball), vmaf_url])
        
        # Extract
        print("  Extracting...")
        run_cmd(["tar", "-xzf", str(tarball), "-C", str(tmpdir)])
        
        vmaf_dir = tmpdir / f"vmaf-{vmaf_version}" / "libvmaf"
        build_dir = vmaf_dir / "build"
        build_dir.mkdir(exist_ok=True)
        
        # Build with meson
        print("  Configuring with meson...")
        run_cmd([
            "meson", "setup",
            f"--prefix={prefix}",
            "--buildtype=release",
            "-Denable_tests=false",
            "-Denable_docs=false",
            str(build_dir)
        ], cwd=str(vmaf_dir))
        
        print(f"  Building with {jobs} jobs...")
        run_cmd(["ninja", "-C", str(build_dir), f"-j{jobs}"])
        
        print("  Installing (requires sudo)...")
        run_cmd(["sudo", "ninja", "-C", str(build_dir), "install"])
        
        # Update library cache
        run_cmd(["sudo", "ldconfig"], check=False)
    
    print("  ✓ libvmaf installed successfully")


def build_ffmpeg(prefix, jobs, with_vmaf=True):
    """Build and install FFmpeg from source."""
    print("\n🔨 Building FFmpeg from source...")
    
    ffmpeg_version = "7.0.2"
    ffmpeg_url = f"https://ffmpeg.org/releases/ffmpeg-{ffmpeg_version}.tar.xz"
    
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        
        # Download FFmpeg
        print(f"  Downloading FFmpeg {ffmpeg_version}...")
        tarball = tmpdir / f"ffmpeg-{ffmpeg_version}.tar.xz"
        run_cmd(["wget", "-O", str(tarball), ffmpeg_url])
        
        # Extract
        print("  Extracting...")
        run_cmd(["tar", "-xJf", str(tarball), "-C", str(tmpdir)])
        
        ffmpeg_dir = tmpdir / f"ffmpeg-{ffmpeg_version}"
        
        # Configure
        print("  Configuring...")
        configure_opts = [
            f"--prefix={prefix}",
            "--enable-gpl",
            "--enable-version3",
            "--enable-nonfree",
            "--enable-shared",
            "--enable-pthreads",
            "--enable-libx264",
            "--enable-libx265",
            "--enable-libvpx",
            "--enable-libopus",
            "--enable-libvorbis",
            "--enable-libass",
            "--enable-libfreetype",
        ]
        
        if with_vmaf:
            # Set PKG_CONFIG_PATH to find libvmaf
            env = os.environ.copy()
            pkg_config_path = f"{prefix}/lib/pkgconfig:{prefix}/lib64/pkgconfig"
            if "PKG_CONFIG_PATH" in env:
                pkg_config_path = f"{pkg_config_path}:{env['PKG_CONFIG_PATH']}"
            env["PKG_CONFIG_PATH"] = pkg_config_path
            
            configure_opts.append("--enable-libvmaf")
        else:
            env = None
        
        run_cmd(["./configure"] + configure_opts, cwd=str(ffmpeg_dir), env=env)
        
        # Build
        print(f"  Building with {jobs} jobs...")
        run_cmd(["make", f"-j{jobs}"], cwd=str(ffmpeg_dir), env=env)
        
        # Install
        print("  Installing (requires sudo)...")
        run_cmd(["sudo", "make", "install"], cwd=str(ffmpeg_dir), env=env)
        
        # Update library cache
        run_cmd(["sudo", "ldconfig"], check=False)
    
    print("  ✓ FFmpeg installed successfully")


def try_install_via_package_manager(pkg_manager):
    """Try to install FFmpeg with VMAF via package manager."""
    print("\n📦 Attempting to install FFmpeg via package manager...")
    
    if pkg_manager == "apt":
        # Try installing ffmpeg first
        run_cmd(["sudo", "apt-get", "update"], check=False)
        run_cmd(["sudo", "apt-get", "install", "-y", "ffmpeg"], check=False)
        
        # Check if VMAF is available
        has_vmaf, _ = check_ffmpeg_has_vmaf()
        if has_vmaf:
            return True
        
        # Try installing libvmaf separately (Ubuntu 22.04+)
        run_cmd(["sudo", "apt-get", "install", "-y", "libvmaf-dev", "libvmaf1"], check=False)
        
    elif pkg_manager in ["dnf", "yum"]:
        # Enable RPM Fusion for more codecs
        run_cmd([
            "sudo", pkg_manager, "install", "-y",
            "https://download1.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm"
        ], check=False)
        run_cmd(["sudo", pkg_manager, "install", "-y", "ffmpeg", "ffmpeg-devel"], check=False)
        
    elif pkg_manager == "brew":
        # Homebrew usually has FFmpeg with VMAF
        run_cmd(["brew", "install", "ffmpeg"], check=False)
    
    return check_ffmpeg_has_vmaf()[0]


def main():
    parser = argparse.ArgumentParser(
        description="Install FFmpeg with PSNR and VMAF support",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    # Install with defaults
    python3 install_ffmpeg_vmaf.py
    
    # Install to custom prefix
    python3 install_ffmpeg_vmaf.py --prefix /opt/ffmpeg
    
    # Force rebuild
    python3 install_ffmpeg_vmaf.py --force
    
    # Skip VMAF (PSNR only)
    python3 install_ffmpeg_vmaf.py --skip-vmaf
        """
    )
    
    parser.add_argument(
        "--prefix", type=str, default="/usr/local",
        help="Installation prefix (default: /usr/local)"
    )
    parser.add_argument(
        "--jobs", type=int, default=multiprocessing.cpu_count(),
        help=f"Number of parallel build jobs (default: {multiprocessing.cpu_count()})"
    )
    parser.add_argument(
        "--force", action="store_true",
        help="Force rebuild even if FFmpeg with VMAF is already installed"
    )
    parser.add_argument(
        "--skip-vmaf", action="store_true",
        help="Skip VMAF installation (PSNR only)"
    )
    parser.add_argument(
        "--from-source", action="store_true",
        help="Build from source instead of trying package manager first"
    )
    
    args = parser.parse_args()
    
    print("=" * 60)
    print("   FFmpeg + VMAF Installation Script")
    print("=" * 60)
    
    # Check current status
    print("\n🔍 Checking current installation...")
    has_vmaf, vmaf_msg = check_ffmpeg_has_vmaf()
    has_psnr = check_ffmpeg_has_psnr()
    
    print(f"  PSNR support: {'✓ Yes' if has_psnr else '✗ No'}")
    print(f"  VMAF support: {'✓ Yes' if has_vmaf else '✗ No'}")
    print(f"  Status: {vmaf_msg}")
    
    # Check if we need to do anything
    if has_vmaf and has_psnr and not args.force:
        print("\n✅ FFmpeg with PSNR and VMAF support is already installed!")
        print("   Use --force to rebuild anyway.")
        return 0
    
    if args.skip_vmaf and has_psnr and not args.force:
        print("\n✅ FFmpeg with PSNR support is already installed!")
        print("   Use --force to rebuild anyway.")
        return 0
    
    # Detect package manager
    pkg_manager = detect_package_manager()
    print(f"\n📦 Detected package manager: {pkg_manager or 'None'}")
    
    if not pkg_manager:
        print("⚠️  No supported package manager found. Will build from source.")
        args.from_source = True
    
    # Try package manager first (unless --from-source)
    if not args.from_source and pkg_manager:
        if try_install_via_package_manager(pkg_manager):
            print("\n✅ FFmpeg with VMAF installed via package manager!")
            return 0
        else:
            print("\n⚠️  Package manager installation didn't provide VMAF support.")
            print("   Will build from source...")
    
    # Build from source
    print(f"\n🔧 Building from source with prefix: {args.prefix}")
    print(f"   Using {args.jobs} parallel jobs")
    
    # Install build dependencies
    if pkg_manager:
        install_build_dependencies(pkg_manager)
    
    # Build libvmaf (unless --skip-vmaf)
    if not args.skip_vmaf:
        build_libvmaf(args.prefix, args.jobs)
    
    # Build FFmpeg
    build_ffmpeg(args.prefix, args.jobs, with_vmaf=not args.skip_vmaf)
    
    # Verify installation
    print("\n🔍 Verifying installation...")
    
    # Update PATH if needed
    bin_path = Path(args.prefix) / "bin"
    if str(bin_path) not in os.environ.get("PATH", ""):
        print(f"\n⚠️  Add {bin_path} to your PATH:")
        print(f'   export PATH="{bin_path}:$PATH"')
    
    # Test
    has_vmaf, _ = check_ffmpeg_has_vmaf()
    has_psnr = check_ffmpeg_has_psnr()
    
    print(f"\n  PSNR support: {'✓ Yes' if has_psnr else '✗ No'}")
    print(f"  VMAF support: {'✓ Yes' if has_vmaf else '✗ No'}")
    
    if (has_vmaf or args.skip_vmaf) and has_psnr:
        print("\n✅ Installation complete!")
        return 0
    else:
        print("\n❌ Installation may have issues. Check the output above.")
        return 1


if __name__ == "__main__":
    sys.exit(main())

