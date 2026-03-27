"""
Vulkan Video Decoder Test Framework
Decoder sample configuration and test framework classes.

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
from pathlib import Path
from typing import List

from tests.libs.video_test_fetch_sample import FetchableResource
from tests.libs.video_test_config_base import (
    BaseTestConfig,
    TestResult,
    VideoTestStatus,
    check_sample_resources,
    create_error_result,
    load_samples_from_json,
)
from tests.libs.video_test_framework_base import (
    VulkanVideoTestFrameworkBase,
)
from tests.libs.video_test_utils import (
    calculate_file_hash,
)


@dataclass(init=False)
class DecodeTestSample(BaseTestConfig):
    """Configuration for decoder test cases with download capability"""
    expected_output_md5: str = ""  # Expected MD5 of decoded YUV output

    def __init__(self, expected_output_md5: str = "", **kwargs):
        """Initialize DecodeTestSample with all fields from base and child"""
        super().__init__(**kwargs)
        self.expected_output_md5 = expected_output_md5

    @classmethod
    def from_dict(cls, data: dict) -> 'DecodeTestSample':
        """Create a DecodeTestSample from a dictionary"""
        return cls(
            **cls._parse_base_fields(data),
            expected_output_md5=data.get("expected_output_md5", ""),
        )

    @property
    def display_name(self) -> str:
        """Get display name with decode_ prefix"""
        return f"decode_{self.name}"

    @property
    def full_path(self) -> Path:
        """Get the full path to the sample file"""
        resources_dir = Path(__file__).parent.parent / "resources"
        return resources_dir / self.source_filepath

    def exists(self) -> bool:
        """Check if the sample file exists"""
        return self.full_path.exists()

    def to_fetchable_resource(self) -> 'FetchableResource':
        """Convert to FetchableResource for downloading"""
        path_obj = Path(self.source_filepath)
        base_dir = str(path_obj.parent)
        filename = path_obj.name

        # Check if checksum has algorithm prefix (e.g., "md5:checksum")
        if self.source_checksum.startswith('md5:'):
            checksum = self.source_checksum[4:]  # Remove "md5:" prefix
            algorithm = 'md5'
        else:
            checksum = self.source_checksum
            algorithm = 'sha256'

        return FetchableResource(
            self.source_url, filename, checksum, base_dir, algorithm
        )


class VulkanVideoDecodeTestFramework(VulkanVideoTestFrameworkBase):
    """Test framework for Vulkan Video decoders"""

    def _load_decode_samples(
            self, json_file: str = "decode_samples.json"
    ) -> List[DecodeTestSample]:
        """Load decode samples from JSON configuration"""
        samples_data = load_samples_from_json(json_file, test_type="decode")
        samples = []

        for sample_data in samples_data:
            try:
                sample = DecodeTestSample.from_dict(sample_data)
                samples.append(sample)
            except (KeyError, ValueError, TypeError) as e:
                msg = (
                    f"⚠️  Failed to load sample "
                    f"{sample_data.get('name', 'unknown')}: {e}"
                )
                print(msg)

        return samples

    def __init__(self, decoder_path: str = None, **options):
        # Call base class constructor
        super().__init__(decoder_path, **options)

        # Decoder-specific attributes
        self.decoder_path = (Path(self.executable_path)
                             if self.executable_path else None)
        self.display = options.get('display', False)
        self.verify_md5 = options.get('verify_md5',  True)

        # Load decode samples from JSON file
        test_suite = options.get('test_suite') or 'decode_samples.json'
        self.decode_samples = self._load_decode_samples(test_suite)

        # Validate paths
        if not self._validate_executable():
            raise FileNotFoundError(
                f"Decoder not found: {self.executable_path}")

    def check_resources(self, auto_download: bool = True,
                        test_configs: List[DecodeTestSample] = None) -> bool:
        """Check if required resource files are available and have correct
        checksums

        Args:
            auto_download: Whether to automatically download missing files
            test_configs: Optional list of test configs to check resources for.
                         If None, checks all loaded samples.
        """
        samples_to_check = (test_configs if test_configs is not None
                            else self.decode_samples)
        return check_sample_resources(samples_to_check,
                                      "decoder resource",
                                      auto_download)

    def _run_decoder_test(self, config: DecodeTestSample) -> TestResult:
        """Run decoder test for specified codec"""
        if not self.decoder_path:
            return create_error_result(config, "Decoder path not specified")

        # Use the sample file directly from the config
        # (since DecodeTestSample now contains everything)
        input_file = config.full_path

        if not input_file.exists():
            return create_error_result(
                config,
                f"Input file not found: {input_file}",
            )

        # Determine output file for MD5 verification
        output_file = None
        should_verify_md5 = (
            self.verify_md5
            and config.expected_output_md5
            and config.expected_output_md5.strip()
        )
        if should_verify_md5:
            output_file = self.results_dir / f"decoded_{config.name}.yuv"

        # Build decoder command using shared method
        cmd = self.build_decoder_command(
            decoder_path=self.decoder_path,
            input_file=input_file,
            output_file=output_file,
            extra_decoder_args=config.extra_args,
            no_display=not self.display,
        )

        # Use base class to execute (handles subprocess details)
        run_cwd = self._default_run_cwd()
        result = self.execute_test_command(
            cmd, config, timeout=self.timeout, cwd=run_cwd
        )

        # Verify MD5 if enabled and test succeeded
        if (should_verify_md5 and output_file and output_file.exists() and
                result.status == VideoTestStatus.SUCCESS):
            actual_md5 = calculate_file_hash(output_file, 'md5')
            if actual_md5:
                if actual_md5.lower() == config.expected_output_md5.lower():
                    print(f"✓ MD5 verification passed: {actual_md5}")
                else:
                    # MD5 mismatch should fail the test
                    result.status = VideoTestStatus.ERROR
                    result.error_message = (
                        "MD5 mismatch: expected "
                        f"{config.expected_output_md5}, got {actual_md5}"
                    )
                    print(f"✗ MD5 verification failed: expected "
                          f"{config.expected_output_md5}, got {actual_md5}")

        # Clean up output file unless keep_files is set
        if (
            output_file
            and output_file.exists()
            and result.status == VideoTestStatus.SUCCESS
            and not self.keep_files
        ):
            output_file.unlink()

        return result

    def create_test_suite(self) -> List[DecodeTestSample]:
        """Create test suite from samples with optional filtering"""
        # Use base class filtering method with skip list
        return self.filter_test_suite(
            self.decode_samples,
            self.skip_filter, test_format="vvs", test_type="decode"
        )

    def run_single_test(self, config: DecodeTestSample) -> TestResult:
        """Run a single test case - implementation for base class"""
        result = self._run_decoder_test(config)
        self._validate_test_result(result)
        return result

    def run_test_suite(
        self, test_configs: List[DecodeTestSample] = None
    ) -> List[TestResult]:
        """Run complete test suite using base class implementation"""
        return self.run_test_suite_base(test_configs, test_type="decode")

    def print_summary(self, results: List[TestResult] = None,
                      test_type: str = "DECODER") -> bool:
        """Print comprehensive test results summary"""
        return super().print_summary(results, test_type)
