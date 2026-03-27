"""
Video Platform Utilities
Platform utility functions for command-line argument parsing and framework
helpers.

Copyright 2025 Igalia S.L.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""
import platform
import shutil
import subprocess

from pathlib import Path
from typing import List, Optional, Set


class PlatformUtils:
    """Utility class for platform-specific operations"""

    @staticmethod
    def is_windows() -> bool:
        """Check if running on Windows"""
        return platform.system() == "Windows"

    @staticmethod
    def get_executable_extension() -> str:
        """Get platform-specific executable extension"""
        return ".exe" if PlatformUtils.is_windows() else ""

    # Common build directory names that can be combined with project root
    BUILD_DIR_NAMES = ['.', 'build', 'BUILD', 'out', 'cmake-build-debug',
                       'cmake-build-release', 'build-debug', 'build-release']

    # Standard paths within build directories to search
    # Prioritize build artifacts over install artifacts
    STANDARD_SEARCH_PATHS = [
        # Direct build outputs first
        'vk_video_encoder/demos',
        'vk_video_decoder/demos',
        'Debug',
        'Release',
        'debug',
        'release',
        'bin',
        '.',
        # Install artifacts last
        'install/Debug/bin',
        'install/Release/bin',
        'install/debug/bin',
        'install/release/bin',
    ]

    @staticmethod
    def _normalize_executable_name(name: str) -> str:
        """Return executable name with platform extension when needed."""
        if PlatformUtils.is_windows() and not name.endswith('.exe'):
            return f"{name}.exe"
        return name

    @staticmethod
    def _resolve_project_root() -> Path:
        """Resolve project root from tests directory, falling back to CWD."""
        try:
            return Path(__file__).resolve().parent.parent.parent
        except (OSError, AttributeError):
            return Path.cwd()

    @staticmethod
    def _expand_build_tree(base_dir: Path,
                           build_dirs: List[str]) -> List[Path]:
        """Collect existing build subdirectories under a base directory."""
        collected: List[Path] = []
        for build_dir in build_dirs:
            build_path = base_dir / build_dir
            if not build_path.exists():
                continue
            for subpath in PlatformUtils.STANDARD_SEARCH_PATHS:
                full_path = build_path / subpath
                if full_path.exists():
                    collected.append(full_path)
        return collected

    @staticmethod
    def _collect_candidate_dirs(
        project_root: Path,
        current_dir: Path,
        build_dirs: List[str],
        search_paths: Optional[List[str]],
    ) -> List[Path]:
        """Gather candidate directories to look for executables."""
        candidates: List[Path] = []
        seen: Set[str] = set()

        def add_path(path: Path) -> None:
            try:
                resolved = str(path.resolve())
            except OSError:
                resolved = str(path)
            if resolved in seen:
                return
            seen.add(resolved)
            candidates.append(path)

        if search_paths:
            for search_dir in search_paths:
                add_path(Path(search_dir).expanduser())

        for path in PlatformUtils._expand_build_tree(project_root, build_dirs):
            add_path(path)

        for path in PlatformUtils._expand_build_tree(current_dir, build_dirs):
            add_path(path)

        return candidates

    @staticmethod
    def find_executable(name: str,
                        search_paths: List[str] = None,
                        build_dirs: List[str] = None) -> Optional[Path]:
        """Find executable in PATH or specified search paths

        Args:
            name: Name of the executable to find
            search_paths: Additional specific paths to search
            build_dirs: List of build directory names to use
                (defaults to BUILD_DIR_NAMES)

        Returns:
            Path to the executable if found, None otherwise
        """
        executable_name = PlatformUtils._normalize_executable_name(name)

        exe_path = Path(executable_name)
        if exe_path.is_file():
            return exe_path.resolve()

        build_dir_list = build_dirs or PlatformUtils.BUILD_DIR_NAMES
        project_root = PlatformUtils._resolve_project_root()

        candidate_dirs = PlatformUtils._collect_candidate_dirs(
            project_root,
            Path.cwd(),
            build_dir_list,
            search_paths,
        )

        for search_dir in candidate_dirs:
            candidate = search_dir / executable_name
            if candidate.is_file():
                return candidate.resolve()

        exe_in_path = shutil.which(executable_name)
        return Path(exe_in_path).resolve() if exe_in_path else None

    @staticmethod
    def resolve_executable_path(path: str, verbose: bool = False) -> str:
        """Resolve and validate executable path

        Args:
            path: Path to executable (may be relative or absolute)
            verbose: Whether to print verbose output

        Returns:
            Resolved absolute path to executable, or original path if not found
        """
        if path and not Path(path).is_absolute():
            found_exe = PlatformUtils.find_executable(path)
            if found_exe:
                resolved_path = str(found_exe)
                if verbose:
                    print(f"✓ Found executable: {resolved_path}")
                return resolved_path
        return path

    @staticmethod
    def get_subprocess_kwargs() -> dict:
        """Get platform-specific subprocess kwargs"""
        kwargs = {
            'capture_output': True,
            'text': True
        }

        # On Windows, prevent console window popup for subprocess
        if PlatformUtils.is_windows():
            # CREATE_NO_WINDOW is only available on Windows and Python 3.7+
            if hasattr(subprocess, 'CREATE_NO_WINDOW'):
                kwargs['creationflags'] = subprocess.CREATE_NO_WINDOW

        return kwargs
