# VulkanFilterYuvCompute Architecture and Requirements

## Document Information
- **Date**: February 3, 2026
- **Version**: 1.0
- **Status**: Production Specification

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture](#2-architecture)
3. [Requirements](#3-requirements)
4. [Component Details](#4-component-details)
5. [Color Space Processing](#5-color-space-processing)
6. [Format Support Matrix](#6-format-support-matrix)
7. [API Reference](#7-api-reference)
8. [Test Coverage](#8-test-coverage)
9. [Known Issues and Limitations](#9-known-issues-and-limitations)

---

## 1. Overview

### 1.1 Purpose

`VulkanFilterYuvCompute` is a GPU-accelerated compute filter for YCbCr image processing. It provides:

- **RGB ↔ YCbCr color space conversion** with configurable primaries (BT.601, BT.709, BT.2020)
- **YCbCr format conversion** between different plane layouts (NV12, P010, I420, NV16, YUV444, etc.)
- **Bit depth conversion** (8-bit ↔ 10-bit ↔ 12-bit) with proper range scaling
- **Tiling mode conversion** (linear ↔ optimal) for CPU/GPU interoperability
- **Buffer ↔ Image interoperability** for flexible I/O workflows
- **Transfer operations** for efficient memory copies using copy engine

### 1.2 Key Features

| Feature | Description |
|---------|-------------|
| Dynamic shader generation | GLSL shaders generated at runtime based on I/O configuration |
| Three-stage pipeline | Normalize → Process → Denormalize architecture |
| Multi-planar YCbCr support | 1-plane, 2-plane, and 3-plane formats |
| Color primaries | BT.601, BT.709, BT.2020 with consistent `ycbcr_utils.h` integration |
| Range support | Full range [0-255] and limited/narrow range [16-235] |
| Subsampling | 4:4:4, 4:2:2, 4:2:0 chroma subsampling with box filter |
| AQ integration | Optional 2x2 subsampled Y output for Adaptive Quantization |
| Pre/Post transfers | Integrated copy engine operations for data staging |

### 1.3 System Context

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        APPLICATION LAYER                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────┐                    │
│  │   Decoder    │   │   Encoder    │   │ Game Engine  │                    │
│  │  (VkVideo)   │   │  (VkVideo)   │   │  (Renderer)  │                    │
│  └──────┬───────┘   └──────┬───────┘   └──────┬───────┘                    │
│         │ YCbCr            │ RGBA             │ RGBA                       │
│         ▼                  ▼                  ▼                            │
│  ┌─────────────────────────────────────────────────────────┐               │
│  │              VulkanFilterYuvCompute                      │               │
│  │  ┌─────────┐   ┌─────────────┐   ┌─────────────┐        │               │
│  │  │ Input   │──▶│ Processing  │──▶│   Output    │        │               │
│  │  │ Stage   │   │   Stage     │   │   Stage     │        │               │
│  │  └─────────┘   └─────────────┘   └─────────────┘        │               │
│  └─────────────────────────────────────────────────────────┘               │
│         │                  │                  │                            │
│         ▼                  ▼                  ▼                            │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────┐                    │
│  │ NV12/P010    │   │   Display    │   │  File Dump   │                    │
│  │  for Encode  │   │   (RGBA)     │   │   (Linear)   │                    │
│  └──────────────┘   └──────────────┘   └──────────────┘                    │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Architecture

### 2.1 Class Hierarchy

```
VkVideoRefCountBase
    └── VulkanCommandBufferPool
            └── VulkanFilter (base class)
                    └── VulkanFilterYuvCompute (compute filter implementation)
```

### 2.2 Component Dependencies

```
VulkanFilterYuvCompute
    ├── VulkanDeviceContext         // Device/queue management
    ├── VulkanDescriptorSetLayout   // Descriptor set management
    ├── VulkanComputePipeline       // Pipeline creation/management
    ├── VulkanShaderCompiler        // GLSL → SPIR-V compilation
    ├── VulkanSamplerYcbcrConversion // YCbCr sampling configuration
    └── ycbcr_utils.h               // Color primaries/conversion coefficients
            ├── YcbcrPrimariesConstants  // Kb, Kr coefficients
            ├── YcbcrBtMatrix            // RGB ↔ YCbCr conversion matrix
            └── YcbcrNormalizeColorRange // Range normalization/denormalization
```

### 2.3 Three-Stage Processing Pipeline

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        COMPUTE SHADER PIPELINE                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌────────────────────┐   ┌────────────────────┐   ┌────────────────────┐  │
│  │    INPUT STAGE     │   │  PROCESSING STAGE  │   │   OUTPUT STAGE     │  │
│  │                    │   │                    │   │                    │  │
│  │ 1. Fetch data      │   │ 1. Color convert   │   │ 1. Denormalize     │  │
│  │    - Image/Buffer  │   │    - RGB ↔ YCbCr   │   │    - Apply range   │  │
│  │    - Per-plane     │──▶│    - Matrix mult   │──▶│    - Scale to      │  │
│  │                    │   │                    │   │      bit depth     │  │
│  │ 2. Normalize       │   │ 2. Chroma process  │   │                    │  │
│  │    - To [0.0, 1.0] │   │    - Subsample     │   │ 2. Pack data       │  │
│  │    - Range adjust  │   │    - Upsample      │   │    - Per-plane     │  │
│  │                    │   │                    │   │    - Bit alignment │  │
│  │ 3. Unpack          │   │ 3. Filter (opt)    │   │                    │  │
│  │    - Multi-planar  │   │    - Box filter    │   │ 3. Write output    │  │
│  │    - Bit shift     │   │    - Y subsampling │   │    - Image/Buffer  │  │
│  │                    │   │                    │   │    - Per-plane     │  │
│  │ Output: vec4[0,1]  │   │ I/O: vec4[0,1]     │   │ Input: vec4[0,1]   │  │
│  └────────────────────┘   └────────────────────┘   └────────────────────┘  │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.4 Descriptor Binding Layout

| Binding | Purpose | Type | Description |
|---------|---------|------|-------------|
| 0 | Input RGBA/Y | STORAGE_IMAGE or COMBINED_IMAGE_SAMPLER | Single-plane or Y plane |
| 1 | Input Y (multi-planar) | STORAGE_IMAGE | Y plane for explicit multi-planar |
| 2 | Input CbCr/Cb | STORAGE_IMAGE | Chroma plane(s) |
| 3 | Input Cr | STORAGE_IMAGE | Cr plane (3-plane formats) |
| 4 | Output RGBA/Y | STORAGE_IMAGE | Single-plane or Y plane |
| 5 | Output Y (multi-planar) | STORAGE_IMAGE | Y plane for explicit multi-planar |
| 6 | Output CbCr/Cb | STORAGE_IMAGE | Chroma plane(s) |
| 7 | Output Cr | STORAGE_IMAGE | Cr plane (3-plane formats) |
| 8 | Push Constants | UNIFORM_BUFFER | Runtime parameters |
| 9 | Subsampled Y | STORAGE_IMAGE | 2x2 subsampled Y for AQ |

### 2.5 Transfer Operations Pipeline

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        FULL EXECUTION PIPELINE                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Stage 1: PRE-TRANSFERS (optional)                                     │   │
│  │   - Transition preTransferSource → TRANSFER_SRC_OPTIMAL               │   │
│  │   - Transition primary input → TRANSFER_DST_OPTIMAL                   │   │
│  │   - Copy: preTransferSource → primary input                           │   │
│  │   - Barrier: TRANSFER → COMPUTE                                       │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                    │                                         │
│                                    ▼                                         │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Stage 2: COMPUTE (skipped for XFER_* filter types)                    │   │
│  │   - Transition inputs → GENERAL (for shader read)                     │   │
│  │   - Transition outputs → GENERAL (for shader write)                   │   │
│  │   - Bind pipeline, descriptors, push constants                        │   │
│  │   - Dispatch compute shader                                           │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                    │                                         │
│                                    ▼                                         │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Stage 3: POST-TRANSFERS (optional)                                    │   │
│  │   - Barrier: COMPUTE → TRANSFER                                       │   │
│  │   - Transition primary output → TRANSFER_SRC_OPTIMAL                  │   │
│  │   - Transition postTransferDest → TRANSFER_DST_OPTIMAL                │   │
│  │   - Copy: primary output → postTransferDest                           │   │
│  │   - Barrier: TRANSFER → HOST (if dest is host-accessible)             │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| FR-001 | Support RGB → YCbCr color conversion | Critical | ✓ Implemented |
| FR-002 | Support YCbCr → RGB color conversion | Critical | ✓ Implemented |
| FR-003 | Support YCbCr format conversion (NV12 ↔ I420, NV12 ↔ P010, etc.) | Critical | ✓ Implemented |
| FR-004 | Support BT.601, BT.709, BT.2020 color primaries | Critical | ✓ Implemented |
| FR-005 | Support full and limited/narrow range | Critical | ✓ Implemented |
| FR-006 | Support 8-bit, 10-bit, 12-bit bit depths | Critical | ✓ Implemented |
| FR-007 | Support 4:4:4, 4:2:2, 4:2:0 chroma subsampling | Critical | ✓ Implemented |
| FR-008 | Support image and buffer I/O | High | ✓ Implemented |
| FR-009 | Support linear and optimal tiling | High | ✓ Implemented |
| FR-010 | Support pre/post transfer operations | High | ✓ Implemented |
| FR-011 | Support 2x2 Y subsampling for AQ | Medium | ✓ Implemented |
| FR-012 | Support transfer-only operations (XFER_*) | Medium | ✓ Implemented |
| FR-013 | Multi-output support (dual/triple output) | Medium | ⏳ Partial |
| FR-014 | Clear operation for YCbCr initialization | Medium | ✓ Implemented |
| FR-015 | Consistent use of ycbcr_utils.h for all color operations | High | ✓ Implemented |

### 3.2 Non-Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| NFR-001 | Dynamic shader generation (no fixed shaders) | Critical | ✓ Implemented |
| NFR-002 | Three-stage pipeline architecture maintained | Critical | ✓ Implemented |
| NFR-003 | All processing in normalized [0.0, 1.0] color space | Critical | ✓ Implemented |
| NFR-004 | Use VkCodecUtils infrastructure (no manual Vulkan allocations) | Critical | ✓ Implemented |
| NFR-005 | Thread-safe filter instance creation | High | ✓ Implemented |
| NFR-006 | Support validation layers without errors | High | ✓ Implemented |
| NFR-007 | Performance: real-time 4K processing at 60fps | Medium | ✓ Verified |
| NFR-008 | Memory efficiency: reuse descriptor sets | Medium | ✓ Implemented |

### 3.3 Color Processing Requirements

| ID | Requirement | Details |
|----|-------------|---------|
| CPR-001 | Use `ycbcr_utils.h::YcbcrPrimariesConstants` | Kr, Kb coefficients from `GetYcbcrPrimariesConstants()` |
| CPR-002 | Use `ycbcr_utils.h::YcbcrBtMatrix` | RGB ↔ YCbCr matrix operations |
| CPR-003 | Use `ycbcr_utils.h::YcbcrNormalizeColorRange` | Range normalization/denormalization |
| CPR-004 | Map VkSamplerYcbcrModelConversion to YcbcrBtStandard | via `GetYcbcrPrimariesConstantsId()` |
| CPR-005 | Map VkSamplerYcbcrRange to YCBCR_COLOR_RANGE | Full → ITU_FULL, Narrow → ITU_NARROW |

### 3.4 Format Support Requirements

| Format Category | VkFormat | Planes | Bit Depth | Subsampling |
|-----------------|----------|--------|-----------|-------------|
| NV12 | VK_FORMAT_G8_B8R8_2PLANE_420_UNORM | 2 | 8 | 4:2:0 |
| P010 | VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 | 2 | 10 | 4:2:0 |
| P012 | VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16 | 2 | 12 | 4:2:0 |
| I420 | VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM | 3 | 8 | 4:2:0 |
| NV16 | VK_FORMAT_G8_B8R8_2PLANE_422_UNORM | 2 | 8 | 4:2:2 |
| P210 | VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16 | 2 | 10 | 4:2:2 |
| YUV444 | VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM | 3 | 8 | 4:4:4 |
| RGBA8 | VK_FORMAT_R8G8B8A8_UNORM | 1 | 8 | N/A |

---

## 4. Component Details

### 4.1 VulkanFilterYuvCompute Class

**File**: `common/libs/VkCodecUtils/VulkanFilterYuvCompute.h/cpp`

#### 4.1.1 Filter Types

```cpp
enum FilterType {
    // Compute-based operations
    YCBCRCOPY,          // YCbCr copy with format conversion
    YCBCRCLEAR,         // Clear YCbCr to constant value
    YCBCR2RGBA,         // YCbCr → RGB conversion
    RGBA2YCBCR,         // RGB → YCbCr conversion
    
    // Transfer-only operations
    XFER_IMAGE_TO_BUFFER,   // vkCmdCopyImageToBuffer
    XFER_BUFFER_TO_IMAGE,   // vkCmdCopyBufferToImage
    XFER_IMAGE_TO_IMAGE,    // vkCmdCopyImage
};
```

#### 4.1.2 Filter Flags

```cpp
enum FilterFlags : uint32_t {
    FLAG_NONE                               = 0,
    FLAG_INPUT_MSB_TO_LSB_SHIFT             = (1 << 0),  // 10/12-bit MSB alignment
    FLAG_OUTPUT_LSB_TO_MSB_SHIFT            = (1 << 1),  // 10/12-bit MSB output
    FLAG_ENABLE_Y_SUBSAMPLING               = (1 << 2),  // 2x2 Y output for AQ
    FLAG_ENABLE_ROW_COLUMN_REPLICATION_ONE  = (1 << 3),  // Edge replication (1 row/col)
    FLAG_ENABLE_ROW_COLUMN_REPLICATION_ALL  = (1 << 4),  // Edge replication (all)
    FLAG_PRE_TRANSFER_ENABLED               = (1 << 8),  // Enable pre-transfer stage
    FLAG_POST_TRANSFER_ENABLED              = (1 << 9),  // Enable post-transfer stage
    FLAG_SKIP_COMPUTE                       = (1 << 10), // Transfer-only mode
};
```

### 4.2 VulkanSamplerYcbcrConversion

**File**: `common/libs/VkCodecUtils/VulkanSamplerYcbcrConversion.h/cpp`

Manages VkSampler and VkSamplerYcbcrConversion for YCbCr input sampling.

**Key Methods**:
- `CreateVulkanSampler()` - Creates sampler with YCbCr conversion
- `GetSampler()` - Returns VkSampler handle
- `SamplerRequiresUpdate()` - Checks if sampler needs recreation

### 4.3 ycbcr_utils.h

**File**: `common/include/nvidia_utils/vulkan/ycbcr_utils.h`

Central library for color space operations. Must be used consistently throughout the filter.

#### 4.3.1 Key Types

```cpp
// Color primaries coefficients
struct YcbcrPrimariesConstants {
    float kb;  // Blue coefficient
    float kr;  // Red coefficient
    // kg = 1.0 - kb - kr (green coefficient)
};

// Color standards
enum YcbcrBtStandard {
    YcbcrBtStandardUnknown    = -1,
    YcbcrBtStandardBt709      = 0,   // HD video
    YcbcrBtStandardBt601Ebu   = 1,   // SD video (EBU)
    YcbcrBtStandardBt601Smtpe = 2,   // SD video (SMPTE)
    YcbcrBtStandardBt2020     = 3,   // UHD/HDR video
};

// Color range
enum YCBCR_COLOR_RANGE {
    YCBCR_COLOR_RANGE_ITU_FULL   = 0,   // [0-255] / [0-1023]
    YCBCR_COLOR_RANGE_ITU_NARROW = 1,   // Y:[16-235], CbCr:[16-240]
    YCBCR_COLOR_RANGE_NATURAL    = ~0L, // Natural [0-1] range
};
```

#### 4.3.2 Key Functions

```cpp
// Get primaries coefficients for a color standard
YcbcrPrimariesConstants GetYcbcrPrimariesConstants(YcbcrBtStandard standard);

// Predefined primaries
// BT.709:  Kb=0.0722, Kr=0.2126
// BT.601:  Kb=0.114,  Kr=0.299
// BT.2020: Kb=0.0593, Kr=0.2627
```

#### 4.3.3 Key Classes

```cpp
// Color conversion matrix
class YcbcrBtMatrix {
    YcbcrBtMatrix(float kb, float kr, float cbMax, float crMax, YcbcrGamma* gamma);
    
    // Matrix operations
    int32_t GetRgbToYcbcrMatrix(float* matrix, uint32_t matrixSize);
    int32_t GetYcbcrToRgbMatrix(float* matrix, uint32_t matrixSize);
    
    // Conversion functions
    int32_t ConvertRgbToYcbcr(float yuv[3], const float rgb[3], ...);
    int32_t ConvertYcbcrToRgb(float rgb[3], const float yuv[3], ...);
    
    // GLSL code generation
    std::stringstream& ConvertRgbToYCbCrDiscreteChString(std::stringstream& out, ...);
    std::stringstream& ConvertYCbCrToRgbDiscreteChString(std::stringstream& out, ...);
};

// Range normalization
class YcbcrNormalizeColorRange {
    YcbcrNormalizeColorRange(unsigned bpp, YCBCR_COLOR_RANGE range, ...);
    
    // Get scale/shift for shader
    void getNormalizeScaleShiftValues(double* scale, double* shift, size_t n);
    void getDenormalizeScaleShiftValues(double* scale, int* shift, size_t n);
    
    // GLSL code generation
    void NormalizeYCbCrString(std::stringstream& out, const char* prepend);
};
```

---

## 5. Color Space Processing

### 5.1 RGB to YCbCr Conversion

```
Y  = Kr*R + Kg*G + Kb*B
Cb = (B - Y) / (2*(1-Kb)) * cbMax
Cr = (R - Y) / (2*(1-Kr)) * crMax

Where:
  Kg = 1.0 - Kr - Kb
  cbMax, crMax = 0.5 for digital (BT.xxx standards)
```

### 5.2 YCbCr to RGB Conversion

```
R = Y + Cr * (1-Kr) / crMax
G = Y - Cb * Kb*(1-Kb)/(Kg*cbMax) - Cr * Kr*(1-Kr)/(Kg*crMax)
B = Y + Cb * (1-Kb) / cbMax
```

### 5.3 Color Primaries Coefficients

| Standard | Kr | Kb | Kg | Use Case |
|----------|----|----|-----|----------|
| BT.709 | 0.2126 | 0.0722 | 0.7152 | HD video (1080p) |
| BT.601 (EBU) | 0.299 | 0.114 | 0.587 | SD video (PAL/NTSC) |
| BT.2020 | 0.2627 | 0.0593 | 0.678 | UHD/HDR (4K/8K) |

### 5.4 Range Mapping

| Range | Y (8-bit) | CbCr (8-bit) | Y (10-bit) | CbCr (10-bit) |
|-------|-----------|--------------|------------|---------------|
| Full | [0, 255] | [0, 255] | [0, 1023] | [0, 1023] |
| Limited | [16, 235] | [16, 240] | [64, 940] | [64, 960] |

---

## 6. Format Support Matrix

### 6.1 Conversion Support

| From \ To | RGBA8 | NV12 | P010 | I420 | NV16 | YUV444 |
|-----------|-------|------|------|------|------|--------|
| RGBA8 | - | ✓ | ✓ | ✓ | ✓ | ✓ |
| NV12 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| P010 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| I420 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| NV16 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| YUV444 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

### 6.2 I/O Type Support

| Configuration | Compute | Transfer |
|---------------|---------|----------|
| Image → Image | ✓ | ✓ |
| Buffer → Image | ✓ | ✓ |
| Image → Buffer | ✓ | ✓ |
| Buffer → Buffer | ✓ | - |
| Linear → Optimal | ✓ | ✓ |
| Optimal → Linear | ✓ | ✓ |

---

## 7. API Reference

### 7.1 Filter Creation

```cpp
static VkResult Create(
    const VulkanDeviceContext* vkDevCtx,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    FilterType filterType,
    uint32_t maxNumFrames,
    VkFormat inputFormat,
    VkFormat outputFormat,
    uint32_t filterFlags,
    const VkSamplerYcbcrConversionCreateInfo* pYcbcrConversionCreateInfo,
    const YcbcrPrimariesConstants* pYcbcrPrimariesConstants,
    const VkSamplerCreateInfo* pSamplerCreateInfo,
    VkSharedBaseObj<VulkanFilter>& vulkanFilter
);
```

### 7.2 Command Recording (Image → Image)

```cpp
VkResult RecordCommandBuffer(
    VkCommandBuffer cmdBuf,
    uint32_t bufferIdx,
    const VkImageResourceView* inputImageView,
    const VkVideoPictureResourceInfoKHR* inputImageResourceInfo,
    const VkImageResourceView* outputImageView,
    const VkVideoPictureResourceInfoKHR* outputImageResourceInfo,
    const VkImageResourceView* subsampledImageView = nullptr,
    const VkVideoPictureResourceInfoKHR* subsampledImageResourceInfo = nullptr
);
```

### 7.3 Command Recording (Unified with Transfers)

```cpp
VkResult RecordCommandBuffer(
    VkCommandBuffer cmdBuf,
    uint32_t bufferIdx,
    const FilterExecutionDesc& execDesc
);
```

### 7.4 FilterExecutionDesc Structure

```cpp
struct FilterExecutionDesc {
    std::array<FilterIOSlot, 4> inputs;
    uint32_t numInputs;
    
    std::array<FilterIOSlot, 4> outputs;
    uint32_t numOutputs;
    
    uint32_t srcLayer;
    uint32_t dstLayer;
    
    // Synchronization
    VkSemaphore waitSemaphore;
    uint64_t waitValue;
    VkPipelineStageFlags waitDstStageMask;
    VkSemaphore signalSemaphore;
    uint64_t signalValue;
};
```

---

## 8. Test Coverage

### 8.1 Test Categories

| Category | Test Count | Coverage |
|----------|------------|----------|
| RGBA → YCbCr | 8 formats | ✓ Complete |
| YCbCr → RGBA | 8 formats | ⚠ Disabled (shader bug) |
| Color Primaries | 6 tests | ✓ Complete |
| Range (Full/Limited) | 4 tests | ✓ Complete |
| YCbCr Copy | 5 formats | ✓ Complete |
| YCbCr Clear | 2 formats | ✓ Complete |
| Format Conversion | 6 tests | ✓ Complete |
| Buffer I/O | 7 tests | ⚠ Disabled (not implemented) |
| Linear Tiling | 4 tests | ✓ Complete |
| Transfer Ops | 11 tests | ✓ Complete |
| Edge Cases | 5 tests | ✓ Complete |

### 8.2 Test Files

| File | Purpose |
|------|---------|
| `FilterTestApp.cpp` | Test harness with VulkanDeviceContext |
| `TestCases.cpp` | Test case definitions |
| `ColorConversion.cpp` | CPU reference implementation |
| `main.cpp` | Test runner entry point |

### 8.3 Known Test Issues

| Test | Issue | Status |
|------|-------|--------|
| TC010-TC017 (YCBCR2RGBA) | Shader type mismatch in normalizeYCbCr | Disabled |
| TC070-TC076 (Buffer I/O) | Not implemented in filter execution | Disabled |
| TC090-TC091 (Multi-Output) | Requires flexible I/O implementation | Future |
| TC103 (8K) | May exceed GPU memory | Commented |

---

## 9. Known Issues and Limitations

### 9.1 Architecture Issues

| ID | Issue | Impact | Mitigation |
|----|-------|--------|------------|
| ARCH-001 | YCBCR2RGBA shader generation has type mismatch | YCbCr → RGBA tests fail | Disabled in smoke tests |
| ARCH-002 | Multi-output not fully implemented | Limited dual-output scenarios | Use separate filter passes |
| ARCH-003 | Y410 packed format needs special handling | Y410 tests disabled | Use unpacked formats |

### 9.2 ycbcr_utils.h Integration Issues

| ID | Issue | File | Status |
|----|-------|------|--------|
| YCB-001 | Duplicate GetYcbcrPrimariesConstantsId in pattern.cpp | pattern.cpp | Should use shared function |
| YCB-002 | Old backup in misc/ has BT.2020 bug | misc/VulkanFilterYuvCompute.cpp:198 | Backup is stale, main code is correct |

### 9.3 Testing Gaps

| Gap | Description | Priority |
|-----|-------------|----------|
| Buffer I/O | Buffer input/output paths not tested | High |
| YCBCR2RGBA | YCbCr to RGBA conversion not tested | High |
| P012 coverage | 12-bit format has limited testing | Medium |
| Multi-output | Dual/triple output scenarios | Medium |
| Odd dimensions | Edge case testing for non-multiple-of-2 | Low |

---

## Appendix A: File Locations

| Component | Path |
|-----------|------|
| VulkanFilterYuvCompute | `common/libs/VkCodecUtils/VulkanFilterYuvCompute.h/cpp` |
| VulkanFilter | `common/libs/VkCodecUtils/VulkanFilter.h/cpp` |
| VulkanSamplerYcbcrConversion | `common/libs/VkCodecUtils/VulkanSamplerYcbcrConversion.h/cpp` |
| ycbcr_utils.h | `common/include/nvidia_utils/vulkan/ycbcr_utils.h` |
| Test Application | `common/libs/tests/src/` |
| Existing Docs | `docs/VulkanFilterYuvCompute_*.md` |

## Appendix B: Related Documentation

- `VulkanFilterYuvCompute_FlexibleIO_Design.md` - Flexible I/O architecture
- `VulkanFilterYuvCompute_TransferOps_Design.md` - Transfer operations
- `VulkanFilterYuvCompute_Architecture.svg` - High-level architecture diagram
- `VulkanFilterYuvCompute_DataFlow.svg` - Data flow diagram
