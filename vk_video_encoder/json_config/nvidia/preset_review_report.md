# Vulkan Video Encoder NVIDIA Preset Review Report

## Scope
- Spreadsheet: `/home/tzlatinski/Downloads/PreferredSettings (2).xlsx`
- NVENC sample app: `/data/nvidia-linux/dev_a/drivers/multimedia/codecs/EncodeAPI/tools/nvEncodeApp`
- NVENC driver: `/data/nvidia-linux/dev_a/drivers/video/encode`
- Vulkan video driver: `/data/nvidia-linux/dev_a/drivers/OpenGL/vulkan/video`
- JSON presets: this directory (`json_config/nvidia`)

## What Was Fixed

1. `qualityPreset` indexing corrected to match NVIDIA P1..P7 enum values (`P1=0 ... P7=6`).
- Root cause: JSONs used 1-based values, but driver enum is 0-based (`NV_ENC_PRESET_P1 = 0`).

2. Tuning mode is now explicit in JSON presets.
- Added per-file `tuningMode` values:
  - `highquality`
  - `lowlatency`
  - `ultralowlatency`
  - `lossless`

3. H.264 GOP/B-frame settings aligned to driver preset tables.
- HQ (`high_quality_p1..p7`):
  - `gopLength=250`, `idrPeriod=250`
  - `bFrameCount`: P1=0, P2=0, P3=1, P4-P7=3
- LL (`low_latency_p1..p3`):
  - `gopLength=4294967295` (infinite)
  - `idrPeriod=4294967295` (infinite)
  - `bFrameCount=0`
- ULL (`ultra_low_latency_p1`):
  - `gopLength=4294967295` (infinite)
  - `idrPeriod=4294967295` (infinite)
  - `bFrameCount=0`
- Lossless (`lossless.json`):
  - `gopLength=250`, `idrPeriod=250`
  - `bFrameCount=0` (P1-style generic lossless)
  - `constQpI/P/B=0`

## Missing/Not Representable in Current JSON Preset Files

The following NVIDIA preset knobs are part of preset tuning logic but are still not encoded in these JSON files:

- `lowDelayKeyFrameScale`
  - LL expects `2`, ULL expects `1`.
- `multiPass`
  - LL expects `DISABLED`, ULL expects `NV_ENC_TWO_PASS_QUARTER_RESOLUTION`.
- `enableLookahead` / `lookaheadDepth`
  - HQ/Lossless P6/P7 include lookahead in NVENC preset tables.
- `useBFramesAsRef` / reference pattern
  - HQ/Lossless P3+ use middle-B reference behavior.
- H.264 entropy and adaptive-transform per preset details
  - These are preset-dependent in NVENC and not explicitly represented in these JSONs.

## Notes About ULL GOP Length

- `PreferredSettings (2).xlsx` contains conflicting ULL GOP hints across sections.
- Current ToT NVENC/Vulkan driver preset logic uses infinite GOP for H.264 LL/ULL tuning.
- Presets were aligned to driver ToT behavior and NVENC tuning guidance.

## Files Updated

- `high_quality_p1.json`
- `high_quality_p2.json`
- `high_quality_p3.json`
- `high_quality_p4.json`
- `high_quality_p5.json`
- `high_quality_p6.json`
- `high_quality_p7.json`
- `low_latency_p1.json`
- `low_latency_p2.json`
- `low_latency_p3.json`
- `ultra_low_latency_p1.json`
- `lossless.json`

