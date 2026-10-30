"""
Unit tests for an encode's validation with the decoder.

The encode framework decodes each stream it encodes with the decoder sample. A
decoder that decodes the stream passes the encode, a decoder that fails on it
fails the encode, and a decoder that refuses it as a stream the device does not
decode (EX_UNAVAILABLE) leaves the encode as it was, with the refusal recorded:
a device can encode a picture smaller than the minimum it decodes.

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
import os
import sys

import pytest

from tests.libs.video_test_config_base import VideoTestStatus
from tests.libs.video_test_framework_encode import VulkanVideoEncodeTestFramework
from tests.unit_tests.test_validation_layer import fake_encoder

# The decoder the tests run: it exits with FAKE_DECODER_EXIT.
FAKE_DECODER = """\
import os
import sys

sys.exit(int(os.environ.get("FAKE_DECODER_EXIT", "0")))
"""


def make_framework(tmp_path):
    """An encode framework over one H.264 sample, with the fake encoder and the
    fake decoder, and no PSNR check."""
    source = tmp_path / "source.yuv"
    source.write_bytes(bytes(16 * 16 * 3 // 2))
    sample = {"name": "h264_sample", "codec": "h264", "width": 16, "height": 16,
              "source_url": "", "source_checksum": "", "source_filepath": str(source),
              "quality_check": False}
    suite = tmp_path / "suite.json"
    suite.write_text(json.dumps({"samples": [sample]}), encoding="utf-8")
    decoder = tmp_path / "fake-vk-video-dec-test"
    decoder.write_text(f"#!{sys.executable}\n" + FAKE_DECODER, encoding="utf-8")
    decoder.chmod(0o755)
    framework = VulkanVideoEncodeTestFramework(
        encoder_path=str(fake_encoder(tmp_path)), decoder=str(decoder),
        validate_with_decoder=True, test_suite=str(suite),
        work_dir=str(tmp_path / "work"), validation_layer=False)
    return framework, framework.encode_samples[0]


@pytest.mark.skipif(os.name == "nt", reason="the fake encoder and decoder are scripts")
class TestDecoderValidation:
    """The decoder's exit status decides the encode's validation."""

    def test_decoded_passes(self, tmp_path, monkeypatch):
        """A stream the decoder decodes passes."""
        monkeypatch.setenv("FAKE_DECODER_EXIT", "0")
        framework, sample = make_framework(tmp_path)
        result = framework.run_single_test(sample)
        assert result.status == VideoTestStatus.SUCCESS, result.error_message
        assert "decoder_validation" not in result.meta
        assert "decoder_validation" not in framework.result_to_dict(result, "encode")

    def test_not_supported_is_recorded(self, tmp_path, monkeypatch):
        """A stream the device does not decode leaves the encode as it was, and
        the result says why it was not validated."""
        monkeypatch.setenv("FAKE_DECODER_EXIT", "69")
        framework, sample = make_framework(tmp_path)
        result = framework.run_single_test(sample)
        assert result.status == VideoTestStatus.SUCCESS, result.error_message
        assert result.meta["decoder_validation"] == "not supported by the decoder"
        assert framework.result_to_dict(result, "encode")["decoder_validation"] == (
            "not supported by the decoder")

    def test_failed_decode_fails(self, tmp_path, monkeypatch):
        """A stream the decoder fails on fails the encode."""
        monkeypatch.setenv("FAKE_DECODER_EXIT", "1")
        framework, sample = make_framework(tmp_path)
        result = framework.run_single_test(sample)
        assert result.status == VideoTestStatus.ERROR
        assert result.error_message == "Decoder validation failed"
        assert "decoder_validation" not in result.meta
