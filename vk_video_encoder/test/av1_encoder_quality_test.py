#!/usr/bin/env python3
"""
AV1 Encoder Quality Regression Test
====================================
Tests H.264, H.265, and AV1 encoding quality at identical QP settings.
Produces an HTML report with:
  - Codec comparison table (file size + avg PSNR)
  - Per-frame PSNR for AV1 (to detect quality collapse)
  - Per-frame sizes for AV1 vs H.265 (to detect P-frame bloat)

Locations are fully configurable: pass --samples-root to derive the
encoder binary path automatically, or override with --encoder. The
encoder command can be executed on a remote host (with NFS-shared
paths) via --remote-host user@ip; defaults to running locally.
"""

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Optional


DEFAULT_WIDTH = 320
DEFAULT_HEIGHT = 240
DEFAULT_FPS = 30
DEFAULT_NUM_FRAMES = 30
DEFAULT_QP = 26
DEFAULT_GOP_SIZES = [2, 5, 10, 30]
DEFAULT_BPP = 8

PIX_FMT_FOR_BPP = {8: "yuv420p", 10: "yuv420p10le"}

ENCODER_REL_PATH = "build/vk_video_encoder/test/vulkan-video-enc-test"

CODECS = {
    "h264": {"flag": "h264", "ext": "264", "label": "H.264"},
    "h265": {"flag": "h265", "ext": "265", "label": "H.265"},
    "av1":  {"flag": "av1",  "ext": "ivf", "label": "AV1"},
}


@dataclass
class FrameInfo:
    index: int
    frame_type: str  # "I" or "P"
    size_bytes: int
    psnr_avg: Optional[float] = None
    psnr_y: Optional[float] = None
    psnr_u: Optional[float] = None
    psnr_v: Optional[float] = None


@dataclass
class EncodeResult:
    codec: str
    gop: int
    file_size: int
    psnr_avg: Optional[float] = None
    psnr_y: Optional[float] = None
    psnr_u: Optional[float] = None
    psnr_v: Optional[float] = None
    frames: list = field(default_factory=list)
    encode_ok: bool = True
    decode_ok: bool = True
    error_msg: str = ""


