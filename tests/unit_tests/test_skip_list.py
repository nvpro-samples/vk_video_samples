"""
Unit tests for skip list functionality.

Tests is_test_skipped(), load_skip_list(), and SkipRule dataclass.

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

import json
import tempfile
from pathlib import Path

from tests.libs.video_test_config_base import (
    SkipRule,
    SkipFilter,
    is_test_skipped,
    load_skip_list,
)


class TestSkipRule:
    """Tests for SkipRule dataclass"""

    def test_skip_rule_defaults(self):
        """Test SkipRule default field values"""
        rule = SkipRule(name="test", test_type="decode", format="vvs")

        assert rule.name == "test"
        assert rule.test_type == "decode"
        assert rule.format == "vvs"
        assert rule.drivers == ["all"]
        assert rule.platforms == ["all"]
        assert rule.reproduction == "always"
        assert rule.reason == ""
        assert rule.bug_url == ""
        assert rule.date_added == ""

    def test_skip_rule_custom_values(self):
        """Test SkipRule with custom values"""
        rule = SkipRule(
            name="av1_*_10bit",
            test_type="decode",
            format="vvs",
            drivers=["nvidia", "amd"],
            platforms=["linux"],
            reproduction="flaky",
            reason="10-bit not supported",
            bug_url="https://example.com/bug/123",
            date_added="2025-01-27"
        )

        assert rule.name == "av1_*_10bit"
        assert rule.drivers == ["nvidia", "amd"]
        assert rule.platforms == ["linux"]
        assert rule.reproduction == "flaky"
        assert rule.reason == "10-bit not supported"


class TestIsTestSkipped:
    """Tests for is_test_skipped() function"""

    def test_exact_name_match(self):
        """Test exact name matching"""
        rules = [SkipRule(name="h264_test", test_type="decode", format="vvs")]

        result = is_test_skipped("h264_test", "vvs", rules, test_type="decode")
        assert result is not None
        assert result.name == "h264_test"

    def test_wildcard_pattern_star(self):
        """Test wildcard pattern with asterisk"""
        rules = [SkipRule(
            name="av1_*_10bit", test_type="decode", format="vvs")]

        # Should match
        result = is_test_skipped(
            "av1_basic_10bit", "vvs", rules, test_type="decode")
        assert result is not None

        result = is_test_skipped(
            "av1_advanced_10bit", "vvs", rules, test_type="decode")
        assert result is not None

        # Should not match
        result = is_test_skipped(
            "av1_basic_8bit", "vvs", rules, test_type="decode")
        assert result is None

    def test_wildcard_pattern_question(self):
        """Test wildcard pattern with question mark"""
        rules = [SkipRule(
            name="h264_clip_?", test_type="decode", format="vvs")]

        # Should match single character
        result = is_test_skipped(
            "h264_clip_a", "vvs", rules, test_type="decode")
        assert result is not None

        result = is_test_skipped(
            "h264_clip_b", "vvs", rules, test_type="decode")
        assert result is not None

        # Should not match multiple characters
        result = is_test_skipped(
            "h264_clip_ab", "vvs", rules, test_type="decode")
        assert result is None

    def test_driver_filter_all(self):
        """Test that 'all' driver matches any driver"""
        rules = [SkipRule(name="test", test_type="decode", format="vvs",
                          drivers=["all"])]

        result = is_test_skipped("test", "vvs", rules, current_driver="nvidia",
                                 test_type="decode")
        assert result is not None

        result = is_test_skipped("test", "vvs", rules, current_driver="amd",
                                 test_type="decode")
        assert result is not None

    def test_driver_filter_specific(self):
        """Test specific driver filtering"""
        rules = [SkipRule(name="test", test_type="decode", format="vvs",
                          drivers=["nvidia", "amd"])]

        # Should match specified drivers
        result = is_test_skipped("test", "vvs", rules, current_driver="nvidia",
                                 test_type="decode")
        assert result is not None

        result = is_test_skipped("test", "vvs", rules, current_driver="amd",
                                 test_type="decode")
        assert result is not None

        # Should not match other drivers
        result = is_test_skipped("test", "vvs", rules, current_driver="intel",
                                 test_type="decode")
        assert result is None

    def test_type_mismatch(self):
        """Test that test type must match"""
        rules = [SkipRule(name="test", test_type="decode", format="vvs")]

        # Should match decode
        result = is_test_skipped("test", "vvs", rules, test_type="decode")
        assert result is not None

        # Should not match encode
        result = is_test_skipped("test", "vvs", rules, test_type="encode")
        assert result is None

    def test_format_mismatch(self):
        """Test that format must match"""
        rules = [SkipRule(name="test", test_type="decode", format="vvs")]

        # Should match vvs format
        result = is_test_skipped("test", "vvs", rules, test_type="decode")
        assert result is not None

        # Should not match fluster format
        result = is_test_skipped("test", "fluster", rules, test_type="decode")
        assert result is None

    def test_empty_rules_list(self):
        """Test with empty skip rules list"""
        result = is_test_skipped("any_test", "vvs", [], test_type="decode")
        assert result is None

    def test_multiple_rules_first_match(self):
        """Test that first matching rule is returned"""
        rules = [
            SkipRule(name="test", test_type="decode", format="vvs",
                     reason="First rule"),
            SkipRule(name="test", test_type="decode", format="vvs",
                     reason="Second rule"),
        ]

        result = is_test_skipped("test", "vvs", rules, test_type="decode")
        assert result is not None
        assert result.reason == "First rule"


class TestLoadSkipList:
    """Tests for load_skip_list() function"""

    def test_load_from_valid_json(self):
        """Test loading skip list from valid JSON file (new format)"""
        skip_data = {
            "version": "1.0",
            "decode": [
                {
                    "name": "test_sample",
                    "format": "vvs",
                    "drivers": ["all"],
                    "reason": "Test reason"
                }
            ],
            "encode": []
        }

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json',
                                         delete=False) as f:
            json.dump(skip_data, f)
            temp_path = f.name

        try:
            rules = load_skip_list(temp_path)
            assert len(rules) == 1
            assert rules[0].name == "test_sample"
            assert rules[0].test_type == "decode"
            assert rules[0].reason == "Test reason"
        finally:
            Path(temp_path).unlink()

    def test_load_from_legacy_json(self):
        """Test loading skip list from legacy JSON file format"""
        skip_data = {
            "version": "1.0",
            "skipped_tests": [
                {
                    "name": "legacy_test",
                    "type": "encode",
                    "format": "vvs",
                    "drivers": ["all"],
                    "reason": "Legacy format test"
                }
            ]
        }

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json',
                                         delete=False) as f:
            json.dump(skip_data, f)
            temp_path = f.name

        try:
            rules = load_skip_list(temp_path)
            assert len(rules) == 1
            assert rules[0].name == "legacy_test"
            assert rules[0].test_type == "encode"
            assert rules[0].reason == "Legacy format test"
        finally:
            Path(temp_path).unlink()

    def test_load_missing_file_returns_empty(self):
        """Test that missing file returns empty list"""
        rules = load_skip_list("/nonexistent/path/skip_list.json")
        assert not rules

    def test_load_with_missing_optional_fields(self):
        """Test loading JSON with missing optional fields uses defaults"""
        skip_data = {
            "decode": [],
            "encode": [
                {
                    "name": "minimal_test",
                    "format": "fluster"
                }
            ]
        }

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json',
                                         delete=False) as f:
            json.dump(skip_data, f)
            temp_path = f.name

        try:
            rules = load_skip_list(temp_path)
            assert len(rules) == 1
            assert rules[0].name == "minimal_test"
            assert rules[0].test_type == "encode"
            assert rules[0].drivers == ["all"]  # Default
            assert rules[0].platforms == ["all"]  # Default
            assert rules[0].reproduction == "always"  # Default
        finally:
            Path(temp_path).unlink()

    def test_load_empty_sections(self):
        """Test loading JSON with empty decode and encode sections"""
        skip_data = {"decode": [], "encode": []}

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json',
                                         delete=False) as f:
            json.dump(skip_data, f)
            temp_path = f.name

        try:
            rules = load_skip_list(temp_path)
            assert not rules
        finally:
            Path(temp_path).unlink()

    def test_load_mixed_decode_encode(self):
        """Test loading JSON with both decode and encode entries"""
        skip_data = {
            "decode": [
                {"name": "decode_test", "format": "vvs"}
            ],
            "encode": [
                {"name": "encode_test", "format": "vvs"}
            ]
        }

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json',
                                         delete=False) as f:
            json.dump(skip_data, f)
            temp_path = f.name

        try:
            rules = load_skip_list(temp_path)
            assert len(rules) == 2
            assert rules[0].name == "decode_test"
            assert rules[0].test_type == "decode"
            assert rules[1].name == "encode_test"
            assert rules[1].test_type == "encode"
        finally:
            Path(temp_path).unlink()


class TestSkipFilter:
    """Tests for SkipFilter enum"""

    def test_skip_filter_values(self):
        """Test SkipFilter enum values"""
        assert SkipFilter.ENABLED.value == "enabled"
        assert SkipFilter.SKIPPED.value == "skipped"
        assert SkipFilter.ALL.value == "all"

    def test_skip_filter_comparison(self):
        """Test SkipFilter enum comparison"""
        assert SkipFilter.ENABLED != SkipFilter.SKIPPED
        assert SkipFilter.ENABLED != SkipFilter.ALL
        assert SkipFilter.SKIPPED != SkipFilter.ALL
