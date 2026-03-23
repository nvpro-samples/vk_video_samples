"""
Video Test Utilities
Common utility functions for command-line argument parsing and framework
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

import hashlib
import json
from pathlib import Path

# Constants
DEFAULT_TEST_TIMEOUT = 120  # seconds


def normalize_test_name(test_name: str) -> str:
    """
    Normalize test name by removing decode_/encode_ prefix if present.

    Args:
        test_name: Original test name

    Returns:
        Normalized test name without decode_/encode_ prefix
    """
    if test_name.startswith("decode_"):
        return test_name[7:]  # len("decode_") == 7
    if test_name.startswith("encode_"):
        return test_name[7:]  # len("encode_") == 7
    return test_name


class ZipSlipError(Exception):
    """Raised when a zip file contains unsafe paths (zip slip attack)"""


class TestSuiteFormatError(Exception):
    """Raised when test suite format is invalid or incompatible"""


def calculate_file_hash(file_path: Path, algorithm: str = 'md5') -> str:
    """Calculate file hash

    Args:
        file_path: Path to file
        algorithm: Hash algorithm ('md5' or 'sha256')

    Returns:
        Hash string (empty on error)
    """
    try:
        hasher = hashlib.md5() if algorithm == 'md5' else hashlib.sha256()
        with open(file_path, 'rb') as f:
            while chunk := f.read(65536):  # Read in 64KB chunks
                hasher.update(chunk)
        return hasher.hexdigest()
    except (OSError, IOError) as e:
        print(f"⚠️  Failed to calculate {algorithm.upper()} "
              f"for {file_path}: {e}")
        return ""


def verify_file_checksum(file_path: Path, expected_checksum: str) -> bool:
    """Verify file checksum

    Supports MD5 (with md5: prefix) and SHA256 (default).

    Args:
        file_path: Path to file
        expected_checksum: Expected checksum (use md5: prefix for MD5)

    Returns:
        True if checksum matches, False otherwise
    """
    try:
        # Detect checksum algorithm
        if expected_checksum.startswith('md5:'):
            algorithm = 'md5'
            expected = expected_checksum[4:]  # Strip md5: prefix
        else:
            algorithm = 'sha256'
            expected = expected_checksum

        actual = calculate_file_hash(file_path, algorithm)
        return actual == expected if actual else False
    except (OSError, IOError):
        return False


def safe_main_wrapper(main_func):
    """Decorator to wrap main function with standard exception handling

    Args:
        main_func: The main function to wrap

    Returns:
        Wrapped function with exception handling

    Catches:
        OSError, ValueError, RuntimeError, KeyboardInterrupt,
        json.JSONDecodeError, TestSuiteFormatError
    """
    def wrapper(*args, **kwargs):
        try:
            return main_func(*args, **kwargs)
        except TestSuiteFormatError as e:
            print(f"✗ Test suite format error: {e}")
            return 1
        except json.JSONDecodeError as e:
            print(f"✗ Invalid JSON in test suite: {e}")
            return 1
        except (OSError, ValueError, RuntimeError, KeyboardInterrupt) as e:
            print(f"✗ FATAL ERROR: {e}")
            return 1
    return wrapper
