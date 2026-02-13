# NVIDIA Preset JSON Guide (Use-Case Oriented)

This directory contains NVIDIA-oriented preset JSON files for `vk_video_encoder`.
These presets are intended to match the NVENC preset+tuning model used by:

- NVIDIA Video Codec SDK preset guidance
- Internal NVENC preset tables
- Vulkan Video quality-level/tuning behavior in current drivers

Use this README to choose the right preset for your workload and understand what each file is optimizing for.

Current driver note:
- `qualityPreset` values `5` (P6) and `6` (P7) are not supported yet.
- Maximum supported `qualityPreset` is currently `4` (P5).

## 1. Quick Start

1. Pick a use case from the matrix below.
2. Start with the recommended JSON file.
3. Run the encoder with `--encoderConfig <file>`.
4. Override bitrate and frame-size related knobs from CLI for your target stream.

Example:

```bash
./vulkan-video-enc-test \
  --encoderConfig vk_video_encoder/json_config/nvidia/high_quality_p4.json \
  -i input.yuv -o out.264 \
  --inputWidth 1920 --inputHeight 1080 --codec h264
```

CLI parameters always override JSON values.

## 2. Use-Case Selection Matrix

| Use case | Primary goal | Recommended file(s) | Why |
|---|---|---|---|
| VOD transcoding / OTT packaging | Quality per bit | `high_quality_p4.json` to `high_quality_p5.json` | More analysis and stronger prediction structure |
| Fast batch transcode | Throughput | `high_quality_p1.json` to `high_quality_p3.json` | Higher speed at lower compression efficiency |
| Interactive streaming (gaming, conferencing) | Lower latency with stable bitrate | `low_latency_p1.json` to `low_latency_p3.json` | CBR + low-latency tuning |
| Very strict latency budgets | Minimal end-to-end delay | `ultra_low_latency_p1.json` | ULL tuning, CBR, no B-frames |
| Mezzanine/mastering capture | Preserve source fidelity | `lossless.json` | CQP with QP=0 |

## 3. Preset Numbering (Important)

NVIDIA P presets are **zero-based** in the current Vulkan/NVENC stack:

- `P1 -> qualityPreset = 0`
- `P2 -> qualityPreset = 1`
- `P3 -> qualityPreset = 2`
- `P4 -> qualityPreset = 3`
- `P5 -> qualityPreset = 4`
- `P6 -> qualityPreset = 5`
- `P7 -> qualityPreset = 6`

`P1` is fastest / lowest quality. `P7` is slowest / highest quality.

Driver support status (current):
- Supported now: `qualityPreset 0..4` (P1..P5)
- Not supported yet: `qualityPreset 5..6` (P6..P7)

## 4. Files in This Directory

| File | Tuning intent | Preset | Current support |
|---|---|---|---|
| `high_quality_p1.json` | High quality | P1 | Supported |
| `high_quality_p2.json` | High quality | P2 | Supported |
| `high_quality_p3.json` | High quality | P3 | Supported |
| `high_quality_p4.json` | High quality | P4 | Supported |
| `high_quality_p5.json` | High quality | P5 | Supported |
| `high_quality_p6.json` | High quality | P6 | Not supported yet (qualityPreset=5) |
| `high_quality_p7.json` | High quality | P7 | Not supported yet (qualityPreset=6) |
| `low_latency_p1.json` | Low latency | P1 | Supported |
| `low_latency_p2.json` | Low latency | P2 | Supported |
| `low_latency_p3.json` | Low latency | P3 | Supported |
| `ultra_low_latency_p1.json` | Ultra-low latency | P1 | Supported |
| `lossless.json` | Lossless | P1-style generic | Supported |

Also see:

- `PreferredSettings_extracted.md` (spreadsheet extraction notes)
- `preset_review_report.md` (what was corrected and what is still missing)

## 5. What These Presets Configure

### 5.1 High Quality (HQ)

`high_quality_p1..p7.json` use:

- `rcMode = vbr`
- `gopLength = 250`
- `idrPeriod = 250`
- `bFrameCount` progression by preset:
  - P1: 0
  - P2: 0
  - P3: 1
  - P4-P7: 3

Use HQ when latency is not the top priority and compression quality matters.

### 5.2 Low Latency (LL)

`low_latency_p1..p3.json` use:

- `rcMode = cbr`
- `gopLength = 4294967295` (infinite)
- `idrPeriod = 4294967295` (infinite)
- `bFrameCount = 0`

