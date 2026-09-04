"""Unit tests for the AV1 quality gate's skip and completeness decisions.

These are the two places the gate can report something other than what
happened: it can call a failure a skip, and it can call a truncated stream a
pass. Both decisions used to be expressions inside main(), reachable only by
running a real encoder against real content on a GPU host. They are named
functions now, and these tests pin them down without hardware.

The classifications asserted here mirror the encoder's own contract in
vk_video_encoder/test/vulkan-video-enc/Main.cpp: exit 69 means the device
cannot do this, and every other nonzero status is an ordinary failure.

Copyright 2025 NVIDIA Corporation.

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

import importlib.util
import os
import sys
import tempfile
from unittest.mock import patch

_HERE = os.path.dirname(os.path.abspath(__file__))
_SCRIPT = os.path.join(_HERE, "av1_encoder_quality_test.py")

_spec = importlib.util.spec_from_file_location("av1_quality_gate", _SCRIPT)
gate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gate)


def row(exit_code=0, encode_ok=True, decode_ok=True, frames_decoded=None,
        codec="av1", gop=8):
    """One result row, shaped like the ones main() accumulates."""
    return gate.EncodeResult(
        codec=codec, gop=gop, file_size=1024,
        encode_ok=encode_ok, decode_ok=decode_ok,
        exit_code=exit_code, frames_decoded=frames_decoded)


def failed_row(exit_code, **kw):
    """A row whose encode failed with the given process exit status."""
    return row(exit_code=exit_code, encode_ok=False, **kw)


# --------------------------------------------------------------------------
# The skip decision. Only the encoder's unsupported status may skip.
# --------------------------------------------------------------------------

def test_all_rows_unsupported_is_a_skip():
    rows = [failed_row(69), failed_row(69, gop=16)]
    assert gate.is_unsupported_run(rows) is True


def test_ordinary_failures_are_not_a_skip():
    # The encoder maps out-of-memory (-1), device-lost (-4),
    # initialization-failed (-3) and any unknown VkResult to EXIT_FAILURE. It
    # prints the same "Error creating the encoder instance" line for each,
    # which is why the text cannot be the signal.
    for status in (1, 2, 255):
        rows = [failed_row(status), failed_row(status, gop=16)]
        assert gate.is_unsupported_run(rows) is False, status


def test_mixed_rows_are_not_a_skip():
    # One unsupported row and one real failure is a failing run, not a skip:
    # the failure still has to be reported.
    rows = [failed_row(69), failed_row(1, gop=16)]
    assert gate.is_unsupported_run(rows) is False


def test_a_judgeable_row_defeats_the_skip():
    # A host whose device merely encodes badly reaches the quality comparison
    # with something to judge, so the run cannot be skipped away.
    rows = [failed_row(69), row(frames_decoded=30, gop=16)]
    assert gate.is_unsupported_run(rows) is False


def test_no_results_is_not_a_skip():
    assert gate.is_unsupported_run([]) is False


def test_a_decode_failure_is_not_a_skip():
    # encode_ok stays True, so this row was produced by a working device.
    rows = [row(exit_code=0, decode_ok=False)]
    assert gate.is_unsupported_run(rows) is False


# --------------------------------------------------------------------------
# The completeness decision. A readable prefix is not a pass.
# --------------------------------------------------------------------------

def test_full_frame_count_is_complete():
    assert gate.short_rows([row(frames_decoded=30)], 30) == []


def test_more_frames_than_requested_is_complete():
    assert gate.short_rows([row(frames_decoded=31)], 30) == []


def test_partial_stream_is_short():
    rows = [row(frames_decoded=3)]
    assert gate.short_rows(rows, 30) == rows


def test_unknown_frame_count_is_short():
    # Absence of a count is not evidence of completeness.
    rows = [row(frames_decoded=None)]
    assert gate.short_rows(rows, 30) == rows


def test_already_failed_rows_are_not_counted_twice():
    # These are reported by the encode/decode checks; counting them here
    # would print a second, less informative failure for the same row.
    rows = [failed_row(1), row(decode_ok=False, frames_decoded=None)]
    assert gate.short_rows(rows, 30) == []


# --------------------------------------------------------------------------
# encode(): the exit status reaches the caller, and a zero exit that printed
# a fatal diagnostic is still a failure.
# --------------------------------------------------------------------------

def run_encode(rc, stderr, out_size=1024):
    """Drive encode() with a canned process result and a real output file."""
    with tempfile.TemporaryDirectory() as d:
        out_path = os.path.join(d, "out.ivf")
        with open(out_path, "wb") as f:
            f.write(b"\0" * out_size)
        with patch.object(gate, "run_cmd", return_value=(rc, "", stderr)):
            return gate.encode(os.path.join(d, "in.yuv"), out_path, "av1",
                               176, 144, 30, 8, 30,
                               encoder_bin="/nonexistent/encoder")


def test_encode_reports_the_unsupported_status():
    ok, err, status = run_encode(
        69, "Error creating the encoder instance: -11\n")
    assert ok is False
    assert status == 69


def test_encode_reports_an_ordinary_failure_status():
    # Same stderr line, different status: this one must not become a skip.
    ok, err, status = run_encode(
        1, "Error creating the encoder instance: -4\n")
    assert ok is False
    assert status == 1


def test_encode_succeeds_on_a_clean_zero_exit():
    ok, err, status = run_encode(0, "")
    assert ok is True
    assert err == ""
    assert status == 0


def test_zero_exit_with_a_frame_error_is_a_failure():
    # The C++ entry point propagates this into its status; if that regresses,
    # the run must not become a quality pass over a truncated stream.
    ok, err, status = run_encode(
        0, "Error encoding frame: 7, error: -4\n", out_size=64)
    assert ok is False
    assert "Error encoding frame:" in err


def test_zero_exit_with_a_bitstream_error_is_a_failure():
    ok, err, status = run_encode(
        0, "Error obtaining the encoded bitstream file: -2\n")
    assert ok is False
    assert "Error obtaining the encoded bitstream file:" in err


def test_zero_exit_with_an_empty_output_is_a_failure():
    ok, err, status = run_encode(0, "", out_size=0)
    assert ok is False
    assert "missing or empty" in err


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            fn()
            print(f"  ok   {name}")
        except AssertionError as exc:
            failures += 1
            print(f"  FAIL {name}: {exc}")
    print(f"\n{failures} failure(s)")
    sys.exit(1 if failures else 0)
