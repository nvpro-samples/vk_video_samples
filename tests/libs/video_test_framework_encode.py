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
    create_skipped_result,
    SKIP_REASON_NO_CONTENT,
    load_samples_from_json,
)
from tests.libs.video_test_framework_base import (
    VulkanVideoTestFrameworkBase,
)
from tests.libs.video_test_fetch_sample import (
    FetchableResource,
)
from tests.libs.video_test_rate_control import (
    RateControlCheck,
    check_stream,
)
from tests.libs.video_test_quality import (
    QualityCheck,
    SourceFormat,
    check_quality,
)
from tests.libs.video_test_validation_layer import (
    LAYER_NAME,
    LayerManifest,
    check_layer_log,
    find_validation_layer,
    layer_environment,
    load_allowlist,
)


@dataclass(init=False)
# pylint: disable=too-many-instance-attributes
class EncodeTestSample(BaseTestConfig):
    """Configuration for an encode test with YUV file information"""
    profile: Optional[str] = None
    source_format: str = "yuv"  # "yuv" or "y4m"
    width: int = 0
    height: int = 0
    validate_with_decoder: bool = True
    rate_control_check: Optional[RateControlCheck] = None
    quality_check: Optional[QualityCheck] = None
    validation_layer: Optional[bool] = None

    def __init__(  # pylint: disable=too-many-arguments,too-many-positional-arguments
        self,
        profile: Optional[str] = None,
        source_format: str = "yuv",
        width: int = 0,
        height: int = 0,
        validate_with_decoder: bool = True,
        rate_control_check: Optional[RateControlCheck] = None,
        quality_check: Optional[QualityCheck] = None,
        validation_layer: Optional[bool] = None,
        **kwargs,
    ):
        """Initialize EncodeTestSample with all fields from base and child"""
        super().__init__(**kwargs)
        self.profile = profile
        self.source_format = source_format
        self.width = width
        self.height = height
        self.validate_with_decoder = validate_with_decoder
        self.rate_control_check = rate_control_check
        self.quality_check = quality_check
        self.validation_layer = validation_layer

    @classmethod
    def from_dict(cls, data: dict) -> 'EncodeTestSample':
        """Create an EncodeTestSample from a dictionary"""
        cls._refuse_unknown_keys(data, frozenset({
            "profile", "source_format", "validate_with_decoder", "width",
            "height", "rate_control_check", "quality_check", "validation_layer"}))
        validation_layer = data.get("validation_layer")
        if validation_layer is not None and not isinstance(validation_layer, bool):
            raise ValueError(
                f"sample {data.get('name')!r}: validation_layer is not true or false")
        return cls(
            **cls._parse_base_fields(data),
            profile=data.get("profile"),
            source_format=data.get("source_format", "yuv"),
            # Per-sample opt-out of the decode round-trip. Some profiles are legal to
            # ENCODE but cannot be DECODED by the same hardware -- H.264 High 4:4:4
            # Predictive is encodable here but NVDEC has no H.264 4:4:4 decode, so the
            # capability query correctly returns
            # VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR and the round-trip can never
            # succeed. Without a per-sample flag such a cell can only fail forever or be
            # skipped entirely, and skipping throws away the encode coverage as well.
            validate_with_decoder=data.get("validate_with_decoder", True),
            width=data.get("width", 0),
            height=data.get("height", 0),
            # Optional measurement of the stream against its rate-control
            # target (tests/libs/video_test_rate_control.py). A malformed
            # object raises here, at load time, rather than silently
            # disabling the check.
            rate_control_check=(
                RateControlCheck.from_dict(data["rate_control_check"])
                if data.get("rate_control_check") else None),
            # The PSNR check (tests/libs/video_test_quality.py) runs on every
            # sample that encodes; "quality_check" gives its calibrated
            # thresholds, and false turns it off for the sample.
            quality_check=(
                None if data.get("quality_check") is False
                else QualityCheck.from_dict(data.get("quality_check") or {})),
            # true runs the encode under the validation layer
            # (tests/libs/video_test_validation_layer.py) when the layer is
            # installed, false never does; without the key the run's
            # --validation-layer decides.
            validation_layer=validation_layer,
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
        self._options['quality_check'] = options.get('quality_check', True)

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

        # --validation-layer runs every encode under the Khronos validation
        # layer, which has to be installed.
        self._options['validation_layer'] = options.get('validation_layer', False)
        if self._options['validation_layer'] and self._layer_manifest() is None:
            raise FileNotFoundError(
                f"--validation-layer: {LAYER_NAME} is not installed: no manifest "
                "of it in VK_ADD_LAYER_PATH, in VK_LAYER_PATH or the loader's "
                "directories, or in VULKAN_SDK")

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

    def _layer_manifest(self) -> Optional[LayerManifest]:
        """The validation layer's manifest, looked up once per run."""
        if 'layer_manifest' not in self._options:
            self._options['layer_manifest'] = find_validation_layer()
        return self._options['layer_manifest']

    def _layer_allowlist(self) -> list:
        """The allowlist of the validation layer's errors, read once per run."""
        if 'layer_allowlist' not in self._options:
            self._options['layer_allowlist'] = load_allowlist()
        return self._options['layer_allowlist']

    def _layer_run(self, config: EncodeTestSample) -> tuple:
        """Whether the encode runs under the validation layer, the layer's
        manifest when it is installed, and the log the layer writes."""
        under_layer = (config.validation_layer
                       if config.validation_layer is not None
                       else self._options['validation_layer'])
        manifest = self._layer_manifest() if under_layer else None
        if manifest is None:
            return under_layer, None, None
        layer_log = self.results_dir / f"validation_layer_{config.name}.log"
        # A log of an earlier run would read as this run's.
        layer_log.unlink(missing_ok=True)
        return under_layer, manifest, layer_log

    def _check_layer(self, result: TestResult, config: EncodeTestSample,
                     manifest: Optional[LayerManifest],
                     layer_log: Optional[Path]) -> None:
        """Fail an encode that succeeded when the validation layer did not
        load, or reported an error that is not on the allowlist."""
        if manifest is None:
            result.meta["validation_layer_summary"] = (
                f"not checked: {LAYER_NAME} is not installed")
            return
        report = check_layer_log(layer_log, manifest, config.codec.value,
                                 self._layer_allowlist())
        result.meta["validation_layer"] = report.to_dict()
        result.meta["validation_layer_summary"] = report.summary()
        if result.status == VideoTestStatus.SUCCESS and not report.passed:
            result.status = VideoTestStatus.ERROR
            result.error_message = (
                "Validation layer: " + "; ".join(report.failures))

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
        if not yuv_file.exists():
            # Same rule as the decoder: content with no source_url is generated
            # locally, so its absence is a skip, not a failure.
            if not config.source_url:
                return create_skipped_result(config, SKIP_REASON_NO_CONTENT)
            return create_error_result(
                config, f"Input YUV file not found: {yuv_file}")

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

        # Under the validation layer when the run or the sample asks for it.
        under_layer, manifest, layer_log = self._layer_run(config)

        # Use base class to execute (handles subprocess details)
        run_cwd = self._default_run_cwd()
        result = self.execute_test_command(
            cmd, config, timeout=self.timeout, cwd=run_cwd,
            output_file=output_file,
            env=layer_environment(manifest, layer_log) if manifest else None,
        )

        # Analyze output
        result.warning_found = self._analyze_encoder_output(
            result.stderr, config
        )

        # The layer's errors fail the encode before its stream is checked.
        if under_layer:
            self._check_layer(result, config, manifest, layer_log)

        # Validate encoded output with decoder if enabled, globally AND for this sample.
        self._check_decoder_validation(result, config, output_file)

        # Measure the stream against the sample's rate-control target. It runs
        # only on a stream that encoded and decoded, and a stream that cannot
        # be measured fails the check.
        if (config.rate_control_check is not None and
                output_file.exists() and
                result.status == VideoTestStatus.SUCCESS):
            report = check_stream(output_file, config.rate_control_check)
            result.meta["rate_control"] = report.to_dict()
            result.meta["rate_control_summary"] = report.summary()
            if not report.passed:
                result.status = VideoTestStatus.ERROR
                result.error_message = (
                    "Rate control check failed: " +
                    "; ".join(report.failures))

        # Measure the pictures against the source, decoded in software. It runs
        # on every stream that encoded and passed the checks above, the decode
        # round trip included where the sample asks for it.
        self._check_quality(result, config, output_file)

        # Clean up the stream and the layer's log only if test succeeded and
        # keep_files is False
        self._remove_outputs(result, output_file, layer_log)

        return result

    def _remove_outputs(self, result: TestResult, *paths: Optional[Path]) -> None:
        """Remove the files of an encode that succeeded, unless the run keeps
        them."""
        if result.status != VideoTestStatus.SUCCESS or self.keep_files:
            return
        for path in paths:
            if path is not None and path.exists():
                path.unlink()

    def _check_decoder_validation(self, result: TestResult, config: EncodeTestSample,
                                  output_file: Path) -> None:
        """Decode a stream that encoded with the decoder, when the run and the
        sample ask for it: a failure of the decoder fails the encode."""
        if (not self.validate_with_decoder or
                not getattr(config, "validate_with_decoder", True) or
                not output_file.exists() or
                result.status != VideoTestStatus.SUCCESS):
            return
        validation_status, validation_output = (
            self._validate_with_decoder(output_file, config)
        )
        if validation_status == VideoTestStatus.NOT_SUPPORTED:
            # The device's decoder does not take the stream: a picture below
            # its minimum coded extent, which can be larger than the
            # encoder's. Nothing to validate on this device; the PSNR check
            # decodes the stream in software.
            result.meta["decoder_validation"] = "not supported by the decoder"
        elif validation_status != VideoTestStatus.SUCCESS:
            # Decoder validation failed - mark encoder test as error
            result.status = VideoTestStatus.ERROR
            result.error_message = "Decoder validation failed"
            # Store validation output for display after test result
            result.meta["decoder_validation_output"] = validation_output

    def _check_quality(self, result: TestResult, config: EncodeTestSample,
                       output_file: Path) -> None:
        """The PSNR check of tests/libs/video_test_quality.py, on a stream
        that passed every check before it, unless the run or the sample turns
        it off."""
        if (not self._options['quality_check'] or config.quality_check is None or
                not output_file.exists() or
                result.status != VideoTestStatus.SUCCESS):
            return
        fmt = SourceFormat.from_encoder_args(
            config.width, config.height, config.extra_args)
        fmt.y4m = config.source_format == "y4m"
        report = check_quality(output_file, config.full_yuv_path, fmt,
                               config.quality_check)
        result.meta["quality"] = report.to_dict()
        result.meta["quality_summary"] = report.summary()
        if not report.passed:
            result.status = VideoTestStatus.ERROR
            result.error_message = (
                "PSNR check failed: " + "; ".join(report.failures))

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
            Tuple of (status: VideoTestStatus, validation_output: str or None)
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