Use LL for real-time applications where deterministic transport and responsiveness are more important than maximum compression efficiency.

### 5.3 Ultra-Low Latency (ULL)

`ultra_low_latency_p1.json` uses:

- `rcMode = cbr`
- `gopLength = 4294967295` (infinite)
- `idrPeriod = 4294967295` (infinite)
- `bFrameCount = 0`

Use ULL for the strictest latency constraints.

### 5.4 Lossless

`lossless.json` uses:

- `rcMode = cqp`
- `constQpI/P/B = 0`
- `gopLength = 250`
- `idrPeriod = 250`
- `bFrameCount = 0`

Use for archival/mezzanine workflows where bitrate is secondary to fidelity.

## 6. Recommended Starting Points by Scenario

### 6.1 Cloud gaming / remote desktop

Start from `low_latency_p2.json`:

```bash
./vulkan-video-enc-test \
  --encoderConfig vk_video_encoder/json_config/nvidia/low_latency_p2.json \
  -i input.yuv -o stream.264 --inputWidth 1920 --inputHeight 1080 --codec h264 \
  --averageBitrate 8000000 --maxBitrate 8000000
```

### 6.2 Conferencing

Start from `low_latency_p1.json`:

```bash
./vulkan-video-enc-test \
  --encoderConfig vk_video_encoder/json_config/nvidia/low_latency_p1.json \
  -i input.yuv -o conf.264 --inputWidth 1280 --inputHeight 720 --codec h264 \
  --averageBitrate 2500000 --maxBitrate 2500000
```

### 6.3 VOD transcode (quality focused)

Start from `high_quality_p5.json`:

```bash
./vulkan-video-enc-test \
  --encoderConfig vk_video_encoder/json_config/nvidia/high_quality_p5.json \
  -i input.yuv -o vod.264 --inputWidth 1920 --inputHeight 1080 --codec h264 \
  --averageBitrate 6000000 --maxBitrate 12000000
```

### 6.4 Near-master recording

Start from `lossless.json`:

```bash
./vulkan-video-enc-test \
  --encoderConfig vk_video_encoder/json_config/nvidia/lossless.json \
  -i input.yuv -o master.264 --inputWidth 1920 --inputHeight 1080 --codec h264
```

## 7. Practical Tuning Rules

- Raise preset level (toward P7) when quality is insufficient and latency budget allows.
- Lower preset level (toward P1) when encode throughput or per-frame latency is the bottleneck.
- For live/interactive paths, keep CBR and avoid B-frames unless you can tolerate added reordering delay.
- For quality workflows, keep VBR and allow higher max bitrate bursts.
- Keep GOP/IDR strategy aligned with your segment/keyframe requirements from the packager/player side.

## 8. Known Gaps vs Full NVENC Preset Model

Current JSON preset files do not fully expose every NVENC preset knob. Important examples:

- `lowDelayKeyFrameScale`
- `multiPass`
- `enableLookahead` / `lookaheadDepth`
- `useBFramesAsRef` reference pattern selection
- H.264 preset-level entropy/adaptive-transform controls

These are tracked in `preset_review_report.md`.

## 9. Note on `tuningMode` in JSON

These preset JSON files include a `tuningMode` field for clarity and future-proofing.
If your local JSON schema/loader does not yet recognize `tuningMode`, pass it explicitly via CLI:

```bash
--tuningMode highquality
--tuningMode lowlatency
--tuningMode ultralowlatency
--tuningMode lossless
```

## 10. Validation Checklist

Before signing off a preset for production:

1. Verify end-to-end latency under target load.
2. Verify quality with representative content (dark scenes, high motion, text/UI).
3. Verify bitrate compliance at transport and mux layer.
4. Verify keyframe behavior against packaging/segment boundaries.
5. Verify stability across dynamic resolution/framerate changes if your pipeline reconfigures at runtime.

## 11. Codec-Specific Recommendations (H.264 / HEVC / AV1)

These preset files were authored and verified primarily for H.264. You can still use the same tuning intent for HEVC/AV1 by overriding `--codec`, but you should validate quality, latency, and caps per codec.

### 11.1 H.264 (AVC)

- Best for broadest compatibility and lowest decoder complexity.
- Recommended defaults:
  - Live interactive: `low_latency_p1.json` or `low_latency_p2.json`
  - VOD: `high_quality_p4.json` to `high_quality_p6.json`
  - Lossless: `lossless.json`
