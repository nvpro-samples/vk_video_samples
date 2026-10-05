"""
Rate-control checks for encoded streams.

Measures how closely an encoded stream follows its rate-control target: the
average bitrate, the size of each frame against the per-frame budget, and the
fullness of a VBV buffer model. Used by encode samples that declare a
"rate_control_check" object, most importantly the ultra-low-latency (ULL) cells,
whose purpose is to hold each frame to its budget under a small VBV.

Frame sizes come from ffprobe's packet list, which needs no decoding. The
decoded frame count is measured separately so a truncated stream cannot pass.

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
import shutil
import subprocess
from pathlib import Path
from typing import List, Optional

# The encoder's default frame rate for every codec (FRAME_RATE_NUM_DEFAULT /
# FRAME_RATE_DEN_DEFAULT in VkEncoderConfigH264.h, VkEncoderConfigH265.h and
# VkEncoderConfigAV1.h).
DEFAULT_FRAME_RATE = 30000.0 / 1001.0


@dataclass
class RateControlCheck:
    """Targets and limits declared by an encode sample.

    Only target_bitrate is required. Every limit that is None is measured and
    reported but not enforced.
    """
    target_bitrate: int
    frame_rate: float = DEFAULT_FRAME_RATE
    vbv_buffer_size: Optional[int] = None
    expected_frames: Optional[int] = None
    max_bitrate_error: Optional[float] = None
    max_frame_ratio: Optional[float] = None
    max_vbv_overflows: Optional[int] = None

    @classmethod
    def from_dict(cls, data: dict) -> 'RateControlCheck':
        """Create from the sample's "rate_control_check" object."""
        if "target_bitrate" not in data:
            raise ValueError("rate_control_check needs target_bitrate")
        known = set(cls.__dataclass_fields__)  # pylint: disable=no-member
        unknown = set(data) - known
        if unknown:
            raise ValueError(
                f"rate_control_check: unknown keys {sorted(unknown)}")
        return cls(**data)

    @property
    def frame_budget_bits(self) -> float:
        """Bits available to each frame at the target bitrate."""
        return self.target_bitrate / self.frame_rate


@dataclass
class RateControlReport:  # pylint: disable=too-many-instance-attributes
    """What was measured, and which limits it broke."""
    frames: int = 0
    decoded_frames: Optional[int] = None
    average_bitrate: float = 0.0
    bitrate_error: float = 0.0
    frame_budget_bits: float = 0.0
    mean_frame_ratio: float = 0.0
    stddev_frame_ratio: float = 0.0
    max_frame_ratio: float = 0.0
    max_frame_index: int = -1
    vbv_overflows: int = 0
    failures: List[str] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        """True when no declared limit was broken."""
        return not self.failures

    def summary(self) -> str:
        """One line for the test log."""
        decoded = ("n/a" if self.decoded_frames is None
                   else str(self.decoded_frames))
        return (f"frames={self.frames} decoded={decoded} "
                f"avg_bitrate={self.average_bitrate:.0f} "
                f"({self.bitrate_error:+.1%}) "
                f"frame/budget mean={self.mean_frame_ratio:.2f} "
                f"sd={self.stddev_frame_ratio:.2f} "
                f"max={self.max_frame_ratio:.2f}@{self.max_frame_index} "
                f"vbv_overflows={self.vbv_overflows}")

    def to_dict(self) -> dict:
        """For the JSON results."""
        return {
            "frames": self.frames,
            "decoded_frames": self.decoded_frames,
            "average_bitrate": self.average_bitrate,
            "bitrate_error": self.bitrate_error,
            "frame_budget_bits": self.frame_budget_bits,
            "mean_frame_ratio": self.mean_frame_ratio,
            "stddev_frame_ratio": self.stddev_frame_ratio,
            "max_frame_ratio": self.max_frame_ratio,
            "max_frame_index": self.max_frame_index,
            "vbv_overflows": self.vbv_overflows,
            "failures": list(self.failures),
        }


