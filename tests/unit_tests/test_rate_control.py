"""
Unit tests for the rate-control check (tests/libs/video_test_rate_control.py)
and for its wiring into the encode samples.

Copyright 2026 NVIDIA Corporation

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

import json
from pathlib import Path

import pytest

from tests.libs import video_test_rate_control as rc
from tests.libs.video_test_framework_encode import EncodeTestSample

# 240 kbps at 30 fps is exactly 8000 bits, i.e. 1000 bytes, per frame.
BUDGET_BYTES = 1000


def check(**kwargs):
    """A check at 240 kbps and 30 fps with the given limits."""
    return rc.RateControlCheck(target_bitrate=240000, frame_rate=30.0,
                               **kwargs)


class TestRateControlCheck:
    """Parsing of the sample's rate_control_check object"""

    def test_defaults_to_encoder_frame_rate(self):
        """The default frame rate is the encoder's 30000/1001"""
        parsed = rc.RateControlCheck.from_dict({"target_bitrate": 300000})
        assert parsed.frame_rate == pytest.approx(30000 / 1001)
        assert parsed.frame_budget_bits == pytest.approx(10010.0)

    def test_requires_target_bitrate(self):
        """A check without a target cannot measure anything"""
        with pytest.raises(ValueError):
            rc.RateControlCheck.from_dict({"max_frame_ratio": 2.0})

    def test_rejects_unknown_keys(self):
        """A misspelled limit must not silently disable itself"""
        with pytest.raises(ValueError):
            rc.RateControlCheck.from_dict({"target_bitrate": 1,
                                           "max_frame_raito": 2.0})


class TestAnalyzeFrameSizes:
    """The measurements and the limits"""

    def test_on_target_stream_passes(self):
        """Every frame exactly at its budget"""
        report = rc.analyze_frame_sizes(
            [BUDGET_BYTES] * 30,
            check(expected_frames=30, max_bitrate_error=0.01,
                  max_frame_ratio=1.0, vbv_buffer_size=8000,
                  max_vbv_overflows=0),
            decoded_frames=30)
        assert report.passed, report.failures
        assert report.average_bitrate == pytest.approx(240000)
        assert report.bitrate_error == pytest.approx(0.0)
        assert report.mean_frame_ratio == pytest.approx(1.0)
        assert report.stddev_frame_ratio == pytest.approx(0.0)
        assert report.vbv_overflows == 0

    def test_largest_frame_is_found(self):
        """The largest frame and its index are reported"""
        sizes = [BUDGET_BYTES] * 10
        sizes[4] = 3 * BUDGET_BYTES
        report = rc.analyze_frame_sizes(sizes, check(max_frame_ratio=2.0))
        assert report.max_frame_index == 4
        assert report.max_frame_ratio == pytest.approx(3.0)
        assert not report.passed
        assert "frame 4" in report.failures[0]

    def test_bitrate_error_limit(self):
        """An average 20% over target breaks a 10% limit"""
        report = rc.analyze_frame_sizes([1200] * 30,
                                        check(max_bitrate_error=0.10))
        assert report.bitrate_error == pytest.approx(0.20)
        assert not report.passed

    def test_vbv_model(self):
        """A one-frame VBV overflows only when the backlog exceeds it"""
        # Fullness after each frame, in bits: 8000, 16000 (overflow), 8000,
        # 0, 0.
        sizes = [2000, 2000, 0, 0, 1000]
        report = rc.analyze_frame_sizes(
            sizes, check(vbv_buffer_size=8000, max_vbv_overflows=0))
        assert report.vbv_overflows == 1
        assert not report.passed

    def test_frame_count_limits(self):
        """Both the packet count and the decoded count must match"""
        report = rc.analyze_frame_sizes([BUDGET_BYTES] * 29,
                                        check(expected_frames=30),
                                        decoded_frames=28)
        assert len(report.failures) == 2

    def test_limits_that_are_none_are_not_enforced(self):
        """Report-only when no limit is declared"""
        report = rc.analyze_frame_sizes([10 * BUDGET_BYTES] * 5, check())
        assert report.passed
        assert report.max_frame_ratio == pytest.approx(10.0)

    def test_empty_stream_fails(self):
        """No frames is never a pass"""
        report = rc.analyze_frame_sizes([], check())
        assert not report.passed


class TestCheckStream:  # pylint: disable=too-few-public-methods
    """check_stream without a usable ffprobe"""

    def test_missing_ffprobe_fails(self, monkeypatch, tmp_path):
        """An unmeasurable stream fails the check rather than passing it"""
        monkeypatch.setattr(rc, "find_ffprobe", lambda: None)
        report = rc.check_stream(tmp_path / "x.264", check())
        assert not report.passed
        assert "ffprobe" in report.failures[0]


class TestEncodeSamples:
    """The ultra-low-latency (ULL) rate-control cells in encode_samples.json"""

    @staticmethod
    def samples():
        """All encode samples"""
        path = Path(__file__).parent.parent / "encode_samples.json"
        return json.loads(path.read_text())["samples"]

    def test_rate_control_check_is_parsed(self):
        """A sample with the object carries a RateControlCheck"""
        data = next(s for s in self.samples()
                    if s["name"] == "h264_ull_cbr_svbv")
        sample = EncodeTestSample.from_dict(data)
        assert isinstance(sample.rate_control_check, rc.RateControlCheck)
        assert sample.rate_control_check.expected_frames == 90

    def test_sample_without_the_object_has_none(self):
        """Other samples are unaffected"""
        data = next(s for s in self.samples()
                    if s["name"] == "h264_default")
        assert EncodeTestSample.from_dict(data).rate_control_check is None

    @pytest.mark.parametrize("codec", ["h264", "h265", "av1"])
    def test_two_arms_per_codec(self, codec):
        """Control and ULL"""
        names = {s["name"] for s in self.samples()}
        for suffix in ("ll_cbr_svbv", "ull_cbr_svbv"):
            assert f"{codec}_{suffix}" in names

    @pytest.mark.parametrize("codec", ["h264", "h265", "av1"])
    def test_arms_differ_only_in_what_they_test(self, codec):
        """The two arms encode the same stream with the same targets"""
        by_name = {s["name"]: s for s in self.samples()}
        control = by_name[f"{codec}_ll_cbr_svbv"]
        ull = by_name[f"{codec}_ull_cbr_svbv"]

        def strip(args, *drop):
            out, skip = [], False
            for arg in args:
                if skip:
                    skip = False
                    continue
                if arg in drop:
                    skip = True
                    continue
                out.append(arg)
            return out

        assert (strip(control["extra_args"], "--tuningMode") ==
                strip(ull["extra_args"], "--tuningMode"))
        assert control["source_filepath"] == ull["source_filepath"]
        assert (control["rate_control_check"]["target_bitrate"] ==
                ull["rate_control_check"]["target_bitrate"])