- Typical tradeoff: higher bitrate than HEVC/AV1 for similar quality.

Example:

```bash
./vulkan-video-enc-test \
  --encoderConfig vk_video_encoder/json_config/nvidia/high_quality_p5.json \
  --codec h264 \
  -i input.yuv -o out_h264.264 --inputWidth 1920 --inputHeight 1080 \
  --averageBitrate 6000000 --maxBitrate 12000000
```

### 11.2 HEVC (H.265)

- Best when you want improved compression efficiency while keeping mature decoder support.
- Recommended defaults:
  - Live interactive: start with `low_latency_p2.json`, override `--codec h265`
  - VOD: start with `high_quality_p5.json`, override `--codec h265`
  - Very low latency: `ultra_low_latency_p1.json`, override `--codec h265`
- Practical bitrate guidance: HEVC often reaches similar visual quality at lower bitrate than H.264.

Example:

```bash
./vulkan-video-enc-test \
  --encoderConfig vk_video_encoder/json_config/nvidia/high_quality_p5.json \
  --codec h265 \
  -i input.yuv -o out_hevc.265 --inputWidth 1920 --inputHeight 1080 \
  --averageBitrate 4500000 --maxBitrate 9000000
```

### 11.3 AV1

- Best compression efficiency option when encoder/decoder support and latency budget allow it.
- Recommended defaults:
  - VOD/high quality: start with `high_quality_p4.json` or `high_quality_p5.json`, override `--codec av1`
  - Live: start with `low_latency_p1.json`, override `--codec av1`, then verify latency and throughput carefully
- Practical guidance: expect higher encoder complexity than H.264/HEVC; validate real-time margin under target load.

Example:

```bash
./vulkan-video-enc-test \
  --encoderConfig vk_video_encoder/json_config/nvidia/high_quality_p4.json \
  --codec av1 \
  -i input.yuv -o out_av1.ivf --inputWidth 1920 --inputHeight 1080 \
  --averageBitrate 3500000 --maxBitrate 7000000
```

### 11.4 Per-Codec Signoff Checklist

1. Confirm codec capability support on target GPU/driver.
2. Confirm latency at steady-state and during scene changes.
3. Confirm bitrate envelope at transport level.
4. Confirm decoder compatibility on target clients.
5. Confirm behavior during dynamic resolution/framerate changes.

## 12. AQ, 2-Pass RC, and Lookahead vs Profiles

This section explains how Adaptive Quantization (AQ), two-pass RC, and lookahead interact with HQ/LL/ULL/Lossless profiles in this codebase.

### 12.1 What This App Exposes Directly

- AQ controls are available only when built with `NV_AQ_GPU_LIB_SUPPORTED` and linked with the AQ Vulkan library.
- AQ controls are CLI-only today (not parsed from preset JSON):
  - `--spatialAQStrength <float>`
  - `--temporalAQStrength <float>`
- AQ strength semantics:
  - valid range is `[-1.0, 1.0]`
  - `< -1.0` disables that AQ mode
  - `0.0` is neutral/default
  - when both are enabled, AQ runs in combined mode
- Internal strength mapping for spatial AQ is normalized to integer `[1..15]`.
- Enabling AQ in CLI auto-enables delta QP-map flow (`enableQpMap`, `DELTA_QP_MAP` path).

### 12.2 What Is Preset-Driven (Not User-Tunable Here)

In the current `vk_video_encoder` preset flow, these knobs are not exposed as direct JSON/CLI controls:

- `multiPass`
- `lowDelayKeyFrameScale`
- `lookaheadDepth` / `lookaheadLevel`

They are selected indirectly by `tuningMode + qualityPreset` through driver preset logic.

Driver-specific note (current integration):

- There is no standalone application knob for 2-pass RC.
- 2-pass RC is enabled by selecting:
  - `tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR`
  - `videoUsageHints` including `VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR`
- Practical CLI form:
  - `--tuningMode ultralowlatency --usageHints streaming`

### 12.3 Profile Interaction Summary

The rows below summarize expected behavior from current preset mappings and driver logic.

