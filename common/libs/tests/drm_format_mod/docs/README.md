# DRM Format Modifier Test Suite

Test suite for validating Vulkan's `VK_EXT_image_drm_format_modifier` extension functionality.

## Purpose

This test validates:
1. **Format Query**: Correct reporting of DRM format modifier properties
2. **Image Creation**: Creating images with linear and optimal DRM modifiers
3. **Export/Import**: DMA-BUF export and import with format modifiers
4. **YCbCr Formats**: Focus on Vulkan Video formats (NV12, P010, etc.)

## Requirements

### Hardware
- NVIDIA GPU with Vulkan Video support
- Linux with DRM/KMS support

### Software
- Vulkan 1.2+ driver with extensions:
  - `VK_EXT_image_drm_format_modifier`
  - `VK_EXT_external_memory_dma_buf`
  - `VK_KHR_external_memory_fd`
  - `VK_KHR_sampler_ycbcr_conversion`

## Building

```bash
cd vulkan-video-samples
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make drm_format_mod_test -j$(nproc)
```

## Running

### Basic smoke test
```bash
./common/libs/tests/drm_format_mod_test
```

### Full test with verbose output
```bash
./common/libs/tests/drm_format_mod_test --all --verbose
```

### Test specific format
```bash
./common/libs/tests/drm_format_mod_test --format NV12
```

### List supported formats
```bash
./common/libs/tests/drm_format_mod_test --list-formats
```

### Generate comprehensive report
```bash
./common/libs/tests/drm_format_mod_test --report
./common/libs/tests/drm_format_mod_test --report --video-only  # Video formats only
./common/libs/tests/drm_format_mod_test --report --report-file myreport.md
```

## Command Line Options

| Option | Description |
|--------|-------------|
| `--help` | Show help message |
| `--verbose` | Enable verbose output (implies --validation) |
| `--validation` | Enable Vulkan validation layers |
| `--all` | Run all tests (not just smoke tests) |
| `--list-formats` | List formats with DRM modifier support |
| `--format <name>` | Test specific format (e.g., NV12, P010, RGBA8) |
| `--rgb-only` | Test only RGB formats |
| `--ycbcr-only` | Test only YCbCr formats |
| `--video-only` | Test only Vulkan Video formats (8/10/12 bit YCbCr) |
| `--export-only` | Skip import tests |
| `--linear-only` | Only test LINEAR modifier |
| `--compression <m>` | Compression mode: `default`, `enable`, `disable` |
| `--report` | Generate comprehensive format support report |
| `--report-file <f>` | Save report to file (default: drm_format_report.md) |
| `--width <N>` | Test image width (default: 256) |
| `--height <N>` | Test image height (default: 256) |

### Compression Testing

The driver reports two sets of DRM modifiers when `__GL_CompressedFormatModifiers`
includes GPU_SUPPORTED (bit 0):

- **Compressed** modifiers: `compressionType != 0`, `pageKind = GENERIC_MEMORY_COMPRESSIBLE`
- **Uncompressed** modifiers: `compressionType = 0`, `pageKind = GENERIC_MEMORY`

Use `--compression enable` to set the env var and test all three modifier types:

```bash
# Test with compression enabled (tests LINEAR + OPTIMAL + COMPRESSED)
./drm_format_mod_test --compression enable --all --verbose

# Test without compression (tests LINEAR + OPTIMAL only)
./drm_format_mod_test --compression disable --all --verbose

# Default (use whatever driver advertises)
./drm_format_mod_test --all --verbose
```

The test runs per format:

| Test | Modifier Selection | When Runs |
|------|-------------------|-----------|
| TC1_Query | All advertised modifiers | Always |
| TC2_Create_LINEAR | `DRM_FORMAT_MOD_LINEAR` (0x0) | Always |
| TC2_Create_OPTIMAL | First uncompressed block-linear | Not `--linear-only` |
| TC3_ExportImport_LINEAR | LINEAR export, explicit import | Always |
| TC3_ExportImport_OPTIMAL | Uncompressed BL export, list-mode import | Not `--linear-only` |
| TC3_ExportImport_COMPRESSED | Compressed BL export, list-mode import | When compressed advertised, not `--compression disable` |

## Test Categories

### TC1xx: Format Query Tests
Validates that `vkGetPhysicalDeviceFormatProperties2` correctly returns DRM modifier properties.

### TC2xx: Image Creation Tests  
Tests image creation with `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` using both explicit and list-based modifier selection.

### TC3xx: Export/Import Tests
Tests round-trip export via `vkGetMemoryFdKHR` and import via `VkImportMemoryFdInfoKHR`.

### TC4xx: YCbCr Video Format Tests
Focus on formats used by Vulkan Video decode/encode.

## Formats Tested

### RGB Formats
- R8, R16 (single channel)
- RG8, RG16 (two channel)
- RGBA8, BGRA8, RGBA16 (four channel)
- A2R10G10B10, A2B10G10R10 (10-bit packed)

### YCbCr Formats (Priority for Vulkan Video)
- **NV12**: 8-bit 4:2:0 semi-planar
- **P010**: 10-bit 4:2:0 semi-planar
- **P012**: 12-bit 4:2:0 semi-planar
- **P016**: 16-bit 4:2:0 semi-planar
- **NV16**: 8-bit 4:2:2 semi-planar
- **P210**: 10-bit 4:2:2 semi-planar

