"""
Unit tests for the sample lists.

Every sample of tests/decode_samples.json and tests/encode_samples.json loads through its class,
and a sample with a key that no class reads is refused.

Copyright 2026 NVIDIA Corporation.

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

import json
from pathlib import Path

import pytest

from tests.libs.video_test_framework_decode import DecodeTestSample
from tests.libs.video_test_framework_encode import EncodeTestSample

TESTS_DIR = Path(__file__).parent.parent

LISTS = [
    ("decode_samples.json", DecodeTestSample),
    ("encode_samples.json", EncodeTestSample),
]


def samples(name):
    """The samples of a list."""
    return json.loads((TESTS_DIR / name).read_text())["samples"]


@pytest.mark.parametrize("name,sample_class", LISTS)
def test_every_sample_loads(name, sample_class):
    """Every sample of the list loads through its class."""
    for data in samples(name):
        sample_class.from_dict(data)


@pytest.mark.parametrize("name,sample_class", LISTS)
def test_unknown_key_is_refused(name, sample_class):
    """A sample with a key that no class reads is refused."""
    data = dict(samples(name)[0], expected_output_crc="00000000")
    with pytest.raises(ValueError, match="unknown keys"):
        sample_class.from_dict(data)
