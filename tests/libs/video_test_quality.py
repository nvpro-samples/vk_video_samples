"""
Picture-quality check for encoded streams.

The framework checks that an encode exits cleanly and that its stream decodes.
Neither says the stream holds the right pictures. This check decodes the stream
with ffmpeg in software, independently of the GPU's decoder, and measures the
PSNR of every decoded picture against the source picture it codes:

- frame count: the stream decodes to exactly as many pictures as were encoded;
- pairing: decoded picture i codes source picture start + i, modulo the
  source's length when the encoder repeats its input (--repeatInputFrames);
- window: an encoder given --encodeWidth/--encodeHeight below its input's
  extent codes the top-left window of that size of each source picture, and
  the decoded pictures are compared with that window;
- extent: a decoded picture larger than the window, as AV1 codes 1080 rows as
  1088, is cropped to the window, and may exceed it by less than 16 samples in
  each dimension; any other size fails. A sample with "exact_extent" requires
  the picture's extent to equal the window: an encoder that codes a larger
  extent, such as the device's minimum coded extent, must crop it back. For
  AV1 the picture's extent is the render size of the frame header, which
  ffmpeg's decoders do not crop to;
- depth: samples are compared at the source's bit depth, with no conversion;
- thresholds: the mean PSNR of each plane over the pictures, and its worst
  picture, against the sample's "quality_check" values, which are calibrated
  per sample on the GPUs. Without them the mean must reach 30 dB.

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

from dataclasses import dataclass, field
import math
import re
import shutil
import subprocess
from pathlib import Path
from typing import List, Optional, Sequence

DEFAULT_MIN_MEAN_PSNR = 30.0
# A decoded picture may exceed the source by the codec's alignment.
MAX_EXTENT_PADDING = 16
PLANES = ("Y", "U", "V")


@dataclass
class QualityCheck:
    """Thresholds an encode sample declares in "quality_check".

    min_mean_psnr applies to each plane's mean PSNR over the pictures, and
    min_picture_psnr, when set, to each plane of the worst picture.
    exact_extent requires the decoded extent to equal the coded window.
    """
    min_mean_psnr: float = DEFAULT_MIN_MEAN_PSNR
    min_picture_psnr: Optional[float] = None
    exact_extent: bool = False

    @classmethod
    def from_dict(cls, data: dict) -> 'QualityCheck':
        """Create from the sample's "quality_check" object."""
        unknown = set(data) - {"min_mean_psnr", "min_picture_psnr", "exact_extent"}
        if unknown:
            raise ValueError(f"quality_check: unknown keys {sorted(unknown)}")
        return cls(
            min_mean_psnr=float(data.get("min_mean_psnr", DEFAULT_MIN_MEAN_PSNR)),
            min_picture_psnr=(float(data["min_picture_psnr"])
                              if data.get("min_picture_psnr") is not None else None),
            exact_extent=bool(data.get("exact_extent", False)),
        )


@dataclass
class SourceFormat:  # pylint: disable=too-many-instance-attributes
    """The raw source the encoder read, and which of its pictures it coded."""
    width: int
    height: int
    bits: int = 8
    chroma: str = "420"
    start: int = 0
    num_frames: Optional[int] = None
    repeat: bool = False
    y4m: bool = False
    encode_width: int = 0
    encode_height: int = 0

    @property
    def window(self) -> tuple:
        """(width, height) of the top-left window of each source picture the
        encoder codes: the whole picture unless --encodeWidth/--encodeHeight
        ask for less, as the encoder reads them."""
        width = self.encode_width if 0 < self.encode_width < self.width else self.width
        height = self.encode_height if 0 < self.encode_height < self.height else self.height
        return width, height

    @property
    def pix_fmt(self) -> str:
        """The ffmpeg pixel format of the source."""
        depth = "" if self.bits == 8 else f"{self.bits}le"
        return f"yuv{self.chroma}p{depth}"

    def plane_sizes(self, width: int, height: int) -> List[tuple]:
        """(width, height) of each plane of a picture of this format."""
        cw = width if self.chroma == "444" else (width + 1) // 2
        ch = height if self.chroma in ("444", "422") else (height + 1) // 2
        return [(width, height), (cw, ch), (cw, ch)]

    def picture_bytes(self, width: int, height: int) -> int:
        """Bytes of one picture of this format at the given extent."""
        samples = sum(w * h for w, h in self.plane_sizes(width, height))
        return samples * (1 if self.bits == 8 else 2)

    @classmethod
    def from_encoder_args(cls, width: int, height: int,
                          args: Optional[Sequence[str]]) -> 'SourceFormat':
        """The format the encoder's own arguments give its raw input."""
        args = list(args or [])
        fmt = cls(width=width, height=height)

        def value(name):
            return args[args.index(name) + 1] if name in args[:-1] else None

        if value("--inputBpp"):
            fmt.bits = int(value("--inputBpp"))
        if value("--inputChromaSubsampling"):
            fmt.chroma = value("--inputChromaSubsampling")
        if value("--startFrame"):
            fmt.start = int(value("--startFrame"))
        if value("--numFrames"):
            fmt.num_frames = int(value("--numFrames"))
        if value("--encodeWidth"):
            fmt.encode_width = int(value("--encodeWidth"))
        if value("--encodeHeight"):
            fmt.encode_height = int(value("--encodeHeight"))
        fmt.repeat = "--repeatInputFrames" in args
        return fmt


