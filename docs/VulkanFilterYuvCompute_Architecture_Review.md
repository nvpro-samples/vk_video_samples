# VulkanFilterYuvCompute Architecture Review

## Document Information
- **Date**: February 3, 2026
- **Version**: 1.0
- **Status**: Review Complete

---

## Executive Summary

The VulkanFilterYuvCompute filter has a well-designed three-stage architecture with dynamic shader generation. However, several issues were identified that should be addressed for production readiness:

1. **Bug Fixed**: BT.2020 color primaries mapping was incorrect in `pattern.cpp`
2. **Improvement Made**: Added centralized `VkYcbcrModelToYcbcrBtStandard()` function to `ycbcr_utils.h`
3. **Remaining Issues**: YCBCR2RGBA shader generation, duplicate code, buffer I/O gaps

---

## 1. Issues Found and Fixed

### 1.1 BT.2020 Color Primaries Bug (FIXED)

**File**: `common/libs/VkCodecUtils/pattern.cpp` line 164

**Issue**: The `GetYcbcrPrimariesConstantsId()` function incorrectly mapped `VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020` to `YcbcrBtStandardBt709` instead of `YcbcrBtStandardBt2020`.

**Impact**: HDR/UHD video content using BT.2020 color primaries would be processed with incorrect color coefficients, leading to color shifts.

**Fix Applied**:
```cpp
// Before (incorrect):
case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020:
    return YcbcrBtStandardBt709;

// After (correct):
case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020:
    return YcbcrBtStandardBt2020;  // Fixed: was incorrectly returning BT.709
```

### 1.2 Centralized VkYcbcrModel Conversion (ADDED)

**File**: `common/include/nvidia_utils/vulkan/ycbcr_utils.h`

**Issue**: The `GetYcbcrPrimariesConstantsId()` function was duplicated in multiple files:
- `VulkanFilterYuvCompute.cpp`
- `pattern.cpp`

**Solution**: Added `VkYcbcrModelToYcbcrBtStandard()` to `ycbcr_utils.h` as the canonical implementation. This function is wrapped in `#ifdef VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709` to avoid Vulkan header dependencies when not needed.

**New Function**:
```cpp
static inline YcbcrBtStandard VkYcbcrModelToYcbcrBtStandard(
    VkSamplerYcbcrModelConversion modelConversion)
{
    switch (modelConversion) {
    case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709:
        return YcbcrBtStandardBt709;
    case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601:
        return YcbcrBtStandardBt601Ebu;
    case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020:
        return YcbcrBtStandardBt2020;
    default:
        break;
    }
    return YcbcrBtStandardUnknown;
}
```

---

## 2. Architecture Strengths

### 2.1 Dynamic Shader Generation
✓ Shaders are generated at runtime based on I/O configuration
✓ Supports all format/tiling/bit-depth combinations
✓ Three-stage pipeline is consistently implemented

### 2.2 VkCodecUtils Integration
✓ Uses `VkImageResource`, `VkBufferResource` for all allocations
✓ Uses `VulkanDeviceContext` for all Vulkan API calls
✓ Follows reference counting pattern (`VkVideoRefCountBase`)

### 2.3 ycbcr_utils.h Consistency
✓ Color primaries use `GetYcbcrPrimariesConstants()`
✓ Range normalization uses `YcbcrNormalizeColorRange`
✓ Matrix operations use `YcbcrBtMatrix`

### 2.4 Transfer Operations
✓ Pre/post transfer operations are well-designed
✓ Multi-planar formats handled correctly plane-by-plane
✓ Layout transitions are properly managed

---

## 3. Remaining Issues

### 3.1 YCBCR2RGBA Shader Generation Bug

**Status**: Not Fixed (requires investigation)

**Description**: YCbCr to RGBA conversion tests (TC010-TC017) are disabled due to shader generation issues. The `normalizeYCbCr` and `shiftCbCr` functions have type mismatches.

**Location**: `VulkanFilterYuvCompute.cpp` in `InitYCBCR2RGBA()`

**Recommendation**: Investigate shader generation for YCBCR2RGBA filter type and fix type mismatches.

### 3.2 Duplicate GetYcbcrPrimariesConstantsId Functions

**Status**: Partially Addressed

**Description**: There are still local `GetYcbcrPrimariesConstantsId` functions in:
- `VulkanFilterYuvCompute.cpp` (line 333)
- `pattern.cpp` (line 156)

**Recommendation**: Refactor to use the new `VkYcbcrModelToYcbcrBtStandard()` from `ycbcr_utils.h`.

### 3.3 Buffer I/O Not Fully Tested

**Status**: Tests Disabled

**Description**: Buffer input/output tests (TC070-TC076) are disabled because the test harness doesn't fully support buffer I/O recording.

**Location**: `FilterTestApp.cpp` line 391

**Recommendation**: Implement buffer I/O path in test harness.

### 3.4 Multi-Output Not Implemented

**Status**: Future Work