## Report Feature

The `--report` option generates a comprehensive format support report that:

1. **Lists all supported/unsupported formats** with their DRM modifier status
2. **Identifies Vulkan Video formats** (8/10/12 bit decode/encode formats)
3. **Flags VIDEO_DRM_FAIL** when a Vulkan Video format lacks DRM modifier support

### Report Status Codes

| Status | Meaning |
|--------|---------|
| `SUPPORTED` | Format works with DRM modifiers (LINEAR and/or tiled) |
| `LINEAR_ONLY` | Only LINEAR modifier available (no tiled) |
| `NOT_SUPPORTED` | No DRM modifiers available for this format |
| `EXPORT_FAIL` | Export to DMA-BUF fails |
| `IMPORT_FAIL` | Import from DMA-BUF fails |
| `VIDEO_DRM_FAIL` | **Critical**: Vulkan Video format with no/broken DRM support |

### Example Report Output

```
================================================================================
                     DRM FORMAT MODIFIER SUPPORT REPORT
================================================================================

SUMMARY:
--------
  Total formats tested:        9
  Vulkan Video formats:        6
  Fully supported:             0
  LINEAR only:                 2
  Not supported:               3
  VIDEO DRM FAILURES:          4

FORMAT                        STATUS            LINEAR  TILED   VIDEO MODS
---------------------------------------------------------------------------
NV12                          LINEAR_ONLY       YES     NO      YES   1
P010                          VIDEO_DRM_FAIL    NO      NO      YES   0
P012                          VIDEO_DRM_FAIL    NO      NO      YES   0

*** WARNING: VIDEO DRM FAILURES ***
The following Vulkan Video formats lack proper DRM modifier support:
  - P010: Vulkan Video format but NO DRM modifier support!
```

The report is also saved to a Markdown file for documentation.

## Output Format

```
[DRM Format Mod Test] Starting tests...
[INFO] Physical device: NVIDIA GeForce RTX 4090
[INFO] VK_EXT_image_drm_format_modifier: supported

=== Format Query Tests ===
[TC101] VK_FORMAT_R8G8B8A8_UNORM: 3 modifiers found
        LINEAR (0x0): planes=1, features=0x1F03
        NVIDIA (0x10): planes=1, features=0x1F03
        NVIDIA (0x20): planes=1, features=0x1F03
[PASS] TC101: Format query correct

[TC102] VK_FORMAT_G8_B8R8_2PLANE_420_UNORM (NV12): 2 modifiers found
        LINEAR (0x0): planes=2, features=0x1503
        NVIDIA (0x10): planes=2, features=0x1503
[PASS] TC102: Format query correct

=== Image Creation Tests ===
[TC201] Create RGBA8 with LINEAR modifier
[PASS] TC201: Image created, actual modifier=0x0

[TC202] Create NV12 with OPTIMAL modifier
[PASS] TC202: Image created, actual modifier=0x10

=== Export/Import Tests ===
[TC301] Export/Import RGBA8
        Exported DMA-BUF FD: 7
        Imported successfully
[PASS] TC301: Round-trip successful

=== Summary ===
Total: 24, Passed: 24, Failed: 0, Skipped: 2
```

## Troubleshooting

### "Extension not supported"
Ensure your driver supports `VK_EXT_image_drm_format_modifier`. Check with:
```bash
vulkaninfo | grep drm_format_modifier
```

### "No modifiers found for format"
The format may not support DRM modifiers on your hardware. This is not necessarily an error.

### "Export failed"
Ensure `VK_EXT_external_memory_dma_buf` is supported and the image was created with export capability.

### Previous crash with --validation and YCbCr formats (RESOLVED)

**This issue has been fixed.** See [VALIDATION_LAYER_CRASH_REPORT.md](VALIDATION_LAYER_CRASH_REPORT.md) for the full post-mortem.

**Original Problem:** The test incorrectly set `VK_IMAGE_CREATE_DISJOINT_BIT` when the format
supported it, but then used `vkBindImageMemory` without the required `VkBindImagePlaneMemoryInfo`
in the pNext chain. This violated **VUID-VkBindImageMemoryInfo-image-07736**.

**Root Cause:** The underlying `VkImageResource::CreateExportable` doesn't support per-plane
memory binding required for disjoint images.

**Fix:** The test now does NOT set `VK_IMAGE_CREATE_DISJOINT_BIT`, using non-disjoint
multi-planar images instead. Non-disjoint images share a single memory allocation across
all planes and can be bound with a single `vkBindImageMemory` call.

| Image Type | Memory Binding | Supported by VkImageResource |
|------------|---------------|------------------------------|
| Non-disjoint | Single vkBindImageMemory | Yes |
| Disjoint | Per-plane VkBindImagePlaneMemoryInfo | No |

The test now works correctly with validation layers enabled on both NVIDIA and Intel.

## See Also

- [DESIGN.md](DESIGN.md) - Detailed design documentation
- [VALIDATION_LAYER_CRASH_REPORT.md](VALIDATION_LAYER_CRASH_REPORT.md) - Analysis of validation layer crash with YCbCr + DRM modifiers
- [VK_EXT_image_drm_format_modifier spec](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_image_drm_format_modifier.html)
- [drm_fourcc.h](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/drm/drm_fourcc.h)
- [Vulkan 1.4.342 Specification](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html)