@dataclass
class QualityReport:  # pylint: disable=too-many-instance-attributes
    """What the check measured, and the failures."""
    frames_decoded: int = 0
    frames_expected: int = 0
    decoded_size: tuple = (0, 0)
    mean_psnr: List[float] = field(default_factory=list)
    worst_psnr: List[float] = field(default_factory=list)
    worst_picture: int = -1
    failures: List[str] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        """True when every check held."""
        return not self.failures

    def summary(self) -> str:
        """One line for the test log."""
        if not self.mean_psnr:
            return "; ".join(self.failures) or "nothing measured"
        means = ", ".join(f"{p} {v:.2f}" for p, v in zip(PLANES, self.mean_psnr))
        worst = ", ".join(f"{p} {v:.2f}" for p, v in zip(PLANES, self.worst_psnr))
        return (f"{self.frames_decoded} pictures, mean PSNR {means} dB, "
                f"worst {worst} dB (picture {self.worst_picture})")

    def to_dict(self) -> dict:
        """For the JSON export."""
        return {
            "frames_decoded": self.frames_decoded,
            "frames_expected": self.frames_expected,
            "decoded_size": list(self.decoded_size),
            "mean_psnr": self.mean_psnr,
            "worst_psnr": self.worst_psnr,
            "worst_picture": self.worst_picture,
            "failures": self.failures,
        }


def psnr(mse: float, peak: int) -> float:
    """PSNR in dB of a mean squared error, infinite for identical planes."""
    return math.inf if mse == 0 else 10.0 * math.log10(peak * peak / mse)


@dataclass
class PlanePair:
    """A decoded plane, a source plane, and the extent to compare: the top-left
    width x height of both, whose rows are decoded_width and source_width long
    (source_width 0: width)."""
    decoded: bytes
    source: bytes
    width: int
    height: int
    decoded_width: int
    source_width: int = 0

    @property
    def source_stride(self) -> int:
        """Samples per row of the source plane."""
        return self.source_width or self.width


def plane_mse(pair: PlanePair, bits: int) -> float:
    """Mean squared error over the compared extent, the top-left of both planes."""
    try:
        import numpy  # pylint: disable=import-outside-toplevel
    except ImportError:
        return plane_mse_python(pair, bits)
    dtype = numpy.uint8 if bits == 8 else numpy.dtype("<u2")
    dec = numpy.frombuffer(pair.decoded, dtype=dtype).reshape(-1, pair.decoded_width)
    src = numpy.frombuffer(pair.source, dtype=dtype).reshape(-1, pair.source_stride)
    diff = (dec[:pair.height, :pair.width].astype(numpy.float64) -
            src[:pair.height, :pair.width])
    return float(numpy.mean(diff * diff))


