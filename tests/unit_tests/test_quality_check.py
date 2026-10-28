"""
Unit tests for the PSNR check of encoded streams.

The streams come from ffmpeg's software encoders over a generated source, so
the check runs here exactly as it runs on a GPU encode, without the GPU:
pictures that match pass, wrong pictures, a missing picture, a wrong size and
a threshold above the measured PSNR fail, and the source is read at its own
depth, pairing and coded window.

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

import shutil
import subprocess

import pytest

from tests.libs.video_test_framework_encode import EncodeTestSample
from tests.unit_tests.mock_framework import MockFramework, make_result
from tests.libs.video_test_quality import (
    PlanePair,
    QualityCheck,
    QualityReport,
    SourceFormat,
    check_quality,
    parse_av1_render_size,
    plane_mse,
    plane_mse_python,
)

WIDTH, HEIGHT, FRAMES = 64, 48, 6


def have_encoder(name):
    """Whether ffmpeg is installed with the software encoder |name|."""
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        return False
    out = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"],
                         capture_output=True, text=True, check=False).stdout
    return f" {name} " in out


needs_x264 = pytest.mark.skipif(not have_encoder("libx264"), reason="needs ffmpeg with libx264")
needs_x265 = pytest.mark.skipif(not have_encoder("libx265"), reason="needs ffmpeg with libx265")


def ffmpeg(*args):
    """Run ffmpeg quietly and fail the test if it fails."""
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", *args], check=True)


def make_source(path, pix_fmt="yuv420p", frames=FRAMES, size=(WIDTH, HEIGHT), pattern="testsrc2"):
    """A raw source of |frames| moving test pictures."""
    ffmpeg("-f", "lavfi", "-i", f"{pattern}=size={size[0]}x{size[1]}:rate=30",
           "-frames:v", str(frames), "-pix_fmt", pix_fmt, "-f", "rawvideo", str(path))
    return path


def encode(source, out, *, pix_fmt="yuv420p", size=(WIDTH, HEIGHT), extra=()):
    """Encode a raw source at high quality, with libx265 for a .265 output and
    libx264 otherwise."""
    if str(out).endswith(".265"):
        codec, quality = "libx265", ["-x265-params", "log-level=0:crf=12"]
    else:
        codec, quality = "libx264", ["-crf", "12"]
    ffmpeg("-f", "rawvideo", "-pix_fmt", pix_fmt, "-s", f"{size[0]}x{size[1]}", "-r", "30",
           "-i", str(source), *extra, "-c:v", codec, *quality, str(out))
    return out


@needs_x264
def test_matching_pictures_pass(tmp_path):
    """A faithful encode passes the default threshold, picture for picture"""
    src = make_source(tmp_path / "src.yuv")
    report = check_quality(encode(src, tmp_path / "out.264"), src, SourceFormat(WIDTH, HEIGHT))
    assert report.passed, report.failures
    assert report.frames_decoded == FRAMES and report.frames_expected == FRAMES
    assert min(report.mean_psnr) > 35


@needs_x264
def test_wrong_pictures_fail(tmp_path):
    """A stream of other pictures decodes cleanly and fails on PSNR"""
    src = make_source(tmp_path / "src.yuv")
    other = make_source(tmp_path / "other.yuv", pattern="mandelbrot")
    report = check_quality(encode(other, tmp_path / "out.264"), src, SourceFormat(WIDTH, HEIGHT))
    assert not report.passed
    assert any(f.startswith("mean Y PSNR") for f in report.failures), report.failures


@needs_x264
def test_missing_picture_fails(tmp_path):
    """A stream one picture short fails on the count, whatever its PSNR"""
    src = make_source(tmp_path / "src.yuv")
    out = encode(src, tmp_path / "out.264", extra=("-frames:v", str(FRAMES - 1)))
    report = check_quality(out, src, SourceFormat(WIDTH, HEIGHT))
    assert f"{FRAMES - 1} pictures decoded, {FRAMES} encoded" in report.failures


@needs_x264
def test_repeated_input_pairs_by_modulo(tmp_path):
    """--repeatInputFrames codes source picture i modulo the source's length"""
    src = make_source(tmp_path / "src.yuv", frames=3)
    looped = tmp_path / "looped.yuv"
    looped.write_bytes(src.read_bytes() * 2)
    out = encode(looped, tmp_path / "out.264")
    fmt = SourceFormat.from_encoder_args(WIDTH, HEIGHT, ["--numFrames", "6", "--repeatInputFrames"])
    report = check_quality(out, src, fmt)
    assert report.passed, report.failures
    plain = check_quality(out, src, SourceFormat(WIDTH, HEIGHT))
    assert "6 pictures decoded, 3 encoded" in plain.failures


