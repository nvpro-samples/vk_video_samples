"""
Scoring for negative test cells (expected_result: "unsupported").

Kept out of the framework base, which is already a large module: this is a
self-contained decision about one TestResult and needs nothing from the
framework instance.

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

from tests.libs.video_test_config_base import (
    TestResult,
    VideoTestStatus,
    VK_RESULT_CODES,
)

# Exit code the apps use for "not supported"; sysexits.h EX_UNAVAILABLE.
EX_UNAVAILABLE = 69

# Printed by VulkanDeviceContext.cpp once a physical device is selected. If it
# is absent the run never reached a capability query, so there is nothing to
# assert about which VkResult came back.
DEVICE_SELECTED_MARKER = "Selected Vulkan physical device"


def _output_states_vk_result(output: str, vk_result_name: str) -> bool:
    """Check the run actually reported the VkResult the cell asserts on.

    The apps print the result both ways -- "-1000023003" and
    "0xc464dc25" -- so either spelling counts.
    """
    code = VK_RESULT_CODES[vk_result_name]
    hex_form = f"0x{code & 0xffffffff:08x}"
    return (str(code) in output) or (hex_form in output.lower())


def score_expected_rejection(result: TestResult) -> None:
    """Score a negative cell: a clean rejection is the pass condition.

    Three outcomes are distinguished on purpose:
      - rejected with the expected VkResult   -> PASS
      - rejected for some other reason        -> FAIL (missing file, a
        crash, or a different capability failing -- all of which would
        otherwise masquerade as "correctly unsupported")
      - completed successfully                -> FAIL, and this is the one
        that matters: it means the driver started accepting a profile the
        hardware cannot do, which no positive cell would ever catch.
    """
    config = result.config
    wanted = getattr(config, "expected_vk_result", "")
    result.meta["expected_rejection"] = True

    if result.status == VideoTestStatus.SUCCESS:
        result.status = VideoTestStatus.ERROR
        result.error_message = (
            "expected this profile to be rejected"
            + (f" with {wanted}" if wanted else "")
            + ", but the run completed successfully"
        )
        return

    if result.status != VideoTestStatus.NOT_SUPPORTED:
        status_name = result.status.value
        result.status = VideoTestStatus.ERROR
        result.error_message = (
            "expected a clean capability rejection (exit "
            f"{EX_UNAVAILABLE}), got {status_name} "
            f"with return code {result.returncode}"
        )
        return

    combined = (result.stdout or "") + (result.stderr or "")
    if wanted and not _output_states_vk_result(combined, wanted):
        if DEVICE_SELECTED_MARKER not in combined:
            # No video device at all -- a CPU-only CI runner with no Vulkan ICD,
            # for instance. Every cell reports NOT_SUPPORTED there, and this one
            # is no different: it never got to ask the driver anything, so
            # failing it would be reporting on the environment, not the driver.
            result.error_message = (
                "no video device was selected, so the expected rejection "
                f"({wanted}) could not be observed"
            )
            return
        result.status = VideoTestStatus.ERROR
        result.error_message = (
            f"rejected, but {wanted} was never reported -- the run "
            "may have stopped for an unrelated reason"
        )
        return

    # Rejected, for the stated reason: that is what this cell asserts.
    result.status = VideoTestStatus.SUCCESS
    result.meta["rejection_note"] = (
        f"correctly rejected with {wanted}" if wanted
        else "correctly rejected"
    )
