# Validation Layer Crash Report: YCbCr + DRM Format Modifiers

**Date:** 2025-02-05  
**Test Application:** drm_format_mod_test  
**Status:** **RESOLVED** - Bug was in the test application, not the validation layer

## Executive Summary

A segmentation fault occurred in the Khronos Vulkan validation layers when calling `vkBindImageMemory` for multi-planar YCbCr images with DRM format modifiers and the `VK_IMAGE_CREATE_DISJOINT_BIT` flag.

**Root Cause:** The test application was setting `VK_IMAGE_CREATE_DISJOINT_BIT` when the format supported it, but then using `vkBindImageMemory` without the required `VkBindImagePlaneMemoryInfo` in the pNext chain. This is a violation of **VUID-VkBindImageMemoryInfo-image-07736**.

**Resolution:** Fixed the test application to NOT set `VK_IMAGE_CREATE_DISJOINT_BIT`, since the underlying `VkImageResource::CreateExportable` infrastructure doesn't support per-plane memory binding.

## Timeline

1. Initial observation: Crash with validation layers on NVIDIA with YCbCr + DRM modifiers
2. Initial hypothesis: Validation layer bug with non-disjoint multi-planar images
3. Root cause discovery: Test app was setting DISJOINT_BIT incorrectly
4. Fix: Removed DISJOINT_BIT from test image creation

## Technical Details

### The Original Bug

The test code at `DrmFormatModTest.cpp` was:

```cpp
// For YCbCr, may need disjoint flag (check format properties)
if (format.isYcbcr && format.planeCount > 1) {
    auto modifiers = queryDrmModifiers(format.vkFormat);
    for (const auto& mod : modifiers) {
        if (mod.modifier == drmModifier) {
            if (mod.features & VK_FORMAT_FEATURE_DISJOINT_BIT) {
                imageInfo.flags |= VK_IMAGE_CREATE_DISJOINT_BIT;  // <-- Problem!
            }
            break;
        }
    }
}
```

This set `VK_IMAGE_CREATE_DISJOINT_BIT` when the format's DRM modifier properties included `VK_FORMAT_FEATURE_DISJOINT_BIT`.

However, `VkImageResource::CreateExportable` then called:

```cpp
result = vkDevCtx->BindImageMemory(device, image, *vkDeviceMemory, imageOffset);
```

This is a simple `vkBindImageMemory` call without any `VkBindImagePlaneMemoryInfo` structure - a spec violation for disjoint images.

### Vulkan Spec Requirements for Disjoint Images

#### VUID-VkBindImageMemoryInfo-image-07736
> If `image` was created with the `VK_IMAGE_CREATE_DISJOINT_BIT` bit set, then the `pNext` chain **must** include a `VkBindImagePlaneMemoryInfo` structure.

For a disjoint multi-planar image, you must:

1. Query per-plane memory requirements via `vkGetImageMemoryRequirements2` with `VkImagePlaneMemoryRequirementsInfo` for each plane
2. Allocate memory for each plane (can be same allocation at different offsets, or separate allocations)
3. Bind each plane separately via `vkBindImageMemory2` with `VkBindImagePlaneMemoryInfo` in pNext

The `VkImageResource` class doesn't implement this per-plane binding, so it cannot correctly handle disjoint images.

### The Fix

```cpp
// Note: We intentionally do NOT set VK_IMAGE_CREATE_DISJOINT_BIT even if the
// format supports it (VK_FORMAT_FEATURE_DISJOINT_BIT). This is because:
// 1. Disjoint images require per-plane memory binding via VkBindImagePlaneMemoryInfo
// 2. VkImageResource::CreateExportable uses vkBindImageMemory which doesn't support this
// 3. Using DISJOINT_BIT without proper per-plane binding violates VUID-VkBindImageMemoryInfo-image-07736
//
// For non-disjoint multi-planar images, all planes share a single memory allocation
// and can be bound with a single vkBindImageMemory call, which is what we support.
(void)format; // Suppress unused warning - we don't set DISJOINT_BIT
```

## Disjoint vs Non-Disjoint Multi-Planar Images

| Aspect | Disjoint | Non-Disjoint |
|--------|----------|--------------|
| Flag | `VK_IMAGE_CREATE_DISJOINT_BIT` | (no flag) |
| Memory | Separate allocation per plane | Single allocation for all planes |
| Memory Query | Per-plane via `VkImagePlaneMemoryRequirementsInfo` | Whole image via `vkGetImageMemoryRequirements` |
| Memory Bind | Per-plane via `VkBindImagePlaneMemoryInfo` | Single `vkBindImageMemory` call |
| Use Case | Importing external planes with different layouts | Standard multi-planar images |

## Why Intel Didn't Crash

Intel Mesa drivers likely:
1. Don't report `VK_FORMAT_FEATURE_DISJOINT_BIT` for YCbCr formats with DRM modifiers, OR
2. The test's format query returned different results

This meant the test didn't set `VK_IMAGE_CREATE_DISJOINT_BIT` on Intel, so no spec violation occurred.

## Validation Layer Behavior

The validation layer crash was actually a **correct failure** - it was attempting to track per-plane memory binding state for a disjoint image, but crashed when the application didn't provide the required `VkBindImagePlaneMemoryInfo`. The crash could be improved to be a more graceful error message, but the underlying validation check is correct.

The crash occurred at `state_tracker.cpp:3801`:

```cpp
if (image_state->disjoint && image_state->IsExternalBuffer() == false) {
    auto plane_info = vku::FindStructInPNextChain<VkBindImagePlaneMemoryInfo>(bind_info.pNext);
    plane_index = vkuGetPlaneIndex(plane_info->planeAspect);  // NULL dereference!
}
```

When `disjoint == true` but `plane_info == nullptr`, this causes a NULL pointer dereference. The validation layer could be improved to check for NULL and emit a proper VUID error instead of crashing, but the application was already in violation.

## Lessons Learned

1. **Check format capabilities carefully**: `VK_FORMAT_FEATURE_DISJOINT_BIT` means the format *can* be used with disjoint images, not that it *must* be.

2. **Infrastructure limitations**: When using wrapper classes like `VkImageResource`, understand their limitations. This class doesn't support per-plane memory binding.

3. **Validation crashes indicate bugs**: A crash in the validation layer during state tracking usually means the application violated a spec requirement, not a layer bug.

## Future Work

If disjoint image support is needed, `VkImageResource::CreateExportable` would need to be extended to:

1. Accept a parameter indicating disjoint vs non-disjoint mode
2. For disjoint mode:
   - Query per-plane memory requirements
   - Allocate/track memory per-plane
   - Use `vkBindImageMemory2` with `VkBindImagePlaneMemoryInfo` for each plane
3. Store per-plane memory handles for proper cleanup

## References

- [Vulkan 1.4.342 Specification](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html)
- VUID-VkBindImageMemoryInfo-image-07736: Disjoint images must use VkBindImagePlaneMemoryInfo
- VUID-VkBindImageMemoryInfo-pNext-01618: VkBindImagePlaneMemoryInfo requires DISJOINT_BIT
- [VK_EXT_image_drm_format_modifier](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_image_drm_format_modifier.html)