@needs_x265
def test_ten_bit_compared_at_its_depth(tmp_path):
    """A 10-bit source is compared as 10-bit samples, with a peak of 1023"""
    src = make_source(tmp_path / "src.yuv", pix_fmt="yuv420p10le")
    out = encode(src, tmp_path / "out.265", pix_fmt="yuv420p10le")
    fmt = SourceFormat.from_encoder_args(WIDTH, HEIGHT, ["--inputBpp", "10"])
    assert fmt.pix_fmt == "yuv420p10le"
    report = check_quality(out, src, fmt)
    assert report.passed, report.failures
    as_8bit = check_quality(out, src, SourceFormat(WIDTH, HEIGHT))
    assert not as_8bit.passed


@needs_x264
def test_padded_picture_cropped_to_the_source(tmp_path):
    """A decoded picture up to 15 samples larger is compared over the source's extent"""
    src = make_source(tmp_path / "src.yuv", size=(WIDTH, HEIGHT - 8))
    padded = encode(src, tmp_path / "padded.264", size=(WIDTH, HEIGHT - 8),
                    extra=("-vf", f"pad={WIDTH}:{HEIGHT}:0:0"))
    report = check_quality(padded, src, SourceFormat(WIDTH, HEIGHT - 8))
    assert report.passed, report.failures
    assert report.decoded_size == (WIDTH, HEIGHT)
    too_large = encode(src, tmp_path / "large.264", size=(WIDTH, HEIGHT - 8),
                       extra=("-vf", f"pad={WIDTH + 16}:{HEIGHT}:0:0"))
    report = check_quality(too_large, src, SourceFormat(WIDTH, HEIGHT - 8))
    assert any(f.startswith("decoded size") for f in report.failures), report.failures


@needs_x264
def test_encode_window_compared_with_the_source_window(tmp_path):
    """--encodeWidth/--encodeHeight code the top-left window of each source picture"""
    src = make_source(tmp_path / "src.yuv")
    window = encode(src, tmp_path / "window.264", extra=("-vf", "crop=32:16:0:0"))
    args = ["--encodeWidth", "32", "--encodeHeight", "16"]
    report = check_quality(window, src, SourceFormat.from_encoder_args(WIDTH, HEIGHT, args))
    assert report.passed, report.failures
    assert report.decoded_size == (32, 16)
    whole = check_quality(window, src, SourceFormat(WIDTH, HEIGHT))
    assert "decoded size 32x16 for a 64x48 picture" in whole.failures
    elsewhere = encode(src, tmp_path / "elsewhere.264", extra=("-vf", "crop=32:16:32:32"))
    report = check_quality(elsewhere, src, SourceFormat.from_encoder_args(WIDTH, HEIGHT, args))
    assert any(f.startswith("mean Y PSNR") for f in report.failures), report.failures


@needs_x264
def test_exact_extent_rejects_an_uncropped_picture(tmp_path):
    """With exact_extent a padded picture fails; it must be cropped to the window"""
    src = make_source(tmp_path / "src.yuv", size=(WIDTH, HEIGHT - 8))
    padded = encode(src, tmp_path / "padded.264", size=(WIDTH, HEIGHT - 8),
                    extra=("-vf", f"pad={WIDTH}:{HEIGHT}:0:0"))
    fmt = SourceFormat(WIDTH, HEIGHT - 8)
    assert check_quality(padded, src, fmt).passed
    report = check_quality(padded, src, fmt, QualityCheck(exact_extent=True))
    assert report.failures == [f"decoded size {WIDTH}x{HEIGHT} for a {WIDTH}x{HEIGHT - 8} picture"]
    cropped = encode(src, tmp_path / "cropped.264", size=(WIDTH, HEIGHT - 8))
    assert check_quality(cropped, src, fmt, QualityCheck(exact_extent=True)).passed


def test_av1_render_size_from_the_trace():
    """The render size of the first AV1 frame header, when it differs from the frame size"""
    def line(name, bits, value):
        return f"[trace_headers @ 0x1] 54          {name:<60}{bits} = {value}"
    rendered = "\n".join([
        line("max_frame_width_minus_1", "10111111", 191),
        line("render_and_frame_size_different", "1", 1),
        line("render_width_minus_1", "0000000001111111", 127),
        line("render_height_minus_1", "0000000000101111", 47),
        line("render_and_frame_size_different", "0", 0)])
    assert parse_av1_render_size(rendered) == (128, 48)
    same = "\n".join([line("render_and_frame_size_different", "0", 0),
                      line("render_and_frame_size_different", "1", 1),
                      line("render_width_minus_1", "0000000001111111", 127),
                      line("render_height_minus_1", "0000000000101111", 47)])
    assert parse_av1_render_size(same) is None
    assert parse_av1_render_size("") is None


