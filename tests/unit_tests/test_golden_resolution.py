"""
Unit tests for golden selection: per-GPU maps and the portable Y4M golden.

A raw golden hashes the decoded SURFACE, whose byte layout follows the decode
output image, so the same pixels can hash differently on another architecture:
output that is sample-exact against an independent decoder still fails a raw
golden minted on a different GPU. Two mechanisms cover that: a Y4M golden
(planar and self-describing, so portable), and a per-GPU map for values that
legitimately differ.

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

from tests.libs.video_test_framework_decode import (
    DecodeTestSample,
    resolve_expected_md5,
)

BLACKWELL = "NVIDIA GeForce RTX 5080"
AMPERE = "NVIDIA GeForce RTX 3080 Ti"


class TestResolveExpectedMd5:
    """Selection of the golden that applies to the GPU under test."""

    def test_plain_string_applies_everywhere(self):
        """A scalar golden is used regardless of GPU."""
        assert resolve_expected_md5("abc123", AMPERE) == "abc123"
        assert resolve_expected_md5("abc123", "") == "abc123"

    def test_empty_stays_empty(self):
        """No golden means no check; None must not crash."""
        assert resolve_expected_md5("", AMPERE) == ""
        assert resolve_expected_md5(None, AMPERE) == ""

    def test_map_selects_by_gpu_fragment(self):
        """Each architecture picks up its own value."""
        golden = {"RTX 50": "blackwell", "RTX 30": "ampere"}
        assert resolve_expected_md5(golden, BLACKWELL) == "blackwell"
        assert resolve_expected_md5(golden, AMPERE) == "ampere"

    def test_unmatched_gpu_falls_back_to_default(self):
        """A GPU with no entry uses default when one is offered."""
        golden = {"RTX 50": "blackwell", "default": "other"}
        assert resolve_expected_md5(golden, AMPERE) == "other"

    def test_unmatched_gpu_without_default_yields_no_check(self):
        """Without a default, an unlisted GPU simply has no golden.

        Better than asserting someone else's hash: a raw golden from another
        architecture would fail on correct pixels.
        """
        assert resolve_expected_md5({"RTX 50": "blackwell"}, AMPERE) == ""

    def test_longest_fragment_wins(self):
        """A specific model overrides a broader family entry."""
        golden = {"RTX 30": "family", "RTX 3080 Ti": "specific"}
        assert resolve_expected_md5(golden, AMPERE) == "specific"

    def test_matching_is_case_insensitive(self):
        """GPU-name casing varies between sources."""
        assert resolve_expected_md5({"rtx 5080": "x"}, BLACKWELL) == "x"


class TestDecodeSampleGoldens:
    """Parsing of both golden fields."""

    @staticmethod
    def _sample(**extra):
        """Build a DecodeTestSample from a minimal cell definition."""
        data = {"name": "c", "codec": "h265", "source_url": "",
                "source_checksum": "", "source_filepath": "f.265"}
        data.update(extra)
        return DecodeTestSample.from_dict(data)

    def test_defaults_are_empty(self):
        """A cell with no goldens carries neither."""
        sample = self._sample()
        assert sample.expected_output_md5 == ""
        assert sample.expected_output_y4m_md5 == ""

    def test_both_fields_round_trip(self):
        """Raw and Y4M goldens are parsed independently."""
        sample = self._sample(expected_output_md5="raw",
                              expected_output_y4m_md5="y4m")
        assert sample.expected_output_md5 == "raw"
        assert sample.expected_output_y4m_md5 == "y4m"

    def test_raw_golden_may_be_a_per_gpu_map(self):
        """The map survives parsing so it can be resolved at run time."""
        sample = self._sample(expected_output_md5={"RTX 50": "bw"})
        assert resolve_expected_md5(
            sample.expected_output_md5, BLACKWELL) == "bw"
