# VulkanFilterYuvCompute Test Suite

This directory contains tests for the `VulkanFilterYuvCompute` class, which provides GPU-accelerated YCbCr color space conversions.

## Test Applications

### 1. `vk_filter_test` - Standalone Filter Tests

Tests the `VulkanFilterYuvCompute` class directly, independent of any application.

**Build:**
```bash
cd /data/nvidia/android-extra/video-apps/vulkan-video-samples
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc) vk_filter_test
```

**Run:**
```bash
# Smoke tests (quick sanity check)
./common/libs/tests/vk_filter_test --smoke

# With validation layers
./common/libs/tests/vk_filter_test --smoke --validate

# List all tests
./common/libs/tests/vk_filter_test --list

# Run specific test
./common/libs/tests/vk_filter_test --test TC001_RGBA_to_NV12

# Run test category
./common/libs/tests/vk_filter_test --rgba2ycbcr
./common/libs/tests/vk_filter_test --copy
```

### 2. ThreadedRenderingVk - Integration Tests

Tests the filter as integrated into the `ThreadedRenderingVk` application, including file dumping.

**Run:**
```bash
cd /data/nvidia/vulkan/samples/ThreadedRenderingVk_Standalone
./scripts/test_dump_formats.sh
```

## Test Scripts

### `scripts/test_filter.sh`

Wrapper script for `vk_filter_test` with nice formatting:

```bash
./scripts/test_filter.sh --help
./scripts/test_filter.sh --smoke -v        # Smoke tests with validation
./scripts/test_filter.sh --rgba2ycbcr      # All RGBA to YCbCr tests
./scripts/test_filter.sh --test TC001      # Specific test
```

### `scripts/run_all_tests.sh`

Comprehensive test runner that runs both standalone and integration tests:

```bash
./scripts/run_all_tests.sh
```

## Test Categories

| Category | Tests | Description | Status |
|----------|-------|-------------|--------|
| `smoke` | 8 | Quick sanity check | ✅ Working |
| `rgba2ycbcr` | 8 | RGBA → YCbCr conversions | ✅ Working |
| `ycbcr2rgba` | 8 | YCbCr → RGBA conversions | ⚠️ Shader bugs |
| `copy` | 5 | YCbCr format copy/convert | ✅ Working |
| `clear` | 2 | YCbCr image clear | ✅ Working |
| `colorprimaries` | 9 | BT.601/709/2020 | ✅ Working |
| `range` | 4 | Full/Limited range | ✅ Working |
| `transfer` | 10 | Pre/post transfer ops | 🔄 In progress |
| `buffer` | 6 | Buffer I/O | 🔄 Not implemented |
| `resolution` | 5 | Edge cases | ⚠️ Some issues |

## Supported Formats

| Format | Type | Bit Depth | Subsampling | Status |
|--------|------|-----------|-------------|--------|
| NV12 | 2-plane | 8-bit | 4:2:0 | ✅ |
| P010 | 2-plane | 10-bit | 4:2:0 | ✅ |
| P012 | 2-plane | 12-bit | 4:2:0 | ✅ |
| I420 | 3-plane | 8-bit | 4:2:0 | ✅ |
| NV16 | 2-plane | 8-bit | 4:2:2 | ✅ |
| P210 | 2-plane | 10-bit | 4:2:2 | ✅ |
| YUV444 | 3-plane | 8-bit | 4:4:4 | ✅ |
| Y410 | Packed | 10-bit | 4:4:4 | ❌ Not yet |

## Known Issues

### YCBCR2RGBA Shader Generation Bug

The `YCBCR2RGBA` filter mode has shader generation issues:
- `'normalizeYCbCr' : no matching overloaded function found`
- `'shiftCbCr' : no matching overloaded function found`

**Workaround:** Tests `TC010_NV12_to_RGBA` etc. are disabled in smoke tests.

### Y410 Packed Format

Y410 is a packed format requiring special shader handling not yet implemented.

**Workaround:** Tests `TC008_RGBA_to_Y410` and `TC017_Y410_to_RGBA` are disabled.

### Buffer I/O

Buffer inputs and outputs are not yet implemented in the filter.

**Workaround:** Tests `TC070_RGBABuffer_to_NV12Image` etc. are disabled.

### Minimum Resolution

2x2 resolution tests may fail due to edge cases in pattern generation.

## Verification

CPU-side verification is implemented in the `ColorConversion` module:
- Test pattern generation (ColorBars, Gradient, Checkerboard, etc.)
- RGBA ↔ YCbCr conversion (BT.601, BT.709, BT.2020)
- Full/Limited range support
- PSNR calculation for quality validation

## Comparison: vk_filter_test vs test_dump_formats.sh

| Feature | vk_filter_test | test_dump_formats.sh |
|---------|----------------|----------------------|
| Tests | Filter class directly | Full app integration |
| Modes | All filter modes | RGBA2YCBCR only |
| Validation | CPU verification | Visual (ffplay) |
| Formats | All | 8-bit only |
| Speed | Fast | Slower (renders frames) |
| Debugging | Easier | Harder |
