# Vulkan Video Samples - Test Suite Documentation

## Overview

This document describes the comprehensive test suite for the Vulkan Video decoder and encoder samples. The tests verify functionality across all supported codecs with optional Vulkan validation layer support.

**Cross-Platform Support:** All test scripts are written in Python and work on both Linux and Windows.

## Supported Codecs

### Decoder
- **H.264/AVC** - MPEG-4 Part 10, widely used video codec
- **H.265/HEVC** - High Efficiency Video Coding, including 10-bit HDR
- **AV1** - AOMedia Video 1, royalty-free next-gen codec (8-bit and 10-bit)
- **VP9** - Google's open video codec

### Encoder
- **H.264/AVC** - 8-bit 4:2:0
- **H.265/HEVC** - 8-bit and 10-bit 4:2:0
- **AV1** - 8-bit and 10-bit 4:2:0

## Test Scripts

### Python Scripts (Cross-Platform - Linux/Windows)

| Script | Description |
|--------|-------------|
| `run_all_video_tests.py` | Unified test runner - runs both decoder and encoder tests |
| `run_decoder_tests.py` | Comprehensive decoder tests for all codecs |
| `run_encoder_tests.py` | Comprehensive encoder tests with AQ support |
| `run_encoder_profile_tests.py` | Sweep all encoder JSON profiles (NVIDIA preset configs) |
| `vulkan_video_test.py` | Full-featured test framework with JSON reporting |

### Legacy Shell Scripts (Linux only)

| Script | Description |
|--------|-------------|
| `run_all_video_tests.sh` | Shell version of unified test runner |
| `run_decoder_tests.sh` | Shell version of decoder tests |
| `run_encoder_tests.sh` | Shell version of encoder tests |
| `run_encoder_profile_tests.sh` | Shell wrapper for full JSON profile sweep |

## Usage

### Quick Start

**Note:** The `--video-dir` parameter is required for all scripts.

```bash
# Run all tests (video-dir is required)
python scripts/run_all_video_tests.py --video-dir /data/misc/VideoClips

# Run with validation layers
python scripts/run_all_video_tests.py --video-dir /data/misc/VideoClips --validate

# Run on a specific remote host
python scripts/run_all_video_tests.py --video-dir /data/misc/VideoClips --remote 192.168.122.216

# Quick test mode (fewer frames)
python scripts/run_all_video_tests.py --video-dir /data/misc/VideoClips --quick --validate

# Run locally (no SSH)
python scripts/run_all_video_tests.py --video-dir /data/misc/VideoClips --local
```

### Decoder Tests Only

```bash
# All decoder tests
python scripts/run_decoder_tests.py --video-dir /data/misc/VideoClips

# With validation
python scripts/run_decoder_tests.py --video-dir /data/misc/VideoClips --validate

# Specific codec only
python scripts/run_decoder_tests.py --video-dir /data/misc/VideoClips --codec h264
python scripts/run_decoder_tests.py --video-dir /data/misc/VideoClips --codec h265
python scripts/run_decoder_tests.py --video-dir /data/misc/VideoClips --codec av1
python scripts/run_decoder_tests.py --video-dir /data/misc/VideoClips --codec vp9
```

### Encoder Tests Only

```bash
# All encoder tests
python scripts/run_encoder_tests.py --video-dir /data/misc/VideoClips

# With AQ (Adaptive Quantization) tests
python scripts/run_encoder_tests.py --video-dir /data/misc/VideoClips --aq

# With validation
python scripts/run_encoder_tests.py --video-dir /data/misc/VideoClips --validate

# Specific codec
python scripts/run_encoder_tests.py --video-dir /data/misc/VideoClips --codec h264
```

### Encoder JSON Profile Tests

```bash
# Run all NVIDIA JSON profiles
python scripts/run_encoder_profile_tests.py --video-dir /data/misc/VideoClips

# Local run with validation
python scripts/run_encoder_profile_tests.py --video-dir /data/misc/VideoClips --local --validate

# Filter by codec or profile name
python scripts/run_encoder_profile_tests.py --video-dir /data/misc/VideoClips --codec h265
python scripts/run_encoder_profile_tests.py --video-dir /data/misc/VideoClips --profile-filter ull

# Current driver support cap (example: skip qualityPreset > 4)
python scripts/run_encoder_profile_tests.py --video-dir /data/misc/VideoClips --max-supported-quality-preset 4

# Shell wrapper (same options)
scripts/run_encoder_profile_tests.sh --video-dir /data/misc/VideoClips --local
```

