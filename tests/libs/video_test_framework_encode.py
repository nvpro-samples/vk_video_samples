"""
Vulkan Video Encoder Test Framework
Encoder sample configuration and test framework classes.

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
import re
from pathlib import Path
from typing import Optional, List

from tests.libs.video_test_config_base import (
    BaseTestConfig,
    CodecType,
    TestResult,
    VideoTestStatus,
    check_sample_resources,
    create_error_result,
    load_samples_from_json,
)
from tests.libs.video_test_framework_base import (
    VulkanVideoTestFrameworkBase,
)
from tests.libs.video_test_fetch_sample import (
    FetchableResource,
)


@dataclass(init=False)
# pylint: disable=too-many-instance-attributes
class EncodeTestSample(BaseTestConfig):
    """Configuration for an encode test with YUV file information"""
    profile: Optional[str] = None
    source_format: str = "yuv"  # "yuv" or "y4m"
    width: int = 0
    height: int = 0

    def __init__(
        self,
        profile: Optional[str] = None,
        source_format: str = "yuv",
        width: int = 0,
        height: int = 0,
        **kwargs,
    ):
        """Initialize EncodeTestSample with all fields from base and child"""
        super().__init__(**kwargs)
        self.profile = profile
        self.source_format = source_format
        self.width = width
        self.height = height

    @classmethod
    def from_dict(cls, data: dict) -> 'EncodeTestSample':
        """Create an EncodeTestSample from a dictionary"""
        return cls(
            **cls._parse_base_fields(data),
            profile=data.get("profile"),
            source_format=data.get("source_format", "yuv"),
            width=data.get("width", 0),
            height=data.get("height", 0),
        )

    @property
    def display_name(self) -> str:
        """Get display name with encode_ prefix"""
        return f"encode_{self.name}"

    @property
    def full_yuv_path(self) -> Path:
        """Get the full path to the YUV file"""
        resources_dir = Path(__file__).parent.parent / "resources"
        return resources_dir / self.source_filepath

    def yuv_exists(self) -> bool:
        """Check if the YUV file exists"""
        return self.full_yuv_path.exists()

    def to_fetchable_resource(self) -> 'FetchableResource':
        """Convert to FetchableResource for downloading YUV file"""
        path_obj = Path(self.source_filepath)
        base_dir = str(path_obj.parent)
        filename = path_obj.name

        # Detect checksum algorithm from prefix
        checksum = self.source_checksum
        algorithm = 'sha256'  # default
        if checksum.startswith('md5:'):
            algorithm = 'md5'
            checksum = checksum[4:]  # Strip md5: prefix

        return FetchableResource(self.source_url, filename,
                                 checksum, base_dir, algorithm)


class VulkanVideoEncodeTestFramework(VulkanVideoTestFrameworkBase):
    """Test framework for Vulkan Video encoders"""

    def __init__(self, encoder_path: str = None, **options):
        # Call base class constructor
        super().__init__(encoder_path, **options)

        # Encoder-specific attributes
        self.encoder_path = (Path(self.executable_path)
                             if self.executable_path else None)

        # Decoder validation configuration (stored in _options to reduce attrs)
        validate_with_decoder = options.get('validate_with_decoder', True)
        decoder_path_str = (options.get('decoder')
                            if validate_with_decoder else None)
        decoder_args = options.get('decoder_args', []) or []

        self._decoder_config = {
            'validate': validate_with_decoder,
            'path': Path(decoder_path_str) if decoder_path_str else None,
            'args': decoder_args,
        }

        # Load encode test samples from JSON file
        test_suite = options.get('test_suite') or 'encode_samples.json'
        self.encode_samples = self._load_encode_samples(test_suite)

        # Validate paths
        if not self._validate_executable():
            raise FileNotFoundError(
                f"Encoder not found: {self.executable_path}")

        # Validate decoder path if validation is enabled
        if (self._decoder_config['validate'] and
                not self._decoder_config['path']):
            raise FileNotFoundError(
                "Decoder path required for validation. "
                "Use --decoder to specify decoder path or "
                "--no-validate-with-decoder to disable validation"
            )

    @property
    def validate_with_decoder(self) -> bool:
        """Whether to validate encoder output with decoder"""
        return self._decoder_config['validate']

    @property
    def decoder_path(self):
        """Path to decoder executable for validation"""
        return self._decoder_config['path']

    @property
    def decoder_args(self) -> list:
        """Additional arguments for decoder validation"""
        return self._decoder_config['args']

    def _load_encode_samples(
            self, json_file: str = "encode_samples.json"
    ) -> List[EncodeTestSample]:
        """Load encode test samples from JSON configuration"""
        samples_data = load_samples_from_json(json_file, test_type="encode")
        samples = []

        for sample_data in samples_data:
            try:
                sample = EncodeTestSample.from_dict(sample_data)
                # Load all samples (filtering happens later)
                samples.append(sample)
            except (KeyError, ValueError, TypeError) as e:
                msg = (
                    "⚠️  Failed to load encode sample "
                    f"{sample_data.get('name', 'unknown')}: {e}"
                )
                print(msg)

        return samples

    def check_resources(self, auto_download: bool = True,
                        test_configs: List[EncodeTestSample] = None) -> bool:
        """Check if required resource files are available and have correct
        checksums

        Args:
            auto_download: Whether to automatically download missing files
            test_configs: Optional list of test configs to check resources for.
                         If None, checks all loaded samples.
        """
        # Create adapter samples for the common check function
        class YUVSampleAdapter:
            """Adapter class to make EncodeTestSample
            compatible with check_sample_resources"""
            def __init__(self, encode_sample):
                """Initialize adapter with encode sample"""
                self.encode_sample = encode_sample

            @property
            def full_path(self):
                """Return full path to the YUV file"""
                return self.encode_sample.full_yuv_path

            def exists(self):
                """Check if YUV file exists"""
                return self.encode_sample.yuv_exists()

            @property
            def checksum(self):
                """Return expected checksum for YUV file"""
                return self.encode_sample.source_checksum

            def to_fetchable_resource(self):
                """Convert to fetchable resource for downloading"""
                return self.encode_sample.to_fetchable_resource()

        # Use provided test configs or all samples
        samples_to_check = (test_configs if test_configs is not None
                            else self.encode_samples)
        adapted_samples = [YUVSampleAdapter(sample)
                           for sample in samples_to_check]
        return check_sample_resources(adapted_samples,
                                      "encoder YUV resource",
                                      auto_download)

    def _run_encoder_test(self, config: EncodeTestSample) -> TestResult:
        """Run encoder test for specified codec and profile"""
        if not self.encoder_path:
            return create_error_result(config, "Encoder path not specified")

        # Use the YUV file specified in the test configuration
        yuv_file = config.full_yuv_path

        # Base command
        cmd = [
            str(self.encoder_path),
            "-i", str(yuv_file),
            "--codec", config.codec.value,
        ]

        # Only add dimensions for raw YUV (Y4M has dimensions in header)
        if config.source_format != "y4m":
            width, height = str(config.width), str(config.height)
            cmd.extend(["--inputWidth", width])
            cmd.extend(["--inputHeight", height])
            cmd.extend(["--inputNumPlanes", "3"])

        cmd.append("--verbose")

        # Add profile if specified
        if config.profile:
            cmd.extend(["--profile", config.profile])

        # Add device ID if specified
        if self.device_id is not None:
            cmd.extend(["--deviceID", self.device_id])

        # Always add noDeviceFallback flag to ensure correct GPU
        cmd.append("--noDeviceFallback")

        # Add extra arguments
        if config.extra_args:
            cmd.extend(config.extra_args)

        # Output file to results folder
        output_file = self.results_dir / (
            f"test_output_{config.name}."
            f"{self._get_output_extension(config.codec)}"
        )
        cmd.extend(["-o", str(output_file)])

        # Use base class to execute (handles subprocess details)
        run_cwd = self._default_run_cwd()
        result = self.execute_test_command(
            cmd, config, timeout=self.timeout, cwd=run_cwd
        )

        # Analyze output
        result.warning_found = self._analyze_encoder_output(
            result.stderr, config
        )

        # Validate encoded output with decoder if enabled
        if (self.validate_with_decoder and
                output_file.exists() and
                result.status == VideoTestStatus.SUCCESS):
            validation_success, validation_output = (
                self._validate_with_decoder(output_file, config)
            )
            if not validation_success:
                # Decoder validation failed - mark encoder test as error
                result.status = VideoTestStatus.ERROR
                result.error_message = "Decoder validation failed"
                # Store validation output for display after test result
                result.meta["decoder_validation_output"] = validation_output

        # Clean up output file only if test succeeded and keep_files is False
        if (output_file.exists() and
                result.status == VideoTestStatus.SUCCESS and
                not self.keep_files):
            output_file.unlink()

        return result

    def _analyze_encoder_output(self, stderr: str,
                                _config: EncodeTestSample) -> bool:
        """Analyze encoder output for general warnings/errors"""
        # Look for any warning messages (general approach)
        warning_patterns = [
            r"warning\s*:",
            r"warn\s*:",
            r"caution\s*:",
            r"deprecated",
            r"not\s+supported",
            r"disabling",
            r"fallback"
        ]

        for pattern in warning_patterns:
            if re.search(pattern, stderr, re.IGNORECASE):
                return True

        return False

    def _validate_with_decoder(
        self, encoded_file: Path, config: EncodeTestSample
    ) -> tuple:
        """Validate encoded output by attempting to decode it

        Args:
            encoded_file: Path to encoded video file
            config: Encoder test configuration

        Returns:
            Tuple of (success: bool, validation_output: str or None)
        """
        # Use base class method to run decoder validation
        return self.run_decoder_validation(
            decoder_path=self.decoder_path,
            input_file=encoded_file,
            extra_decoder_args=self.decoder_args,
            config=config,
        )

    def _get_output_extension(self, codec: CodecType) -> str:
        """Get appropriate file extension for codec"""
        extensions = {
            CodecType.H264: "264",
            CodecType.H265: "265",
            CodecType.AV1: "ivf"
        }
        return extensions.get(codec, "bin")

    def create_test_suite(self) -> List[EncodeTestSample]:
        """Create test suite from samples with optional filtering"""
        # Use base class filtering method with skip list
        return self.filter_test_suite(
            self.encode_samples,
            self.skip_filter, test_format="vvs", test_type="encode"
        )

    def run_single_test(self, config: EncodeTestSample) -> TestResult:
        """Run a single test case - implementation for base class"""
        result = self._run_encoder_test(config)
        self._validate_test_result(result)
        return result

    def run_test_suite(
        self, test_configs: List[EncodeTestSample] = None
    ) -> List[TestResult]:
        """Run complete test suite using base class implementation"""
        return self.run_test_suite_base(test_configs, test_type="encode")

    def print_summary(self, results: List[TestResult] = None,
                      test_type: str = "ENCODER") -> bool:
        """Print comprehensive test results summary"""
        return super().print_summary(results, test_type)