def analyze_frame_sizes(frame_bytes: List[int], check: RateControlCheck,
                        decoded_frames: Optional[int] = None
                        ) -> RateControlReport:
    """Measure a list of per-frame sizes, in bytes, against the check.

    The VBV model is the encoder-side leaky bucket: each frame adds its bits,
    each frame interval drains one frame budget, and the fullness never drops
    below zero. A frame that leaves the fullness above vbv_buffer_size is an
    overflow, i.e. a frame the decoder-side buffer could not have held.
    """
    report = RateControlReport(decoded_frames=decoded_frames)
    report.frames = len(frame_bytes)
    report.frame_budget_bits = check.frame_budget_bits

    if report.frames == 0:
        report.failures.append("the stream has no frames")
        return report

    bits = [8 * size for size in frame_bytes]
    duration = report.frames / check.frame_rate
    report.average_bitrate = sum(bits) / duration
    report.bitrate_error = (report.average_bitrate - check.target_bitrate) / \
        check.target_bitrate

    ratios = [b / check.frame_budget_bits for b in bits]
    report.mean_frame_ratio = sum(ratios) / len(ratios)
    report.stddev_frame_ratio = math.sqrt(
        sum((r - report.mean_frame_ratio) ** 2 for r in ratios) / len(ratios))
    report.max_frame_index = max(range(len(ratios)), key=ratios.__getitem__)
    report.max_frame_ratio = ratios[report.max_frame_index]

    if check.vbv_buffer_size:
        fullness = 0.0
        for b in bits:
            fullness = max(0.0, fullness + b - check.frame_budget_bits)
            if fullness > check.vbv_buffer_size:
                report.vbv_overflows += 1

    # Limits.
    if (check.expected_frames is not None and
            report.frames != check.expected_frames):
        report.failures.append(
            f"stream has {report.frames} frames, expected "
            f"{check.expected_frames}")
    if (check.expected_frames is not None and decoded_frames is not None and
            decoded_frames != check.expected_frames):
        report.failures.append(
            f"decoded {decoded_frames} frames, expected "
            f"{check.expected_frames}")
    if (check.max_bitrate_error is not None and
            abs(report.bitrate_error) > check.max_bitrate_error):
        report.failures.append(
            f"average bitrate off target by {report.bitrate_error:+.1%}, "
            f"limit {check.max_bitrate_error:.0%}")
    if (check.max_frame_ratio is not None and
            report.max_frame_ratio > check.max_frame_ratio):
        report.failures.append(
            f"frame {report.max_frame_index} is "
            f"{report.max_frame_ratio:.2f}x its budget, limit "
            f"{check.max_frame_ratio:.2f}x")
    if (check.max_vbv_overflows is not None and
            report.vbv_overflows > check.max_vbv_overflows):
        report.failures.append(
            f"{report.vbv_overflows} VBV overflows, limit "
            f"{check.max_vbv_overflows}")
    return report


def find_ffprobe() -> Optional[str]:
    """Path to ffprobe, or None."""
    return shutil.which("ffprobe")


def probe_packet_sizes(ffprobe: str, stream: Path) -> List[int]:
    """Per-frame sizes in bytes, in stream order, without decoding."""
    out = subprocess.run(
        [ffprobe, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "packet=size", "-of", "csv=p=0", str(stream)],
        capture_output=True, text=True, check=True, timeout=120).stdout
    return [int(line) for line in out.split() if line.strip().isdigit()]


def probe_decoded_frames(ffprobe: str, stream: Path) -> Optional[int]:
    """Number of frames that decode, or None if ffprobe could not count."""
    out = subprocess.run(
        [ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0",
         "-show_entries", "stream=nb_read_frames", "-of", "csv=p=0",
         str(stream)],
        capture_output=True, text=True, check=False, timeout=300).stdout
    value = out.strip().split(",")[0] if out.strip() else ""
    return int(value) if value.isdigit() else None


def check_stream(stream: Path, check: RateControlCheck) -> RateControlReport:
    """Measure an encoded stream against the check.

    Fails, rather than passes, when the stream cannot be measured.
    """
    ffprobe = find_ffprobe()
    if ffprobe is None:
        report = RateControlReport()
        report.failures.append(
            "ffprobe not found on PATH; the rate-control check cannot run")
        return report
    try:
        sizes = probe_packet_sizes(ffprobe, stream)
    except (subprocess.SubprocessError, OSError) as err:
        report = RateControlReport()
        report.failures.append(f"ffprobe could not read the stream: {err}")
        return report
    return analyze_frame_sizes(sizes, check,
                               probe_decoded_frames(ffprobe, stream))
