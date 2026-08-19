"""
Unit tests for negative test cells (expected_result: "unsupported").

A negative cell asserts that the device REJECTS a profile it cannot
implement -- VP9 Profile 3, H.264 4:4:4 decode, 12-bit encode. The risk such
a cell guards against is not a crash but a silence: without it, "correctly
rejected" and
"never tested" produce the same empty result, and the day a driver starts
accepting one of these profiles and emitting garbage, nothing goes red.

These tests pin down the scoring, including the two ways a negative cell must
FAIL -- the profile being accepted, and the rejection carrying a different
VkResult than the one claimed.

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

# The methods under test are the framework's internal scoring entry points --
# _validate_test_result() and _parse_base_fields(). Testing them through the public
# surface would mean running a real decoder against real content, which is what the
# integration suites do; these tests exist precisely to pin the scoring down without
# hardware. Same disable, for the same reason, as tests/vvs_test_runner.py.
# pylint: disable=protected-access

import pytest

from tests.libs.video_test_config_base import (
    BaseTestConfig,
    CodecType,
    ExpectedResult,
    TestResult,
    VideoTestStatus,
)
from tests.unit_tests.mock_framework import MockFramework

# What the apps actually print when the query rejects a profile, copied from a
# real run on an RTX 5080. Both spellings of the result appear.
REJECTION_OUTPUT = (
    "*** Selected Vulkan physical device with name: RTX 5080 ***\n"
    "ERROR [VulkanVideoCapabilities.h:58]: GetVideoDecodeCapabilities() "
    "failed. "
    "result: 0xc464dc25 (-1000023003)\n"
    "*** Could not get Video Capabilities :-1000023003 ***\n"
)
FORMAT_NOT_SUPPORTED = "VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR"
CODEC_NOT_SUPPORTED = "VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR"


def make_config(expected_result=ExpectedResult.UNSUPPORTED,
                expected_vk_result=FORMAT_NOT_SUPPORTED):
    """Build a config for a negative (or, by argument, a normal) cell."""
    return BaseTestConfig(
        name="vp9_profile3_12_422_4k_unsupported",
        codec=CodecType.VP9,
        expected_result=expected_result,
        expected_vk_result=expected_vk_result,
    )


def make_result(config, returncode, status, stdout="", stderr=""):
    """Build a TestResult as execute_test_command would."""
    return TestResult(
        config=config,
        returncode=returncode,
        execution_time=0.0,
        status=status,
        stdout=stdout,
        stderr=stderr,
    )


class TestExpectedRejectionScoring:
    """Scoring of expected_result: unsupported cells."""

    @pytest.fixture
    def framework(self):
        """Framework instance for the scoring tests."""
        return MockFramework()

    def test_clean_rejection_with_expected_result_passes(self, framework):
        """Exit 69 carrying the claimed VkResult is the pass condition."""
        result = make_result(make_config(), 69,
                             VideoTestStatus.NOT_SUPPORTED,
                             stdout=REJECTION_OUTPUT)
        framework._validate_test_result(result)
        assert result.status == VideoTestStatus.SUCCESS
        assert result.meta["expected_rejection"] is True
        assert FORMAT_NOT_SUPPORTED in result.meta["rejection_note"]

    def test_hex_only_output_still_matches(self, framework):
        """The hex spelling of the VkResult counts as reporting it."""
        result = make_result(make_config(), 69,
                             VideoTestStatus.NOT_SUPPORTED,
                             stderr="*** Selected Vulkan physical device ***"
                                    "\nVkResult: 0xC464DC25\n")
        framework._validate_test_result(result)
        assert result.status == VideoTestStatus.SUCCESS

    def test_successful_run_fails_the_cell(self, framework):
        """The case that matters: the driver ACCEPTED a profile it cannot do.

        No positive cell can catch this -- there is no positive cell for a
        profile the hardware does not implement.
        """
        result = make_result(make_config(), 0, VideoTestStatus.SUCCESS)
        framework._validate_test_result(result)
        assert result.status == VideoTestStatus.ERROR
        assert "completed successfully" in result.error_message

    def test_rejection_with_a_different_vk_result_fails(self, framework):
        """Rejected, but not for the stated reason, is not a pass.

        Otherwise a missing file or an unrelated capability failure would score
        as "correctly unsupported".
        """
        config = make_config(expected_vk_result=CODEC_NOT_SUPPORTED)
        result = make_result(config, 69, VideoTestStatus.NOT_SUPPORTED,
                             stdout=REJECTION_OUTPUT)
        framework._validate_test_result(result)
        assert result.status == VideoTestStatus.ERROR
        assert CODEC_NOT_SUPPORTED in result.error_message

    def test_no_video_device_is_neutral_not_a_failure(self, framework):
        """A machine with no Vulkan video device cannot answer the question.

        A CI runner with no ICD exits EX_UNAVAILABLE for every cell. Scoring a
        negative cell as failed there would be reporting on the environment
        rather than on the driver, so it stays NOT_SUPPORTED like the rest.
        """
        result = make_result(make_config(), 69,
                             VideoTestStatus.NOT_SUPPORTED,
                             stderr="Video decode queue family not supported\n")
        framework._validate_test_result(result)
        assert result.status == VideoTestStatus.NOT_SUPPORTED
        assert "no video device was selected" in result.error_message

    def test_skipped_cell_is_left_alone(self, framework):
        """Content absent -> the cell never ran, so it cannot be scored.

        Without this, a negative cell whose generated content is missing is
        reported as "expected a rejection, got skipped" -- a failure invented by
        the scoring rather than observed from the device.
        """
        result = make_result(make_config(), 0, VideoTestStatus.SKIPPED)
        framework._validate_test_result(result)
        assert result.status == VideoTestStatus.SKIPPED

    def test_crash_fails_the_cell(self, framework):
        """An abort is not a clean rejection, even on an unsupported profile.

        An app that assert()s on the capability failure and dies (SIGABRT,
        exit 134) has reported nothing, so it must not score as a pass.
        """
        result = make_result(make_config(), -6, VideoTestStatus.CRASH,
                             stdout=REJECTION_OUTPUT)
        framework._validate_test_result(result)
        assert result.status == VideoTestStatus.ERROR
        assert "clean capability rejection" in result.error_message

    def test_generic_error_fails_the_cell(self, framework):
        """A plain failure is not a rejection either."""
        result = make_result(make_config(), 1, VideoTestStatus.ERROR)
        framework._validate_test_result(result)
        assert result.status == VideoTestStatus.ERROR
        assert "clean capability rejection" in result.error_message

    def test_rejection_without_a_claimed_vk_result_passes(self, framework):
        """expected_vk_result is optional; exit 69 alone then suffices."""
        config = make_config(expected_vk_result="")
        result = make_result(config, 69, VideoTestStatus.NOT_SUPPORTED)
        framework._validate_test_result(result)
        assert result.status == VideoTestStatus.SUCCESS

    def test_normal_cell_is_untouched(self, framework):
        """A cell without expected_result keeps the ordinary scoring."""
        config = make_config(expected_result=ExpectedResult.SUCCESS,
                             expected_vk_result="")
        result = make_result(config, 69, VideoTestStatus.NOT_SUPPORTED)
        framework._validate_test_result(result)
        assert result.status == VideoTestStatus.NOT_SUPPORTED
        assert "expected_rejection" not in result.meta


class TestExpectedResultParsing:
    """Parsing of the new fields out of the sample JSON."""

    def test_defaults_to_success(self):
        """Cells that say nothing are ordinary positive cells."""
        parsed = BaseTestConfig._parse_base_fields({
            "name": "x", "codec": "h264", "source_url": "",
            "source_checksum": "", "source_filepath": "",
        })
        assert parsed["expected_result"] == ExpectedResult.SUCCESS
        assert parsed["expected_vk_result"] == ""

    def test_unsupported_is_parsed(self):
        """A negative cell round-trips through _parse_base_fields."""
        parsed = BaseTestConfig._parse_base_fields({
            "name": "x", "codec": "vp9", "source_url": "",
            "source_checksum": "", "source_filepath": "",
            "expected_result": "unsupported",
            "expected_vk_result": FORMAT_NOT_SUPPORTED,
        })
        assert parsed["expected_result"] == ExpectedResult.UNSUPPORTED
        assert parsed["expected_vk_result"] == FORMAT_NOT_SUPPORTED

    def test_unknown_vk_result_name_is_rejected(self):
        """A typo in expected_vk_result must not silently disable the check."""
        with pytest.raises(ValueError, match="unknown expected_vk_result"):
            BaseTestConfig._parse_base_fields({
                "name": "x", "codec": "vp9", "source_url": "",
                "source_checksum": "", "source_filepath": "",
                "expected_result": "unsupported",
                "expected_vk_result": "VK_ERROR_TYPO_NOT_SUPPORTED_KHR",
            })

    def test_unknown_expected_result_is_rejected(self):
        """Likewise for the expected_result value itself."""
        with pytest.raises(ValueError):
            BaseTestConfig._parse_base_fields({
                "name": "x", "codec": "vp9", "source_url": "",
                "source_checksum": "", "source_filepath": "",
                "expected_result": "xfail",
            })
