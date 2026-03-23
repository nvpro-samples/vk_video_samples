"""
Video Test Result Reporter
Handles formatting and printing of test results and summaries.

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

from typing import List
from tests.libs.video_test_config_base import TestResult, VideoTestStatus


def get_status_display(status: VideoTestStatus) -> tuple:
    """Get status display string and symbol

    Args:
        status: VideoTestStatus enum value

    Returns:
        Tuple of (status_text, status_symbol)
    """
    if status == VideoTestStatus.SUCCESS:
        return "PASS", "✓"
    if status == VideoTestStatus.NOT_SUPPORTED:
        return "N/S", "○"
    if status == VideoTestStatus.CRASH:
        return "CRASH", "💥"
    if status == VideoTestStatus.SKIPPED:
        return "SKIP", "⊘"
    return "FAIL", "✗"


def print_codec_breakdown(codec_results: dict) -> None:
    """Print codec breakdown results

    Args:
        codec_results: Dictionary mapping codec names to count dictionaries
    """
    for codec, counts in codec_results.items():
        skipped = counts.get('skipped', 0)
        skipped_str = f", {skipped:2} skip" if skipped > 0 else ""
        print(
            f"{codec.upper():8} - {counts['pass']:2} pass, "
            f"{counts['not_supported']:2} N/S, "
            f"{counts['crash']:2} crash, "
            f"{counts['fail']:2} fail{skipped_str} ({counts['total']:2} total)"
        )


def print_detailed_results(results: List[TestResult]) -> None:
    """Print detailed test results

    Args:
        results: List of TestResult objects
    """
    for result in results:
        config = result.config
        status, status_symbol = get_status_display(result.status)

        test_name = (config.display_name
                     if hasattr(config, 'display_name')
                     else config.name)
        print(
            f"{status_symbol} {config.codec.value:4} {test_name:35} - "
            f"{status:5} ({result.execution_time:.2f}s)"
        )


def print_final_summary(counts: tuple, test_type: str) -> bool:
    """Print final summary and return success status

    Args:
        counts: Tuple of (passed, not_supported, crashed, failed)
        test_type: Type of test (e.g., "decoder", "encoder")

    Returns:
        True if all tests passed (or only not supported), False otherwise
    """
    passed, not_supported, crashed, failed = counts
    total_errors = failed + crashed
    test_type_upper = test_type.upper()

    if total_errors == 0:
        if not_supported > 0:
            print(
                f"\n✓ ALL TESTS COMPLETED - {passed} passed, "
                f"{not_supported} not supported by hardware/driver"
            )
        else:
            print(f"\n🎉 ALL {test_type_upper} TESTS PASSED!")
        return True

    if crashed > 0 and failed > 0:
        print(
            f"\n💥 {crashed} {test_type_upper} TEST(S) CRASHED, "
            f"{failed} FAILED!"
        )
    elif crashed > 0:
        print(f"\n💥 {crashed} {test_type_upper} TEST(S) CRASHED!")
    else:
        print(f"\n✗ {failed} {test_type_upper} TEST(S) FAILED!")
    return False


def print_command_output(result: TestResult, max_lines: int = 0) -> None:
    """Print stdout/stderr to aid debugging.

    Args:
        result: TestResult object
        max_lines: Maximum lines to show (0 = unlimited)
    """
    print("   === Command Output ===")
    if result.stdout:
        print("   STDOUT:")
        lines = result.stdout.splitlines()
        if 0 < max_lines < len(lines):
            for line in lines[:max_lines]:
                print(f"     {line}")
            print(f"     ... ({len(lines) - max_lines} more lines)")
        else:
            for line in lines:
                print(f"     {line}")
    if result.stderr:
        print("   STDERR:")
        lines = result.stderr.splitlines()
        if 0 < max_lines < len(lines):
            for line in lines[:max_lines]:
                print(f"     {line}")
            print(f"     ... ({len(lines) - max_lines} more lines)")
        else:
            for line in lines:
                print(f"     {line}")
