"""
Video Test Framework Base
Contains the base class for Vulkan Video test frameworks.

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

import fnmatch
import json
import shutil
import subprocess
import time
from pathlib import Path
from typing import Dict, List, Optional

from tests.libs.video_test_config_base import (
    BaseTestConfig,
    SkipFilter,
    SkipRule,
    TestResult,
    VideoTestStatus,
    create_error_result,
    is_test_skipped,
    load_skip_list,
)

from tests.libs.video_test_platform_utils import PlatformUtils
from tests.libs.video_test_driver_detect import (
    parse_driver_from_output, parse_system_info_from_output, SystemInfo,
    get_os_info
)
from tests.libs.video_test_result_reporter import (
    get_status_display, print_codec_breakdown, print_detailed_results,
    print_final_summary, print_command_output)
from tests.libs.video_test_utils import DEFAULT_TEST_TIMEOUT

# Exit codes from sysexits.h
EX_OK = 0                 # Successful termination
EX_UNAVAILABLE = 69       # Service unavailable (used for "not supported")

# Unix signals (as positive return codes from wait())
SIGABRT = 6               # Abort signal
SIGSEGV = 11              # Segmentation fault

# Windows-specific error codes
WIN_ACCESS_VIOLATION = 0xC0000005      # Access violation (unsigned)
WIN_ACCESS_VIOLATION_SIGNED = 3221225477  # Same as unsigned repr
WIN_ASSERT_ABORT_SIGNED = -1073741819  # Assert/abort (signed int32)
WIN_COMMON_CRASH_CODES = (-1, 1, 3, 22)    # Common crash return codes


# pylint: disable=too-many-public-methods,too-many-instance-attributes
class VulkanVideoTestFrameworkBase:
    """Base class for Vulkan Video test frameworks providing common
    functionality"""

    def __init__(self, executable_path: str = None, **options):
        self.executable_path = executable_path
        self.work_dir = options.get('work_dir')
        self.device_id = options.get('device_id')
        self.verbose = options.get('verbose', False)

        ignore_skip_list = options.get('ignore_skip_list', False)
        only_skipped = options.get('only_skipped', False)
        if only_skipped:
            skip_filter = SkipFilter.SKIPPED
        elif ignore_skip_list:
            skip_filter = SkipFilter.ALL
        else:
            skip_filter = SkipFilter.ENABLED

        skip_list_path = options.get('skip_list')
        self._skip_rules: List[SkipRule] = []
        if skip_filter != SkipFilter.ALL:
            try:
                self._skip_rules = load_skip_list(skip_list_path)
            except (OSError, json.JSONDecodeError) as e:
                print(f"⚠️  Failed to load skip list: {e}")

        self._options = {
            'keep_files': options.get('keep_files', False),
            'no_auto_download': options.get('no_auto_download', False),
            'timeout': int(options.get('timeout', DEFAULT_TEST_TIMEOUT)),
            'skip_filter': skip_filter,
            'show_skipped': options.get('show_skipped', False),
            'extended': options.get('extended', False),
            'codec_filter': options.get('codec_filter'),
            'test_pattern': options.get('test_pattern'),
        }

        self.resources_dir.mkdir(exist_ok=True)
        self.work_dir_path.mkdir(parents=True, exist_ok=True)
        self.results_dir.mkdir(exist_ok=True)

        self.results: List[TestResult] = []
        self._detected_driver: Optional[str] = None
        self._system_info: Optional[SystemInfo] = None
        self._test_pattern_active = False
        self._skipped_samples: Dict[str, Optional[SkipRule]] = {}

    @property
    def skipped_samples(self) -> Dict[str, Optional[SkipRule]]:
        """Samples marked as skipped during filtering (read-only access)."""
        return dict(self._skipped_samples)

    @property
    def keep_files(self) -> bool:
        """Whether to keep output artifacts after the run."""
        return bool(self._options['keep_files'])

    @property
    def no_auto_download(self) -> bool:
        """If true, do not auto-download missing/corrupt samples."""
        return bool(self._options['no_auto_download'])

    @property
    def timeout(self) -> int:
        """Per-test timeout in seconds."""
        return int(self._options['timeout'])

    @property
    def skip_filter(self) -> SkipFilter:
        """Skip filter mode (ENABLED, SKIPPED, or ALL)."""
        return self._options['skip_filter']

    @property
    def skip_rules(self) -> List[SkipRule]:
        """List of skip rules loaded from skip list."""
        return self._skip_rules

    @property
    def show_skipped(self) -> bool:
        """Whether to show skipped tests in summary output."""
        return bool(self._options['show_skipped'])

    @property
    def ignore_skip_list(self) -> bool:
        """Whether to ignore the skip list and run all tests."""
        return self.skip_filter == SkipFilter.ALL

    @property
    def only_skipped(self) -> bool:
        """Whether to run only skipped tests."""
        return self.skip_filter == SkipFilter.SKIPPED

    @property
    def current_driver(self) -> str:
        """
        Get the currently detected driver name.

        Returns "all" if driver hasn't been detected yet.
        Driver is detected from the first test run output.
        """
        return self._detected_driver or "all"

    @property
    def system_info(self) -> SystemInfo:
        """
        Get the detected system information.

        Returns SystemInfo with OS info even if GPU info hasn't been detected.
        GPU/driver info is populated from the first test run output.
        """
        if self._system_info is None:
            self._system_info = SystemInfo(os_name=get_os_info())
        return self._system_info

    def _detect_driver_from_output(
            self, stdout: str, stderr: str = "") -> None:
        """Detect and cache driver and system info from test output
        (first call only)."""
        if self._detected_driver is not None:
            return

        driver = parse_driver_from_output(stdout, stderr)
        if driver:
            self._detected_driver = driver
            if self.verbose:
                print(f"✓ Detected driver: {driver}")

        # Also capture full system info
        if self._system_info is None or self._system_info.gpu_name == "":
            self._system_info = parse_system_info_from_output(stdout, stderr)
            if self.verbose and self._system_info.gpu_name:
                print(f"✓ Detected GPU: {self._system_info.gpu_name}")

    def _validate_executable(self) -> bool:
        """Validate that the test executable exists in filesystem or PATH."""
        if not self.executable_path:
            print("✗ Executable path not specified")
            return False

        found_exe = PlatformUtils.find_executable(self.executable_path)

        if found_exe:
            self.executable_path = str(found_exe)
            if self.verbose:
                print(f"✓ Found executable: {self.executable_path}")
            return True

        print(f"✗ Executable not found: {self.executable_path}")
        print("Please ensure the executable is built and available in PATH "
              "or provide full path.")
        print("  Searched in: build directories, install/*/bin, PATH, "
              "and common output directories")
        return False

    def check_resources(self, auto_download: bool = True,
                        test_configs: list = None) -> bool:
        """Check if required resource files are available - to be implemented
        by subclasses

        Args:
            auto_download: Whether to automatically download missing files
            test_configs: Optional list of test configs to check resources for
        """
        raise NotImplementedError("Subclasses must implement check_resources")

    def cleanup_results(self, test_type: str = "test") -> None:
        """Clean up output artifacts if keep_files is False and no failures."""
        has_failures = any(
            r.status in [VideoTestStatus.ERROR, VideoTestStatus.CRASH]
            for r in self.results
        )

        if self.results:
            json_filename = f"{test_type}_results.json"
            json_file = self.results_dir / json_filename
            try:
                self.export_results_json(str(json_file), test_type)
                if has_failures:
                    print(f"🔍 Test failures detected - results saved to: "
                          f"{json_file}")
                else:
                    print(f"📊 Results exported to: {json_file}")
            except (OSError, IOError, ValueError, TypeError) as e:
                print(f"⚠️  Could not save JSON results file: {e}")

        if self.keep_files:
            print(f"📁 Results kept in: {self.results_dir}")
            return

        if has_failures:
            print(f"🔍 Test failures detected - results kept for debugging "
                  f"in: {self.results_dir}")
            return

        try:
            for item in self.results_dir.iterdir():
                if item.is_file() and not item.name.endswith('_results.json'):
                    item.unlink()
                elif item.is_dir():
                    shutil.rmtree(item)
            print("🧹 Cleaned up output artifacts")
        except (OSError, PermissionError) as e:
            print(f"⚠️  Failed to clean up results: {e}")

    def create_test_suite(self):
        """Create test suite - to be implemented by subclasses"""
        raise NotImplementedError(
            "Subclasses must implement create_test_suite method"
        )

    def run_test_suite(self, test_configs) -> List[TestResult]:
        """Run test suite using base implementation"""
        return self.run_test_suite_base(test_configs)

    def determine_test_status(self, returncode: int,
                              _stderr: str = "") -> VideoTestStatus:
        """Determine test status based on return code."""
        status = VideoTestStatus.ERROR
        if returncode == EX_OK:
            status = VideoTestStatus.SUCCESS
        elif returncode == EX_UNAVAILABLE:
            status = VideoTestStatus.NOT_SUPPORTED
        else:
            abs_code = abs(returncode)
            if abs_code in (SIGABRT, SIGSEGV):
                status = VideoTestStatus.CRASH
            elif PlatformUtils.is_windows():
                win_violations = (WIN_ACCESS_VIOLATION,
                                  WIN_ACCESS_VIOLATION_SIGNED)
                if abs_code in win_violations:
                    status = VideoTestStatus.CRASH
                elif returncode == WIN_ASSERT_ABORT_SIGNED:
                    status = VideoTestStatus.CRASH
                elif returncode in WIN_COMMON_CRASH_CODES:
                    status = VideoTestStatus.CRASH

        return status

    def _count_results_by_status(self, results: List[TestResult]) -> tuple:
        """Count results by status type"""
        passed = sum(1 for r in results if r.status == VideoTestStatus.SUCCESS)
        not_supported = sum(
            1 for r in results if r.status == VideoTestStatus.NOT_SUPPORTED
        )
        crashed = sum(1 for r in results if r.status == VideoTestStatus.CRASH)
        failed = sum(1 for r in results if r.status == VideoTestStatus.ERROR)
        skipped = sum(
            1 for r in results if r.status == VideoTestStatus.SKIPPED
        )
        return passed, not_supported, crashed, failed, skipped

    def _count_skipped_tests(self, samples: list, test_format: str = "vvs",
                             test_type: str = "decode") -> int:
        """Count skipped tests from samples list based on skip rules"""
        count = 0
        for sample in samples:
            skip_rule = is_test_skipped(
                sample.name, test_format, self._skip_rules,
                current_driver=self.current_driver, test_type=test_type
            )
            if skip_rule is not None:
                count += 1
        return count

    def _count_non_skipped_tests(self, samples: list, test_format: str = "vvs",
                                 test_type: str = "decode") -> int:
        """Count non-skipped tests from samples list"""
        return len(samples) - self._count_skipped_tests(
            samples, test_format, test_type
        )

    def _group_results_by_codec(self, results: List[TestResult]) -> dict:
        """Group results by codec with counts"""
        codec_results = {}
        for result in results:
            codec = result.config.codec.value
            if codec not in codec_results:
                codec_results[codec] = {
                    "pass": 0, "not_supported": 0, "crash": 0, "fail": 0,
                    "skipped": 0, "total": 0
                }

            codec_results[codec]["total"] += 1
            if result.status == VideoTestStatus.SUCCESS:
                codec_results[codec]["pass"] += 1
            elif result.status == VideoTestStatus.NOT_SUPPORTED:
                codec_results[codec]["not_supported"] += 1
            elif result.status == VideoTestStatus.CRASH:
                codec_results[codec]["crash"] += 1
            elif result.status == VideoTestStatus.SKIPPED:
                codec_results[codec]["skipped"] += 1
            else:
                codec_results[codec]["fail"] += 1
        return codec_results

    def print_summary(  # pylint: disable=too-many-locals
            self, results: List[TestResult] = None,
            test_type: str = "TEST") -> bool:
        """Print comprehensive test results summary with codec breakdown"""
        if results is None:
            results = self.results

        test_type_upper = test_type.upper()
        print("=" * 70)
        print(f"VULKAN VIDEO {test_type_upper} TEST RESULTS SUMMARY")
        print("=" * 70)

        (passed, not_supported, crashed, failed,
         skipped_hw) = self._count_results_by_status(results)
        codec_results = self._group_results_by_codec(results)

        print_codec_breakdown(codec_results)
        print("-" * 70)

        print_detailed_results(results)

        print("-" * 70)

        # Print system info header
        print()
        print(f"### {self.system_info.get_header()}")
        print()

        # Skipped tests are now included in results, so just use len(results)
        print(f"Total Tests:   {len(results):3}")
        print(f"Passed:        {passed:3}")
        print(f"Crashed:       {crashed:3}")
        print(f"Failed:        {failed:3}")
        print(f"Not Supported: {not_supported:3}")
        if skipped_hw > 0:
            print(f"Skipped:       {skipped_hw:3} (in skip list)")
        effective_total = len(results) - not_supported - skipped_hw
        if effective_total > 0:
            print(f"Success Rate: {passed/effective_total*100:.1f}%")

        return print_final_summary(
            (passed, not_supported, crashed, failed), test_type
        )

    def _default_run_cwd(self) -> Optional[Path]:
        """Return default working directory for subprocess execution."""
        if self.results_dir and self.results_dir.exists():
            return self.results_dir
        return None

    def _result_to_dict(self, result: TestResult, test_type: str) -> dict:
        """Convert a TestResult to a dictionary for JSON export."""
        test_name = (result.config.display_name
                     if hasattr(result.config, 'display_name')
                     else result.config.name)
        result_dict = {
            "name": test_name,
            "codec": result.config.codec.value,
            "test_type": test_type,
            "description": result.config.description,
            "status": result.status.value,
            "success": result.success,
            "returncode": result.returncode,
            "execution_time_ms": round(
                result.execution_time * 1000, 2
            ),
            "warning_found": result.warning_found,
            "error_message": result.error_message,
            "command_line": result.command_line
        }

        if hasattr(result.config, 'full_path'):
            result_dict["input_file"] = str(result.config.full_path)
        elif hasattr(result.config, 'full_yuv_path'):
            result_dict["input_file"] = str(result.config.full_yuv_path)

        if hasattr(result.config, 'profile') and result.config.profile:
            result_dict["profile"] = result.config.profile

        return result_dict

    def result_to_dict(self, result: TestResult, test_type: str) -> dict:
        """Public wrapper for converting a TestResult to dictionary form."""
        return self._result_to_dict(result, test_type)

    def export_results_json(self, output_file: str, test_type: str) -> bool:
        """Export test results to JSON file. Returns True on success."""
        try:
            results_data = [
                self.result_to_dict(result, test_type)
                for result in self.results
            ]
            Path(output_file).parent.mkdir(parents=True, exist_ok=True)

            sys_info = self.system_info
            with open(output_file, 'w', encoding='utf-8') as f:
                json.dump({
                    "system_info": {
                        "gpu_name": sys_info.gpu_name,
                        "driver_name": sys_info.driver_name,
                        "driver_version": sys_info.driver_version,
                        "os_name": sys_info.os_name,
                    },
                    "summary": {
                        "total_tests": len(self.results),
                        "passed": sum(
                            1 for r in self.results
                            if r.status == VideoTestStatus.SUCCESS
                        ),
                        "not_supported": sum(
                            1 for r in self.results
                            if r.status == VideoTestStatus.NOT_SUPPORTED
                        ),
                        "crashed": sum(
                            1 for r in self.results
                            if r.status == VideoTestStatus.CRASH
                        ),
                        "failed": sum(
                            1 for r in self.results
                            if r.status == VideoTestStatus.ERROR
                        )
                    },
                    "results": results_data
                }, f, indent=2)

            return True

        except (OSError, IOError) as e:
            print(f"✗ Failed to export {test_type} results: {e}")
            return False

    @property
    def test_dir(self) -> Path:
        """Directory containing this test module."""
        return Path(__file__).parent.parent

    @property
    def project_root(self) -> Path:
        """Repository root directory (one level above tests)."""
        return self.test_dir.parent

    @property
    def resources_dir(self) -> Path:
        """Folder where test resources are stored."""
        return self.test_dir / "resources"

    @property
    def work_dir_path(self) -> Path:
        """Working directory used to place outputs and results."""
        return Path(self.work_dir) if self.work_dir else self.test_dir

    @property
    def results_dir(self) -> Path:
        """Directory where per-run results are written."""
        return self.work_dir_path / "results"

    def _validate_test_result(self, result: TestResult) -> None:
        """Validate test result against expectations."""
        config = result.config

        if result.status == VideoTestStatus.ERROR:
            if not result.error_message:
                result.error_message = (
                    f"Expected success but got return code {result.returncode}"
                )
        elif result.status == VideoTestStatus.CRASH:
            if not result.error_message:
                result.error_message = (
                    f"Application crashed with return code {result.returncode}"
                )

        if result.warning_found and self.verbose:
            print(f"  ⚠️  Warning detected in {config.name}")

    def execute_test_command(
        self,
        cmd: list,
        config: BaseTestConfig,
        timeout: int = DEFAULT_TEST_TIMEOUT,
        cwd: Optional[Path] = None,
    ) -> TestResult:
        """Execute a test command and return result."""
        command_line = ' '.join(cmd)

        if self.verbose:
            print(f"    Command: {command_line}")

        try:
            subprocess_kwargs = PlatformUtils.get_subprocess_kwargs()
            cfg_timeout = getattr(config, 'timeout', None)
            if cfg_timeout is not None:
                try:
                    subprocess_kwargs['timeout'] = int(cfg_timeout)
                except (TypeError, ValueError):
                    subprocess_kwargs['timeout'] = (
                        int(self.timeout) if self.timeout else int(timeout)
                    )
            else:
                subprocess_kwargs['timeout'] = (
                    int(self.timeout) if self.timeout else int(timeout)
                )
            if cwd is not None:
                subprocess_kwargs['cwd'] = str(cwd)

            result = subprocess.run(cmd, check=False, **subprocess_kwargs)
            self._detect_driver_from_output(result.stdout, result.stderr)

            return TestResult(
                config=config,
                returncode=result.returncode,
                stdout=result.stdout,
                stderr=result.stderr,
                execution_time=0,
                status=self.determine_test_status(
                    result.returncode, result.stderr),
                command_line=command_line
            )

        except subprocess.TimeoutExpired:
            return create_error_result(
                config, "Test timed out", command_line)
        except (OSError, subprocess.SubprocessError) as e:
            return create_error_result(config, str(e), command_line)

    def build_decoder_command(  # pylint: disable=too-many-arguments
        self,
        decoder_path: Path,
        input_file: Path,
        *,
        output_file: Path = None,
        extra_decoder_args: list = None,
        no_display: bool = True,
    ) -> list:
        """Build decoder command with standard options."""
        cmd = [
            str(decoder_path),
            "-i", str(input_file),
            "--verbose",
        ]
        has_filter_arg = (extra_decoder_args
                          and "--enablePostProcessFilter"
                          in extra_decoder_args)
        if not has_filter_arg:
            cmd.extend(["--enablePostProcessFilter", "0"])

        if output_file:
            cmd.extend(["-o", str(output_file)])
        if no_display:
            cmd.append("--noPresent")
        if self.device_id is not None:
            cmd.extend(["--deviceID", str(self.device_id)])
        cmd.append("--noDeviceFallback")
        if extra_decoder_args:
            cmd.extend(extra_decoder_args)

        return cmd

    def run_decoder_validation(
        self,
        decoder_path: Path,
        input_file: Path,
        extra_decoder_args: list = None,
        config: BaseTestConfig = None,
    ) -> tuple:
        """Validate encoded file with decoder. Returns (success, output)."""
        if not decoder_path or not decoder_path.exists():
            print("  ⚠️  Decoder path not valid for validation")
            return False, "Decoder path not valid"

        print(f"  🔍 Validating with decoder: {input_file.name}")

        cmd = self.build_decoder_command(
            decoder_path=decoder_path,
            input_file=input_file,
            output_file=None,
            extra_decoder_args=extra_decoder_args,
            no_display=True,
        )

        if self.verbose:
            print(f"    Decoder command: {' '.join(cmd)}")

        run_cwd = self._default_run_cwd()
        result = self.execute_test_command(
            cmd, config, timeout=self.timeout, cwd=run_cwd
        )

        if self.verbose:
            if result.stdout:
                print("    Decoder stdout:")
                print(result.stdout)
            if result.stderr:
                print("    Decoder stderr:")
                print(result.stderr)

        if result.status == VideoTestStatus.SUCCESS:
            print("  ✓ Decoder validation passed")
            return True, None

        print("  ✗ Decoder validation failed")
        output_lines = [
            f"status: {result.status.name}, exit code: {result.returncode}"]
        if result.stdout:
            for line in result.stdout.strip().split('\n')[-10:]:
                output_lines.append(line)
        if result.stderr:
            for line in result.stderr.strip().split('\n')[-10:]:
                output_lines.append(line)

        return False, '\n'.join(output_lines)

    # pylint: disable=too-many-arguments,too-many-positional-arguments
    # pylint: disable=too-many-locals
    def filter_test_suite(
        self,
        samples: list,
        skip_filter: SkipFilter = SkipFilter.ENABLED,
        test_format: str = "vvs",
        test_type: str = "decode",
    ) -> list:
        """Filter test samples based on codec, pattern, and skip list."""
        codec_filter = self._options.get('codec_filter')
        test_pattern = self._options.get('test_pattern')
        self._test_pattern_active = test_pattern is not None
        self._skipped_samples = {}  # Maps sample name to skip_rule
        filtered_samples = []

        for sample in samples:
            if codec_filter and sample.codec.value != codec_filter:
                continue

            display_name = getattr(sample, 'display_name', sample.name)
            base_name = sample.name
            exact_match = (test_pattern and
                           test_pattern in (display_name, base_name))
            pattern_match = test_pattern and (
                fnmatch.fnmatch(display_name, test_pattern) or
                fnmatch.fnmatch(base_name, test_pattern)
            )

            if test_pattern and not exact_match and not pattern_match:
                continue

            user_requested = exact_match or pattern_match
            if (getattr(sample, 'extended', False)
                    and not self._options.get('extended', False)
                    and not user_requested):
                continue

            skip_rule = is_test_skipped(
                sample.name, test_format, self._skip_rules,
                current_driver=None, test_type=test_type
            )
            is_all_drivers_skip = (skip_rule is not None and
                                   "all" in skip_rule.drivers)

            skipped_mode = skip_filter == SkipFilter.SKIPPED
            enabled_mode = skip_filter == SkipFilter.ENABLED
            # When user explicitly requests a test (exact or pattern match),
            # bypass skip list filtering
            if skipped_mode and not is_all_drivers_skip and not user_requested:
                continue
            if enabled_mode and is_all_drivers_skip and not user_requested:
                # Track skipped samples - they will be shown but not run
                self._skipped_samples[sample.name] = skip_rule
                filtered_samples.append(sample)
                continue

            filtered_samples.append(sample)

        return filtered_samples

    def _should_mask_as_skipped(
        self, skip_rule: Optional[SkipRule], result: TestResult
    ) -> bool:
        """Check if a failing test should be masked as SKIPPED (skip list).

        A test is masked when:
        - It has a skip rule (in the skip list)
        - We're not in only_skipped mode
        - The user didn't explicitly request this test with -t pattern
        - The test failed (CRASH or ERROR status)
        """
        if skip_rule is None:
            return False
        if self.only_skipped:
            return False
        if self._test_pattern_active:
            return False
        return result.status in (VideoTestStatus.CRASH, VideoTestStatus.ERROR)

    def run_test_suite_base(self, test_configs: list,
                            test_type: str = "decode") -> List[TestResult]:
        """Run complete test suite with common flow."""
        if test_configs is None:
            test_configs = self.create_test_suite()

        self._print_suite_start()
        if not self._check_and_prepare_resources(test_configs):
            return []

        print()
        results: List[TestResult] = []
        total = len(test_configs)

        for i, config in enumerate(test_configs, 1):
            test_name = getattr(config, 'display_name', config.name)

            # Check if this test is in the universal skip list
            if config.name in self._skipped_samples:
                skip_rule = self._skipped_samples[config.name]
                reason = skip_rule.reason if skip_rule else "In skip list"
                print(f"[{i}/{total}] Skipping: {test_name} ({reason})")
                skipped_result = TestResult(
                    config=config,
                    returncode=0,
                    execution_time=0,
                    status=VideoTestStatus.SKIPPED,
                    stdout="",
                    stderr="",
                    error_message=f"Skipped: {reason}",
                )
                results.append(skipped_result)
                self.results.append(skipped_result)
                print()
                continue

            print(f"[{i}/{total}] Running: {test_name}")

            try:
                start_time = time.time()
                result = self.run_single_test(config)
                result.execution_time = time.time() - start_time

                skip_rule = is_test_skipped(
                    config.name, "vvs", self._skip_rules,
                    current_driver=self.current_driver, test_type=test_type
                )

                if self._should_mask_as_skipped(skip_rule, result):
                    driver_info = (f"driver '{self.current_driver}'"
                                   if self._detected_driver
                                   else f"drivers {skip_rule.drivers}")
                    print(f"  ⚠️  Test in skip list for {driver_info} - "
                          f"marking as SKIPPED")
                    result.status = VideoTestStatus.SKIPPED
                    result.error_message = (
                        f"Skipped: in skip list for {driver_info}. "
                        f"Reason: {skip_rule.reason or 'N/A'}"
                    )

                results.append(result)
                self.results.append(result)
                self._print_single_result(result)

            except (KeyError, ValueError, AttributeError, TypeError) as err:
                error_result = TestResult(
                    config=config,
                    returncode=-1,
                    execution_time=0,
                    status=VideoTestStatus.ERROR,
                    stdout="",
                    stderr="",
                    error_message=str(err),
                )
                results.append(error_result)
                self.results.append(error_result)
                print(f"⚠️  ERROR: {err}")

            print()

        return results

    def run_single_test(self, config):
        """Run a single test - to be implemented by subclasses"""
        raise NotImplementedError(
            "Subclasses must implement run_single_test method"
        )

    def _print_suite_start(self) -> None:
        """Print header information for the test run."""
        print("=" * 70)
        print("VULKAN VIDEO TEST SUITE")
        print("=" * 70)
        print(f"Binary: {self.executable_path}")
        if self.work_dir:
            print(f"Work Dir: {self.work_dir}")
        if self._detected_driver:
            print(f"Driver: {self._detected_driver}")
        print()

    def _check_and_prepare_resources(self, test_configs: list = None) -> bool:
        """Check and optionally download resources."""
        auto_download = not self.no_auto_download

        try:
            ok = self.check_resources(auto_download=auto_download,
                                      test_configs=test_configs)
        except TypeError:
            ok = self.check_resources(auto_download=auto_download)

        if ok:
            return True
        if auto_download:
            print(
                "✗ FATAL: Missing or corrupt resource files could not be "
                "downloaded"
            )
        else:
            print(
                "✗ FATAL: Missing or corrupt resource files (auto-download "
                "disabled)"
            )
        return False

    def _print_single_result(self, result: TestResult) -> None:
        """Print a concise line for the result and optional diagnostics."""
        label, symbol = get_status_display(result.status)
        print(f"{symbol} {label} ({result.execution_time:.2f}s)")

        if result.error_message:
            print(f"   Error: {result.error_message}")

        if result.status in (VideoTestStatus.CRASH, VideoTestStatus.ERROR):
            print(f"   Command: {result.command_line}")
            print_command_output(result)
        elif self.verbose and (result.stdout or result.stderr):
            print_command_output(result)

        decoder_output = result.meta.get("decoder_validation_output")
        if decoder_output and result.status == VideoTestStatus.ERROR:
            print("   --- Decoder validation output (last 10 lines) ---")
            for line in decoder_output.split('\n'):
                print(f"   | {line}")
            print("   --- End of decoder validation output ---")


__all__ = ['VulkanVideoTestFrameworkBase']
