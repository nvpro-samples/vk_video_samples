"""
Unit tests for running encodes under the Khronos validation layer.

The layer's manifest is found where the loader looks for it, the encode runs
with the layer enabled and logging to a file of its own, and the log's errors
fail the sample unless the allowlist names them. The framework tests run a
fake encoder that writes the log as the layer would, so they need no GPU and
no layer.

Copyright 2026 NVIDIA Corporation

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
import subprocess
import sys
from pathlib import Path

import pytest

from tests.libs.video_test_config_base import VideoTestStatus
from tests.libs.video_test_framework_encode import (
    EncodeTestSample,
    VulkanVideoEncodeTestFramework,
)
from tests.libs.video_test_validation_layer import (
    DEFAULT_ALLOWLIST,
    LAYER_NAME,
    AllowedMessage,
    LayerManifest,
    check_layer_log,
    find_validation_layer,
    layer_environment,
    load_allowlist,
    parse_layer_log,
    parse_version,
)

TESTS_DIR = Path(__file__).parent.parent

# Two messages as the 1.4.357 layer logs them; the 1.4.313 layer logs the
# same lines. Each names its VUID twice: in its first line and in its link.
CURRENT_LOG = """\
[AsyncAssembly] Started 2 assembly worker threads
Validation Error: [ VUID-vkCmdDraw-None-09600 ] | MessageID = 0x46582f7b
vkQueueSubmit2KHR(): pSubmits[0] command buffer VkCommandBuffer 0x58e9 expects VkImage \
0x10f0 to be in layout VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR--instead, current layout is \
VK_IMAGE_LAYOUT_UNDEFINED.
The Vulkan spec states: all image subresources identified by that descriptor must be in the \
image layout identified when the descriptor was written \
(https://vulkan.lunarg.com/doc/view/1.4.357.1/linux/antora/spec/latest/chapters/\
drawing.html#VUID-vkCmdDraw-None-09600)
Objects: 2
    [0] VkCommandBuffer 0x58e9
    [1] VkImage 0x10f0

Validation Error: [ VUID-vkCmdEncodeVideoKHR-predictionMode-10331 ] | MessageID = 0xa7bec545
vkCmdEncodeVideoKHR(): pEncodeInfo->pNext<VkVideoEncodeAV1PictureInfoKHR>.predictionMode is \
VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_UNIDIRECTIONAL_COMPOUND_KHR but referenceNameSlotIndices[] \
does not contain a non-negative DPB slot index for any of the valid pairs.
The Vulkan spec states: there must be at least two non-negative elements \
(https://vulkan.lunarg.com/doc/view/1.4.357.1/linux/antora/spec/latest/chapters/\
videocoding.html#VUID-vkCmdEncodeVideoKHR-predictionMode-10331)
Objects: 1
    [0] VkCommandBuffer 0x556d

Done processing 8 input frames!
"""

# A message as older layers log it, on one line.
ONE_LINE_LOG = (
    "Validation Error: [ VUID-vkCmdDraw-None-02699 ] Object 0: handle = 0x5581, "
    "type = VK_OBJECT_TYPE_DESCRIPTOR_SET; | MessageID = 0x1608dec0 | vkCmdDraw: the "
    "descriptor set is invalid. The Vulkan spec states: ... "
    "(https://vulkan.lunarg.com/doc/view/1.3.250.1/linux/1.3-extensions/"
    "vkspec.html#VUID-vkCmdDraw-None-02699)\n"
    "Validation Warning: [ WARNING-CreateInstance-status-message ] | MessageID = 0x5f7ef5\n"
    "Khronos Validation Layer Active\n")

PREDICTION_MODE = "VUID-vkCmdEncodeVideoKHR-predictionMode-10331"


def write_manifest(directory, version="1.4.357", name=LAYER_NAME, file_name=None):
    """A layer manifest in |directory|, as the SDK installs it."""
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / (file_name or "VkLayer_khronos_validation.json")
    path.write_text(json.dumps({
        "file_format_version": "1.2.0",
        "layer": {"name": name, "type": "GLOBAL", "api_version": version,
                  "library_path": "libVkLayer_khronos_validation.so",
                  "implementation_version": "1"},
    }), encoding="utf-8")
    return path


def bare_environ(tmp_path, **variables):
    """An environment in which the loader's default directories lie under
    tmp_path, and VK_LAYER_PATH is an empty directory unless given."""
    home = tmp_path / "home"
    empty = tmp_path / "empty"
    empty.mkdir(exist_ok=True)
    environ = {"HOME": str(home), "XDG_CONFIG_HOME": str(home / ".config"),
               "XDG_CONFIG_DIRS": str(tmp_path / "xdg"),
               "XDG_DATA_HOME": str(home / ".local/share"),
               "XDG_DATA_DIRS": str(tmp_path / "share"),
               "VK_LAYER_PATH": str(empty)}
    environ.update(variables)
    return {key: value for key, value in environ.items() if value is not None}


class TestFindLayer:
    """The manifest is the one the loader loads."""

    def test_version_parses(self):
        """A manifest's api_version is three numbers."""
        assert parse_version("1.4.357") == (1, 4, 357)
        assert parse_version(" 1.3.250 ") == (1, 3, 250)
        for text in ("1.4", "1.4.x", "", "v1.4.357"):
            with pytest.raises(ValueError):
                parse_version(text)

    def test_add_layer_path_directory_and_file(self, tmp_path):
        """VK_ADD_LAYER_PATH names directories or manifest files."""
        manifest = write_manifest(tmp_path / "sdk")
        for place in (manifest.parent, manifest):
            found = find_validation_layer(bare_environ(tmp_path, VK_ADD_LAYER_PATH=str(place)))
            assert found == LayerManifest(manifest, (1, 4, 357))

    def test_add_layer_path_before_layer_path(self, tmp_path):
        """VK_ADD_LAYER_PATH is searched first, as the loader does."""
        added = write_manifest(tmp_path / "added", "1.4.357")
        listed = write_manifest(tmp_path / "listed", "1.4.313")
        found = find_validation_layer(bare_environ(
            tmp_path, VK_ADD_LAYER_PATH=str(added.parent), VK_LAYER_PATH=str(listed.parent)))
        assert found.path == added
        found = find_validation_layer(bare_environ(tmp_path, VK_LAYER_PATH=str(listed.parent)))
        assert found.path == listed and found.api_version == (1, 4, 313)

    @pytest.mark.skipif(os.name == "nt", reason="the loader reads the registry on Windows")
    def test_default_directories(self, tmp_path):
        """Without VK_LAYER_PATH the loader's own directories are searched;
        with it, they are not."""
        manifest = write_manifest(tmp_path / "home/.config/vulkan/explicit_layer.d")
        found = find_validation_layer(bare_environ(tmp_path, VK_LAYER_PATH=None))
        assert found == LayerManifest(manifest, (1, 4, 357))
        assert find_validation_layer(bare_environ(tmp_path)) is None

    def test_vulkan_sdk(self, tmp_path):
        """A layer the loader does not find is found in VULKAN_SDK, and the
        encode runs with its directory added to VK_ADD_LAYER_PATH."""
        layer_dir = (tmp_path / "sdk/Bin" if os.name == "nt"
                     else tmp_path / "sdk/share/vulkan/explicit_layer.d")
        manifest = write_manifest(layer_dir)
        found = find_validation_layer(bare_environ(
            tmp_path, VULKAN_SDK=str(tmp_path / "sdk"), VK_ADD_LAYER_PATH="/other"))
        assert found == LayerManifest(manifest, (1, 4, 357), add_to_search_path=True)
        env = layer_environment(found, tmp_path / "layer.log", {"VK_ADD_LAYER_PATH": "/other"})
        assert env["VK_ADD_LAYER_PATH"] == os.pathsep.join([str(layer_dir), "/other"])

    def test_other_manifests_are_not_the_layer(self, tmp_path):
        """Manifests of other layers, broken files and a manifest of several
        layers are read the way the loader reads them."""
        place = tmp_path / "layers"
        write_manifest(place, name="VK_LAYER_LUNARG_api_dump", file_name="a_api_dump.json")
        (place / "b_broken.json").write_text("{", encoding="utf-8")
        (place / "c_several.json").write_text(json.dumps({"layers": [
            {"name": "VK_LAYER_KHRONOS_profiles", "api_version": "1.4.357"},
            {"name": LAYER_NAME, "api_version": "1.4.341"}]}), encoding="utf-8")
        found = find_validation_layer(bare_environ(tmp_path, VK_ADD_LAYER_PATH=str(place)))
        assert found.path == place / "c_several.json"
        assert found.api_version == (1, 4, 341)

    def test_not_installed(self, tmp_path):
        """No manifest anywhere: no layer."""
        assert find_validation_layer(bare_environ(tmp_path)) is None


class TestEnvironment:
    """The encode's environment enables the layer and its log."""

    def test_layer_enabled_with_its_log(self, tmp_path):
        """The layer is enabled next to the layers already enabled, logs every
        error and warning to the file, and drops no repeat."""
        manifest = LayerManifest(tmp_path / "VkLayer_khronos_validation.json", (1, 4, 357))
        env = layer_environment(manifest, tmp_path / "layer.log", {
            "VK_LOADER_LAYERS_ENABLE": "VK_LAYER_LUNARG_api_dump", "PATH": "/bin"})
        assert env["VK_LOADER_LAYERS_ENABLE"] == f"VK_LAYER_LUNARG_api_dump,{LAYER_NAME}"
        assert env["VK_INSTANCE_LAYERS"] == LAYER_NAME
        assert env["VK_KHRONOS_VALIDATION_LOG_FILENAME"] == str(tmp_path / "layer.log")
        assert env["VK_KHRONOS_VALIDATION_DEBUG_ACTION"] == "VK_DBG_LAYER_ACTION_LOG_MSG"
        assert env["VK_KHRONOS_VALIDATION_REPORT_FLAGS"] == "error,warn"
        assert env["VK_KHRONOS_VALIDATION_ENABLE_MESSAGE_LIMIT"] == "false"
        assert env["PATH"] == "/bin"
        assert "VK_ADD_LAYER_PATH" not in env

    def test_search_path_kept(self, tmp_path):
        """A layer the loader finds by itself adds nothing to
        VK_ADD_LAYER_PATH."""
        manifest = write_manifest(tmp_path / "added")
        environ = bare_environ(tmp_path, VK_ADD_LAYER_PATH=str(manifest.parent))
        found = find_validation_layer(environ)
        assert not found.add_to_search_path
        env = layer_environment(found, tmp_path / "layer.log", environ)
        assert env["VK_ADD_LAYER_PATH"] == str(manifest.parent)

    def test_instance_layers_kept(self, tmp_path):
        """The layer is added to the instance layers already named, once."""
        manifest = LayerManifest(tmp_path / "VkLayer_khronos_validation.json", (1, 4, 357))
        env = layer_environment(manifest, tmp_path / "layer.log",
                                {"VK_INSTANCE_LAYERS": "VK_LAYER_LUNARG_api_dump"})
        assert env["VK_INSTANCE_LAYERS"] == f"VK_LAYER_LUNARG_api_dump{os.pathsep}{LAYER_NAME}"
        env = layer_environment(manifest, tmp_path / "layer.log",
                                {"VK_INSTANCE_LAYERS": LAYER_NAME})
        assert env["VK_INSTANCE_LAYERS"] == LAYER_NAME


class TestLog:
    """The log's messages, one per first line."""

    def test_messages_counted_once(self):
        """A VUID in a message's link is not a message."""
        messages = parse_layer_log(CURRENT_LOG)
        assert [(m.severity, m.vuid) for m in messages] == [
            ("Error", "VUID-vkCmdDraw-None-09600"), ("Error", PREDICTION_MODE)]
        assert "VK_IMAGE_LAYOUT_UNDEFINED" in messages[0].text
        assert "predictionMode" not in messages[0].text
        assert "Done processing" not in messages[1].text

    def test_one_line_messages(self):
        """Older layers write a message on one line."""
        messages = parse_layer_log(ONE_LINE_LOG)
        assert [(m.severity, m.vuid) for m in messages] == [
            ("Error", "VUID-vkCmdDraw-None-02699"),
            ("Warning", "WARNING-CreateInstance-status-message")]
        assert "descriptor set is invalid" in messages[0].text

    def test_no_messages(self):
        """An empty log and other output have no messages."""
        assert not parse_layer_log("")
        assert not parse_layer_log("Validation of the stream passed\nVUID-x in passing\n")


class TestAllowlist:
    """The allowlist names errors narrowly, and refuses what it cannot read."""

    def test_committed_allowlist(self):
        """The allowlist next to the samples loads, and allows the layer's false
        prediction mode report for AV1, up to the layer that fixes it."""
        entries = load_allowlist(DEFAULT_ALLOWLIST)
        message = parse_layer_log(CURRENT_LOG)[1]
        assert any(e.allows(message, "av1", (1, 4, 357)) for e in entries)
        assert any(e.allows(message, "av1", (1, 4, 313)) for e in entries)
        assert not any(e.allows(message, "av1", (1, 4, 363)) for e in entries)
        assert not any(e.allows(message, "h265", (1, 4, 357)) for e in entries)
        assert all(e.reason for e in entries)

    def test_entry_narrows(self):
        """Codecs, layer versions and the message's text narrow an entry; its
        VUID may hold wildcards."""
        messages = parse_layer_log(CURRENT_LOG)
        draw, prediction = messages[0], messages[1]
        entry = AllowedMessage.from_dict({
            "vuid": "VUID-vkCmdDraw-*", "reason": "r", "codecs": ["h264"],
            "min_layer_version": "1.4.313", "max_layer_version": "1.4.357",
            "message": "VIDEO_ENCODE_DPB"})
        assert entry.allows(draw, "h264", (1, 4, 357))
        assert not entry.allows(prediction, "h264", (1, 4, 357))
        assert not entry.allows(draw, "h265", (1, 4, 357))
        assert not entry.allows(draw, "h264", (1, 4, 309))
        assert not entry.allows(draw, "h264", (1, 4, 363))
        other_text = AllowedMessage.from_dict({
            "vuid": "VUID-vkCmdDraw-None-09600", "reason": "r", "message": "DECODE_DPB"})
        assert not other_text.allows(draw, "h264", (1, 4, 357))

    @pytest.mark.parametrize("entry", [
        {"vuid": "VUID-x", "reason": "r", "codec": ["av1"]},
        {"vuid": "VUID-x"},
        {"reason": "r"},
        {"vuid": "VUID-x", "reason": "r", "max_layer_version": "1.4"},
        {"vuid": "VUID-x", "reason": "r", "codecs": "av1"},
        "VUID-x",
    ])
    def test_malformed_entry(self, entry):
        """A misspelt key, a missing VUID or reason, or a malformed value fails
        the load instead of widening or dropping the entry."""
        with pytest.raises(ValueError):
            AllowedMessage.from_dict(entry)

    def test_malformed_file(self, tmp_path):
        """An unknown section fails the load too."""
        path = tmp_path / "allowlist.json"
        path.write_text(json.dumps({"encodes": []}), encoding="utf-8")
        with pytest.raises(ValueError):
            load_allowlist(path)


class TestReport:
    """A log sorted into errors, allowed errors and warnings."""

    MANIFEST = LayerManifest(Path("VkLayer_khronos_validation.json"), (1, 4, 357))

    def test_no_log(self, tmp_path):
        """A layer that wrote no log never loaded: the check fails."""
        report = check_layer_log(tmp_path / "layer.log", self.MANIFEST, "h264", [])
        assert not report.loaded and not report.passed
        assert "did not load" in report.failures[0]

    def test_empty_log(self, tmp_path):
        """A layer that reported nothing passes."""
        log = tmp_path / "layer.log"
        log.write_text("", encoding="utf-8")
        report = check_layer_log(log, self.MANIFEST, "h264", [])
        assert report.passed and report.summary() == "layer 1.4.357: no errors"

    def test_errors_allowed_and_warnings(self, tmp_path):
        """Errors fail unless allowed; warnings are reported and do not fail."""
        log = tmp_path / "layer.log"
        log.write_text(CURRENT_LOG + ONE_LINE_LOG, encoding="utf-8")
        report = check_layer_log(log, self.MANIFEST, "av1", load_allowlist())
        assert report.errors == {"VUID-vkCmdDraw-None-09600": 1, "VUID-vkCmdDraw-None-02699": 1}
        assert report.allowed == {PREDICTION_MODE: 1}
        assert report.warnings == {"WARNING-CreateInstance-status-message": 1}
        assert not report.passed
        assert report.failures == [
            "VUID-vkCmdDraw-None-02699 x1, VUID-vkCmdDraw-None-09600 x1"]
        assert report.summary() == (
            "layer 1.4.357: errors VUID-vkCmdDraw-None-02699 x1, VUID-vkCmdDraw-None-09600 x1; "
            f"allowed {PREDICTION_MODE} x1; "
            "warnings WARNING-CreateInstance-status-message x1")
        assert report.to_dict()["allowed"] == {PREDICTION_MODE: 1}


# The encoder the framework tests run: it writes its output, and, when the
# environment enables the layer, the layer's log, with FAKE_LAYER_LOG's text.
# FAKE_LAYER_LOADS=0 plays a layer the loader did not load; FAKE_ELEVATED=1 an
# elevated process, whose loader reads VK_INSTANCE_LAYERS and not
# VK_LOADER_LAYERS_ENABLE.
FAKE_ENCODER = """\
import os
import sys

args = sys.argv[1:]
with open(args[args.index("-o") + 1], "wb") as out:
    out.write(b"stream")
log = os.environ.get("VK_KHRONOS_VALIDATION_LOG_FILENAME")
enabled = os.environ.get("VK_INSTANCE_LAYERS", "").split(os.pathsep)
if os.environ.get("FAKE_ELEVATED") != "1":
    enabled += os.environ.get("VK_LOADER_LAYERS_ENABLE", "").split(",")
if log and "VK_LAYER_KHRONOS_validation" in enabled and \\
        os.environ.get("FAKE_LAYER_LOADS", "1") == "1":
    with open(log, "w", encoding="utf-8") as layer_log:
        layer_log.write(os.environ.get("FAKE_LAYER_LOG", ""))
with open(args[args.index("-o") + 1] + ".env", "w", encoding="utf-8") as env:
    env.write("layer" if log else "no layer")
"""


def fake_encoder(tmp_path):
    """The fake encoder as an executable."""
    path = tmp_path / "fake-vk-video-enc-test"
    path.write_text(f"#!{sys.executable}\n" + FAKE_ENCODER, encoding="utf-8")
    path.chmod(0o755)
    return path


def make_framework(tmp_path, monkeypatch, *, layer="1.4.357",  # pylint: disable=too-many-arguments
                   run_option=True, validation_layer=None, codec="h264"):
    """An encode framework over one sample of |codec| and the fake encoder,
    with the layer of version |layer| installed, or none."""
    tmp_path.mkdir(parents=True, exist_ok=True)
    source = tmp_path / "source.yuv"
    source.write_bytes(bytes(16 * 16 * 3 // 2))
    sample = {"name": f"{codec}_sample", "codec": codec, "width": 16, "height": 16,
              "source_url": "", "source_checksum": "", "source_filepath": str(source),
              "quality_check": False}
    if validation_layer is not None:
        sample["validation_layer"] = validation_layer
    suite = tmp_path / "suite.json"
    suite.write_text(json.dumps({"samples": [sample]}), encoding="utf-8")
    empty = tmp_path / "empty"
    empty.mkdir(exist_ok=True)
    monkeypatch.setenv("VK_LAYER_PATH", str(empty))
    monkeypatch.delenv("VULKAN_SDK", raising=False)
    monkeypatch.delenv("VK_LOADER_LAYERS_ENABLE", raising=False)
    monkeypatch.delenv("VK_INSTANCE_LAYERS", raising=False)
    if layer:
        monkeypatch.setenv("VK_ADD_LAYER_PATH", str(write_manifest(tmp_path / "layer", layer)))
    else:
        monkeypatch.delenv("VK_ADD_LAYER_PATH", raising=False)
    framework = VulkanVideoEncodeTestFramework(
        encoder_path=str(fake_encoder(tmp_path)), validate_with_decoder=False,
        test_suite=str(suite), work_dir=str(tmp_path / "work"),
        validation_layer=run_option)
    return framework, framework.encode_samples[0]


needs_posix = pytest.mark.skipif(os.name == "nt", reason="the fake encoder is a script")


@needs_posix
class TestFramework:
    """The encode framework runs the encoder under the layer and checks it."""

    def test_clean_encode_passes(self, tmp_path, monkeypatch):
        """No error: the sample passes and its log is removed."""
        framework, sample = make_framework(tmp_path, monkeypatch)
        result = framework.run_single_test(sample)
        assert result.status == VideoTestStatus.SUCCESS, result.error_message
        assert result.meta["validation_layer_summary"] == "layer 1.4.357: no errors"
        assert result.meta["validation_layer"]["loaded"]
        assert not list(framework.results_dir.glob("validation_layer_*.log"))

    def test_error_fails(self, tmp_path, monkeypatch):
        """An error of the layer fails a sample whose stream is fine, and its
        log is kept."""
        monkeypatch.setenv("FAKE_LAYER_LOG", CURRENT_LOG)
        framework, sample = make_framework(tmp_path, monkeypatch)
        result = framework.run_single_test(sample)
        assert result.status == VideoTestStatus.ERROR
        assert "VUID-vkCmdDraw-None-09600 x1" in result.error_message
        assert PREDICTION_MODE in result.error_message
        assert (framework.results_dir / "validation_layer_h264_sample.log").exists()
        assert framework.result_to_dict(result, "encode")["validation_layer"]["errors"]

    def test_allowlist_by_codec_and_layer(self, tmp_path, monkeypatch):
        """The AV1 prediction mode report passes with the layers that make it
        falsely, and fails with a layer that does not."""
        monkeypatch.setenv("FAKE_LAYER_LOG", CURRENT_LOG.split("\n\n")[1] + "\n")
        for layer, status in (("1.4.357", VideoTestStatus.SUCCESS),
                              ("1.4.363", VideoTestStatus.ERROR)):
            framework, sample = make_framework(tmp_path / layer, monkeypatch,
                                               layer=layer, codec="av1")
            result = framework.run_single_test(sample)
            assert result.status == status, (layer, result.error_message)

    def test_elevated_process_loads_the_layer(self, tmp_path, monkeypatch):
        """In an elevated process, whose loader ignores VK_LOADER_LAYERS_ENABLE,
        the layer loads through VK_INSTANCE_LAYERS, and a clean encode passes."""
        monkeypatch.setenv("FAKE_ELEVATED", "1")
        framework, sample = make_framework(tmp_path, monkeypatch)
        result = framework.run_single_test(sample)
        assert result.status == VideoTestStatus.SUCCESS, result.error_message
        assert result.meta["validation_layer"]["loaded"]

    def test_layer_that_did_not_load_fails(self, tmp_path, monkeypatch):
        """A layer that wrote no log fails the sample: nothing was checked."""
        monkeypatch.setenv("FAKE_LAYER_LOADS", "0")
        framework, sample = make_framework(tmp_path, monkeypatch)
        result = framework.run_single_test(sample)
        assert result.status == VideoTestStatus.ERROR
        assert "did not load" in result.error_message

    def test_sample_keeps_out(self, tmp_path, monkeypatch):
        """"validation_layer": false keeps a sample from the layer."""
        framework, sample = make_framework(tmp_path, monkeypatch, validation_layer=False)
        result = framework.run_single_test(sample)
        assert result.status == VideoTestStatus.SUCCESS
        assert "validation_layer_summary" not in result.meta

    def test_sample_asks_without_the_option(self, tmp_path, monkeypatch):
        """"validation_layer": true runs the sample under the layer without
        --validation-layer, and without the layer installed runs it alone."""
        monkeypatch.setenv("FAKE_LAYER_LOG", CURRENT_LOG)
        framework, sample = make_framework(tmp_path / "with", monkeypatch,
                                           run_option=False, validation_layer=True)
        assert framework.run_single_test(sample).status == VideoTestStatus.ERROR
        framework, sample = make_framework(tmp_path / "without", monkeypatch, layer=None,
                                           run_option=False, validation_layer=True)
        result = framework.run_single_test(sample)
        assert result.status == VideoTestStatus.SUCCESS
        assert result.meta["validation_layer_summary"] == (
            f"not checked: {LAYER_NAME} is not installed")

    def test_option_without_the_layer_refused(self, tmp_path, monkeypatch):
        """--validation-layer without the layer installed refuses to run."""
        with pytest.raises(FileNotFoundError, match="is not installed"):
            make_framework(tmp_path, monkeypatch, layer=None)

    def test_no_layer_without_asking(self, tmp_path, monkeypatch):
        """Neither the run nor the sample asks: no layer."""
        framework, sample = make_framework(tmp_path, monkeypatch, run_option=False)
        result = framework.run_single_test(sample)
        assert result.status == VideoTestStatus.SUCCESS
        assert "validation_layer_summary" not in result.meta
        assert (framework.results_dir / "test_output_h264_sample.264.env").read_text(
            encoding="utf-8") == "no layer"


class TestSamples:
    """The samples' key and the runner's option."""

    def test_key_must_be_boolean(self):
        """"validation_layer" is true or false."""
        sample = {"name": "s", "codec": "h264", "source_filepath": "x.yuv",
                  "validation_layer": "yes"}
        with pytest.raises(ValueError):
            EncodeTestSample.from_dict(sample)

    def test_regression_samples_ask_for_the_layer(self):
        """The samples whose defects only the layer sees run under it."""
        samples = json.loads((TESTS_DIR / "encode_samples.json").read_text(encoding="utf-8"))
        asking = sorted(s["name"] for s in samples["samples"] if s.get("validation_layer"))
        assert asking == ["av1_default", "av1_resolution_too_small",
                          "h264_resolution_too_small", "h265_resolution_too_small"]

    @needs_posix
    def test_runner_refuses_without_the_layer(self, tmp_path):
        """The runner fails at once when --validation-layer finds no layer."""
        empty = tmp_path / "empty"
        empty.mkdir()
        env = {key: value for key, value in os.environ.items()
               if key not in ("VULKAN_SDK", "VK_ADD_LAYER_PATH")}
        env["VK_LAYER_PATH"] = str(empty)
        result = subprocess.run(
            [sys.executable, str(TESTS_DIR / "vvs_test_runner.py"), "--encoder-only",
             "--encoder", str(fake_encoder(tmp_path)), "--no-validate-with-decoder",
             "--validation-layer", "--no-auto-download", "--work-dir", str(tmp_path / "w"),
             "--test", "h264_resolution_too_small"],
            capture_output=True, text=True, cwd=TESTS_DIR.parent, env=env, timeout=60,
            check=False)
        assert result.returncode == 1
        assert f"{LAYER_NAME} is not installed" in result.stdout
        help_text = subprocess.run(
            [sys.executable, str(TESTS_DIR / "vvs_test_runner.py"), "--help"],
            capture_output=True, text=True, cwd=TESTS_DIR.parent, timeout=60,
            check=False).stdout
        assert "--validation-layer" in help_text