def run_cmd(cmd, description="", timeout=120, remote_host=None):
    """Run a command, return (returncode, stdout, stderr).

    If remote_host is set (e.g. "user@192.168.122.216" or "192.168.122.216"),
    the command is wrapped with ssh and executed on the remote host. Paths
    are assumed to be valid on the remote (typically via NFS-shared mounts).
    """
    if remote_host:
        if isinstance(cmd, list):
            remote_cmd = " ".join(shlex.quote(c) for c in cmd)
        else:
            remote_cmd = cmd
        cmd = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
               remote_host, remote_cmd]

    if description:
        print(f"  >> {description}{' [remote: ' + remote_host + ']' if remote_host else ''}")
    print(f"     $ {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    try:
        r = subprocess.run(
            cmd, shell=isinstance(cmd, str),
            capture_output=True, text=True, timeout=timeout,
        )
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return -1, "", f"TIMEOUT after {timeout}s"


def generate_test_input(yuv_path, width, height, fps, num_frames, bpp=8):
    """Generate raw YUV420p (8-bit or 10-bit) test frames with ffmpeg."""
    duration = num_frames / fps
    pix_fmt = PIX_FMT_FOR_BPP[bpp]
    cmd = [
        "ffmpeg", "-y", "-f", "lavfi",
        "-i", f"testsrc=duration={duration}:size={width}x{height}:rate={fps}",
        "-pix_fmt", pix_fmt, "-f", "rawvideo", yuv_path,
    ]
    rc, out, err = run_cmd(cmd, f"Generating test input YUV ({bpp}-bit, {pix_fmt})")
    if rc != 0:
        print(f"ERROR generating test input: {err}")
        sys.exit(1)
    print(f"     Generated {yuv_path} ({os.path.getsize(yuv_path)} bytes)")


def encode(yuv_path, out_path, codec, width, height, num_frames, gop, qp,
           encoder_bin, remote_host=None, bpp=8):
    """Encode YUV with the Vulkan encoder.

    The encoder process runs on remote_host if set (the GPU machine);
    YUV/output paths must be reachable on that host (e.g. NFS-shared).
    """
    flag = CODECS[codec]["flag"]
    cmd = [
        encoder_bin,
        "-i", yuv_path,
        "-o", out_path,
        "-c", flag,
        "--inputWidth", str(width),
        "--inputHeight", str(height),
        "--inputBpp", str(bpp),
        "--numFrames", str(num_frames),
        "--gopFrameCount", str(gop),
        "--idrPeriod", str(gop),
        "--consecutiveBFrameCount", "0",
        "--rateControlMode", "disabled",
        "--qpI", str(qp),
        "--qpP", str(qp),
    ]
    rc, out, err = run_cmd(cmd, f"Encoding {CODECS[codec]['label']} GOP={gop}",
                           remote_host=remote_host)
    if rc != 0:
        return False, f"Encoder returned {rc}: {err[-500:]}"
    if not os.path.exists(out_path) or os.path.getsize(out_path) == 0:
        return False, "Output file missing or empty"
    return True, ""


def decode_to_yuv(encoded_path, decoded_yuv_path, codec, bpp=8):
    """Decode encoded file back to raw YUV420p (matching bit depth)."""
    pix_fmt = PIX_FMT_FOR_BPP[bpp]
    cmd = [
        "ffmpeg", "-y", "-i", encoded_path,
        "-pix_fmt", pix_fmt, "-f", "rawvideo", decoded_yuv_path,
    ]
    rc, out, err = run_cmd(cmd, f"Decoding {encoded_path}")
    if rc != 0:
        return False, f"Decode failed: {err[-500:]}"
    return True, ""


def measure_psnr_overall(ref_yuv, dec_yuv, width, height, bpp=8):
    """Measure overall PSNR between reference and decoded YUV (bit-depth aware)."""
    pix_fmt = PIX_FMT_FOR_BPP[bpp]
    cmd = [
        "ffmpeg",
        "-s", f"{width}x{height}", "-pix_fmt", pix_fmt, "-f", "rawvideo", "-i", ref_yuv,
        "-s", f"{width}x{height}", "-pix_fmt", pix_fmt, "-f", "rawvideo", "-i", dec_yuv,
        "-lavfi", "psnr", "-f", "null", "-",
    ]
    rc, out, err = run_cmd(cmd, "Measuring overall PSNR")
    combined = out + err
    match = re.search(r'average:(\S+)', combined)
    if match:
        val = match.group(1)
        return float(val) if val != "inf" else 999.99
    return None


def measure_psnr_per_frame(ref_yuv, dec_yuv, width, height, logfile, bpp=8):
    """Measure per-frame PSNR (bit-depth aware), returns list of per-frame dicts."""
    pix_fmt = PIX_FMT_FOR_BPP[bpp]
    cmd = [
        "ffmpeg",
        "-s", f"{width}x{height}", "-pix_fmt", pix_fmt, "-f", "rawvideo", "-i", ref_yuv,
        "-s", f"{width}x{height}", "-pix_fmt", pix_fmt, "-f", "rawvideo", "-i", dec_yuv,
        "-lavfi", f"psnr=stats_file={logfile}", "-f", "null", "-",
    ]
    rc, out, err = run_cmd(cmd, "Measuring per-frame PSNR")
    frames = []
    if os.path.exists(logfile):
        with open(logfile) as f:
            for line in f:
                m = re.match(
                    r'n:(\d+)\s+mse_avg:(\S+)\s+mse_y:(\S+)\s+mse_u:(\S+)\s+mse_v:(\S+)\s+'
                    r'psnr_avg:(\S+)\s+psnr_y:(\S+)\s+psnr_u:(\S+)\s+psnr_v:(\S+)',
                    line.strip(),
                )
                if m:
                    def safe_float(s):
                        return 999.99 if s == "inf" else float(s)
                    frames.append({
                        "n": int(m.group(1)),
                        "psnr_avg": safe_float(m.group(6)),
                        "psnr_y": safe_float(m.group(7)),
                        "psnr_u": safe_float(m.group(8)),
                        "psnr_v": safe_float(m.group(9)),
                    })
    return frames


def get_per_frame_sizes_ivf(ivf_path):
    """Parse IVF container to get per-frame sizes."""
    sizes = []
    try:
        with open(ivf_path, "rb") as f:
            header = f.read(32)
            if len(header) < 32:
                return sizes
            num_frames_field = int.from_bytes(header[24:28], "little")
            for i in range(num_frames_field):
                frame_header = f.read(12)
                if len(frame_header) < 12:
                    break
                frame_len = int.from_bytes(frame_header[0:4], "little")
                frame_data = f.read(frame_len)
                if len(frame_data) < frame_len:
                    break
                sizes.append(frame_len)
    except Exception as e:
        print(f"  Warning: IVF parse error: {e}")
    return sizes


def get_per_frame_sizes_h26x(encoded_path, codec):
    """Use ffprobe to get per-frame sizes for H.264/H.265."""
    cmd = [
        "ffprobe", "-v", "quiet",
        "-show_frames", "-select_streams", "v",
        "-print_format", "json", encoded_path,
    ]
    rc, out, err = run_cmd(cmd, f"Getting per-frame sizes for {codec}")
    sizes = []
    types = []
    if rc == 0 and out:
        try:
            data = json.loads(out)
            for fr in data.get("frames", []):
                sz = int(fr.get("pkt_size", 0))
                ft = fr.get("pict_type", "?")
                sizes.append(sz)
                types.append(ft)
        except json.JSONDecodeError:
            pass
    return sizes, types


def psnr_color(val):
    """Return CSS color class based on PSNR value."""
    if val is None:
        return "gray"
    if val >= 50:
        return "#27ae60"  # green
    if val >= 45:
        return "#f39c12"  # orange
    return "#e74c3c"      # red


def psnr_color_class(val):
    if val is None:
        return "psnr-na"
    if val >= 50:
        return "psnr-good"
    if val >= 45:
        return "psnr-warn"
    return "psnr-bad"


def generate_html_report(results, per_frame_data, per_frame_sizes, args, output_path, encoder_bin):
    """Generate a comprehensive HTML report."""
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Vulkan Video Encoder Quality Report</title>
<style>
  :root {{
    --bg: #1a1a2e;
    --surface: #16213e;
    --surface2: #0f3460;
    --text: #e0e0e0;
    --text-dim: #a0a0a0;
    --accent: #e94560;
    --border: #2a2a4a;
  }}
  * {{ margin: 0; padding: 0; box-sizing: border-box; }}
  body {{
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
    background: var(--bg);
    color: var(--text);
    padding: 2rem;
    line-height: 1.6;
  }}
  h1 {{
    color: #fff;
    font-size: 1.8rem;
    margin-bottom: 0.3rem;
    border-bottom: 2px solid var(--accent);
    padding-bottom: 0.5rem;
  }}
  h2 {{
    color: #ddd;
    font-size: 1.3rem;
    margin: 2rem 0 0.8rem 0;
    padding-bottom: 0.3rem;
    border-bottom: 1px solid var(--border);
  }}
  h3 {{
    color: #ccc;
    font-size: 1.1rem;
    margin: 1.5rem 0 0.5rem 0;
  }}
  .meta {{
    color: var(--text-dim);
    font-size: 0.85rem;
    margin-bottom: 1.5rem;
  }}
  table {{
    border-collapse: collapse;
    width: auto;
    margin: 0.5rem 0 1.5rem 0;
    font-size: 0.9rem;
  }}
  th, td {{
    padding: 0.45rem 1rem;
    text-align: left;
    border: 1px solid var(--border);
  }}
  th {{
    background: var(--surface2);
    color: #fff;
    font-weight: 600;
    white-space: nowrap;
  }}
  td {{
    background: var(--surface);
  }}
  tr:hover td {{
    background: #1a2a5e;
  }}
  .psnr-good {{ color: #27ae60; font-weight: 700; }}
  .psnr-warn {{ color: #f39c12; font-weight: 700; }}
  .psnr-bad  {{ color: #e74c3c; font-weight: 700; }}
  .psnr-na   {{ color: #888; }}
  .frame-type-i {{ color: #3498db; font-weight: 700; }}
  .frame-type-p {{ color: var(--text-dim); }}
  .note {{ color: var(--text-dim); font-style: italic; font-size: 0.85rem; }}
  .codec-section {{
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 1.2rem 1.5rem;
    margin: 1rem 0;
  }}
  .summary-box {{
    background: var(--surface2);
    border-radius: 6px;
    padding: 1rem 1.5rem;
    margin: 1rem 0;
    border-left: 4px solid var(--accent);
  }}
  .side-by-side {{
    display: flex;
    gap: 2rem;
    flex-wrap: wrap;
  }}
  .side-by-side > div {{
    flex: 1;
    min-width: 300px;
  }}
  .error {{ color: #e74c3c; font-weight: bold; }}
  footer {{
    margin-top: 3rem;
    padding-top: 1rem;
    border-top: 1px solid var(--border);
    color: var(--text-dim);
    font-size: 0.8rem;
  }}
</style>
</head>
<body>

<h1>Vulkan Video Encoder Quality Report</h1>
<div class="meta">
  Generated: {now}<br>
  Resolution: {args.width}x{args.height} | Frames: {args.num_frames} | QP: {args.qp} | FPS: {args.fps}<br>
  Rate Control: disabled (CQP) | GOP sizes: {', '.join(str(g) for g in args.gop_sizes)}<br>
  Encoder: <code>{encoder_bin}</code>
</div>
"""

    # ── Section 1: Codec Comparison Table ──
    html += f"""
<h2>Codec Comparison at Identical QP={args.qp} ({args.num_frames} frames, CQP/disabled rate control)</h2>
<table>
  <tr>
    <th>Codec</th>
    <th>GOP</th>
    <th>File Size</th>
    <th>PSNR (avg)</th>
  </tr>
"""
    for codec_key in ["h264", "h265", "av1"]:
        label = CODECS[codec_key]["label"]
        codec_results = [r for r in results if r.codec == codec_key]
        codec_results.sort(key=lambda r: r.gop)
        for r in codec_results:
            if not r.encode_ok:
                html += f'  <tr><td>{label}</td><td>{r.gop}</td><td colspan="2" class="error">{r.error_msg}</td></tr>\n'
                continue
            psnr_str = f"{r.psnr_avg:.2f} dB" if r.psnr_avg is not None else "N/A"
            psnr_cls = psnr_color_class(r.psnr_avg)
            html += (
                f'  <tr><td>{label}</td><td>{r.gop}</td>'
                f'<td>{r.file_size:,} B</td>'
                f'<td class="{psnr_cls}">{psnr_str}</td></tr>\n'
            )

    html += "</table>\n"

    # ── Section 2: Per-codec detailed sections ──
    for codec_key in ["h264", "h265", "av1"]:
        label = CODECS[codec_key]["label"]
        codec_results = [r for r in results if r.codec == codec_key]
        codec_results.sort(key=lambda r: r.gop)

        html += f'\n<div class="codec-section">\n<h2>{label} Detailed Results</h2>\n'

        for r in codec_results:
            if not r.encode_ok:
                html += f'<h3>GOP={r.gop}</h3><p class="error">Encode failed: {r.error_msg}</p>\n'
                continue

            key = (codec_key, r.gop)
            frames_psnr = per_frame_data.get(key, [])
            frames_sizes = per_frame_sizes.get(key, ([], []))

            html += f'<h3>GOP={r.gop} &mdash; File size: {r.file_size:,} B, Avg PSNR: '
            psnr_cls = psnr_color_class(r.psnr_avg)
            psnr_str = f"{r.psnr_avg:.2f} dB" if r.psnr_avg is not None else "N/A"
            html += f'<span class="{psnr_cls}">{psnr_str}</span></h3>\n'

            # Per-frame tables side by side: PSNR + sizes
            html += '<div class="side-by-side">\n'

            # Per-frame PSNR
            if frames_psnr:
                html += '<div>\n<table>\n<tr><th>Frame</th><th>PSNR (avg)</th><th>Type</th><th>Notes</th></tr>\n'
                for i, fp in enumerate(frames_psnr):
                    frame_num = fp["n"] + 1
                    is_iframe = (i % r.gop == 0)
                    ft = "I" if is_iframe else "P"
                    ft_cls = "frame-type-i" if is_iframe else "frame-type-p"
                    psnr_val = fp["psnr_avg"]
                    psnr_cls = psnr_color_class(psnr_val)
                    psnr_s = f"{psnr_val:.2f} dB" if psnr_val < 900 else "inf"

                    note = ""
                    if is_iframe:
                        note = "I-frame"
                        if i > 0:
                            note += " (recovers)" if psnr_val > 40 else " (I-frame)"
                    elif i > 0 and frames_psnr[i-1]["psnr_avg"] - psnr_val > 5:
                        note = "severe degradation"
                    elif i > 0 and frames_psnr[i-1]["psnr_avg"] - psnr_val > 2:
                        note = "degrading"

                    html += (
                        f'<tr><td>{frame_num}</td>'
                        f'<td class="{psnr_cls}">{psnr_s}</td>'
                        f'<td class="{ft_cls}">{ft}-frame</td>'
                        f'<td class="note">{note}</td></tr>\n'
                    )
                html += '</table>\n</div>\n'

            # Per-frame sizes
            sizes, types = frames_sizes
            if sizes:
                html += '<div>\n<table>\n<tr><th>Frame</th><th>Type</th><th>Size</th></tr>\n'
                for i, sz in enumerate(sizes):
                    is_iframe = (i % r.gop == 0)
                    ft = "I" if is_iframe else "P"
                    ft_cls = "frame-type-i" if is_iframe else "frame-type-p"
                    # For H.26x we have pict_type from ffprobe
                    if types and i < len(types):
                        ft = types[i]
                        ft_cls = "frame-type-i" if ft == "I" else "frame-type-p"
                    html += f'<tr><td>{i+1}</td><td class="{ft_cls}">{ft}</td><td>{sz:,} B</td></tr>\n'
                html += '</table>\n</div>\n'

            html += '</div>\n'  # side-by-side

        html += '</div>\n'  # codec-section

    # ── Section 3: AV1 vs H.265 per-frame size comparison for largest GOP ──
    max_gop = max(args.gop_sizes)
    av1_sizes_key = ("av1", max_gop)
    h265_sizes_key = ("h265", max_gop)

    av1_frame_sizes = per_frame_sizes.get(av1_sizes_key, ([], []))
    h265_frame_sizes = per_frame_sizes.get(h265_sizes_key, ([], []))

    if av1_frame_sizes[0] or h265_frame_sizes[0]:
        html += f'\n<h2>Per-Frame Sizes: AV1 vs H.265 (GOP={max_gop})</h2>\n'
        html += '<div class="summary-box">\n'
        html += '<p>AV1 P-frames should remain small like H.265. If they grow to I-frame size, '
        html += 'reference frame management is broken.</p>\n</div>\n'
        html += '<div class="side-by-side">\n'

        for label, (sizes, types), codec_key in [
            ("AV1", av1_frame_sizes, "av1"),
            ("H.265", h265_frame_sizes, "h265"),
        ]:
            if sizes:
                html += f'<div>\n<h3>{label} GOP={max_gop}</h3>\n'
                html += '<table>\n<tr><th>Frame</th><th>Type</th><th>Size</th></tr>\n'
                for i, sz in enumerate(sizes):
                    is_iframe = (i % max_gop == 0)
                    ft = "I" if is_iframe else "P"
                    if types and i < len(types):
                        ft = types[i]
                    ft_cls = "frame-type-i" if ft == "I" else "frame-type-p"
                    html += f'<tr><td>{i+1}</td><td class="{ft_cls}">{ft}</td><td>{sz:,} B</td></tr>\n'
                html += '</table>\n</div>\n'

        html += '</div>\n'

    # ── Footer ──
    html += f"""
<footer>
  Vulkan Video Encoder Quality Test &mdash; {now}<br>
  PSNR color coding: <span class="psnr-good">&ge;50 dB (good)</span> |
  <span class="psnr-warn">45&ndash;50 dB (warning)</span> |
  <span class="psnr-bad">&lt;45 dB (bad)</span>
</footer>
</body>
</html>
"""
    with open(output_path, "w") as f:
        f.write(html)
    print(f"\nReport written to: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Vulkan Video Encoder Quality Test",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:

  # Local run, explicit encoder binary
  %(prog)s --encoder ./build/vk_video_encoder/test/vulkan-video-enc-test

  # Local run, derive encoder from samples-root
  %(prog)s --samples-root /path/to/vulkan-video-samples

  # Remote run on GPU VM (NFS-shared paths)
  %(prog)s --samples-root /data/.../vulkan-video-samples \\
           --remote-host tzlatinski@192.168.122.216
""")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS)
    parser.add_argument("--num-frames", type=int, default=DEFAULT_NUM_FRAMES)
    parser.add_argument("--qp", type=int, default=DEFAULT_QP)
    parser.add_argument("--bpp", type=int, default=DEFAULT_BPP, choices=[8, 10],
                        help="Input/encode bit depth (8 or 10).")
    parser.add_argument("--gop-sizes", type=int, nargs="+", default=DEFAULT_GOP_SIZES)
    parser.add_argument("--codecs", type=str, nargs="+", default=list(CODECS.keys()),
                        choices=list(CODECS.keys()))
    parser.add_argument("--output-dir", type=str, default="/tmp/vk_enc_quality_test")
    parser.add_argument("--report", type=str, default=None,
                        help="HTML report output path (default: <output-dir>/report.html)")
    parser.add_argument("--samples-root", type=str, default=None,
                        help="Root of vulkan-video-samples checkout. "
                             "Used to derive --encoder if not given "
                             "(<root>/" + ENCODER_REL_PATH + ").")
    parser.add_argument("--encoder", type=str, default=None,
                        help="Full path to vulkan-video-enc-test binary "
                             "(overrides --samples-root derivation).")
    parser.add_argument("--remote-host", type=str, default=None,
                        help="Run encoder on remote host via ssh "
                             "(e.g. user@192.168.122.216 or 192.168.122.216). "
                             "Default: run locally. Encoder paths must be "
                             "reachable on the remote (NFS-shared).")
    args = parser.parse_args()

    encoder_bin = args.encoder
    if not encoder_bin:
        if not args.samples_root:
            parser.error("either --encoder or --samples-root must be specified")
        encoder_bin = os.path.join(args.samples_root, ENCODER_REL_PATH)

    if not args.remote_host and not os.path.isfile(encoder_bin):
        parser.error(f"encoder binary not found: {encoder_bin}\n"
                     f"  Pass --encoder PATH or --samples-root DIR (must contain "
                     f"{ENCODER_REL_PATH}). Use --remote-host to skip the local "
                     f"existence check.")

    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            parser.error(f"{tool} not found on PATH (required for encode/decode/PSNR)")

    os.makedirs(args.output_dir, exist_ok=True)
    if args.report is None:
        args.report = os.path.join(args.output_dir, "report.html")

    yuv_path = os.path.join(args.output_dir, f"test_frames_{args.width}x{args.height}.yuv")

    print("=" * 70)
    print("Vulkan Video Encoder Quality Test")
    print("=" * 70)
    print(f"  Resolution : {args.width}x{args.height}")
    print(f"  Bit depth  : {args.bpp}")
    print(f"  Frames     : {args.num_frames}")
    print(f"  QP         : {args.qp}")
    print(f"  GOP sizes  : {args.gop_sizes}")
    print(f"  Codecs     : {args.codecs}")
    print(f"  Output dir : {args.output_dir}")
    print(f"  Encoder    : {encoder_bin}")
    print(f"  Run mode   : {'remote (' + args.remote_host + ')' if args.remote_host else 'local'}")
    print()

    # Step 1: Generate test input
    print("─" * 50)
    print("Step 1: Generate test input")
    print("─" * 50)
    generate_test_input(yuv_path, args.width, args.height, args.fps, args.num_frames,
                        bpp=args.bpp)
    print()

    results = []
    per_frame_data = {}
    per_frame_sizes = {}

    # Step 2: Encode + Decode + Measure for each codec/GOP combo
    for codec_key in args.codecs:
        label = CODECS[codec_key]["label"]
        print("─" * 50)
        print(f"Step 2: Testing {label}")
        print("─" * 50)

        for gop in args.gop_sizes:
            print(f"\n  --- {label} GOP={gop} ---")
            ext = CODECS[codec_key]["ext"]
            encoded_path = os.path.join(args.output_dir, f"{codec_key}_gop{gop}.{ext}")
            decoded_path = os.path.join(args.output_dir, f"decoded_{codec_key}_gop{gop}.yuv")
            psnr_log = os.path.join(args.output_dir, f"psnr_{codec_key}_gop{gop}.log")

            result = EncodeResult(codec=codec_key, gop=gop, file_size=0)

            # Encode
            ok, err = encode(yuv_path, encoded_path, codec_key,
                             args.width, args.height, args.num_frames, gop, args.qp,
                             encoder_bin=encoder_bin, remote_host=args.remote_host,
                             bpp=args.bpp)
            if not ok:
                result.encode_ok = False
                result.error_msg = err
                results.append(result)
                print(f"  ENCODE FAILED: {err}")
                continue

            result.file_size = os.path.getsize(encoded_path)

            # Decode
            ok, err = decode_to_yuv(encoded_path, decoded_path, codec_key, bpp=args.bpp)
            if not ok:
                result.decode_ok = False
                result.error_msg = err
                results.append(result)
                print(f"  DECODE FAILED: {err}")
                continue

            # Overall PSNR
            psnr = measure_psnr_overall(yuv_path, decoded_path, args.width, args.height,
                                        bpp=args.bpp)
            result.psnr_avg = psnr
            print(f"  File size: {result.file_size:,} B | PSNR: {psnr:.2f} dB" if psnr else
                  f"  File size: {result.file_size:,} B | PSNR: N/A")

            # Per-frame PSNR
            frames_psnr = measure_psnr_per_frame(yuv_path, decoded_path,
                                                  args.width, args.height, psnr_log,
                                                  bpp=args.bpp)
            per_frame_data[(codec_key, gop)] = frames_psnr

            # Per-frame sizes
            if codec_key == "av1":
                sizes = get_per_frame_sizes_ivf(encoded_path)
                is_iframe_types = []
                for i in range(len(sizes)):
                    is_iframe_types.append("I" if i % gop == 0 else "P")
                per_frame_sizes[(codec_key, gop)] = (sizes, is_iframe_types)
            else:
                sizes, types = get_per_frame_sizes_h26x(encoded_path, codec_key)
                per_frame_sizes[(codec_key, gop)] = (sizes, types)

            results.append(result)

    # Step 3: Generate report
    print("\n" + "─" * 50)
    print("Step 3: Generating HTML report")
    print("─" * 50)
    generate_html_report(results, per_frame_data, per_frame_sizes, args, args.report, encoder_bin)

    # Console summary
    print("\n" + "=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print(f"{'Codec':<8} {'GOP':>4} {'File Size':>12} {'PSNR (avg)':>12}")
    print("-" * 40)
    for r in results:
        label = CODECS[r.codec]["label"]
        if not r.encode_ok:
            print(f"{label:<8} {r.gop:>4} {'FAILED':>12} {'':>12}")
            continue
        psnr_s = f"{r.psnr_avg:.2f} dB" if r.psnr_avg is not None else "N/A"
        print(f"{label:<8} {r.gop:>4} {r.file_size:>10,} B {psnr_s:>12}")
    print()

    # Flag AV1 issues
    av1_results = [r for r in results if r.codec == "av1" and r.encode_ok]
    if av1_results:
        print("AV1 Quality Analysis:")
        for r in av1_results:
            frames = per_frame_data.get(("av1", r.gop), [])
            if not frames:
                continue
            min_psnr = min(f["psnr_avg"] for f in frames)
            max_psnr = max(f["psnr_avg"] for f in frames)
            spread = max_psnr - min_psnr
            if spread > 10:
                print(f"  ⚠ GOP={r.gop}: PSNR spread {spread:.1f} dB "
                      f"(min={min_psnr:.1f}, max={max_psnr:.1f}) — quality collapse detected")
            else:
                print(f"  ✓ GOP={r.gop}: PSNR spread {spread:.1f} dB — acceptable")

    return 0


if __name__ == "__main__":
    sys.exit(main())
