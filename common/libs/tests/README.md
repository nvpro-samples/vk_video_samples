# VulkanFilterYuvCompute Test Suite

This directory contains tests for the `VulkanFilterYuvCompute` class, which provides GPU-accelerated YCbCr color space conversions.

## Test Applications

### 1. `vk_filter_test` - Standalone Filter Tests

Tests the `VulkanFilterYuvCompute` class directly, independent of any application.

**Build:**
```bash
cd <vulkan-video-samples>
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc) vk_filter_test
```

**Run** (from the build directory):
```bash
# Smoke tests (quick sanity check)
./bin/vk_filter_test --smoke

# With validation layers
./bin/vk_filter_test --smoke --validate

# List every set, and the disabled cases with their defects
./bin/vk_filter_test --list

# Run one case by name, a disabled one included
./bin/vk_filter_test --test TC001_RGBA_to_NV12

# Run a set: --all runs every set but the disabled cases
./bin/vk_filter_test --all
./bin/vk_filter_test --regression    # also --production, --primaries
```

### 2. ThreadedRenderingVk - Integration Tests

Tests the filter as integrated into the `ThreadedRenderingVk` application, including file dumping.

**Run:**
```bash
cd <ThreadedRenderingVk>
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

## Test Sets

`--list` prints each set and the disabled cases. `--all` runs the standard, regression,
production, buffer I/O and transfer sets; the smoke and primaries sets are subsets of the
standard set.

| Set | Option | What it holds |
|-----|--------|---------------|
| Smoke | `--smoke` (the default) | One case per major category |
| Standard | in `--all` | RGBA to YCbCr, YCbCr to RGBA, copy, clear, format conversion, linear tiling, multi-output, component order, edge cases |
| Regression | `--regression` | BT.2020 cases |
| Production | `--production` | SD to 4K, full and limited range |
| Color primaries | `--primaries` | BT.601, BT.709, BT.2020 |
| Buffer I/O | in `--all` | Empty: every case is disabled |
| Transfer | in `--all` | Pre- and post-transfer around the filter |

### Disabled cases

YCbCr to RGBA from a packed input (TC017), RGBA to Y410 (TC008), buffer I/O (TC070 to TC076)
and the transfer-only filter types (TC117 to TC120) are disabled.
A case that fails for a known defect is in no set: `getDisabledTests()` in
`src/TestCases.cpp` lists it with the defect, `--list` prints both, and `--test <name>` runs
it to reproduce the defect. A case leaves the list in the change that fixes its defect. The
device-free CTest `FilterListTests` fails when a case that `src/TestCases.cpp` defines is in
no set and not disabled, or is both.

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