**Description**: Dual/triple output scenarios (TC090-TC091) are not fully implemented. The architecture supports it but test coverage is missing.

**Recommendation**: Complete flexible I/O implementation per `VulkanFilterYuvCompute_FlexibleIO_Design.md`.

---

## 4. Code Modularity Assessment

### 4.1 Shader Generation (Good)

| Function | Purpose | Status |
|----------|---------|--------|
| `GenHeaderAndPushConst()` | Shader header generation | ✓ Clean |
| `GenImageIoBindingLayout()` | Image binding generation | ✓ Clean |
| `GenBufferIoBindingLayout()` | Buffer binding generation | ✓ Clean |
| `ShaderGeneratePlaneDescriptors()` | Per-plane descriptors | ✓ Clean |
| `InitYCBCRCOPY()` | YCbCr copy shader | ✓ Clean |
| `InitYCBCRCLEAR()` | YCbCr clear shader | ✓ Clean |
| `InitRGBA2YCBCR()` | RGB→YCbCr shader | ✓ Clean |
| `InitYCBCR2RGBA()` | YCbCr→RGB shader | ⚠ Has issues |

### 4.2 Transfer Operations (Good)

| Function | Purpose | Status |
|----------|---------|--------|
| `RecordLayoutTransition()` | Image layout transitions | ✓ Clean |
| `RecordPreTransfers()` | Pre-compute transfers | ✓ Clean |
| `RecordPostTransfers()` | Post-compute transfers | ✓ Clean |
| `RecordTransferOp()` | Single transfer operation | ✓ Clean |
| `CalculateBufferImageCopyRegions()` | Multi-planar copy regions | ✓ Clean |

### 4.3 Resource Management (Good)

| Component | Pattern | Status |
|-----------|---------|--------|
| Filter creation | Factory pattern via `Create()` | ✓ Clean |
| Descriptor sets | Push descriptors | ✓ Clean |
| Command buffers | Pool management via base class | ✓ Clean |
| Pipeline | Separate `VulkanComputePipeline` class | ✓ Clean |

---

## 5. Recommendations

### 5.1 High Priority

1. **Fix YCBCR2RGBA shader generation** - Enable the disabled tests
2. **Implement buffer I/O in test harness** - Complete test coverage
3. **Refactor duplicate functions** - Use centralized `VkYcbcrModelToYcbcrBtStandard()`

### 5.2 Medium Priority

4. **Complete multi-output support** - Per FlexibleIO design doc
5. **Add Y410 packed format support** - Currently disabled
6. **Improve error handling** - Add more descriptive error messages

### 5.3 Low Priority

7. **Remove stale backup in misc/** - Contains old buggy code
8. **Add performance benchmarks** - Verify real-time 4K@60fps claim
9. **Add shader dump option** - For debugging shader generation

---

## 6. Test Coverage Gaps

### 6.1 Format Coverage

| Format | RGBA→YCbCr | YCbCr→RGBA | Copy | Clear |
|--------|------------|------------|------|-------|
| NV12 | ✓ | ⚠ Disabled | ✓ | ✓ |
| P010 | ✓ | ⚠ Disabled | ✓ | ✓ |
| P012 | ✓ | ⚠ Disabled | - | - |
| I420 | ✓ | ⚠ Disabled | ✓ | - |
| NV16 | ✓ | ⚠ Disabled | ✓ | - |
| P210 | ✓ | ⚠ Disabled | - | - |
| YUV444 | ✓ | ⚠ Disabled | ✓ | - |
| Y410 | ⚠ Disabled | ⚠ Disabled | - | - |

### 6.2 Feature Coverage

| Feature | Test Coverage |
|---------|---------------|
| Color Primaries (BT.601/709/2020) | ✓ Complete |
| Range (Full/Limited) | ✓ Complete |
| Linear Tiling | ✓ Complete |
| Pre-Transfer | ✓ Complete |
| Post-Transfer | ✓ Complete |
| Buffer I/O | ⚠ Disabled |
| Multi-Output | ⚠ Not implemented |
| Y Subsampling (AQ) | ⚠ Partial |

---

## 7. Files Modified

| File | Change |
|------|--------|
| `common/libs/VkCodecUtils/pattern.cpp` | Fixed BT.2020 mapping bug |
| `common/include/nvidia_utils/vulkan/ycbcr_utils.h` | Added `VkYcbcrModelToYcbcrBtStandard()` |

---

## 8. Conclusion

The VulkanFilterYuvCompute architecture is well-designed and modular. The main issues are:

1. **Bug (FIXED)**: BT.2020 color primaries mapping in pattern.cpp
2. **Bug (OPEN)**: YCBCR2RGBA shader generation type mismatches
3. **Gap (OPEN)**: Buffer I/O test coverage
4. **Gap (OPEN)**: Multi-output implementation

With the high-priority fixes applied, the filter is suitable for production use with RGB→YCbCr conversions and YCbCr format conversions. YCbCr→RGBA conversions should be verified after the shader generation fix.
