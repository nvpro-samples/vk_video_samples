"""
Unit tests for utility functions.

Tests calculate_file_hash() and verify_file_checksum().

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
import tempfile
from pathlib import Path

from tests.libs.video_test_utils import (
    calculate_file_hash, verify_file_checksum)


class TestCalculateFileHash:
    """Tests for calculate_file_hash() function"""

    def test_md5_hash(self):
        """Test MD5 hash calculation"""
        content = b"Hello, World!"
        expected_md5 = hashlib.md5(content).hexdigest()

        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(content)
            temp_path = Path(f.name)

        try:
            result = calculate_file_hash(temp_path, algorithm='md5')
            assert result == expected_md5
        finally:
            temp_path.unlink()

    def test_sha256_hash(self):
        """Test SHA256 hash calculation"""
        content = b"Hello, World!"
        expected_sha256 = hashlib.sha256(content).hexdigest()

        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(content)
            temp_path = Path(f.name)

        try:
            result = calculate_file_hash(temp_path, algorithm='sha256')
            assert result == expected_sha256
        finally:
            temp_path.unlink()

    def test_default_algorithm_is_md5(self):
        """Test that default algorithm is MD5"""
        content = b"test content"
        expected_md5 = hashlib.md5(content).hexdigest()

        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(content)
            temp_path = Path(f.name)

        try:
            result = calculate_file_hash(temp_path)  # No algorithm specified
            assert result == expected_md5
        finally:
            temp_path.unlink()

    def test_nonexistent_file_returns_empty(self):
        """Test that nonexistent file returns empty string"""
        result = calculate_file_hash(Path("/nonexistent/file.txt"))
        assert result == ""

    def test_empty_file(self):
        """Test hash of empty file"""
        with tempfile.NamedTemporaryFile(delete=False) as f:
            temp_path = Path(f.name)

        try:
            result_md5 = calculate_file_hash(temp_path, 'md5')
            result_sha256 = calculate_file_hash(temp_path, 'sha256')

            # Known hashes for empty content
            assert result_md5 == hashlib.md5(b"").hexdigest()
            assert result_sha256 == hashlib.sha256(b"").hexdigest()
        finally:
            temp_path.unlink()


class TestVerifyFileChecksum:
    """Tests for verify_file_checksum() function"""

    def test_sha256_checksum_valid(self):
        """Test valid SHA256 checksum verification"""
        content = b"test content for sha256"
        expected_checksum = hashlib.sha256(content).hexdigest()

        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(content)
            temp_path = Path(f.name)

        try:
            result = verify_file_checksum(temp_path, expected_checksum)
            assert result is True
        finally:
            temp_path.unlink()

    def test_sha256_checksum_invalid(self):
        """Test invalid SHA256 checksum verification"""
        content = b"test content"

        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(content)
            temp_path = Path(f.name)

        try:
            result = verify_file_checksum(temp_path, "invalid_checksum")
            assert result is False
        finally:
            temp_path.unlink()

    def test_md5_checksum_with_prefix(self):
        """Test MD5 checksum with md5: prefix"""
        content = b"test content for md5"
        expected_md5 = hashlib.md5(content).hexdigest()

        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(content)
            temp_path = Path(f.name)

        try:
            # Use md5: prefix
            result = verify_file_checksum(temp_path, f"md5:{expected_md5}")
            assert result is True
        finally:
            temp_path.unlink()

    def test_md5_checksum_without_prefix_fails(self):
        """Test that MD5 hash without prefix is treated as SHA256 and fails"""
        content = b"test content"
        md5_hash = hashlib.md5(content).hexdigest()

        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(content)
            temp_path = Path(f.name)

        try:
            # Without md5: prefix, it's treated as SHA256
            result = verify_file_checksum(temp_path, md5_hash)
            assert result is False  # MD5 hash != SHA256 hash
        finally:
            temp_path.unlink()

    def test_nonexistent_file_returns_false(self):
        """Test that nonexistent file returns False"""
        result = verify_file_checksum(
            Path("/nonexistent/file.txt"),
            "any_checksum"
        )
        assert result is False

    def test_empty_checksum(self):
        """Test behavior with empty expected checksum"""
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b"content")
            temp_path = Path(f.name)

        try:
            result = verify_file_checksum(temp_path, "")
            # Empty checksum won't match actual hash
            assert result is False
        finally:
            temp_path.unlink()