@needs_x264
def test_thresholds(tmp_path):
    """The sample's thresholds apply to every plane's mean and worst picture"""
    src = make_source(tmp_path / "src.yuv")
    out = encode(src, tmp_path / "out.264")
    report = check_quality(out, src, SourceFormat(WIDTH, HEIGHT), QualityCheck(min_mean_psnr=99))
    assert len([f for f in report.failures if f.startswith("mean")]) == 3
    report = check_quality(out, src, SourceFormat(WIDTH, HEIGHT),
                           QualityCheck(min_mean_psnr=0, min_picture_psnr=99))
    assert len([f for f in report.failures if f.startswith("worst")]) == 3


def test_source_format_from_encoder_args():
    """The source format follows the encoder's own input arguments"""
    fmt = SourceFormat.from_encoder_args(352, 288, [
        "--inputBpp", "10", "--inputChromaSubsampling", "444", "--startFrame", "2",
        "--numFrames", "90", "--repeatInputFrames"])
    assert (fmt.pix_fmt, fmt.start, fmt.num_frames, fmt.repeat) == ("yuv444p10le", 2, 90, True)
    assert SourceFormat.from_encoder_args(352, 288, None).pix_fmt == "yuv420p"
    assert SourceFormat.from_encoder_args(352, 288, None).window == (352, 288)
    window = ["--encodeWidth", "128", "--encodeHeight", "48"]
    assert SourceFormat.from_encoder_args(352, 288, window).window == (128, 48)
    larger = ["--encodeWidth", "400", "--encodeHeight", "0"]
    assert SourceFormat.from_encoder_args(352, 288, larger).window == (352, 288)
    assert SourceFormat(352, 288).picture_bytes(352, 288) == 352 * 288 * 3 // 2


def test_sample_quality_check_parsing():
    """A sample gets the default check, its own thresholds, or none with false"""
    base = {"name": "h264_x", "codec": "h264", "width": 352, "height": 288,
            "source_url": "", "source_checksum": "", "source_filepath": "x.yuv"}
    assert EncodeTestSample.from_dict(base).quality_check == QualityCheck()
    tuned = EncodeTestSample.from_dict({**base, "quality_check": {"min_mean_psnr": 41.5}})
    assert tuned.quality_check.min_mean_psnr == 41.5
    assert not tuned.quality_check.exact_extent
    exact = EncodeTestSample.from_dict({**base, "quality_check": {"exact_extent": True}})
    assert exact.quality_check.exact_extent
    assert EncodeTestSample.from_dict({**base, "quality_check": False}).quality_check is None
    with pytest.raises(ValueError):
        EncodeTestSample.from_dict({**base, "quality_check": {"min_psnr": 30}})


def test_report_reaches_the_json_export():
    """The JSON export carries a result's quality report, and no key without one"""
    base = {"name": "h264_x", "codec": "h264", "width": 352, "height": 288,
            "source_url": "", "source_checksum": "", "source_filepath": "x.yuv"}
    result = make_result(EncodeTestSample.from_dict(base))
    framework = MockFramework()
    assert "quality" not in framework.result_to_dict(result, "encoder")
    report = QualityReport(frames_decoded=2, frames_expected=2, mean_psnr=[40.0, 42.0, 43.0])
    result.meta["quality"] = report.to_dict()
    assert framework.result_to_dict(result, "encoder")["quality"] == report.to_dict()


def test_python_and_numpy_agree():
    """The fallback without numpy computes the same error"""
    numpy = pytest.importorskip("numpy")
    rng = numpy.random.default_rng(1)
    for bits, dtype in ((8, numpy.uint8), (10, numpy.dtype("<u2"))):
        dec = rng.integers(0, 1 << bits, size=(10, 24)).astype(dtype)
        src = rng.integers(0, 1 << bits, size=(8, 20)).astype(dtype)
        pair = PlanePair(dec.tobytes(), src.tobytes(), 20, 8, 24)
        assert plane_mse(pair, bits) == pytest.approx(plane_mse_python(pair, bits))
        window = PlanePair(dec.tobytes(), src.tobytes(), 12, 5, 24, 20)
        assert plane_mse(window, bits) == pytest.approx(plane_mse_python(window, bits))
        expected = numpy.mean((dec[:5, :12].astype(float) - src[:5, :12]) ** 2)
        assert plane_mse(window, bits) == pytest.approx(expected)
