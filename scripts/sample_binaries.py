#!/usr/bin/env python3
# Copyright 2026 NVIDIA Corporation.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Where a build leaves the sample applications.

A single-configuration generator (Ninja, Make) leaves vk-video-dec-test in
<build>/vk_video_decoder/demos/; a multi-configuration one (MSVC, Ninja
Multi-Config) in a folder for each configuration below it. The legacy runners
(run_decoder_tests.py, run_encoder_tests.py, run_encoder_profile_tests.py)
find their binary here.
"""

import sys
from pathlib import Path

CONFIGURATIONS = ("Release", "Debug", "RelWithDebInfo", "MinSizeRel")


def sample_binary(build_dir: Path, component: str, name: str) -> Path:
    """The path of the sample <name> built in <build_dir>/<component>/demos.

    The first of the demos folder and its configuration folders that holds
    it; when none does, as for a build read on another host, where this
    platform's usual generator puts it: demos/Release on Windows, demos
    elsewhere.
    """
    demos = Path(build_dir) / component / "demos"
    exe = name + ".exe" if sys.platform == "win32" else name
    candidates = [demos / config / exe for config in CONFIGURATIONS]
    if sys.platform == "win32":
        candidates.append(demos / exe)
    else:
        candidates.insert(0, demos / exe)
    return next((path for path in candidates if path.exists()), candidates[0])