### Advanced Test Framework (JSON Reports)

```bash
# Full test suite with JSON reporting
python scripts/vulkan_video_test.py --video-dir /data/misc/VideoClips --test all

# Decoder only with validation
python scripts/vulkan_video_test.py --video-dir /data/misc/VideoClips --test decoder --validate

# Encoder with AQ
python scripts/vulkan_video_test.py --video-dir /data/misc/VideoClips --test encoder --aq

# On a remote host
python scripts/vulkan_video_test.py --video-dir /data/misc/VideoClips --remote 192.168.122.216
```

### Windows Usage

All Python scripts work on Windows. Use `python` instead of `python3`:

```cmd
REM Run all tests on Windows
python scripts\run_all_video_tests.py --video-dir C:\VideoClips --local

REM Decoder tests
python scripts\run_decoder_tests.py --video-dir C:\VideoClips --local

REM Encoder tests with AQ
python scripts\run_encoder_tests.py --video-dir C:\VideoClips --aq --local
```

## Remote Execution

All tests are designed to run on a remote host with an NVIDIA GPU. The host machine orchestrates the tests via SSH.

### Default Remote Configuration
- **Host**: 127.0.0.1 (localhost by default)
- **User**: Current user ($USER)
- **NFS**: Directories are shared, so local changes are immediately visible on remote

### Customizing Remote Target

```bash
# Using command-line arguments
python scripts/run_all_video_tests.py --video-dir /data/misc/VideoClips --remote 192.168.122.216

# With custom username
python scripts/run_all_video_tests.py --video-dir /data/misc/VideoClips --remote 192.168.1.100 --remote-user nvidia

# Run locally (if host has GPU)
python scripts/run_all_video_tests.py --video-dir /data/misc/VideoClips --local
```

## Validation Layers

When enabled, validation layers check for:
- Vulkan API usage errors
- Synchronization issues
- Memory access violations
- Video-specific validation

### Enabling Validation

```bash
# All test scripts support --validate
python scripts/run_all_video_tests.py --video-dir /data/misc/VideoClips --validate
python scripts/run_decoder_tests.py --video-dir /data/misc/VideoClips --validate
python scripts/run_encoder_tests.py --video-dir /data/misc/VideoClips --validate
python scripts/run_encoder_profile_tests.py --video-dir /data/misc/VideoClips --validate
```

The scripts set these environment variables on the remote host:
```bash
VK_LOADER_LAYERS_ENABLE='*validation'
VK_VALIDATION_VALIDATE_SYNC=true
```

## Test Content

### Location
Test content directory must be specified via `--video-dir` argument. There is no default location.

### Decoder Test Content

| Format | Files | Description |
|--------|-------|-------------|
| H.264 | `cts/clip-*.h264`, `*.264`, `*.mkv`, `*.mp4` | CTS clips, Jellyfish, container formats |
| H.265 | `cts/clip-d.h265`, `*.h265`, `*.mkv` | CTS clips, 4K, 10-bit HDR content |
| AV1 | `cts/*.ivf`, `av1-test-content/` | CTS test vectors, 8-bit and 10-bit |
| VP9 | `*.webm`, `*.ivf` | WebM containers (when available) |

### Encoder Test Content (YUV)

Encoder profile and encoder tests expect YUV input (e.g. under `--video-dir` or `--input-file`).
YUV at any resolution and format (e.g. 4:2:0 8-bit, P010 10-bit) can be generated by the
**ThreadedRenderingVk** renderer and then used for these tests.

To generate all required YUV files (~128 frames each, correct names for profile tests), use the
renderer’s scripts (from the ThreadedRenderingVk_Standalone repo):

```bash
# From ThreadedRenderingVk_Standalone (build first)
./scripts/generate_encoder_yuv.sh --output-dir /path/to/yuv_out --frames 128

# Optional: verify with ffplay (parses filenames for resolution/YCbCr, runs ffplay on each)
./scripts/verify_yuv_ffplay.sh /path/to/yuv_out --duration 3

# Then run encoder profile tests with that dir
python scripts/run_encoder_profile_tests.py --video-dir /path/to/yuv_out
```

