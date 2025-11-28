#!/usr/bin/env python3
"""
Cross-platform script to fetch and build Vulkan Video Samples with Adaptive Quantization (AQ) support.

This script builds the Vulkan Video Samples project (main project) with the AQ library as a dependency.

The script:
1. Clones/fetches the AQ library repository (dependency) if needed
2. Configures and builds the Vulkan Video Samples project (main project) with AQ support
3. Supports both Debug and Release builds
4. Works on both Windows and Linux

Usage:
    Linux:
        python3 scripts/build_aq_project.py --target-dir /path/to/build --aq-repo-url <url> --build-type Debug

    Windows:
        python scripts\build_aq_project.py --target-dir C:\path\to\build --aq-repo-url <url> --build-type Debug
"""

import argparse
import sys
import os
import subprocess
import platform
import shutil
from pathlib import Path


def detect_platform():
    """Detect the current platform."""
    return platform.system().lower()


def run_command(cmd, cwd=None, check=True, shell=False):
    """
    Run a command and return the result.
    
    Args:
        cmd: Command to run (list or string)
        cwd: Working directory
        check: Whether to raise exception on non-zero return code
        shell: Whether to run in shell
    
    Returns:
        CompletedProcess object
    """
    system = detect_platform()
    
    # On Windows, use shell=True for some commands
    if system == "windows" and isinstance(cmd, str):
        shell = True
    
    print(f"Running: {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    if cwd:
        print(f"  Working directory: {cwd}")
    
    result = subprocess.run(
        cmd,
        cwd=cwd,
        shell=shell,
        check=check,
        capture_output=False
    )
    
    return result


def git_clone_or_update(repo_url, dest_path, branch=None):
    """
    Clone a git repository or update it if it already exists.
    
    Args:
        repo_url: Git repository URL
        dest_path: Destination path
        branch: Branch to checkout (optional)
    
    Returns:
        True if successful, False otherwise
    """
    dest_path = Path(dest_path)
    
    if dest_path.exists() and (dest_path / ".git").exists():
        print(f"Repository already exists at {dest_path}, updating...")
        try:
            # Fetch latest changes
            run_command(["git", "fetch"], cwd=dest_path, check=False)
            if branch:
                run_command(["git", "checkout", branch], cwd=dest_path, check=False)
            run_command(["git", "pull"], cwd=dest_path, check=False)
            print(f"Repository updated successfully.")
            return True
        except Exception as e:
            print(f"Warning: Failed to update repository: {e}")
            print("Continuing with existing repository...")
            return True
    else:
        print(f"Cloning repository from {repo_url} to {dest_path}...")
        try:
            cmd = ["git", "clone", repo_url, str(dest_path)]
            if branch:
                cmd.extend(["--branch", branch])
            run_command(cmd, check=True)
            print(f"Repository cloned successfully.")
            return True
        except subprocess.CalledProcessError as e:
            print(f"Error: Failed to clone repository: {e}", file=sys.stderr)
            return False


def setup_build_directory(target_dir, build_type):
    """
    Setup build directory for the specified build type.
    
    Args:
        target_dir: Target directory for builds
        build_type: Build type (Debug or Release)
    
    Returns:
        Path to build directory
    """
    target_dir = Path(target_dir)
    
    if build_type == "Release":
        build_dir = target_dir / "build-release"
    else:
        build_dir = target_dir / "build"
    
    build_dir.mkdir(parents=True, exist_ok=True)
    
    return build_dir


def configure_cmake(project_root, build_dir, aq_lib_path, build_type, cuda_architectures="86;89;90"):
    """
    Configure CMake for the project.
    
    Args:
        project_root: Root directory of vulkan-video-samples
        build_dir: Build directory
        aq_lib_path: Path to AQ library (aq-vulkan directory)
        build_type: Build type (Debug or Release)
        cuda_architectures: CUDA architectures string
    
    Returns:
        True if successful, False otherwise
    """
    project_root = Path(project_root)
    build_dir = Path(build_dir)
    aq_lib_path = Path(aq_lib_path)
    
    # Verify AQ library path exists
    if not aq_lib_path.exists():
        print(f"Error: AQ library path does not exist: {aq_lib_path}", file=sys.stderr)
        return False
    
    cmake_lists = aq_lib_path / "CMakeLists.txt"
    if not cmake_lists.exists():
        print(f"Error: CMakeLists.txt not found at: {cmake_lists}", file=sys.stderr)
        return False
    
    # Convert path to forward slashes for CMake (even on Windows)
    aq_lib_path_str = str(aq_lib_path).replace("\\", "/")
    
    print(f"\nConfiguring CMake ({build_type} build)...")
    print(f"  Project root: {project_root}")
    print(f"  Build directory: {build_dir}")
    print(f"  AQ library path: {aq_lib_path_str}")
    print(f"  CUDA architectures: {cuda_architectures}")
    
    cmd = [
        "cmake",
        "..",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DCMAKE_CUDA_ARCHITECTURES={cuda_architectures}",
        f"-DNV_AQ_GPU_LIB={aq_lib_path_str}"
    ]
    
    try:
        run_command(cmd, cwd=build_dir, check=True)
        print("CMake configuration completed successfully.")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error: CMake configuration failed: {e}", file=sys.stderr)
        return False


def build_project(build_dir, build_type, num_jobs=None):
    """
    Build the project using CMake.
    
    Args:
        build_dir: Build directory
        build_type: Build type (Debug or Release)
        num_jobs: Number of parallel jobs (None for auto-detect)
    
    Returns:
        True if successful, False otherwise
    """
    build_dir = Path(build_dir)
    
    system = detect_platform()
    
    if num_jobs is None:
        if system == "windows":
            num_jobs = os.cpu_count()
        else:
            num_jobs = os.cpu_count()
    
    print(f"\nBuilding project ({build_type} build)...")
    print(f"  Build directory: {build_dir}")
    print(f"  Parallel jobs: {num_jobs}")
    
    cmd = [
        "cmake",
        "--build",
        ".",
        "--config",
        build_type
    ]
    
    if num_jobs:
        cmd.extend(["-j", str(num_jobs)])
    
    try:
        run_command(cmd, cwd=build_dir, check=True)
        print("Build completed successfully.")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error: Build failed: {e}", file=sys.stderr)
        return False


def install_project(build_dir, build_type):
    """
    Install the project using CMake.
    
    Args:
        build_dir: Build directory
        build_type: Build type (Debug or Release)
    
    Returns:
        True if successful, False otherwise
    """
    build_dir = Path(build_dir)
    
    print(f"\nInstalling project ({build_type} build)...")
    
    cmd = [
        "cmake",
        "--install",
        ".",
        "--config",
        build_type
    ]
    
    try:
        run_command(cmd, cwd=build_dir, check=True)
        print("Installation completed successfully.")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Warning: Installation failed: {e}", file=sys.stderr)
        # Installation failure is not critical
        return True


def verify_build(build_dir, build_type):
    """
    Verify that the build was successful by checking for key files.
    
    Args:
        build_dir: Build directory
        build_type: Build type (Debug or Release)
    
    Returns:
        True if verification passes, False otherwise
    """
    build_dir = Path(build_dir)
    system = detect_platform()
    
    print(f"\nVerifying build ({build_type})...")
    
    # Check for AQ library
    if system == "windows":
        aq_lib = build_dir / "lib" / "nvenc_aq_vulkan.dll"
        encoder_exe = build_dir / "vk_video_encoder" / "test" / build_type / "vulkan-video-enc-test.exe"
    else:
        aq_lib = build_dir / "lib" / "libnvenc_aq_vulkan.so"
        encoder_exe = build_dir / "vk_video_encoder" / "test" / "vulkan-video-enc-test"
    
    checks_passed = True
    
    if aq_lib.exists():
        print(f"  ✓ AQ library found: {aq_lib}")
    else:
        print(f"  ✗ AQ library not found: {aq_lib}", file=sys.stderr)
        checks_passed = False
    
    if encoder_exe.exists():
        print(f"  ✓ Encoder executable found: {encoder_exe}")
    else:
        print(f"  ✗ Encoder executable not found: {encoder_exe}", file=sys.stderr)
        checks_passed = False
    
    return checks_passed


def main():
    parser = argparse.ArgumentParser(
        description="Fetch and build Vulkan Video Samples with Adaptive Quantization (AQ) support",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Clone both repositories and build Debug version
  # Structure: target_dir/vulkan-video-samples and target_dir/algo
  python3 scripts/build_aq_project.py \\
      --target-dir /path/to/target \\
      --vulkan-samples-repo-url https://github.com/nvpro-samples/vk_video_samples \\
      --aq-repo-url https://gitlab-master.nvidia.com/vulkan-video/vulkan-video-verification \\
      --build-type Debug

  # Clone both repositories and build Release version
  python3 scripts/build_aq_project.py \\
      --target-dir /path/to/target \\
      --vulkan-samples-repo-url https://github.com/nvpro-samples/vk_video_samples \\
      --aq-repo-url https://gitlab-master.nvidia.com/vulkan-video/vulkan-video-verification \\
      --build-type Release

  # Use existing repositories in target_dir
  python3 scripts/build_aq_project.py \\
      --target-dir /path/to/target \\
      --build-type Debug

  # Use custom project root but clone AQ library
  python3 scripts/build_aq_project.py \\
      --target-dir /path/to/target \\
      --project-root /path/to/existing/vulkan-video-samples \\
      --aq-repo-url https://gitlab-master.nvidia.com/vulkan-video/vulkan-video-verification \\
      --build-type Debug
        """
    )
    
    # Required arguments
    parser.add_argument(
        "--target-dir",
        type=Path,
        required=True,
        help="Target directory where repositories will be cloned and builds will occur (required). Structure: target_dir/vulkan-video-samples and target_dir/algo"
    )
    parser.add_argument(
        "--build-type",
        choices=["Debug", "Release"],
        required=True,
        help="Build type: Debug or Release (required)"
    )
    
    # AQ Library options (dependency)
    parser.add_argument(
        "--aq-repo-url",
        type=str,
        help="Git repository URL for AQ library (algo repository). If not specified, will use existing target_dir/algo"
    )
    parser.add_argument(
        "--aq-branch",
        type=str,
        default="main",
        help="Branch to checkout for AQ repository (default: main)"
    )
    parser.add_argument(
        "--aq-lib-path",
        type=Path,
        help="Path to existing aq-vulkan directory (alternative to --aq-repo-url, overrides target_dir/algo)"
    )
    
    # Main project options (vulkan-video-samples)
    parser.add_argument(
        "--vulkan-samples-repo-url",
        type=str,
        help="Git repository URL for vulkan-video-samples. If not specified, will use existing target_dir/vulkan-video-samples"
    )
    parser.add_argument(
        "--vulkan-samples-branch",
        type=str,
        default="vulkan-aq-lib-integration",
        help="Branch to checkout for vulkan-video-samples repository (default: vulkan-aq-lib-integration)"
    )
    parser.add_argument(
        "--project-root",
        type=Path,
        help="Path to existing vulkan-video-samples directory (alternative to --vulkan-samples-repo-url, overrides target_dir/vulkan-video-samples)"
    )
    parser.add_argument(
        "--cuda-architectures",
        type=str,
        default="86;89;90",
        help="CUDA architectures to compile for (default: 86;89;90)"
    )
    parser.add_argument(
        "--num-jobs",
        type=int,
        help="Number of parallel build jobs (default: auto-detect)"
    )
    parser.add_argument(
        "--skip-install",
        action="store_true",
        help="Skip installation step"
    )
    parser.add_argument(
        "--skip-verify",
        action="store_true",
        help="Skip build verification"
    )
    
    # Execution options
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands without executing them"
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print verbose output"
    )
    
    args = parser.parse_args()
    
    # Validate arguments
    if args.aq_lib_path and args.aq_repo_url:
        parser.error("Cannot specify both --aq-lib-path and --aq-repo-url")
    
    if args.project_root and args.vulkan_samples_repo_url:
        parser.error("Cannot specify both --project-root and --vulkan-samples-repo-url")

    # Resolve target directory
    target_dir = Path(args.target_dir).resolve()
    target_dir.mkdir(parents=True, exist_ok=True)
    
    print(f"\n{'='*60}")
    print("Vulkan Video Samples Build Configuration")
    print(f"{'='*60}")
    print(f"  Target directory: {target_dir}")
    print(f"  Build type: {args.build_type}")

    # Determine vulkan-video-samples project root (main project)
    project_root = None

    if args.project_root:
        project_root = Path(args.project_root).resolve()
        if not (project_root / "CMakeLists.txt").exists():
            parser.error(f"CMakeLists.txt not found in vulkan-video-samples project root: {project_root}")
    elif args.vulkan_samples_repo_url:
        # Clone or update vulkan-video-samples repository (main project) into target directory
        vulkan_samples_path = target_dir / "vulkan-video-samples"
        vulkan_samples_branch = args.vulkan_samples_branch

        print(f"\n{'='*60}")
        print("Fetching Vulkan Video Samples (Main Project)")
        print(f"{'='*60}")
        print(f"  Repository URL: {args.vulkan_samples_repo_url}")
        print(f"  Target directory: {vulkan_samples_path}")
        print(f"  Branch: {vulkan_samples_branch}")

        if args.dry_run:
            print(f"[DRY RUN] Would clone {args.vulkan_samples_repo_url} to {vulkan_samples_path}")
            project_root = vulkan_samples_path
        else:
            if git_clone_or_update(args.vulkan_samples_repo_url, vulkan_samples_path, vulkan_samples_branch):
                project_root = vulkan_samples_path
                print(f"  Main project: {project_root}")
            else:
                print("Error: Failed to fetch vulkan-video-samples repository", file=sys.stderr)
                return 1
    else:
        # Use existing target_dir/vulkan-video-samples
        project_root = target_dir / "vulkan-video-samples"
        if not (project_root / "CMakeLists.txt").exists():
            parser.error(f"vulkan-video-samples not found at {project_root}. Specify --vulkan-samples-repo-url or --project-root")

    if not project_root:
        parser.error("Must specify either --project-root, --vulkan-samples-repo-url, or have target_dir/vulkan-video-samples exist")
    
    project_root = Path(project_root).resolve()
    print(f"  Main project (vulkan-video-samples): {project_root}")
    
    # Determine AQ library path (dependency)
    aq_lib_path = None
    
    if args.aq_lib_path:
        aq_lib_path = Path(args.aq_lib_path).resolve()
        if not aq_lib_path.exists():
            parser.error(f"AQ library path does not exist: {aq_lib_path}")
        print(f"  AQ library (dependency): {aq_lib_path}")
    elif args.aq_repo_url:
        # Clone or update AQ repository (dependency) into target_dir/algo
        aq_repo_path = target_dir / "algo"
        aq_branch = args.aq_branch
        
        print(f"\n{'='*60}")
        print("Fetching AQ Library Dependency")
        print(f"{'='*60}")
        print(f"  Repository URL: {args.aq_repo_url}")
        print(f"  Target directory: {aq_repo_path}")
        print(f"  Branch: {aq_branch}")
        
        if args.dry_run:
            print(f"[DRY RUN] Would clone {args.aq_repo_url} to {aq_repo_path}")
            aq_lib_path = aq_repo_path / "aq-vulkan"
        else:
            if git_clone_or_update(args.aq_repo_url, aq_repo_path, aq_branch):
                aq_lib_path = aq_repo_path / "aq-vulkan"
                print(f"  AQ library (dependency): {aq_lib_path}")
            else:
                print("Error: Failed to fetch AQ library repository", file=sys.stderr)
                return 1
    else:
        # Use existing target_dir/algo
        aq_repo_path = target_dir / "algo"
        aq_lib_path = aq_repo_path / "aq-vulkan"
        if not (aq_lib_path / "CMakeLists.txt").exists():
            parser.error(f"AQ library not found at {aq_lib_path}. Specify --aq-repo-url or --aq-lib-path")
        print(f"  AQ library (dependency): {aq_lib_path}")
    
    if not aq_lib_path or not aq_lib_path.exists():
        parser.error(f"AQ library path is invalid: {aq_lib_path}")
    
    # Verify AQ library structure
    if not (aq_lib_path / "CMakeLists.txt").exists():
        parser.error(f"CMakeLists.txt not found in AQ library path: {aq_lib_path}")
    
    # Process build
    success = True
    build_type = args.build_type
    
    print(f"\n{'='*60}")
    print(f"Building Vulkan Video Samples ({build_type})")
    print(f"{'='*60}")
    print(f"  Main project: {project_root}")
    print(f"  AQ dependency: {aq_lib_path}")
    
    # Setup build directory (inside project_root)
    if build_type == "Release":
        build_dir = project_root / "build-release"
    else:
        build_dir = project_root / "build"
    
    build_dir.mkdir(parents=True, exist_ok=True)
    print(f"  Build directory: {build_dir}")
    
    if args.dry_run:
        print(f"[DRY RUN] Would configure CMake in {build_dir}")
        print(f"[DRY RUN] Would build {build_type} configuration")
        if not args.skip_install:
            print(f"[DRY RUN] Would install {build_type} configuration")
        return 0
    
    # Configure CMake
    if not configure_cmake(project_root, build_dir, aq_lib_path, build_type, args.cuda_architectures):
        success = False
    
    # Build project
    if success and not build_project(build_dir, build_type, args.num_jobs):
        success = False
    
    # Install project
    if success and not args.skip_install:
        install_project(build_dir, build_type)
    
    # Verify build
    if success and not args.skip_verify:
        if not verify_build(build_dir, build_type):
            print(f"Warning: Build verification failed for {build_type}", file=sys.stderr)
            # Don't fail on verification warnings
    
    if success:
        print(f"\n{'='*60}")
        print("Build Process Completed Successfully")
        print(f"{'='*60}")
        print(f"\nBuild outputs:")
        system = detect_platform()
        if system == "windows":
            encoder_exe = build_dir / "vk_video_encoder" / "test" / build_type / "vulkan-video-enc-test.exe"
        else:
            encoder_exe = build_dir / "vk_video_encoder" / "test" / "vulkan-video-enc-test"
        print(f"  Encoder executable: {encoder_exe}")
        print(f"  AQ library: {build_dir / 'lib' / ('nvenc_aq_vulkan.dll' if system == 'windows' else 'libnvenc_aq_vulkan.so')}")
        return 0
    else:
        print(f"\n{'='*60}")
        print("Build Process Failed")
        print(f"{'='*60}")
        return 1


if __name__ == "__main__":
    sys.exit(main())

