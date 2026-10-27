"""
Unit tests for the verdict of a run in which no test passed.

A run with no failure and no pass verified nothing: on a host with no Vulkan
Video device every cell is not supported, and so is every cell of a broken
build that cannot find its device. Such a run fails unless --allow-no-pass
says nothing passing is expected. One pass, or any failure, decides the run as
it always has.

Copyright 2026 NVIDIA Corporation.

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

import pytest

from tests.libs.video_test_config_base import (
    BaseTestConfig,
    CodecType,
    VideoTestStatus,
)
from tests.libs.video_test_result_reporter import print_final_summary
from tests.unit_tests.mock_framework import MockFramework, make_result
from tests.vvs_test_runner import VulkanVideoTestFramework

NS = VideoTestStatus.NOT_SUPPORTED
OK = VideoTestStatus.SUCCESS
SKIP = VideoTestStatus.SKIPPED
ERR = VideoTestStatus.ERROR
CRASH = VideoTestStatus.CRASH


def run_verdict(statuses, allow_no_pass=False):
    """The runner's verdict over decode results with these statuses.

    With no executable the runner builds no sub-framework; a MockFramework
    stands in for the decode one, so the decode set's own summary and the
    combined one both judge the results given here, as after a real run.
    """
    framework = VulkanVideoTestFramework(
        encoder_path=None, decoder_path=None, allow_no_pass=allow_no_pass)
    framework.decode_framework = MockFramework()
    config = BaseTestConfig(name="h264_1080p", codec=CodecType.H264)
    results = [
        make_result(config, returncode=0 if status == OK else 1, status=status)
        for status in statuses
    ]
    framework.all_results = list(results)
    return framework.print_summary(decode_results=results)


class TestNothingPassed:
    """The run fails when nothing passed, unless allowed"""

    @pytest.mark.parametrize("statuses", [
        [NS, NS, NS],
        [SKIP, SKIP],
        [NS, SKIP],
    ])
    def test_nothing_passed_fails(self, statuses, capsys):
        """No failure and no pass is a failed run by default"""
        assert run_verdict(statuses) is False
        assert "NO TEST PASSED" in capsys.readouterr().out

    @pytest.mark.parametrize("statuses", [
        [NS, NS, NS],
        [SKIP, SKIP],
    ])
    def test_nothing_passed_allowed(self, statuses, capsys):
        """--allow-no-pass accepts a run with no failure and no pass"""
        assert run_verdict(statuses, allow_no_pass=True) is True
        assert "accepted by --allow-no-pass" in capsys.readouterr().out

    def test_one_pass_decides(self):
        """A single pass among unsupported cells passes, as before"""
        assert run_verdict([NS, OK, NS]) is True

    @pytest.mark.parametrize("statuses", [
        [ERR],
        [CRASH, NS],
        [OK, ERR],
    ])
    def test_failure_fails_even_when_allowed(self, statuses):
        """--allow-no-pass never excuses a failure or a crash"""
        assert run_verdict(statuses, allow_no_pass=True) is False


class TestFinalSummaryText:
    """A set with no pass does not claim that its tests passed"""

    def test_no_pass_is_not_all_passed(self, capsys):
        """Zero passes prints NO ... PASSED, not ALL ... PASSED"""
        assert print_final_summary((0, 0, 0, 0), "decoder") is True
        out = capsys.readouterr().out
        assert "NO DECODER TEST PASSED" in out
        assert "ALL DECODER TESTS PASSED" not in out

    def test_passes_still_reported(self, capsys):
        """A set with passes reports them as before"""
        assert print_final_summary((3, 0, 0, 0), "encoder") is True
        assert "ALL ENCODER TESTS PASSED" in capsys.readouterr().out
