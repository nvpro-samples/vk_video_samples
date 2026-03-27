#!/usr/bin/env python3
"""
Base fetch sample system for Vulkan Video Test Framework
Provides common download logic.

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

import ssl
import hashlib
import time
from pathlib import Path
from typing import List
from dataclasses import dataclass

# Import URL libraries at top level
from urllib.request import urlopen


# Supported hash algorithms
SUPPORTED_HASH_ALGORITHMS = ('sha256', 'md5')

# Chunk size for streaming file reads (64KB)
STREAMING_CHUNK_SIZE = 65536


def compute_checksum(data: bytes, algorithm: str = 'sha256') -> str:
    """Compute checksum of binary data

    Args:
        data: Binary data to checksum
        algorithm: Hash algorithm ('sha256' or 'md5')

    Returns:
        Hex digest of checksum

    Raises:
        ValueError: If algorithm is not supported
    """
    if algorithm not in SUPPORTED_HASH_ALGORITHMS:
        raise ValueError(
            f"Unsupported hash algorithm: '{algorithm}'. "
            f"Supported algorithms: {', '.join(SUPPORTED_HASH_ALGORITHMS)}"
        )
    if algorithm == 'md5':
        return hashlib.md5(data).hexdigest()
    return hashlib.sha256(data).hexdigest()


def compute_file_checksum_streaming(
        file_path: Path, algorithm: str = 'sha256') -> str:
    """Compute checksum of a file using streaming to avoid memory issues.

    Args:
        file_path: Path to file to checksum
        algorithm: Hash algorithm ('sha256' or 'md5')

    Returns:
        Hex digest of checksum

    Raises:
        ValueError: If algorithm is not supported
        OSError: If file cannot be read
    """
    if algorithm not in SUPPORTED_HASH_ALGORITHMS:
        raise ValueError(
            f"Unsupported hash algorithm: '{algorithm}'. "
            f"Supported algorithms: {', '.join(SUPPORTED_HASH_ALGORITHMS)}"
        )

    hasher = hashlib.md5() if algorithm == 'md5' else hashlib.sha256()
    with open(file_path, 'rb') as f:
        while chunk := f.read(STREAMING_CHUNK_SIZE):
            hasher.update(chunk)
    return hasher.hexdigest()


def read_binary_file(filename: str) -> bytes:
    """Read binary file and return contents"""
    with open(filename, 'rb') as f:
        return f.read()


def write_binary_file(filename: str, data: bytes) -> None:
    """Write binary data to file"""
    with open(filename, 'wb') as f:
        f.write(data)


@dataclass
class FetchableResource:
    """A resource that can be downloaded and verified"""
    url: str
    filename: str
    checksum: str
    base_dir: str
    checksum_algorithm: str = 'sha256'  # 'sha256' or 'md5'

    # Class variable shared across all instances
    _resources_dir = Path(__file__).parent.parent / "resources"

    def __post_init__(self):
        """Initialize computed properties"""
        self.last_error = None  # Track last error message
        self.actual_checksum = None  # Track actual checksum if verify failed

    @property
    def full_path(self) -> Path:
        """Get the full path where this resource should be stored"""
        return self._resources_dir / self.base_dir / self.filename

    def exists(self) -> bool:
        """Check if the resource file exists"""
        return self.full_path.exists()

    def is_file_up_to_date(self) -> bool:
        """Check if existing file has correct checksum using streaming hash"""
        if not self.exists():
            return False

        try:
            # Use streaming hash to avoid loading entire file into memory
            actual_checksum = compute_file_checksum_streaming(
                self.full_path, self.checksum_algorithm
            )
            return actual_checksum == self.checksum
        except (OSError, IOError, ValueError):
            return False

    def clean(self) -> None:
        """Remove the resource file if it exists"""
        try:
            if self.exists():
                self.full_path.unlink()
        except OSError:
            pass

    def connect_to_url(self, url: str, insecure: bool = False):
        """Connect to URL with optional SSL verification bypass

        Args:
            url: URL to connect to
            insecure: If True, skip SSL cert verification (NOT RECOMMENDED)

        Returns:
            URL response object

        Warning:
            Using insecure=True disables SSL certificate verification,
            making connections vulnerable to man-in-the-middle attacks.
        """
        if insecure:
            print("⚠️  WARNING: SSL certificate verification disabled. "
                  "Connection may be insecure.")
            ssl_context = ssl.create_default_context()
            ssl_context.check_hostname = False
            ssl_context.verify_mode = ssl.CERT_NONE
            return urlopen(url, context=ssl_context)
        return urlopen(url)

    @staticmethod
    def _format_eta(seconds: float) -> str:
        """Format ETA seconds into human-readable string"""
        if seconds < 60:
            return f"{int(seconds)}s"
        if seconds < 3600:
            return f"{int(seconds / 60)}m {int(seconds % 60)}s"
        hours = int(seconds / 3600)
        minutes = int((seconds % 3600) / 60)
        return f"{hours}h {minutes}m"

    # pylint: disable-next=too-many-locals
    def _download_with_progress(self, req, total_size: int) -> bytes:
        """Download data with progress bar and rate estimation"""
        data = bytearray()
        downloaded = 0
        start_time = time.time()
        last_update_time = start_time

        size_mb = total_size / (1024*1024)
        print(f"Downloading {self.filename} ({size_mb:.1f} MB)")

        while True:
            chunk = req.read(8192)
            if not chunk:
                break
            data.extend(chunk)
            downloaded += len(chunk)

            # Update progress bar (throttle to every 0.1 seconds)
            current_time = time.time()
            time_elapsed = current_time - last_update_time >= 0.1
            if time_elapsed or downloaded == total_size:
                elapsed = current_time - start_time
                if elapsed > 0:
                    # Calculate metrics
                    rate = downloaded / elapsed
                    remaining = (total_size - downloaded) / rate
                    eta_str = self._format_eta(remaining)

                    # Build and print progress bar
                    progress = downloaded / total_size
                    filled = int(40 * progress)
                    dl_mb = downloaded / (1024*1024)
                    total_mb = total_size / (1024*1024)
                    mb_info = f'{dl_mb:.1f}/{total_mb:.1f} MB'

                    pbar = f'[{"=" * filled}{"-" * (40 - filled)}]'
                    stats = f'{mb_info} | {rate / (1024*1024):.2f} MB/s'
                    pct = progress * 100
                    print(f'\r{pbar} {pct:.1f}% {stats} | ETA: {eta_str}',
                          end='', flush=True)

                    last_update_time = current_time

        print()  # New line after progress bar
        return bytes(data)

    def fetch_and_verify_file(self, insecure: bool = False) -> bool:
        """Download and verify the resource file"""
        # Reset error tracking
        self.last_error = None
        self.actual_checksum = None

        try:
            print(f"Fetching {self.url}")
            req = self.connect_to_url(self.url, insecure)

            # Get file size and download
            content_length = req.headers.get('Content-Length')
            total_size = int(content_length) if content_length else None

            if total_size:
                data = self._download_with_progress(req, total_size)
            else:
                # No content length, download without progress bar
                data = bytes(req.read())

            # Verify checksum
            file_checksum = compute_checksum(data, self.checksum_algorithm)
            if file_checksum != self.checksum:
                # Store actual checksum for reporting
                self.actual_checksum = file_checksum
                raise ValueError(
                    f"Checksum mismatch for {self.filename}, "
                    f"expected {self.checksum}, got {file_checksum}"
                )

            # Save file
            self.full_path.parent.mkdir(parents=True, exist_ok=True)
            write_binary_file(str(self.full_path), data)
            print(f"✓ Downloaded and verified: {self.full_path}")
            return True

        except (OSError, IOError, ValueError) as e:
            self.last_error = str(e)
            print(f"\n✗ Failed to download {self.filename}")
            return False

    def update(self, insecure: bool = False) -> bool:
        """Update the resource if needed (download if missing or outdated)"""
        if not self.is_file_up_to_date():
            self.clean()
            return self.fetch_and_verify_file(insecure)
        return True


class SampleFetcher:
    """Base class for fetching test samples"""

    def __init__(self, resources: List[FetchableResource]):
        self.resources = resources
        self.failed_downloads = []  # Track failed downloads

    def fetch_all(self, insecure: bool = False) -> bool:
        """Fetch all resources, collecting errors to display at the end"""
        self.failed_downloads = []  # Reset failed downloads list

        for resource in self.resources:
            if not resource.update(insecure):
                # Keep the failed resource and track the failure
                self.failed_downloads.append(resource)

        # Display all failures at the end
        if self.failed_downloads:
            print("\n" + "=" * 70)
            print("DOWNLOAD FAILURES SUMMARY:")
            print("=" * 70)
            for resource in self.failed_downloads:
                print(f"✗ {resource.filename}")
                print(f"  URL: {resource.url}")
                print(f"  Expected at: {resource.full_path}")

                # Show checksum comparison if available
                if resource.actual_checksum:
                    algo = resource.checksum_algorithm.upper()
                    print(f"  Expected {algo}: {resource.checksum}")
                    print(f"  Actual {algo}:   {resource.actual_checksum}")

                # Show error message if available
                if resource.last_error:
                    print(f"  Error: {resource.last_error}")
                print()
            print("=" * 70)
            return False

        return True

    def clean_all(self) -> None:
        """Clean all resources"""
        for resource in self.resources:
            resource.clean()
