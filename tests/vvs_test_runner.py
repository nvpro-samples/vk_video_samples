#!/usr/bin/env python3
"""
Vulkan Video Samples Test Framework
Main orchestrator that invokes separate encoder and decoder test frameworks.

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

# pylint: disable=wrong-import-position,protected-access,import-error
import sys

import pathlib
if __package__ is None or __package__ == "":
    # Add repository root so 'tests' becomes importable
    sys.path.append(str(pathlib.Path(__file__).resolve().parent.parent))

import json
from pathlib import Path
from typing import List, Tuple, Optional
from enum import Enum
import argparse
from dataclasses import dataclass

# Import the specialized test frameworks and base classes
from tests.libs.video_test_config_base import (
    TestResult,
    VideoTestStatus,
    load_samples_from_json,
    download_sample_assets,
    load_and_download_samples,
    load_skip_list,
    is_test_skipped,
    SkipFilter,
)

from tests.libs.video_test_platform_utils import (
    PlatformUtils,
)

from tests.libs.video_test_utils import safe_main_wrapper, DEFAULT_TEST_TIMEOUT
from tests.libs.video_test_framework_encode import (
    VulkanVideoEncodeTestFramework,
    EncodeTestSample,
)
from tests.libs.video_test_framework_decode import (
    VulkanVideoDecodeTestFramework,
    DecodeTestSample,
)


@dataclass
class FrameworkConfig:  # pylint: disable=too-many-instance-attributes
    """Configuration settings for the test framework"""
    encoder_path: Optional[str] = None
    decoder_path: Optional[str] = None
    work_dir: Optional[str] = None
    device_id: Optional[str] = None
    verbose: bool = False
    keep_files: bool = False
    no_auto_download: bool = False
    timeout: int = DEFAULT_TEST_TIMEOUT


class TestType(Enum):
    """Enumeration of test types for video codec testing"""
    ENCODER = "encoder"
    DECODER = "decoder"


class VulkanVideoTestFramework:  # pylint: disable=too-many-instance-attributes
    """Main test framework orchestrator for Vulkan Video codecs"""

    def __init__(self, encoder_path: Optional[str] = None,
                 decoder_path: Optional[str] = None, **options):
        # Create configuration object
        self.config = FrameworkConfig(
            encoder_path=encoder_path,
            decoder_path=decoder_path,
            work_dir=options.get('work_dir'),
            device_id=options.get('device_id'),
            verbose=options.get('verbose', False),
            keep_files=options.get('keep_files', False),
            no_auto_download=options.get('no_auto_download', False),
            timeout=options.get('timeout', DEFAULT_TEST_TIMEOUT)
        )
        # Skip list configuration
        self.skip_list_path = options.get('skip_list', 'skipped_samples.json')
        self.ignore_skip_list = options.get('ignore_skip_list', False)
        self.only_skipped = options.get('only_skipped', False)
        self.show_skipped = options.get('show_skipped', False)

        # Determine skip filter mode
        if self.ignore_skip_list:
            self.skip_filter = SkipFilter.ALL
        elif self.only_skipped:
            self.skip_filter = SkipFilter.SKIPPED
        else:
            self.skip_filter = SkipFilter.ENABLED

        # Track if a test pattern filter is active (set in run_test_suite)
        self._test_pattern_active = False

        # Load skip rules
        self.skip_rules = load_skip_list(self.skip_list_path)

        # Setup directories
        self.test_dir = Path(__file__).parent
        self.results_dir = self.test_dir / "results"
        self.results_dir.mkdir(exist_ok=True)

        # Initialize specialized test frameworks
        self.encode_framework = None
        self.decode_framework = None

        # Common options shared by both sub-frameworks
        common_opts = {
            'work_dir': self.config.work_dir,
            'device_id': self.config.device_id,
            'verbose': self.config.verbose,
            'keep_files': self.config.keep_files,
            'no_auto_download': self.config.no_auto_download,
            'timeout': self.config.timeout,
            'skip_list': options.get('skip_list'),
            'ignore_skip_list': self.ignore_skip_list,
            'only_skipped': self.only_skipped,
            'show_skipped': self.show_skipped,
            'extended': options.get('extended', False),
            'codec_filter': options.get('codec_filter'),
            'test_pattern': options.get('test_pattern'),
        }

        if encoder_path and Path(encoder_path).exists():
            encoder_options = {
                **common_opts,
                'test_suite': options.get('encode_test_suite'),
            }
            # Pass decoder path for validation if available
            if decoder_path and Path(decoder_path).exists():
                encoder_options['decoder'] = decoder_path
            else:
                encoder_options['validate_with_decoder'] = False
            # Pass encoder-specific CLI options
            if options.get('no_validate_with_decoder', False):
                encoder_options['validate_with_decoder'] = False
            decoder_args = options.get('decoder_args')
            if decoder_args:
                encoder_options['decoder_args'] = decoder_args

            self.encode_framework = VulkanVideoEncodeTestFramework(
                encoder_path=encoder_path,
                **encoder_options
            )

        if decoder_path and Path(decoder_path).exists():
            decoder_options = {
                **common_opts,
                'test_suite': options.get('decode_test_suite'),
                'display': options.get('decode_display', False),
                'verify_md5': not options.get('no_verify_md5', False),
            }

            self.decode_framework = VulkanVideoDecodeTestFramework(
                decoder_path=decoder_path,
                **decoder_options
            )

        # Combined results tracking
        self.all_results: List[TestResult] = []

    def check_resources(self, auto_download: bool = True,
                        encode_configs: list = None,
                        decode_configs: list = None) -> bool:
        """Check if required resource files are available and have correct
        checksums"""
        encode_ok = True
        decode_ok = True

        # Override auto_download based on the framework's no_auto_download
        # setting
        effective_auto_download = (
            auto_download and not self.config.no_auto_download
        )

        if self.encode_framework:
            encode_ok = self.encode_framework.check_resources(
                effective_auto_download,
                test_configs=encode_configs
            )

        if self.decode_framework:
            decode_ok = self.decode_framework.check_resources(
                effective_auto_download,
                test_configs=decode_configs
            )

        return encode_ok and decode_ok

    def cleanup_results(self):
        """Clean up output artifacts if keep_files is False"""
        if self.encode_framework:
            self.encode_framework.cleanup_results("encode")

        if self.decode_framework:
            self.decode_framework.cleanup_results("decode")

    def download_assets(self) -> bool:
        """Download test assets using the fetch scripts"""
        encode_ok = True
        decode_ok = True

        if self.encode_framework:
            encode_ok = download_sample_assets(
                self.encode_framework.encode_samples, "encode test"
            )

        if self.decode_framework:
            decode_ok = download_sample_assets(
                self.decode_framework.decode_samples, "decode test"
            )

        return encode_ok and decode_ok

    def create_test_suite(self,
                          test_type_filter: Optional[TestType] = None
                          ) -> Tuple[List, List]:
        """Create test suites for encoder and decoder"""
        encode_tests = []
        decode_tests = []

        if (self.encode_framework and (test_type_filter is None or
                                       test_type_filter == TestType.ENCODER)):
            encode_tests = self.encode_framework.create_test_suite()

        if (self.decode_framework and (test_type_filter is None or
                                       test_type_filter == TestType.DECODER)):
            decode_tests = self.decode_framework.create_test_suite()

        return encode_tests, decode_tests

    def run_test_suite(
        self,
        test_type_filter: Optional[TestType] = None,
    ) -> Tuple[List, List]:
        """Run complete test suite"""
        # Check if at least one framework is available
        if not self.encode_framework and not self.decode_framework:
            print("✗ FATAL: No encoder or decoder executables found!")
            print(f"  Encoder path: {self.config.encoder_path}")
            print(f"  Decoder path: {self.config.decoder_path}")
            print("\nPlease ensure executables are built and accessible.")
            print(
                "You can specify paths with --encoder and --decoder options."
            )
            return [], []

        print("=== Vulkan Video Samples Test Framework ===")
        print(f"Encoder: {self.config.encoder_path}")
        print(f"Decoder: {self.config.decoder_path}")
        if self.config.work_dir:
            print(f"Work Dir: {self.config.work_dir}")
        print()

        # Create filtered test suites first so we only download
        # resources for tests that will actually run
        encode_test_configs = []
        decode_test_configs = []

        if (self.encode_framework and
                (test_type_filter is None or
                 test_type_filter == TestType.ENCODER)):
            encode_test_configs = (
                self.encode_framework.create_test_suite()
            )

        if (self.decode_framework and
                (test_type_filter is None or
                 test_type_filter == TestType.DECODER)):
            decode_test_configs = (
                self.decode_framework.create_test_suite()
            )

        # Check resource files only for the filtered test configs.
        # Pass empty list (not None) when a test type is excluded,
        # so check_resources skips it rather than downloading all.
        if not self.check_resources(
            auto_download=True,
            encode_configs=encode_test_configs,
            decode_configs=decode_test_configs,
        ):
            print("✗ FATAL: Missing or corrupt resource files "
                  "could not be downloaded")
            return [], []

        encode_results = []
        decode_results = []

        # Run encoder tests
        if encode_test_configs:
            print("\n" + "=" * 50)
            print("RUNNING ENCODER TESTS")
            print("=" * 50)

            encode_results = self.encode_framework.run_test_suite(
                encode_test_configs)
            self.all_results.extend(encode_results)

        # Run decoder tests
        if decode_test_configs:
            print("\n" + "=" * 50)
            print("RUNNING DECODER TESTS")
            print("=" * 50)

            decode_results = self.decode_framework.run_test_suite(
                decode_test_configs)
            self.all_results.extend(decode_results)

        return encode_results, decode_results

    def _count_skipped_tests(self) -> int:
        """Count skipped tests from both frameworks using skip list"""
        total_skipped = 0
        if self.encode_framework and self.encode_framework.encode_samples:
            for sample in self.encode_framework.encode_samples:
                if is_test_skipped(sample.name, "vvs", self.skip_rules,
                                   test_type="encode"):
                    total_skipped += 1
        if self.decode_framework and self.decode_framework.decode_samples:
            for sample in self.decode_framework.decode_samples:
                if is_test_skipped(sample.name, "vvs", self.skip_rules,
                                   test_type="decode"):
                    total_skipped += 1
        return total_skipped

    def _count_non_skipped_tests(self) -> int:
        """Count non-skipped (enabled) tests from both frameworks"""
        total_enabled = 0
        if self.encode_framework and self.encode_framework.encode_samples:
            for sample in self.encode_framework.encode_samples:
                if not is_test_skipped(sample.name, "vvs", self.skip_rules,
                                       test_type="encode"):
                    total_enabled += 1
        if self.decode_framework and self.decode_framework.decode_samples:
            for sample in self.decode_framework.decode_samples:
                if not is_test_skipped(sample.name, "vvs", self.skip_rules,
                                       test_type="decode"):
                    total_enabled += 1
        return total_enabled

    def _print_final_status(self, overall_success: bool,
                            test_counts: dict) -> None:
        """Print final status message

        Args:
            overall_success: Whether all tests passed
            test_counts: Dict with keys 'passed', 'not_supported',
                        'crashed', 'failed'
        """
        passed = test_counts['passed']
        not_supported = test_counts['not_supported']
        crashed = test_counts['crashed']
        failed = test_counts['failed']

        if overall_success:
            if not_supported > 0:
                print(f"\n✓ ALL TESTS COMPLETED - {passed} passed, "
                      f"{not_supported} not supported by "
                      f"hardware/driver")
            else:
                print("\n🎉 ALL TESTS PASSED!")
        elif crashed > 0 and failed > 0:
            print(f"\n💥 {crashed} TEST(S) CRASHED, "
                  f"{failed} FAILED!")
        elif crashed > 0:
            print(f"\n💥 {crashed} TEST(S) CRASHED!")
        else:
            print(f"\n✗ {failed} TEST(S) FAILED!")

    # pylint: disable=too-many-locals,too-many-branches,too-many-statements
    def print_summary(self, encode_results: Optional[List] = None,
                      decode_results: Optional[List] = None) -> bool:
        """Print comprehensive test results summary"""
        print("\n" + "=" * 70)
        print("VULKAN VIDEO CODEC TEST RESULTS SUMMARY")
        print("=" * 70)

        encode_success = True
        decode_success = True

        if encode_results and self.encode_framework:
            print("\nENCODER RESULTS:")
            encode_success = self.encode_framework.print_summary(
                encode_results)

        if decode_results and self.decode_framework:
            print("\nDECODER RESULTS:")
            decode_success = self.decode_framework.print_summary(
                decode_results)

        # Combined summary
        total_tests = len(self.all_results)
        total_passed = sum(1 for r in self.all_results
                           if r.status == VideoTestStatus.SUCCESS)
        total_not_supported = sum(
            1 for r in self.all_results
            if r.status == VideoTestStatus.NOT_SUPPORTED
        )
        total_crashed = sum(1 for r in self.all_results
                            if r.status == VideoTestStatus.CRASH)
        total_failed = sum(1 for r in self.all_results
                           if r.status == VideoTestStatus.ERROR)
        # Count tests skipped due to driver-specific skip rules
        total_skipped = sum(1 for r in self.all_results
                            if r.status == VideoTestStatus.SKIPPED)

        print("\n" + "=" * 70)
        print("OVERALL SUMMARY")
        print("=" * 70)

        # Get system info from whichever framework ran tests
        system_info = None
        if self.decode_framework:
            system_info = self.decode_framework.system_info
        elif self.encode_framework:
            system_info = self.encode_framework.system_info
        if system_info:
            print()
            print(f"### {system_info.get_header()}")
            print()

        # Skipped tests are now included in all_results
        print(f"Total Tests:   {total_tests:3}")
        print(f"Passed:        {total_passed:3}")
        print(f"Crashed:       {total_crashed:3}")
        print(f"Failed:        {total_failed:3}")
        print(f"Not Supported: {total_not_supported:3}")
        if total_skipped > 0:
            print(f"Skipped:       {total_skipped:3} (in skip list)")
        # Calculate success rate excluding not supported and skipped tests
        effective_total = total_tests - total_not_supported - total_skipped
        if effective_total > 0:
            success_rate = total_passed / effective_total * 100
            print(f"Success Rate: {success_rate:.1f}%")

        overall_success = encode_success and decode_success
        self._print_final_status(
            overall_success,
            {
                'passed': total_passed,
                'not_supported': total_not_supported,
                'crashed': total_crashed,
                'failed': total_failed
            }
        )

        return overall_success

    def export_results_json(self, output_file: str) -> bool:
        """Export test results to JSON file"""
        try:
            combined_results = []

            # Combine results from both frameworks using base class helper
            if self.encode_framework:
                for result in self.encode_framework.results:
                    result_dict = self.encode_framework._result_to_dict(
                        result,
                        "encoder",
                    )
                    combined_results.append(result_dict)

            if self.decode_framework:
                for result in self.decode_framework.results:
                    result_dict = self.decode_framework._result_to_dict(
                        result,
                        "decoder",
                    )
                    combined_results.append(result_dict)

            # Ensure directory exists
            Path(output_file).parent.mkdir(parents=True, exist_ok=True)

            with open(output_file, 'w', encoding='utf-8') as f:
                json.dump({
                    "summary": {
                        "total_tests": len(combined_results),
                        "passed": sum(1 for r in combined_results
                                      if r["success"]),
                        "not_supported": sum(
                            1 for r in combined_results
                            if r["status"] == "not_supported"
                        ),
                        "crashed": sum(
                            1 for r in combined_results
                            if r["status"] == "crash"
                        ),
                        "failed": sum(
                            1 for r in combined_results
                            if r["status"] == "error"
                        )
                    },
                    "results": combined_results
                }, f, indent=2)

            print(f"✓ Results exported to {output_file}")
            return True

        except (OSError, IOError) as e:
            print(f"✗ Failed to export results: {e}")
            return False


# pylint: disable=too-many-arguments,too-many-positional-arguments
def _print_sample_section(
        header: str, json_file: str, test_type: str,
        prefix: str, skip_rules: list,
        codec_filter: Optional[str] = None) -> tuple:
    """Print a single sample section (decode or encode).

    Returns:
        Tuple of (sample_count, skipped_count).
    """
    print(f"\n{header}")
    print("-" * 70)
    samples = load_samples_from_json(json_file)
    if not samples:
        print(f"No {test_type} samples found")
        return 0, 0

    print(f"{'Name':<40} {'Codec':<8} Description")
    print("-" * 70)
    skipped = 0
    count = 0
    for sample in samples:
        codec = sample.get('codec', 'unknown')
        if codec_filter and codec != codec_filter:
            continue
        count += 1
        name = f"{prefix}{sample['name']}"
        description = sample.get('description', '')
        tags = []
        if is_test_skipped(sample['name'], "vvs", skip_rules,
                           test_type=test_type):
            tags.append("SKIPPED")
            skipped += 1
        if sample.get('extended', False):
            tags.append("EXTENDED")
        if tags:
            tag_str = ",".join(tags)
            name = f"[{tag_str}] {name}"
        print(f"{name:<40} {codec:<8} {description}")
    return count, skipped


def list_all_samples(
        skip_list_path: str = "skipped_samples.json",
        test_type_filter: Optional[TestType] = None,
        codec_filter: Optional[str] = None) -> None:
    """List available test samples from encoder and/or decoder

    Args:
        skip_list_path: Path to skip list JSON file
        test_type_filter: Optional filter to show only encoder
            or decoder
        codec_filter: Optional codec name to filter samples
    """
    print("=" * 70)
    print("AVAILABLE TEST SAMPLES")
    print("=" * 70)

    skip_rules = load_skip_list(skip_list_path)

    decoder_count, skipped_decode = 0, 0
    encoder_count, skipped_encode = 0, 0

    if test_type_filter is None or test_type_filter == TestType.DECODER:
        decoder_count, skipped_decode = _print_sample_section(
            "📹 DECODER SAMPLES:", "decode_samples.json",
            "decode", "decode_", skip_rules, codec_filter)

    if test_type_filter is None or test_type_filter == TestType.ENCODER:
        encoder_count, skipped_encode = _print_sample_section(
            "✏️  ENCODER SAMPLES:", "encode_samples.json",
            "encode", "encode_", skip_rules, codec_filter)

    print("=" * 70)

    total_count = decoder_count + encoder_count
    total_skipped = skipped_decode + skipped_encode

    print(f"\nTotal: {total_count} samples "
          f"({decoder_count} decoder, {encoder_count} encoder)")
    if total_skipped > 0:
        print(f"Skipped: {total_skipped} samples "
              f"(use --ignore-skip-list to run all)")
    print("\nUse --test '<pattern>' to filter samples "
          "(e.g., --test 'decode_h264_*')")


def create_argument_parser() -> argparse.ArgumentParser:
    """Create and configure the argument parser"""
    parser = argparse.ArgumentParser(
        description="Vulkan Video Samples Test Framework")

    # Platform-specific default executable names using PlatformUtils
    encoder_default = ("vk-video-enc-test" +
                       PlatformUtils.get_executable_extension())
    decoder_default = ("vk-video-dec-test" +
                       PlatformUtils.get_executable_extension())

    # General options
    parser.add_argument(
        "--work-dir", "-w",
        help="Working directory for test files")
    parser.add_argument(
        "--export-json", "-j",
        help="Export results to JSON file")
    parser.add_argument(
        "--codec", "-c",
        choices=["h264", "h265", "av1", "vp9"],
        help="Test only specific codec")
    parser.add_argument(
        "--test", "-t",
        help="Filter tests by name pattern (supports wildcards "
             "like 'h264_*' or 'av1_*')")
    parser.add_argument(
        "--verbose", "-v", action="store_true",
        help="Show command lines being executed")
    parser.add_argument(
        "--keep-files", action="store_true",
        help="Keep output artifacts after testing")
    parser.add_argument(
        "--no-auto-download", action="store_true",
        help="Skip automatic download of missing/corrupt sample files")
    parser.add_argument(
        "--download-only", action="store_true",
        help="Download all test resources (encode and decode) and exit "
             "without running tests")
    parser.add_argument(
        "--deviceID",
        help="Vulkan device ID to use for testing "
             "(decimal or hex with 0x prefix)")
    parser.add_argument(
        "--list-samples", action="store_true",
        help="List all available test samples and exit")
    parser.add_argument(
        "--skip-list",
        help="Path to custom skip list JSON (default: skipped_samples.json)")
    parser.add_argument(
        "--ignore-skip-list", action="store_true",
        help="Ignore the skip list and run all tests")
    parser.add_argument(
        "--only-skipped", action="store_true",
        help="Run only skipped tests (excludes non-skipped tests)")
    parser.add_argument(
        "--show-skipped", action="store_true",
        help="Show skipped tests in summary output")
    parser.add_argument(
        "--extended", action="store_true",
        help="Include extended tests (e.g., resolution boundary tests with "
             "large files)")
    parser.add_argument(
        "--timeout",
        type=int,
        default=DEFAULT_TEST_TIMEOUT,
        help=f"Per-test timeout in seconds (default: {DEFAULT_TEST_TIMEOUT})")

    # Encoder-specific options
    encoder_group = parser.add_argument_group("encoder options")
    encoder_group.add_argument(
        "--encoder", "-e",
        default=encoder_default,
        help="Path to vk-video-enc-test executable")
    encoder_group.add_argument(
        "--encoder-only", action="store_true",
        help="Run only encoder tests")
    encoder_group.add_argument(
        "--encode-test-suite",
        help="Path to custom encode test suite JSON file")
    encoder_group.add_argument(
        "--no-validate-with-decoder", action="store_true",
        help="Disable validation of encoder output with decoder "
             "(validation enabled by default)")
    encoder_group.add_argument(
        "--decoder-args", nargs="+",
        help="Additional arguments to pass to decoder during "
             "encoder validation")

    # Decoder-specific options
    decoder_group = parser.add_argument_group("decoder options")
    decoder_group.add_argument(
        "--decoder", "-d",
        default=decoder_default,
        help="Path to vk-video-dec-test executable")
    decoder_group.add_argument(
        "--decoder-only", action="store_true",
        help="Run only decoder tests")
    decoder_group.add_argument(
        "--decode-test-suite",
        help="Path to custom decode test suite JSON file")
    decoder_group.add_argument(
        "--decode-display", action="store_true",
        help="Enable display output for decode tests "
             "(removes --noPresent from decoder commands)")
    decoder_group.add_argument(
        "--no-verify-md5", action="store_true",
        help="Disable MD5 verification of decoded output "
             "(enabled by default when expected_output_md5 is present)")
    return parser


def find_executables(args: argparse.Namespace) -> Tuple[str, str]:
    """Find and resolve executable paths"""
    encoder_path = args.encoder
    decoder_path = args.decoder

    # Try to find encoder using centralized search
    if encoder_path and not Path(encoder_path).is_absolute():
        found_encoder = PlatformUtils.find_executable(encoder_path)
        if found_encoder:
            encoder_path = str(found_encoder)
            if args.verbose:
                print(f"✓ Found encoder: {encoder_path}")

    # Try to find decoder using centralized search
    if decoder_path and not Path(decoder_path).is_absolute():
        found_decoder = PlatformUtils.find_executable(decoder_path)
        if found_decoder:
            decoder_path = str(found_decoder)
            if args.verbose:
                print(f"✓ Found decoder: {decoder_path}")

    return encoder_path, decoder_path


def download_all_resources(args: argparse.Namespace) -> bool:
    """Download test resources, respecting --decoder-only/--encoder-only
    and --extended flags."""
    include_extended = getattr(args, 'extended', False)
    label = "All" if include_extended else "Standard"
    print(f"=== Downloading {label} Test Resources ===\n")

    decoder_only = getattr(args, 'decoder_only', False)
    encoder_only = getattr(args, 'encoder_only', False)

    decode_json = args.decode_test_suite or "decode_samples.json"
    encode_json = args.encode_test_suite or "encode_samples.json"

    decode_ok = True
    encode_ok = True

    if not encoder_only:
        decode_ok = load_and_download_samples(
            DecodeTestSample, decode_json, "decode",
            include_extended=include_extended,
        )

    if not decoder_only:
        encode_ok = load_and_download_samples(
            EncodeTestSample, encode_json, "encode",
            include_extended=include_extended,
        )

    success = decode_ok and encode_ok
    if success:
        print("\n✓ All resources downloaded successfully")
    else:
        print("\n✗ Some resources failed to download")

    return success


def run_framework_tests(args: argparse.Namespace, encoder_path: str,
                        decoder_path: str) -> bool:
    """Run the actual test framework"""
    device_id = args.deviceID if args.deviceID else None

    # Create test framework with resolved paths
    framework = VulkanVideoTestFramework(
        encoder_path=encoder_path,
        decoder_path=decoder_path,
        work_dir=args.work_dir,
        device_id=device_id,
        verbose=args.verbose,
        keep_files=args.keep_files,
        no_auto_download=args.no_auto_download,
        skip_list=args.skip_list,
        ignore_skip_list=args.ignore_skip_list,
        only_skipped=args.only_skipped,
        show_skipped=args.show_skipped,
        encode_test_suite=args.encode_test_suite,
        decode_test_suite=args.decode_test_suite,
        timeout=args.timeout,
        decode_display=args.decode_display,
        no_verify_md5=args.no_verify_md5,
        no_validate_with_decoder=args.no_validate_with_decoder,
        decoder_args=args.decoder_args,
        extended=args.extended,
        codec_filter=args.codec,
        test_pattern=args.test,
    )

    # Determine test type filter
    # Both flags together means run everything (no filter)
    test_type_filter = None
    if args.encoder_only and not args.decoder_only:
        test_type_filter = TestType.ENCODER
    elif args.decoder_only and not args.encoder_only:
        test_type_filter = TestType.DECODER

    # Run tests
    encode_results, decode_results = framework.run_test_suite(
        test_type_filter=test_type_filter,
    )

    if not encode_results and not decode_results:
        print("No tests were run!")
        return False

    # Print summary
    success = framework.print_summary(encode_results, decode_results)

    # Cleanup results if requested
    framework.cleanup_results()

    # Export results if requested (after cleanup to preserve export files)
    if args.export_json:
        json_path = Path(args.export_json)
        if not json_path.is_absolute():
            json_path = Path.cwd() / json_path
        framework.export_results_json(str(json_path))

    return success


@safe_main_wrapper
def main() -> int:
    """Main entry point for the video codec test framework"""
    parser = create_argument_parser()

    args = parser.parse_args()

    # Handle --list-samples option
    if args.list_samples:
        skip_list_path = args.skip_list or "skipped_samples.json"
        test_type_filter = None
        if args.encoder_only and not args.decoder_only:
            test_type_filter = TestType.ENCODER
        elif args.decoder_only and not args.encoder_only:
            test_type_filter = TestType.DECODER
        list_all_samples(skip_list_path, test_type_filter,
                         args.codec)
        return 0

    # Handle --download-only option
    if args.download_only:
        success = download_all_resources(args)
        return 0 if success else 1

    # Find executable paths
    encoder_path, decoder_path = find_executables(args)

    # Run the framework tests
    success = run_framework_tests(args, encoder_path, decoder_path)
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