See ThreadedRenderingVk_Standalone `docs/encoder_yuv_generation.md` for full options and the verification script filename convention.

| Resolution | 8-bit | 10-bit |
|------------|-------|--------|
| 128x128 | ✓ | ✓ |
| 176x144 | ✓ | ✓ |
| 352x288 | ✓ | ✓ |
| 720x480 | ✓ | ✓ |
| 1920x1080 | ✓ | ✓ |
| 3840x2160 | ✓ | ✓ |

## Adaptive Quantization (AQ) Tests

AQ testing verifies the adaptive quantization feature that improves video quality by adjusting quantization based on content analysis.

### AQ Modes

| Mode | Spatial AQ | Temporal AQ | Description |
|------|------------|-------------|-------------|
| Spatial only | 0.0 to 1.0 | -2.0 (disabled) | Content-adaptive spatial QP |
| Temporal only | -2.0 (disabled) | 0.0 to 1.0 | Motion-based temporal QP |
| Combined | 0.0 to 1.0 | 0.0 to 1.0 | Both spatial and temporal |

### Running AQ Tests

```bash
python scripts/run_encoder_tests.py --video-dir /data/misc/VideoClips --aq
python scripts/run_all_video_tests.py --video-dir /data/misc/VideoClips --encoder --aq
python scripts/vulkan_video_test.py --video-dir /data/misc/VideoClips --test encoder --aq
```

## Output and Reports

### Test Output Directory
- Default: `/tmp/vulkan_video_tests/` (decoder)
- Default: `/tmp/vulkan_encoder_tests/` (encoder)

### JSON Report
The vulkan_video_test.py framework generates a JSON report:
```bash
python scripts/vulkan_video_test.py --video-dir /data/misc/VideoClips --test all
# Creates: /tmp/vulkan_video_tests/test_report.json
```

Report contents:
```json
{
  "timestamp": "2026-02-03T...",
  "config": { "validation_enabled": true, ... },
  "summary": { "total": 100, "passed": 98, "failed": 2, ... },
  "results": [ { "name": "...", "status": "PASSED", ... } ]
}
```

## Test Categories

### Decoder Tests

1. **Basic Decode** - Standard playback for each codec
2. **Resolution Tests** - 144p to 4K content
3. **Bit Depth** - 8-bit and 10-bit content
4. **Container Formats** - Raw streams, MKV, MP4
5. **Special Features** - Film grain, SVC, HDR

### Encoder Tests

1. **Basic Encode** - Standard encoding for each codec
2. **Resolution Tests** - Multiple resolutions
3. **Bit Depth** - 8-bit and 10-bit encoding
4. **GOP Structure** - I-only, IPP, IPB patterns
5. **Rate Control** - CBR, VBR modes
6. **AQ Tests** - Spatial, temporal, combined

## Troubleshooting

### Remote Connection Issues
```bash
# Test SSH connectivity
ssh user@<remote-host> "echo OK"

# Check if decoder exists on remote
ssh user@<remote-host> "ls -la /path/to/build/vk_video_decoder/demos/"
```

### Missing Test Content
```bash
# List available test content
ls /data/misc/VideoClips/cts/
ls /data/misc/VideoClips/cts/video/
```

### Validation Errors
Review the verbose output to identify specific Vulkan API errors:
```bash
python scripts/run_decoder_tests.py --video-dir /data/misc/VideoClips --validate --verbose
```

## CI/CD Integration

The test scripts return appropriate exit codes:
- `0` - All tests passed
- `1` - One or more tests failed

Example GitHub Actions usage:
```yaml
- name: Run Vulkan Video Tests
  run: |
    python scripts/run_all_video_tests.py --video-dir ${{ env.VIDEO_CLIPS_DIR }} --validate --quick --local
```

## Contributing

When adding new test cases:
1. Add test content to your video clips directory
2. Update `run_decoder_tests.py` or `run_encoder_tests.py`
3. Update Python test cases in `vulkan_video_test.py`
4. Document new test categories in this file

## Requirements

- Python 3.7 or later
- SSH client (for remote execution on Linux)
- Vulkan SDK with validation layers (for --validate option)
