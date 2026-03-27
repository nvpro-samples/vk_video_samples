"""
Unit tests for test suite filtering functionality.

Tests filter_test_suite() method and pattern matching behavior.

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

from dataclasses import dataclass
from typing import Optional

from tests.libs.video_test_config_base import (
    CodecType,
    SkipFilter,
    SkipRule,
)
from tests.libs.video_test_framework_base import VulkanVideoTestFrameworkBase


@dataclass
class MockSample:
    """Mock sample for testing filter_test_suite"""
    name: str
    codec: CodecType
    _display_name: Optional[str] = None

    @property
    def display_name(self) -> str:
        """Return display name with prefix if not explicitly set"""
        if self._display_name:
            return self._display_name
        return f"decode_{self.name}"


class MockFramework(VulkanVideoTestFrameworkBase):
    """Mock framework for testing filter_test_suite"""

    def __init__(self, skip_rules=None):
        super().__init__(executable_path=None)
        self._skip_rules = skip_rules or []
        self._options['skip_filter'] = SkipFilter.ENABLED

    @property
    def skip_filter(self) -> SkipFilter:
        """Return the current skip filter mode."""
        return self._options['skip_filter']

    def set_skip_filter(self, mode: SkipFilter):
        """Set the skip filter mode."""
        self._options['skip_filter'] = mode

    def set_filter(self, codec_filter=None, test_pattern=None):
        """Set codec and test pattern filters."""
        self._options['codec_filter'] = codec_filter
        self._options['test_pattern'] = test_pattern

    def check_resources(self, _auto_download=True, _test_configs=None):
        """Check resources - mock implementation always returns True."""
        return True

    def create_test_suite(self):
        """Create test suite - mock implementation returns empty list."""
        return []

    def run_single_test(self, _config):
        """Run single test - mock implementation does nothing."""


class TestFilterByCodec:
    """Tests for codec filtering"""

    def test_filter_single_codec(self):
        """Test filtering by single codec"""
        framework = MockFramework()
        samples = [
            MockSample(name="test1", codec=CodecType.H264),
            MockSample(name="test2", codec=CodecType.H265),
            MockSample(name="test3", codec=CodecType.H264),
            MockSample(name="test4", codec=CodecType.AV1),
        ]

        framework.set_filter(codec_filter="h264")
        result = framework.filter_test_suite(samples)
        assert len(result) == 2
        assert all(s.codec == CodecType.H264 for s in result)

    def test_filter_no_codec_returns_all(self):
        """Test that no codec filter returns all samples"""
        framework = MockFramework()
        samples = [
            MockSample(name="test1", codec=CodecType.H264),
            MockSample(name="test2", codec=CodecType.H265),
        ]

        result = framework.filter_test_suite(samples)
        assert len(result) == 2


class TestFilterByPattern:
    """Tests for pattern matching"""

    def test_filter_by_display_name_prefix(self):
        """Test filtering by display name with prefix"""
        framework = MockFramework()
        samples = [
            MockSample(name="h264_test", codec=CodecType.H264),
            MockSample(name="h265_test", codec=CodecType.H265),
        ]

        framework.set_filter(test_pattern="decode_h264_*")
        result = framework.filter_test_suite(samples)
        assert len(result) == 1
        assert result[0].name == "h264_test"

    def test_filter_by_base_name(self):
        """Test filtering by base name without prefix"""
        framework = MockFramework()
        samples = [
            MockSample(name="h264_test", codec=CodecType.H264),
            MockSample(name="h265_test", codec=CodecType.H265),
        ]

        framework.set_filter(test_pattern="h264_*")
        result = framework.filter_test_suite(samples)
        assert len(result) == 1
        assert result[0].name == "h264_test"

    def test_filter_exact_match_base_name(self):
        """Test exact match by base name"""
        framework = MockFramework()
        samples = [
            MockSample(name="h264_test", codec=CodecType.H264),
            MockSample(name="h264_test_extended", codec=CodecType.H264),
        ]

        framework.set_filter(test_pattern="h264_test")
        result = framework.filter_test_suite(samples)
        assert len(result) == 1
        assert result[0].name == "h264_test"

    def test_filter_exact_match_display_name(self):
        """Test exact match by display name"""
        framework = MockFramework()
        samples = [
            MockSample(name="h264_test", codec=CodecType.H264),
        ]

        framework.set_filter(test_pattern="decode_h264_test")
        result = framework.filter_test_suite(samples)
        assert len(result) == 1

    def test_filter_wildcard_matches_multiple(self):
        """Test wildcard pattern matching multiple samples"""
        framework = MockFramework()
        samples = [
            MockSample(name="av1_basic_8bit", codec=CodecType.AV1),
            MockSample(name="av1_basic_10bit", codec=CodecType.AV1),
            MockSample(name="av1_advanced_8bit", codec=CodecType.AV1),
            MockSample(name="h264_basic", codec=CodecType.H264),
        ]

        framework.set_filter(test_pattern="av1_*")
        result = framework.filter_test_suite(samples)
        assert len(result) == 3
        assert all(s.codec == CodecType.AV1 for s in result)


class TestFilterBySkipList:
    """Tests for skip list filtering"""

    def test_skip_mode_enabled_marks_skipped(self):
        """Test ENABLED mode includes skipped tests but marks them"""
        skip_rules = [
            SkipRule(name="skipped_test", test_type="decode", format="vvs")
        ]
        framework = MockFramework(skip_rules=skip_rules)
        framework.set_skip_filter(SkipFilter.ENABLED)

        samples = [
            MockSample(name="skipped_test", codec=CodecType.H264),
            MockSample(name="allowed_test", codec=CodecType.H264),
        ]

        result = framework.filter_test_suite(
            samples, skip_filter=SkipFilter.ENABLED,
            test_format="vvs", test_type="decode"
        )
        # Both tests are in the result, but skipped_test is marked
        assert len(result) == 2
        assert "skipped_test" in framework.skipped_samples
        assert "allowed_test" not in framework.skipped_samples

    def test_skip_mode_skipped_only_skipped(self):
        """Test SKIPPED mode runs only skipped tests"""
        skip_rules = [
            SkipRule(name="skipped_test", test_type="decode", format="vvs")
        ]
        framework = MockFramework(skip_rules=skip_rules)

        samples = [
            MockSample(name="skipped_test", codec=CodecType.H264),
            MockSample(name="allowed_test", codec=CodecType.H264),
        ]

        result = framework.filter_test_suite(
            samples, skip_filter=SkipFilter.SKIPPED,
            test_format="vvs", test_type="decode"
        )
        assert len(result) == 1
        assert result[0].name == "skipped_test"

    def test_skip_mode_all_includes_everything(self):
        """Test ALL mode includes both skipped and non-skipped"""
        skip_rules = [
            SkipRule(name="skipped_test", test_type="decode", format="vvs")
        ]
        framework = MockFramework(skip_rules=skip_rules)

        samples = [
            MockSample(name="skipped_test", codec=CodecType.H264),
            MockSample(name="allowed_test", codec=CodecType.H264),
        ]

        result = framework.filter_test_suite(
            samples, skip_filter=SkipFilter.ALL,
            test_format="vvs", test_type="decode"
        )
        assert len(result) == 2

    def test_exact_match_overrides_skip_in_enabled_mode(self):
        """Test that exact match can run skipped test in ENABLED mode"""
        skip_rules = [
            SkipRule(name="skipped_test", test_type="decode", format="vvs")
        ]
        framework = MockFramework(skip_rules=skip_rules)

        samples = [
            MockSample(name="skipped_test", codec=CodecType.H264),
            MockSample(name="allowed_test", codec=CodecType.H264),
        ]

        # Exact match by base name should override skip
        framework.set_filter(test_pattern="skipped_test")
        result = framework.filter_test_suite(
            samples, skip_filter=SkipFilter.ENABLED,
            test_format="vvs", test_type="decode"
        )
        assert len(result) == 1
        assert result[0].name == "skipped_test"
        # Verify it's NOT marked as skipped (so it will actually run)
        assert "skipped_test" not in framework.skipped_samples


class TestFilterCombined:
    """Tests for combined filtering"""

    def test_codec_and_pattern_combined(self):
        """Test codec and pattern filters combined"""
        framework = MockFramework()
        samples = [
            MockSample(name="h264_basic", codec=CodecType.H264),
            MockSample(name="h264_advanced", codec=CodecType.H264),
            MockSample(name="h265_basic", codec=CodecType.H265),
        ]

        framework.set_filter(
            codec_filter="h264", test_pattern="*_basic")
        result = framework.filter_test_suite(samples)
        assert len(result) == 1
        assert result[0].name == "h264_basic"

    def test_empty_samples_list(self):
        """Test filtering empty samples list"""
        framework = MockFramework()
        result = framework.filter_test_suite([])
        assert not result

    def test_no_matches_returns_empty(self):
        """Test that no matches returns empty list"""
        framework = MockFramework()
        samples = [
            MockSample(name="test1", codec=CodecType.H264),
        ]

        framework.set_filter(test_pattern="nonexistent_*")
        result = framework.filter_test_suite(samples)
        assert not result
