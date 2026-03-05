# DRM Format Modifier Test Suite - Design Document

## Overview

This test suite validates Vulkan's `VK_EXT_image_drm_format_modifier` extension functionality, focusing on:
1. Correct querying of DRM format modifier properties
2. Image creation with DRM format modifiers (linear and tiled)
3. Export and import of images via DMA-BUF with DRM format modifiers
4. Comprehensive format coverage including RGB and YCbCr formats

## Background

### VK_EXT_image_drm_format_modifier Extension

The `VK_EXT_image_drm_format_modifier` extension enables Vulkan to work with DRM (Direct Rendering Manager) format modifiers, which are 64-bit vendor-prefixed values describing memory layouts. Key concepts:

- **DRM_FORMAT_MOD_LINEAR (0)**: Linear/raster memory layout, universally supported
- **Vendor-specific modifiers**: Tiled/block-linear layouts (e.g., NVIDIA, AMD, Intel)
- **VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT**: New tiling mode for DRM modifier images

### Key Vulkan Structures

```cpp
// Query available modifiers for a format
VkDrmFormatModifierPropertiesListEXT {
    uint32_t drmFormatModifierCount;
    VkDrmFormatModifierPropertiesEXT* pDrmFormatModifierProperties;
};

VkDrmFormatModifierPropertiesEXT {
    uint64_t drmFormatModifier;
    uint32_t drmFormatModifierPlaneCount;
    VkFormatFeatureFlags drmFormatModifierTilingFeatures;
};

// Create image with modifier list (driver selects)
VkImageDrmFormatModifierListCreateInfoEXT {
    uint32_t drmFormatModifierCount;
    const uint64_t* pDrmFormatModifiers;
};

// Create image with explicit modifier (for import)
VkImageDrmFormatModifierExplicitCreateInfoEXT {
    uint64_t drmFormatModifier;
    uint32_t drmFormatModifierPlaneCount;
    const VkSubresourceLayout* pPlaneLayouts;
};

// Query actual modifier used
VkImageDrmFormatModifierPropertiesEXT {
    uint64_t drmFormatModifier;
};
```

### DRM Format Codes (drm_fourcc.h)

Common DRM formats mapped to Vulkan:

| DRM Format | Vulkan Format | Description |
|------------|---------------|-------------|
| DRM_FORMAT_R8 | VK_FORMAT_R8_UNORM | 8-bit red |
| DRM_FORMAT_RG88 | VK_FORMAT_R8G8_UNORM | 8-bit RG |
| DRM_FORMAT_XRGB8888 | VK_FORMAT_B8G8R8A8_UNORM | 32-bit BGRX |
| DRM_FORMAT_ARGB8888 | VK_FORMAT_B8G8R8A8_UNORM | 32-bit BGRA |
| DRM_FORMAT_ABGR8888 | VK_FORMAT_R8G8B8A8_UNORM | 32-bit RGBA |
| DRM_FORMAT_XBGR2101010 | VK_FORMAT_A2B10G10R10_UNORM_PACK32 | 10-10-10-2 |
| DRM_FORMAT_ABGR16161616F | VK_FORMAT_R16G16B16A16_SFLOAT | 64-bit RGBA float |
| DRM_FORMAT_NV12 | VK_FORMAT_G8_B8R8_2PLANE_420_UNORM | NV12 (8-bit 4:2:0) |
| DRM_FORMAT_P010 | VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 | P010 (10-bit 4:2:0) |
| DRM_FORMAT_P012 | VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16 | P012 (12-bit 4:2:0) |
| DRM_FORMAT_P016 | VK_FORMAT_G16_B16R16_2PLANE_420_UNORM | P016 (16-bit 4:2:0) |

## Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                     DrmFormatModTest                            │
├─────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐   │
│  │ FormatQuery  │  │ ImageCreate  │  │  ExportImport        │   │
│  │   Tests      │  │   Tests      │  │    Tests             │   │
│  └──────────────┘  └──────────────┘  └──────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│                      TestInfrastructure                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐   │
│  │ DrmFormats   │  │ TestCases    │  │  Validation          │   │
│  │   Mapping    │  │   Registry   │  │    Helpers           │   │
│  └──────────────┘  └──────────────┘  └──────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│                     VkCodecUtils Infrastructure                 │
│  ┌──────────────────┐  ┌───────────────────────────────────┐   │
│  │ VulkanDeviceCtx  │  │  VkImageResource / VkBufferRes    │   │
│  └──────────────────┘  └───────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### Test Categories

#### 1. Format Query Tests (TC1xx)
- Query DRM modifiers for each format via `vkGetPhysicalDeviceFormatProperties2`
- Validate modifier properties (plane count, features)
- Compare linear vs optimal tiling feature flags

#### 2. Image Creation Tests (TC2xx)
- Create images with LINEAR modifier (0)
- Create images with driver-selected optimal modifier
- Validate plane layouts via `vkGetImageSubresourceLayout`
- Test multi-planar YCbCr formats

#### 3. Export/Import Tests (TC3xx)
- Export DMA-BUF FD via `vkGetMemoryFdKHR`
- Query actual modifier via `vkGetImageDrmFormatModifierPropertiesEXT`
- Import on same device (round-trip test)
- Validate image contents after import

#### 4. YCbCr Video Format Tests (TC4xx)
- Focus on Vulkan Video formats (NV12, P010, etc.)
- Test with `VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR`
- Test with `VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR`

## Formats Under Test

### RGB Formats

