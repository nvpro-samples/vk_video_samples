"""
Unit tests for test status determination.

Tests determine_test_status() method return code mapping, and the artifact
check that completes it: a run that exits successfully and produced nothing
is not a pass, and the exit code cannot say so on its own.

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

# check_declared_output is the scoring step for a declared output. Reaching it
# through the public surface would mean running a real encoder against real
# content, which is what the integration suites do; these tests exist to pin
# the scoring down without hardware.
# pylint: disable=protected-access

from unittest.mock import patch

import pytest

from tests.libs.video_test_config_base import (
    BaseTestConfig,
    CodecType,
    ExpectedResult,
    VideoTestStatus,
)
from tests.libs.video_test_output_check import check_declared_output
from tests.unit_tests.mock_framework import MockFramework, make_result


def make_config(expected_result=ExpectedResult.SUCCESS):
    """Build an ordinary encode cell, or by argument a negative one."""
    return BaseTestConfig(
        name="h264_1080p",
        codec=CodecType.H264,
        expected_result=expected_result,
    )


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


class TestDeclaredOutputCheck:
    """The artifact half of the verdict.

    A process that exits 0 has reached its own exit; it has not necessarily
    done the work. An encode that loses the device on its first frame opens
    its bitstream, writes nothing and exits 0, and until the artifact is
    looked at that run is indistinguishable from a complete one.
    """

    @pytest.fixture
    def framework(self):
        """Create mock framework for tests"""
        return MockFramework()

    def test_empty_output_fails_a_zero_exit(self, tmp_path):
        """A 0-byte artifact turns a successful exit into an ERROR."""
        output = tmp_path / "out.264"
        output.write_bytes(b"")
        config = make_config()
        result = make_result(config)

        check_declared_output(result, config, output)

        assert result.status == VideoTestStatus.ERROR
        assert "0 bytes" in result.error_message
        assert result.returncode == 0

    def test_missing_output_fails_a_zero_exit(self, tmp_path):
        """An artifact that was never written is the same failure."""
        output = tmp_path / "never_written.265"
        config = make_config()
        result = make_result(config)

        check_declared_output(result, config, output)

        assert result.status == VideoTestStatus.ERROR
        assert "does not exist" in result.error_message

    def test_non_empty_output_stays_a_pass(self, tmp_path):
        """The ordinary case: bytes were written, the verdict stands."""
        output = tmp_path / "out.ivf"
        output.write_bytes(b"\x00" * 64)
        config = make_config()
        result = make_result(config)

        check_declared_output(result, config, output)

        assert result.status == VideoTestStatus.SUCCESS
        assert result.meta["output_size"] == 64

    def test_no_declared_output_is_not_judged(self):
        """A run that was never asked for an output has nothing to check.

        The decoder runs with no -o whenever there is no golden to compare
        against, and the decode-side validation of an encode never names one.
        """
        config = make_config()
        result = make_result(config)

        check_declared_output(result, config, None)

        assert result.status == VideoTestStatus.SUCCESS
        assert "output_size" not in result.meta

    def test_negative_cell_is_not_judged(self, tmp_path):
        """A cell whose pass condition is a refusal writes no bitstream."""
        output = tmp_path / "out.ivf"
        config = make_config(expected_result=ExpectedResult.UNSUPPORTED)
        result = make_result(config, returncode=69,
                             status=VideoTestStatus.SUCCESS)

        check_declared_output(result, config, output)

        assert result.status == VideoTestStatus.SUCCESS

    def test_an_existing_verdict_is_not_overwritten(self, tmp_path):
        """A more specific status survives; this one only adds to a pass."""
        output = tmp_path / "out.264"
        config = make_config()
        for status in (VideoTestStatus.NOT_SUPPORTED,
                       VideoTestStatus.SKIPPED,
                       VideoTestStatus.CRASH):
            result = make_result(config, status=status)
            check_declared_output(result, config, output)
            assert result.status == status

    def test_suppressed_output_is_not_judged(self, tmp_path):
        """--disableFileOutput asks for no bitstream and wins over -o.

        The encode command always ends in -o, so a run that suppresses the
        bitstream still names a path. It opens that path and leaves it at
        zero bytes, which is the result it was asked for.
        """
        output = tmp_path / "out.264"
        output.write_bytes(b"")
        config = make_config()
        result = make_result(config)
        cmd = ["vk-video-enc", "-i", "in.yuv", "--codec", "h264",
               "--disableFileOutput", "-o", str(output)]

        check_declared_output(result, config, output, cmd)

        assert result.status == VideoTestStatus.SUCCESS
        assert "output_size" not in result.meta

    def test_zero_frames_is_not_judged(self, tmp_path):
        """A run asked for no frames has no bitstream to write."""
        output = tmp_path / "out.265"
        output.write_bytes(b"")
        config = make_config()
        result = make_result(config)
        cmd = ["vk-video-enc", "-i", "in.yuv", "--codec", "h265",
               "--numFrames", "0", "--repeatInputFrames", "-o", str(output)]

        check_declared_output(result, config, output, cmd)

        assert result.status == VideoTestStatus.SUCCESS

    def test_an_ordinary_command_is_still_judged(self, tmp_path):
        """The exemption is read off the command and nothing else.

        A command carrying neither flag is judged exactly as before, and a
        frame count that is not zero is not a frame count of zero.
        """
        output = tmp_path / "out.ivf"
        output.write_bytes(b"")
        config = make_config()
        cmd = ["vk-video-enc", "-i", "in.yuv", "--codec", "av1",
               "--numFrames", "16", "-o", str(output)]

        result = make_result(config)
        check_declared_output(result, config, output, cmd)
        assert result.status == VideoTestStatus.ERROR

        # And with no command at all, which is how the older callers reach it.
        result = make_result(config)
        check_declared_output(result, config, output)
        assert result.status == VideoTestStatus.ERROR
