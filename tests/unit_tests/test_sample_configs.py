"""
Unit tests for sample configuration classes.

Tests DecodeTestSample, EncodeTestSample, and CodecType.

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

import pytest

from tests.libs.video_test_config_base import CodecType, BaseTestConfig
from tests.libs.video_test_framework_decode import DecodeTestSample
from tests.libs.video_test_framework_encode import EncodeTestSample


class TestCodecType:
    """Tests for CodecType enum"""

    def test_codec_type_values(self):
        """Test CodecType enum values"""
        assert CodecType.H264.value == "h264"
        assert CodecType.H265.value == "h265"
        assert CodecType.AV1.value == "av1"
        assert CodecType.VP9.value == "vp9"

    def test_codec_type_from_string(self):
        """Test creating CodecType from string"""
        assert CodecType("h264") == CodecType.H264
        assert CodecType("h265") == CodecType.H265
        assert CodecType("av1") == CodecType.AV1
        assert CodecType("vp9") == CodecType.VP9

    def test_codec_type_invalid_string(self):
        """Test that invalid string raises ValueError"""
        with pytest.raises(ValueError):
            CodecType("invalid")


class TestDecodeTestSample:
    """Tests for DecodeTestSample class"""

    def test_from_dict_valid(self):
        """Test creating DecodeTestSample from valid dictionary"""
        data = {
            "name": "h264_test",
            "codec": "h264",
            "description": "Test description",
            "source_url": "https://example.com/test.h264",
            "source_checksum": "abc123",
            "source_filepath": "video/test.h264",
            "expected_output_md5": "def456",
        }

        sample = DecodeTestSample.from_dict(data)

        assert sample.name == "h264_test"
        assert sample.codec == CodecType.H264
        assert sample.description == "Test description"
        assert sample.source_url == "https://example.com/test.h264"
        assert sample.source_checksum == "abc123"
        assert sample.source_filepath == "video/test.h264"
        assert sample.expected_output_md5 == "def456"

    def test_from_dict_optional_fields(self):
        """Test creating DecodeTestSample with missing optional fields"""
        data = {
            "name": "minimal_test",
            "codec": "av1",
            "source_url": "https://example.com/test.ivf",
            "source_checksum": "abc123",
            "source_filepath": "video/test.ivf",
        }

        sample = DecodeTestSample.from_dict(data)

        assert sample.name == "minimal_test"
        assert sample.codec == CodecType.AV1
        assert sample.description == ""  # Default
        assert sample.expected_output_md5 == ""  # Default

    def test_from_dict_missing_required_field(self):
        """Test that missing required field raises KeyError"""
        data = {
            "name": "test",
            # Missing 'codec'
            "source_url": "https://example.com/test.h264",
            "source_checksum": "abc123",
            "source_filepath": "video/test.h264",
        }

        with pytest.raises(KeyError):
            DecodeTestSample.from_dict(data)

    def test_display_name_prefix(self):
        """Test that display_name adds decode_ prefix"""
        sample = DecodeTestSample(
            name="h264_test",
            codec=CodecType.H264,
            source_url="",
            source_checksum="",
            source_filepath="",
        )

        assert sample.display_name == "decode_h264_test"

    def test_display_name_different_codecs(self):
        """Test display_name with different codecs"""
        samples = [
            DecodeTestSample(name="test", codec=CodecType.H264,
                             source_url="", source_checksum="",
                             source_filepath=""),
            DecodeTestSample(name="test", codec=CodecType.H265,
                             source_url="", source_checksum="",
                             source_filepath=""),
            DecodeTestSample(name="test", codec=CodecType.AV1,
                             source_url="", source_checksum="",
                             source_filepath=""),
        ]

        # All should have same display_name regardless of codec
        for sample in samples:
            assert sample.display_name == "decode_test"


class TestEncodeTestSample:
    """Tests for EncodeTestSample class"""

    def test_from_dict_valid(self):
        """Test creating EncodeTestSample from valid dictionary"""
        data = {
            "name": "h264_main",
            "codec": "h264",
            "profile": "main",
            "description": "Main profile test",
            "width": 352,
            "height": 288,
            "source_url": "https://example.com/test.yuv",
            "source_checksum": "abc123",
            "source_filepath": "video/test.yuv",
        }

        sample = EncodeTestSample.from_dict(data)

        assert sample.name == "h264_main"
        assert sample.codec == CodecType.H264
        assert sample.profile == "main"
        assert sample.width == 352
        assert sample.height == 288
        assert sample.source_filepath == "video/test.yuv"

    def test_from_dict_optional_fields(self):
        """Test creating EncodeTestSample with missing optional fields"""
        data = {
            "name": "minimal_test",
            "codec": "h265",
            "source_url": "https://example.com/test.yuv",
            "source_checksum": "abc123",
            "source_filepath": "video/test.yuv",
        }

        sample = EncodeTestSample.from_dict(data)

        assert sample.name == "minimal_test"
        assert sample.profile is None  # Default
        assert sample.width == 0  # Default
        assert sample.height == 0  # Default
        assert sample.source_format == "yuv"  # Default

    def test_from_dict_with_extra_args(self):
        """Test creating EncodeTestSample with extra_args"""
        data = {
            "name": "cbr_test",
            "codec": "h264",
            "extra_args": [
                "--rateControlMode", "cbr", "--averageBitrate", "2000000"],
            "source_url": "https://example.com/test.yuv",
            "source_checksum": "abc123",
            "source_filepath": "video/test.yuv",
        }

        sample = EncodeTestSample.from_dict(data)

        assert sample.extra_args == ["--rateControlMode", "cbr",
                                     "--averageBitrate", "2000000"]

    def test_display_name_prefix(self):
        """Test that display_name adds encode_ prefix"""
        sample = EncodeTestSample(
            name="h264_test",
            codec=CodecType.H264,
            source_url="",
            source_checksum="",
            source_filepath="",
        )

        assert sample.display_name == "encode_h264_test"

    def test_from_dict_missing_required_field(self):
        """Test that missing required field raises KeyError"""
        data = {
            "name": "test",
            # Missing 'codec'
            "source_url": "https://example.com/test.yuv",
            "source_checksum": "abc123",
            "source_filepath": "video/test.yuv",
        }

        with pytest.raises(KeyError):
            EncodeTestSample.from_dict(data)


class TestBaseTestConfig:
    """Tests for BaseTestConfig base class"""

    def test_default_values(self):
        """Test BaseTestConfig default values"""
        config = BaseTestConfig(
            name="test",
            codec=CodecType.H264,
        )

        assert config.name == "test"
        assert config.codec == CodecType.H264
        assert config.extra_args is None
        assert config.description == ""
        assert config.timeout is None
        assert config.source_url == ""
        assert config.source_checksum == ""
        assert config.source_filepath == ""

    def test_custom_values(self):
        """Test BaseTestConfig with custom values"""
        config = BaseTestConfig(
            name="custom_test",
            codec=CodecType.H265,
            extra_args=["--arg1", "value1"],
            description="Custom description",
            timeout=60,
            source_url="https://example.com/test.h265",
            source_checksum="abc123",
            source_filepath="video/test.h265",
        )

        assert config.name == "custom_test"
        assert config.codec == CodecType.H265
        assert config.extra_args == ["--arg1", "value1"]
        assert config.description == "Custom description"
        assert config.timeout == 60
        assert config.source_url == "https://example.com/test.h265"
        assert config.source_checksum == "abc123"
        assert config.source_filepath == "video/test.h265"