| Format Category | VkFormat | Bit Depth |
|----------------|----------|-----------|
| R only | VK_FORMAT_R8_UNORM | 8 |
| R only | VK_FORMAT_R16_UNORM | 16 |
| RG | VK_FORMAT_R8G8_UNORM | 8 |
| RG | VK_FORMAT_R16G16_UNORM | 16 |
| RGB/RGBA | VK_FORMAT_R8G8B8A8_UNORM | 8 |
| RGB/RGBA | VK_FORMAT_B8G8R8A8_UNORM | 8 |
| RGB/RGBA | VK_FORMAT_R16G16B16A16_UNORM | 16 |
| 10-10-10-2 | VK_FORMAT_A2R10G10B10_UNORM_PACK32 | 10 |
| 10-10-10-2 | VK_FORMAT_A2B10G10R10_UNORM_PACK32 | 10 |

### YCbCr Formats (Vulkan Video Priority)

| Format | VkFormat | Planes | Subsampling |
|--------|----------|--------|-------------|
| NV12 | VK_FORMAT_G8_B8R8_2PLANE_420_UNORM | 2 | 4:2:0 |
| NV21 | VK_FORMAT_G8_B8R8_2PLANE_420_UNORM | 2 | 4:2:0 |
| P010 | VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 | 2 | 4:2:0 |
| P012 | VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16 | 2 | 4:2:0 |
| P016 | VK_FORMAT_G16_B16R16_2PLANE_420_UNORM | 2 | 4:2:0 |
| NV16 | VK_FORMAT_G8_B8R8_2PLANE_422_UNORM | 2 | 4:2:2 |
| P210 | VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16 | 2 | 4:2:2 |
| Y210 | VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16 | 1 | 4:2:2 |
| Y410 | VK_FORMAT_A2B10G10R10_UNORM_PACK32 | 1 | 4:4:4 |

### Tiling Modes

| Mode | Description | Modifier |
|------|-------------|----------|
| LINEAR | Row-major, scanline layout | DRM_FORMAT_MOD_LINEAR (0) |
| OPTIMAL | Driver-selected tiled layout | Vendor-specific (>0) |

## Test Flow

### Test Execution Flow

```
main()
  │
  ├── Initialize VulkanDeviceContext
  │     ├── Add DRM format modifier extensions
  │     ├── Create instance with validation
  │     └── Create device with compute queue
  │
  ├── Query supported formats
  │     └── Filter by DRM modifier support
  │
  ├── For each format:
  │     │
  │     ├── TC1: Query Modifier Properties
  │     │     ├── Call vkGetPhysicalDeviceFormatProperties2
  │     │     ├── Validate VkDrmFormatModifierPropertiesListEXT
  │     │     └── Check LINEAR modifier exists
  │     │
  │     ├── TC2: Create Images
  │     │     ├── Create with LINEAR modifier
  │     │     ├── Create with OPTIMAL modifier (driver selects)
  │     │     ├── Query plane layouts
  │     │     └── Validate properties match
  │     │
  │     └── TC3: Export/Import
  │           ├── Create exportable image
  │           ├── Export DMA-BUF FD
  │           ├── Query actual modifier
  │           ├── Import on same device
  │           └── Validate round-trip
  │
  └── Report results
```

## API Reference

### Key Functions Used

```cpp
// Query format modifier support
void vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkFormatProperties2* pFormatProperties);  // pNext: VkDrmFormatModifierPropertiesListEXT

// Query image format properties with modifier
VkResult vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,  // pNext: VkPhysicalDeviceImageDrmFormatModifierInfoEXT
    VkImageFormatProperties2* pImageFormatProperties);

// Get actual modifier used by image
VkResult vkGetImageDrmFormatModifierPropertiesEXT(
    VkDevice device,
    VkImage image,
    VkImageDrmFormatModifierPropertiesEXT* pProperties);

// Export memory as DMA-BUF
VkResult vkGetMemoryFdKHR(
    VkDevice device,
    const VkMemoryGetFdInfoKHR* pGetFdInfo,
    int* pFd);

// Import DMA-BUF memory
// Via VkImportMemoryFdInfoKHR in VkMemoryAllocateInfo::pNext
```

## Expected Results

### Pass Criteria

1. **Format Query**: All modifiers have valid plane counts and non-zero features
2. **Image Creation**: Image creates successfully with expected tiling
3. **Modifier Match**: Queried modifier matches requested (for explicit) or is valid (for list)
4. **Export**: DMA-BUF FD is valid (>= 0)
5. **Import**: Imported image is usable and has same properties

### Known Limitations

- Some formats may not support DRM modifiers at all
- Vendor-specific modifiers may have restrictions
- Cross-device import is not tested (requires separate physical devices)
- Some YCbCr formats may only work with specific usage flags

## Implementation Notes

### Extension Dependencies

Required:
- `VK_KHR_external_memory`
- `VK_KHR_external_memory_fd`
- `VK_EXT_external_memory_dma_buf`
- `VK_EXT_image_drm_format_modifier`
- `VK_KHR_image_format_list` (for MUTABLE_FORMAT_BIT with modifiers)
- `VK_KHR_sampler_ycbcr_conversion` (for YCbCr formats)

### VkCodecUtils Integration

Uses existing infrastructure:
- `VulkanDeviceContext` for device management
- `VkImageResource::CreateExportable()` for exportable images
- Pattern follows FilterTestApp from vk_filter_test

### Error Handling

- Skip formats that don't support DRM modifiers
- Skip tests that require unsupported extensions
- Report clear pass/fail with diagnostic info

## References

1. [VK_EXT_image_drm_format_modifier](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_image_drm_format_modifier.html)
2. [drm_fourcc.h](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/drm/drm_fourcc.h)
3. [VK_KHR_external_memory_fd](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_KHR_external_memory_fd.html)
4. [VK_EXT_external_memory_dma_buf](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_external_memory_dma_buf.html)