def plane_mse_python(pair: PlanePair, bits: int) -> float:
    """plane_mse without numpy."""
    step = 1 if bits == 8 else 2
    total = 0
    for row in range(pair.height):
        first = row * pair.decoded_width * step
        dec_row = memoryview(pair.decoded[first:first + pair.width * step])
        src_first = row * pair.source_stride * step
        src_row = memoryview(pair.source[src_first:src_first + pair.width * step])
        if step == 2:
            dec_row, src_row = dec_row.cast("H"), src_row.cast("H")
        total += sum((a - b) * (a - b) for a, b in zip(dec_row, src_row))
    return total / (pair.width * pair.height)


def read_y4m_header(path: Path) -> dict:
    """Width, height, chroma and depth of a Y4M file, and where its frames start."""
    with open(path, "rb") as f:
        header = f.readline().split()
    info = {"width": 0, "height": 0, "chroma": "420", "bits": 8}
    for token in header[1:]:
        key, value = chr(token[0]), token[1:].decode()
        if key == "W":
            info["width"] = int(value)
        elif key == "H":
            info["height"] = int(value)
        elif key == "C":
            info["chroma"] = value[:3]
            info["bits"] = int(value.split("p")[1]) if "p" in value[3:] else 8
    return info


def source_pictures(path: Path, fmt: SourceFormat) -> List[bytes]:
    """Every picture of the source, as bytes."""
    data = path.read_bytes()
    size = fmt.picture_bytes(fmt.width, fmt.height)
    if not fmt.y4m:
        return [data[i:i + size] for i in range(0, len(data) - size + 1, size)]
    pictures = []
    pos = data.index(b"\n") + 1
    while pos < len(data):
        pos = data.index(b"\n", pos) + 1  # the FRAME line
        pictures.append(data[pos:pos + size])
        pos += size
    return pictures


def decode(stream: Path, pix_fmt: str) -> tuple:
    """(width, height, raw bytes, error) of the stream decoded by ffmpeg."""
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0", str(stream)],
        capture_output=True, text=True, check=False)
    try:
        width, height = (int(x) for x in probe.stdout.strip().split(",")[:2])
    except ValueError:
        return 0, 0, b"", f"ffprobe cannot read the stream: {probe.stderr.strip()[:200]}"
    raw = subprocess.run(
        ["ffmpeg", "-nostdin", "-v", "error", "-i", str(stream), "-fps_mode", "passthrough",
         "-f", "rawvideo", "-pix_fmt", pix_fmt, "-"],
        capture_output=True, check=False)
    if raw.returncode != 0:
        detail = raw.stderr.decode(errors="replace")[:200]
        return width, height, b"", f"ffmpeg cannot decode the stream: {detail}"
    return width, height, raw.stdout, None


def parse_av1_render_size(trace: str) -> Optional[tuple]:
    """(width, height) the first AV1 frame header of a trace_headers log
    renders at, when it signals a render size different from the frame size."""
    fields = {}
    for name, value in re.findall(
            r"\b(render_and_frame_size_different|render_width_minus_1|"
            r"render_height_minus_1)\s+[01]+ = (\d+)", trace):
        if name == "render_and_frame_size_different" and fields:
            break
        fields[name] = int(value)
        if len(fields) == 3:
            break
    if fields.get("render_and_frame_size_different") != 1 or len(fields) != 3:
        return None
    return fields["render_width_minus_1"] + 1, fields["render_height_minus_1"] + 1


def av1_render_size(stream: Path) -> Optional[tuple]:
    """The render size an AV1 stream's first frame header signals, if any."""
    trace = subprocess.run(
        ["ffmpeg", "-nostdin", "-hide_banner", "-i", str(stream), "-c", "copy",
         "-bsf:v", "trace_headers", "-f", "null", "-"],
        capture_output=True, text=True, check=False)
    return parse_av1_render_size(trace.stderr)


def stream_codec(stream: Path) -> str:
    """The codec name ffprobe gives the stream's first video stream."""
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=codec_name", "-of", "csv=p=0", str(stream)],
        capture_output=True, text=True, check=False)
    return probe.stdout.strip()


def expected_frames(fmt: SourceFormat, source_count: int) -> int:
    """How many pictures the encoder codes from a source of source_count."""
    if fmt.repeat and fmt.num_frames:
        return fmt.num_frames
    if fmt.num_frames:
        return min(fmt.num_frames, source_count - fmt.start)
    return source_count - fmt.start


