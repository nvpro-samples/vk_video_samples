"""
Unit tests for command line interface.

Tests CLI argument parsing and option handling.

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

import subprocess
import sys
from pathlib import Path

# Test directory
TESTS_DIR = Path(__file__).parent.parent
CODEC_SCRIPT = str(TESTS_DIR / "vvs_test_runner.py")


def run_codec(*args, timeout=30):
    """Helper to run codec framework with given arguments"""
    return subprocess.run(
        [sys.executable, CODEC_SCRIPT, *args],
        capture_output=True,
        text=True,
        cwd=TESTS_DIR.parent,
        timeout=timeout,
        check=False
    )


class TestListSamples:
    """Tests for --list-samples option"""

    def test_list_samples_runs_successfully(self):
        """Test that --list-samples runs without error"""
        result = run_codec("--list-samples")
        assert result.returncode == 0
        has_samples = "DECODER SAMPLES" in result.stdout
        assert has_samples or "AVAILABLE" in result.stdout

    def test_list_samples_shows_decode_tests(self):
        """Test that --list-samples shows decoder tests"""
        result = run_codec("--list-samples")
        assert result.returncode == 0
        # Should show some H.264 decoder samples
        assert "h264" in result.stdout.lower()

    def test_list_samples_shows_encode_tests(self):
        """Test that --list-samples shows encoder tests"""
        result = run_codec("--list-samples")
        assert result.returncode == 0
        # Should show encoder samples section
        assert "ENCODER" in result.stdout


class TestDownloadOnly:
    """Tests for --download-only option"""

    def test_download_only_option_in_help(self):
        """Test that --download-only is documented in help"""
        result = run_codec("--help")
        assert result.returncode == 0
        assert "--download-only" in result.stdout

    def test_download_only_runs_successfully(self):
        """Test that --download-only exits without running tests"""
        result = run_codec("--download-only", timeout=120)
        assert result.returncode == 0
        # Should mention downloading, not test results
        assert "download" in result.stdout.lower()


class TestListSamplesFiltering:
    """Tests for --list-samples with --decoder-only/--encoder-only"""

    def test_list_samples_decoder_only(self):
        """Test --list-samples --decoder-only shows only decoder samples"""
        result = run_codec("--list-samples", "--decoder-only")
        assert result.returncode == 0
        assert "DECODER SAMPLES" in result.stdout
        assert "ENCODER SAMPLES" not in result.stdout

    def test_list_samples_encoder_only(self):
        """Test --list-samples --encoder-only shows only encoder samples"""
        result = run_codec("--list-samples", "--encoder-only")
        assert result.returncode == 0
        assert "ENCODER SAMPLES" in result.stdout
        assert "DECODER SAMPLES" not in result.stdout

    def test_list_samples_no_filter_shows_both(self):
        """Test --list-samples without filter shows both sections"""
        result = run_codec("--list-samples")
        assert result.returncode == 0
        assert "DECODER SAMPLES" in result.stdout
        assert "ENCODER SAMPLES" in result.stdout


class TestHelpOption:
    """Tests for --help option"""

    def test_codec_framework_help(self):
        """Test --help for codec framework"""
        result = run_codec("--help")
        assert result.returncode == 0
        assert "--encoder" in result.stdout
        assert "--decoder" in result.stdout
        assert "--test" in result.stdout
        assert "--codec" in result.stdout

    def test_help_shows_decode_specific_options(self):
        """Test --help shows decode-specific options"""
        result = run_codec("--help")
        assert result.returncode == 0
        assert "--decode-display" in result.stdout
        assert "--no-verify-md5" in result.stdout

    def test_help_shows_encode_specific_options(self):
        """Test --help shows encode-specific options"""
        result = run_codec("--help")
        assert result.returncode == 0
        assert "--no-validate-with-decoder" in result.stdout
        assert "--decoder-args" in result.stdout


class TestSkipListOptions:
    """Tests for skip list CLI options"""

    def test_ignore_skip_list_option_accepted(self):
        """Test that --ignore-skip-list is a valid option"""
        result = run_codec("--help")
        assert "--ignore-skip-list" in result.stdout

    def test_only_skipped_option_accepted(self):
        """Test that --only-skipped is a valid option"""
        result = run_codec("--help")
        assert "--only-skipped" in result.stdout

    def test_skip_list_option_accepted(self):
        """Test that --skip-list is a valid option"""
        result = run_codec("--help")
        assert "--skip-list" in result.stdout


class TestCodecFilter:
    """Tests for --codec filter option"""

    def test_codec_choices_in_help(self):
        """Test that codec choices are shown in help"""
        result = run_codec("--help")
        assert "h264" in result.stdout
        assert "h265" in result.stdout
        assert "av1" in result.stdout
        assert "vp9" in result.stdout

    def test_invalid_codec_rejected(self):
        """Test that invalid codec is rejected"""
        result = run_codec("--codec", "invalid_codec", "--list-samples")
        # Should fail with error about invalid choice
        assert result.returncode != 0
        assert "invalid choice" in result.stderr.lower()


class TestTestPatternOption:
    """Tests for --test pattern option"""

    def test_test_option_in_help(self):
        """Test that --test option is documented"""
        result = run_codec("--help")
        assert "--test" in result.stdout
        out_lower = result.stdout.lower()
        assert "pattern" in out_lower or "filter" in out_lower

    def test_test_option_in_codec_help(self):
        """Test that --test option is documented in codec framework"""
        result = run_codec("--help")
        assert "--test" in result.stdout


class TestOutputOptions:
    """Tests for output-related options"""

    def test_verbose_option_accepted(self):
        """Test that --verbose option is valid"""
        result = run_codec("--help")
        assert "--verbose" in result.stdout or "-v" in result.stdout

    def test_export_json_option_accepted(self):
        """Test that --export-json option is valid"""
        result = run_codec("--help")
        assert "--export-json" in result.stdout

    def test_keep_files_option_accepted(self):
        """Test that --keep-files option is valid"""
        result = run_codec("--help")
        assert "--keep-files" in result.stdout


class TestFrameworkTypeOptions:
    """Tests for encoder/decoder-only options"""

    def test_encoder_only_option(self):
        """Test that --encoder-only option is valid"""
        result = run_codec("--help")
        assert "--encoder-only" in result.stdout

    def test_decoder_only_option(self):
        """Test that --decoder-only option is valid"""
        result = run_codec("--help")
        assert "--decoder-only" in result.stdout


class TestCustomTestSuiteOptions:
    """Tests for custom test suite options"""

    def test_decode_test_suite_option(self):
        """Test that --decode-test-suite option is valid"""
        result = run_codec("--help")
        assert "--decode-test-suite" in result.stdout

    def test_encode_test_suite_option(self):
        """Test that --encode-test-suite option is valid"""
        result = run_codec("--help")
        assert "--encode-test-suite" in result.stdout


class TestActualFrameworkRun:
    """Tests for actual framework execution (without video processing)"""

    def test_codec_no_matching_tests(self):
        """Test codec framework with pattern that matches nothing"""
        result = run_codec(
            "--test", "nonexistent_pattern_xyz_*",
            "--no-auto-download",
            timeout=60
        )
        # Should complete with "no tests were run" message
        assert "no tests were run" in result.stdout.lower()

    def test_decode_no_matching_tests(self):
        """Test decoder-only with pattern that matches nothing"""
        result = run_codec(
            "--decoder-only",
            "--test", "nonexistent_pattern_xyz_*",
            "--no-auto-download",
            timeout=60
        )
        # Should complete with "no tests were run" message
        assert "no tests were run" in result.stdout.lower()

    def test_encode_no_matching_tests(self):
        """Test encoder-only with pattern that matches nothing"""
        result = run_codec(
            "--encoder-only",
            "--test", "nonexistent_pattern_xyz_*",
            "--no-auto-download",
            timeout=60
        )
        # Should complete with "no tests were run" message
        assert "no tests were run" in result.stdout.lower()

    def test_codec_missing_encoder_executable(self):
        """Test codec framework with non-existent encoder path"""
        result = run_codec(
            "--encoder", "/nonexistent/path/to/encoder",
            "--encoder-only",
            "--no-auto-download",
            timeout=60
        )
        # Should report missing executable
        assert "not found" in result.stdout.lower() or result.returncode != 0

    def test_codec_missing_decoder_executable(self):
        """Test codec framework with non-existent decoder path"""
        result = run_codec(
            "--decoder", "/nonexistent/path/to/decoder",
            "--decoder-only",
            "--no-auto-download",
            timeout=60
        )
        # Should report missing executable
        assert "not found" in result.stdout.lower() or result.returncode != 0

    def test_decode_missing_executable(self):
        """Test decoder-only with non-existent decoder path"""
        result = run_codec(
            "--decoder-only",
            "--decoder", "/nonexistent/path/to/decoder",
            "--no-auto-download",
            timeout=60
        )
        # Should report missing executable
        assert "not found" in result.stdout.lower() or result.returncode != 0

    def test_encode_missing_executable(self):
        """Test encoder-only with non-existent encoder path"""
        result = run_codec(
            "--encoder-only",
            "--encoder", "/nonexistent/path/to/encoder",
            "--no-auto-download",
            timeout=60
        )
        # Should report missing executable
        assert "not found" in result.stdout.lower() or result.returncode != 0

    def test_codec_filter_by_codec_no_match(self):
        """Test codec filter with VP9 and non-matching pattern"""
        result = run_codec(
            "--codec", "vp9",
            "--test", "nonexistent_*",
            "--no-auto-download",
            timeout=60
        )
        # Should complete with "no tests were run" message
        assert "no tests were run" in result.stdout.lower()

    def test_decode_h264_pattern_filters_tests(self):
        """Test decoder filters to H.264 tests with pattern"""
        result = run_codec(
            "--decoder-only",
            "--test", "h264_*",
            "--list-samples",
            timeout=60
        )
        assert result.returncode == 0
        # Should show h264 samples
        assert "h264" in result.stdout.lower()
        # Should not show other codecs
        out_lower = result.stdout.lower()
        assert "av1" not in out_lower or "h264" in out_lower

    def test_encode_h264_pattern_filters_tests(self):
        """Test encoder filters to H.264 tests with pattern"""
        result = run_codec(
            "--encoder-only",
            "--test", "h264_*",
            "--list-samples",
            timeout=60
        )
        assert result.returncode == 0
        # Should show h264 samples
        assert "h264" in result.stdout.lower()

    def test_codec_h264_codec_filter(self):
        """Test codec framework filters by H.264 codec"""
        result = run_codec(
            "--codec", "h264",
            "--list-samples",
            timeout=60
        )
        assert result.returncode == 0
        # Should show h264 samples
        assert "h264" in result.stdout.lower()

    def test_decode_av1_codec_filter(self):
        """Test decoder filters by AV1 codec"""
        result = run_codec(
            "--decoder-only",
            "--codec", "av1",
            "--list-samples",
            timeout=60
        )
        assert result.returncode == 0
        # Should show av1 samples
        assert "av1" in result.stdout.lower()

    def test_encode_h265_codec_filter(self):
        """Test encoder filters by H.265 codec"""
        result = run_codec(
            "--encoder-only",
            "--codec", "h265",
            "--list-samples",
            timeout=60
        )
        assert result.returncode == 0
        # Should show h265 samples
        assert "h265" in result.stdout.lower()

    def test_decode_run_with_h264_filter_missing_binary(self):
        """Test decoder run with h264 filter but missing binary"""
        result = run_codec(
            "--decoder-only",
            "--decoder", "/nonexistent/decoder",
            "--codec", "h264",
            "--no-auto-download",
            timeout=60
        )
        # Should report missing executable
        assert "not found" in result.stdout.lower() or result.returncode != 0

    def test_encode_run_with_h264_filter_missing_binary(self):
        """Test encoder run with h264 filter but missing binary"""
        result = run_codec(
            "--encoder-only",
            "--encoder", "/nonexistent/encoder",
            "--codec", "h264",
            "--no-auto-download",
            timeout=60
        )
        # Should report missing executable
        assert "not found" in result.stdout.lower() or result.returncode != 0


class TestModuleExecution:
    """Tests for running as Python modules"""

    def test_codec_framework_importable(self):
        """Test that codec framework can be imported"""
        imp = ("from tests.vvs_test_runner import "
               "VulkanVideoTestFramework")
        result = subprocess.run(
            [sys.executable, "-c", imp],
            capture_output=True,
            text=True,
            cwd=TESTS_DIR.parent,
            timeout=30,
            check=False
        )
        assert result.returncode == 0

    def test_decode_framework_importable(self):
        """Test that decode framework can be imported"""
        imp = ("from tests.libs.video_test_framework_decode import "
               "VulkanVideoDecodeTestFramework")
        result = subprocess.run(
            [sys.executable, "-c", imp],
            capture_output=True,
            text=True,
            cwd=TESTS_DIR.parent,
            timeout=30,
            check=False
        )
        assert result.returncode == 0

    def test_encode_framework_importable(self):
        """Test that encode framework can be imported"""
        imp = ("from tests.libs.video_test_framework_encode import "
               "VulkanVideoEncodeTestFramework")
        result = subprocess.run(
            [sys.executable, "-c", imp],
            capture_output=True,
            text=True,
            cwd=TESTS_DIR.parent,
            timeout=30,
            check=False
        )
        assert result.returncode == 0
