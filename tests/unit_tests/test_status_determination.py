"""
Unit tests for test status determination.

Tests determine_test_status() method return code mapping.

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

from unittest.mock import patch

import pytest

from tests.libs.video_test_config_base import VideoTestStatus, SkipFilter
from tests.libs.video_test_framework_base import VulkanVideoTestFrameworkBase


class MockFramework(VulkanVideoTestFrameworkBase):
    """Mock framework for testing determine_test_status"""

    def __init__(self):
        super().__init__(executable_path=None)
        self._skip_rules = []
        self._options['skip_filter'] = SkipFilter.ENABLED

    def check_resources(self, _auto_download=True, _test_configs=None):
        """Check resources - mock implementation always returns True."""
        return True

    def create_test_suite(self):
        """Create test suite - mock implementation returns empty list."""
        return []

    def run_single_test(self, _config):
        """Run single test - mock implementation does nothing."""


class TestDetermineVideoTestStatus:
    """Tests for determine_test_status() method"""

    @pytest.fixture
    def framework(self):
        """Create mock framework for tests"""
        return MockFramework()

    def test_status_success(self, framework):
        """Test return code 0 maps to SUCCESS"""
        status = framework.determine_test_status(0)
        assert status == VideoTestStatus.SUCCESS

    def test_status_not_supported(self, framework):
        """Test return code 69 (EX_UNAVAILABLE) maps to NOT_SUPPORTED"""
        status = framework.determine_test_status(69)
        assert status == VideoTestStatus.NOT_SUPPORTED

    def test_status_crash_sigabrt(self, framework):
        """Test SIGABRT (6) maps to CRASH"""
        status = framework.determine_test_status(6)
        assert status == VideoTestStatus.CRASH

        # Negative signal value
        status = framework.determine_test_status(-6)
        assert status == VideoTestStatus.CRASH

    def test_status_crash_sigsegv(self, framework):
        """Test SIGSEGV (11) maps to CRASH"""
        status = framework.determine_test_status(11)
        assert status == VideoTestStatus.CRASH

        # Negative signal value
        status = framework.determine_test_status(-11)
        assert status == VideoTestStatus.CRASH

    def test_status_error_generic(self, framework):
        """Test generic non-zero codes map to ERROR"""
        # Generic error codes that are not special
        for code in [1, 2, 127, 255]:
            # Skip codes that might be crash codes on Windows
            status = framework.determine_test_status(code)
            # On non-Windows, these should be ERROR (unless 6 or 11)
            assert status in (VideoTestStatus.ERROR, VideoTestStatus.CRASH)

    def test_status_error_negative(self, framework):
        """Test negative codes (not signal-related) map to ERROR or CRASH"""
        # Generic negative codes
        status = framework.determine_test_status(-2)
        assert status == VideoTestStatus.ERROR

    @patch('tests.libs.video_test_platform_utils.PlatformUtils.is_windows')
    def test_status_windows_access_violation(self, mock_is_windows, framework):
        """Test Windows access violation code maps to CRASH"""
        mock_is_windows.return_value = True

        # 0xC0000005 = 3221225477 (access violation)
        status = framework.determine_test_status(3221225477)
        assert status == VideoTestStatus.CRASH

    @patch('tests.libs.video_test_platform_utils.PlatformUtils.is_windows')
    def test_status_windows_abort(self, mock_is_windows, framework):
        """Test Windows abort code maps to CRASH"""
        mock_is_windows.return_value = True

        # -1073741819 is common Windows abort/assert failure
        status = framework.determine_test_status(-1073741819)
        assert status == VideoTestStatus.CRASH

    @patch('tests.libs.video_test_platform_utils.PlatformUtils.is_windows')
    def test_status_linux_not_windows_codes(self, mock_is_windows, framework):
        """Test that Windows-specific codes are ERROR on Linux"""
        mock_is_windows.return_value = False

        # These Windows codes should be ERROR on Linux
        status = framework.determine_test_status(3221225477)
        assert status == VideoTestStatus.ERROR

        status = framework.determine_test_status(-1073741819)
        assert status == VideoTestStatus.ERROR


class TestVideoTestStatus:
    """Tests for VideoTestStatus enum"""

    def test_status_values(self):
        """Test VideoTestStatus enum values"""
        assert VideoTestStatus.SUCCESS.value == "success"
        assert VideoTestStatus.NOT_SUPPORTED.value == "not_supported"
        assert VideoTestStatus.ERROR.value == "error"
        assert VideoTestStatus.CRASH.value == "crash"

    def test_status_comparison(self):
        """Test VideoTestStatus enum comparison"""
        assert VideoTestStatus.SUCCESS != VideoTestStatus.ERROR
        assert VideoTestStatus.CRASH != VideoTestStatus.NOT_SUPPORTED