# pylint: disable-next=too-many-locals
def picture_psnr(decoded: bytes, source: bytes, fmt: SourceFormat,
                 decoded_size: tuple) -> List[float]:
    """PSNR of each plane of one decoded picture against its source picture."""
    step = 1 if fmt.bits == 8 else 2
    peak = (1 << fmt.bits) - 1
    dec_pos = src_pos = 0
    values = []
    for (dw, dh), (sw, sh), (ww, wh) in zip(fmt.plane_sizes(*decoded_size),
                                            fmt.plane_sizes(fmt.width, fmt.height),
                                            fmt.plane_sizes(*fmt.window)):
        pair = PlanePair(decoded[dec_pos:dec_pos + dw * dh * step],
                         source[src_pos:src_pos + sw * sh * step], ww, wh, dw, sw)
        values.append(psnr(plane_mse(pair, fmt.bits), peak))
        dec_pos += dw * dh * step
        src_pos += sw * sh * step
    return values


def judge(report: QualityReport, per_picture: List[List[float]],
          check: QualityCheck) -> None:
    """Fill the report's means and worst values, and its threshold failures."""
    count = len(per_picture)
    finite = [[v if math.isfinite(v) else 100.0 for v in p] for p in per_picture]
    report.mean_psnr = [sum(p[k] for p in finite) / count for k in range(3)]
    report.worst_psnr = [min(p[k] for p in finite) for k in range(3)]
    report.worst_picture = min(range(count), key=lambda i: min(finite[i]))
    for plane, mean, worst in zip(PLANES, report.mean_psnr, report.worst_psnr):
        if mean < check.min_mean_psnr:
            report.failures.append(
                f"mean {plane} PSNR {mean:.2f} dB below {check.min_mean_psnr:.2f} dB")
        if check.min_picture_psnr is not None and worst < check.min_picture_psnr:
            report.failures.append(
                f"worst {plane} PSNR {worst:.2f} dB below {check.min_picture_psnr:.2f} dB")


# pylint: disable-next=too-many-locals
def check_quality(stream: Path, source: Path, fmt: SourceFormat,
                  check: Optional[QualityCheck] = None) -> QualityReport:
    """Decode |stream| and measure it against |source|."""
    report = QualityReport()
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        report.failures.append("ffmpeg and ffprobe are needed for the PSNR check")
        return report
    if fmt.y4m:
        info = read_y4m_header(source)
        fmt.width, fmt.height = info["width"], info["height"]
        fmt.chroma, fmt.bits = info["chroma"], info["bits"]
    pictures = source_pictures(source, fmt)
    report.frames_expected = expected_frames(fmt, len(pictures))

    width, height, raw, error = decode(stream, fmt.pix_fmt)
    report.decoded_size = (width, height)
    if error:
        report.failures.append(error)
        return report
    check = check or QualityCheck()
    window_width, window_height = fmt.window
    if check.exact_extent:
        shown, what = (width, height), "decoded size"
        if stream_codec(stream) == "av1":
            render = av1_render_size(stream)
            if render:
                shown, what = render, "render size"
        if shown != fmt.window or width < window_width or height < window_height:
            report.failures.append(
                f"{what} {shown[0]}x{shown[1]} for a {window_width}x{window_height} picture")
            return report
    elif not (window_width <= width < window_width + MAX_EXTENT_PADDING and
              window_height <= height < window_height + MAX_EXTENT_PADDING):
        report.failures.append(
            f"decoded size {width}x{height} for a {window_width}x{window_height} picture")
        return report
    size = fmt.picture_bytes(width, height)
    report.frames_decoded = len(raw) // size
    if report.frames_decoded != report.frames_expected:
        report.failures.append(
            f"{report.frames_decoded} pictures decoded, {report.frames_expected} encoded")
    per_picture = [
        picture_psnr(raw[i * size:(i + 1) * size],
                     pictures[(fmt.start + i) % len(pictures) if fmt.repeat else fmt.start + i],
                     fmt, report.decoded_size)
        for i in range(min(report.frames_decoded, report.frames_expected))]
    if not per_picture:
        report.failures.append("no picture to measure")
        return report
    judge(report, per_picture, check)
    return report