| Profile | RC mode intent | Lookahead intent | 2-pass / low-delay intent | AQ guidance |
|---|---|---|---|---|
| HQ | VBR | Quality-first presets; deeper analysis in slower presets | No explicit LL-style low-delay scaling | Spatial AQ first; add temporal AQ when motion complexity benefits |
| LL | CBR | Latency-first; avoid reordering-heavy behavior | `lowDelayKeyFrameScale=2`, multipass disabled (preset path) | Prefer mild spatial AQ; temporal AQ only if latency budget allows |
| ULL | CBR | Strict latency; minimal pipeline delay | In current driver flow, use `ULL + STREAMING` to activate 2-pass RC behavior; low-delay keyframe scaling remains preset-driven | Usually avoid AQ; if needed, use conservative spatial AQ and measure latency |
| Lossless | CQP/CONSTQP | Fidelity-first | Multipass generally irrelevant for lossless goal | Keep AQ disabled (driver paths reject AQ with lossless) |

### 12.4 Codec Notes for Lookahead and 2-Pass Behavior

- H.264:
  - HQ/Lossless presets include lookahead behavior in slower presets in NVENC preset tables.
  - LL/ULL presets carry low-delay keyframe scaling and multipass defaults.
- HEVC:
  - LL/ULL behavior is similar to H.264 for low-delay scaling and multipass intent.
  - HQ does not generally rely on the same lookahead pattern as H.264 HQ; UHQ profiles (not in these JSONs) are where explicit lookahead-level tuning appears.
- AV1:
  - LL/ULL similarly map to low-delay scaling and multipass intent in preset tables.
  - UHQ (not in these JSONs) is where explicit lookahead-level tuning appears.

### 12.5 Important Compatibility Rules

- AQ with lossless is invalid in H.264/HEVC/AV1 driver paths.
- AQ and lookahead-level/CUTREE style controls can conflict (notably in H.264/HEVC paths).
- Older legacy 2-pass RC modes are not valid with new P1-P7 presets; new presets expect tuning-based multipass selection.
- New P1-P7 presets require valid tuning mode (`tuningInfo` / `tuningMode`).

### 12.6 AQ Usage Patterns (CLI)

Spatial AQ only:

```bash
--spatialAQStrength 0.0 --temporalAQStrength -2.0
```

Temporal AQ only:

```bash
--spatialAQStrength -2.0 --temporalAQStrength 0.0
```

Combined AQ:

```bash
--spatialAQStrength 0.25 --temporalAQStrength 0.50
```

### 12.7 Practical Recommendations by Profile

1. HQ: start with spatial AQ, then add temporal AQ for high-motion content after objective and subjective checks.
2. LL: prioritize latency consistency; use AQ only if quality gains are clear and latency variance remains acceptable.
3. ULL: keep AQ off by default; only enable conservative AQ after strict latency validation.
4. Lossless: do not enable AQ.

### 12.8 Cross-Check: Online NVENC Docs and `nvEncodeApp` Sources

- Terminology correction:
  - `nvcuvid` refers to the NVDEC/NVDECODE decode API (`nvcuvid.h` / `cuviddec.h`), not encoder control.
  - Encoder control is via NVENC/NVENCODE (`nvEncodeAPI.h`), and sample app `nvEncodeApp`.
- NVENC Programming Guide confirms:
  - AQ has two flavors (spatial and temporal).
  - Spatial AQ uses `enableAQ` + `aqStrength` (1..15), and can improve perceptual quality while reducing PSNR.
  - Temporal AQ can increase per-frame bitrate fluctuation inside GOP; not recommended for strict per-frame CBR behavior.
  - Two-pass frame modes are `NV_ENC_TWO_PASS_QUARTER_RESOLUTION` and `NV_ENC_TWO_PASS_FULL_RESOLUTION`, with quality/performance trade-offs.
  - In CBR mode, `lowDelayKeyFrameScale` is used to control I/P frame bit ratio and reduce congestion risk.
  - Lookahead/Lookahead-level improves quality and RC behavior, but adds buffering/latency and can return `NV_ENC_ERR_NEED_MORE_INPUT` until enough frames are queued.
- `nvEncodeApp` source cross-check confirms encoder-side knobs exist and are wired:
  - AQ: `enableAQ`, `aqStrength`, `enableTemporalAQ`
  - Lookahead: `enableLookahead`, `lookaheadDepth`
  - Low-delay/2-pass knobs: `lowDelayKeyFrameScale`, `multiPass`
  - Preset model: `tuningInfo + presetGUID` via `NvEncGetEncodePresetConfigEx`
