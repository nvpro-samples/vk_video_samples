"""Did the run produce the output the cell declared?

Split out of video_test_framework_base so that module stays under the
line ceiling, and because this is a separable question: everything here reads
a finished command line and a finished file, and touches no framework state.

Copyright 2025 NVIDIA Corporation.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
"""

from tests.libs.video_test_config_base import (
    BaseTestConfig,
    ExpectedResult,
    TestResult,
    VideoTestStatus,
)


# Flags under which an empty or absent declared output is the result
# that was asked for. They are read off the command that was actually
# issued, so a flag the framework adds counts the same as one a cell
# declared, and no codec and no filename appears here.
#
#   --disableFileOutput -- suppresses every write to the bitstream file
#       and wins over an -o that also names one, so the path is opened
#       and left at zero bytes. It is the flag an in-memory capture uses.
#   --numFrames 0 -- asks the encoder for no frames, so there is no
#       bitstream for it to write.
_NO_OUTPUT_FLAGS = ("--disableFileOutput",)
_NO_OUTPUT_FLAG_VALUES = (("--numFrames", "0"),)


def command_declines_output(cmd) -> bool:
    """True when the command itself says it will produce no output."""
    if not cmd:
        return False
    argv = [str(arg) for arg in cmd]
    if any(flag in argv for flag in _NO_OUTPUT_FLAGS):
        return True
    for flag, value in _NO_OUTPUT_FLAG_VALUES:
        for index in range(len(argv) - 1):
            if (argv[index] == flag) and (argv[index + 1] == value):
                return True
    return False


def check_declared_output(result: TestResult,
                          config: BaseTestConfig,
                          output_file,
                          cmd=None) -> None:
    """Downgrade a successful run that produced nothing.

    A return code says the process reached its own exit, not that it did
    the work. An encode that loses the device, or that stops on its first
    frame, can still open its output, write no bytes and exit 0 -- and a
    verdict read from the exit code alone scores that as a pass over a
    file of zero bytes.

    Deliberately narrow, because "produced no output" is a legitimate
    result in four shapes this must not touch:

      * a command that was never asked for an output. The decoder runs
        with no -o whenever there is no golden to compare against, and
        the decode-side validation of an encode never names one at all;
        |output_file| is None for both and there is nothing to check.
      * a NEGATIVE cell, whose whole point is that the codec, profile or
        bit depth is refused before a bitstream exists. Those declare
        expected_result: unsupported and are scored on their exit code
        and VkResult, not on their artifacts.
      * a run that already failed, crashed, was skipped or reported the
        feature unsupported. Its status is the diagnosis; replacing it
        with this one would lose the more specific answer.
      * a command that ASKED for no output. An -o is appended to every
        encode command, so a cell that also carries a flag suppressing
        the bitstream, or that asks for no frames, names a path it was
        never going to fill. The command is what says so.

    Nothing here knows a codec or a filename. The path is the one the
    caller put on the command line, whatever it named it.
    """
    if output_file is None:
        return
    if command_declines_output(cmd):
        return
    if result.status != VideoTestStatus.SUCCESS:
        return
    expected = getattr(config, "expected_result", ExpectedResult.SUCCESS)
    if expected != ExpectedResult.SUCCESS:
        return

    try:
        size = output_file.stat().st_size
    except OSError:
        size = None

    result.meta["output_size"] = size
    if size is None:
        result.status = VideoTestStatus.ERROR
        result.error_message = (
            f"Exited successfully but wrote no output: "
            f"{output_file} does not exist"
        )
    elif size == 0:
        result.status = VideoTestStatus.ERROR
        result.error_message = (
            f"Exited successfully but produced an empty output: "
            f"{output_file} is 0 bytes"
        )