- Current `vk_video_encoder` preset JSON path still does not expose direct JSON/CLI controls for `multiPass`, `lowDelayKeyFrameScale`, and lookahead-level tuning; these stay preset/driver-driven unless loader/schema and CLI are extended.
- Current driver policy to get 2-pass RC in this Vulkan path is to use `ULL + STREAMING` usage hint (no separate user knob).

## 13. Test Input YUV Generation

The encoder profile tests require raw YUV input files at standard resolutions.
Use the **ThreadedRenderingVk** renderer's GPU compute filter pipeline to generate them:

```bash
# Generate ALL formats headless (no display needed) — 5 resolutions × 8 formats
cd /data/nvidia/vulkan/samples/ThreadedRenderingVk_Standalone
./scripts/generate_encoder_yuv.sh --output-dir /data/misc/VideoClips/ycbcr --frames 128 --all

# Generate only 4:2:0 (8-bit + 10-bit) — sufficient for most H.264/H.265 profiles
./scripts/generate_encoder_yuv.sh --output-dir /data/misc/VideoClips/ycbcr --frames 128
```

**Generated file naming:** `{W}x{H}_{subsampling}_{bitdepth}.yuv`

| Format ID | Name | File suffix | Subsampling | Bits |
|:---------:|------|-------------|:-----------:|:----:|
| 0 | NV12 | `_420_8le` | 4:2:0 | 8 |
| 1 | P010 | `_420_10le` | 4:2:0 | 10 |
| 2 | P012 | `_420_12le` | 4:2:0 | 12 |
| 3 | I420 | `_420_8le` | 4:2:0 | 8 |
| 4 | NV16 | `_422_8le` | 4:2:2 | 8 |
| 5 | P210 | `_422_10le` | 4:2:2 | 10 |
| 6 | YUV444_8 | `_444_8le` | 4:4:4 | 8 |
| 7 | Y410 | `_444_10le_packed` | 4:4:4 | 10 |
| 8 | YUV444_10 | `_444_10le` | 4:4:4 | 10 |

**Verify with ffplay:**

```bash
# Size verification + visual playback
python3 scripts/verify_yuv_ffmpeg.py /data/misc/VideoClips/ycbcr --frames 128 --show
```

**Resolutions generated:** 176×144, 352×288, 720×480, 1280×720, 1920×1080

## 14. Profile Test Runner

### Directory structure

```
vk_video_encoder/json_config/         ← --profile-dir (base)
├── encoder_config.default.json       ← generic profiles
├── encoder_config.schema.json
├── nvidia/                           ← IHV: NVIDIA
│   ├── high_quality_p1.json ... p7
│   ├── low_latency_p1.json ... p3
│   ├── ultra_low_latency_p1.json
│   ├── lossless.json
│   └── README.md                     ← this file
└── intel/                            ← IHV: Intel (future)
    └── ...
```

### Running profile tests (`run_encoder_profile_tests.py`)

Runs each JSON profile against all 3 codecs (H.264, H.265, AV1). The CLI `-c` flag
overrides the JSON codec field, so one profile serves as a base config for all codecs.
Input YUV resolution/bitdepth/chroma are auto-detected from filenames.

```bash
# Run ALL profiles × all codecs with auto-detected YUV
python3 scripts/run_encoder_profile_tests.py \
    --video-dir /data/misc/VideoClips/ycbcr --local

# Run only NVIDIA profiles
python3 scripts/run_encoder_profile_tests.py \
    --video-dir /data/misc/VideoClips/ycbcr --profile-filter nvidia --local

# Single profile, single codec
python3 scripts/run_encoder_profile_tests.py \
    --video-dir /data/misc/VideoClips/ycbcr \
    --profile-filter nvidia/high_quality_p4 --codec h265 --local

# Verbose (show commands + output filenames)
python3 scripts/run_encoder_profile_tests.py \
    --video-dir /data/misc/VideoClips/ycbcr --local --verbose
```

| Option | Description |
|--------|-------------|
| `--video-dir PATH` | Directory with YUV files (auto-parsed from filenames) |
| `--profile-dir PATH` | JSON config base dir (default: `vk_video_encoder/json_config`) |
| `--profile-filter STR` | Match against relative path: `nvidia`, `nvidia/high_quality_p4`, `lossless` |
| `--codec CODEC` | Run only one codec: `h264`, `h265`, `av1` (default: all 3) |
| `--max-frames N` | Frames to encode per test (default: 30) |
| `--local` | Run on local machine (default: SSH to remote) |
| `--validate` | Enable Vulkan validation layers |
| `--verbose` | Show full commands and output filenames |
| `--output-dir PATH` | Where to write bitstreams (default: `/tmp/vulkan_encoder_profile_tests`) |

**Output filenames:** `{input_base}_{codec}_{ihv}_{profile}{ext}`
  e.g. `1920x1080_420_8le_h265_nvidia_high_quality_p4.265`

## 15. Decoder Roundtrip Verification

### Decode test (`run_decoder_roundtrip.py`)

Decodes every encoded bitstream in a directory using `vulkan-video-dec-test` to verify
the encode→decode roundtrip. Auto-discovers `.264`, `.265`, `.ivf` files.

```bash
# Decode all bitstreams
DISPLAY=:0 python3 scripts/run_decoder_roundtrip.py /tmp/vulkan_encoder_profile_tests

# Only H.265 files
DISPLAY=:0 python3 scripts/run_decoder_roundtrip.py /tmp/vulkan_encoder_profile_tests --filter h265

# Only lossless profiles
DISPLAY=:0 python3 scripts/run_decoder_roundtrip.py /tmp/vulkan_encoder_profile_tests --filter lossless

# Verbose (show decode commands)
DISPLAY=:0 python3 scripts/run_decoder_roundtrip.py /tmp/vulkan_encoder_profile_tests --verbose
```

| Option | Description |
|--------|-------------|
| `directory` | Directory containing encoded bitstreams |
| `--filter PAT` | Only decode files matching pattern |
| `--decoder PATH` | Path to decoder binary (default: auto-detect from project) |
| `--timeout N` | Seconds per decode (default: 60) |
| `--verbose` | Show decode commands |

**Note:** The decoder currently requires `DISPLAY` (no headless mode in `vulkan-video-dec-test`).

### Visual playback (`play_encoded_ffplay.py`)

Plays encoded bitstreams with ffplay for visual quality inspection. Press Q to advance,
Ctrl+C to stop.

```bash
# Play all bitstreams sequentially
DISPLAY=:0 python3 scripts/play_encoded_ffplay.py /tmp/vulkan_encoder_profile_tests

# Only AV1 files
DISPLAY=:0 python3 scripts/play_encoded_ffplay.py /tmp/vulkan_encoder_profile_tests --filter av1

# Play one file (loops)
DISPLAY=:0 python3 scripts/play_encoded_ffplay.py \
    --play /tmp/vulkan_encoder_profile_tests/1920x1080_420_8le_h265_nvidia_high_quality_p4.265
```

| Option | Description |
|--------|-------------|
| `directory` | Directory containing encoded bitstreams |
| `--filter PAT` | Only play files matching pattern |
| `--play FILE` | Play a single file (loops) |

## 16. Complete Test Workflow

End-to-end workflow from YUV generation through encode, decode, and visual verification:

```bash
# 1. Generate YUV test content (headless, no display needed)
cd /data/nvidia/vulkan/samples/ThreadedRenderingVk_Standalone
./scripts/generate_encoder_yuv.sh --output-dir /data/misc/VideoClips/ycbcr --frames 32 --all

# 2. Verify YUV files
python3 scripts/verify_yuv_ffmpeg.py /data/misc/VideoClips/ycbcr --frames 32

# 3. Run encoder profiles (all codecs × all profiles)
cd /data/nvidia/android-extra/video-apps/vulkan-video-samples
python3 scripts/run_encoder_profile_tests.py \
    --video-dir /data/misc/VideoClips/ycbcr --local --max-frames 30

# 4. Decode all bitstreams (roundtrip verification)
DISPLAY=:0 python3 scripts/run_decoder_roundtrip.py /tmp/vulkan_encoder_profile_tests

# 5. Visual playback of encoded output
DISPLAY=:0 python3 scripts/play_encoded_ffplay.py /tmp/vulkan_encoder_profile_tests
```

## 17. References

- NVIDIA Video Codec SDK Programming Guide:
  - https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html
- NVIDIA NVDEC / NVDECODE Programming Guide (`nvcuvid` decode APIs):
  - https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvdec-video-decoder-api-prog-guide/index.html
- NVIDIA preset model overview:
  - https://developer.nvidia.com/blog/introducing-video-codec-sdk-10-presets/
