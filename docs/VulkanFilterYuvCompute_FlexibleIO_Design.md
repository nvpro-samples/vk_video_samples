# VulkanFilterYuvCompute Flexible I/O Architecture

## Document Information
- **Date**: January 23, 2026
- **Author**: AI Assistant
- **Status**: Design Proposal
- **Related Commit**: fe9187b16ab7c81cc0bb4db153edf4386bc0feaa

## Related Diagrams
- [VulkanFilterYuvCompute_Architecture.svg](VulkanFilterYuvCompute_Architecture.svg) - High-level architecture with I/O slots
- [VulkanFilterYuvCompute_Internal_Architecture.svg](VulkanFilterYuvCompute_Internal_Architecture.svg) - Internal architecture with binding layout
- [VulkanFilterYuvCompute_DataFlow.svg](VulkanFilterYuvCompute_DataFlow.svg) - Data flow through the filter stages

---

## 1. Problem Statement

### 1.1 Current Limitations

The `VulkanFilterYuvCompute` filter currently has a fixed binding layout:

| Binding | Purpose | Type |
|---------|---------|------|
| 0-3 | Input (RGBA/YCbCr planes) | Image or Buffer |
| 4-7 | Output (RGBA/YCbCr planes) | Image or Buffer |
| 8 | Uniform buffer (parameters) | Buffer |
| 9 | Subsampled Y output (AQ) | Image |

**Problems:**

1. **Fixed Number of Outputs**: Only one primary output (bindings 4-7) plus one optional subsampled output (binding 9). This is insufficient for use cases requiring:
   - Optimal YCbCr for encoder
   - Linear YCbCr for file dumping
   - 2x2 subsampled Y for AQ algorithms

2. **Broken Subsampled Image Dispatch** (commit fe9187b):
   - The dispatch was changed to adapt to output chroma subsampling
   - However, the subsampled Y output (binding 9) is **always** 2x2 subsampled, regardless of the primary output format
   - For 4:4:4 output, dispatch is at full resolution, but subsampled Y still needs 2x2 processing
   - The shader must handle double pixels when dispatch doesn't match 2x2

3. **Fixed Input Configuration**: Only one input image/buffer set (bindings 0-3)

4. **Hard-coded Naming**: Shader variable names like `inputImage`, `outputImage`, `subsampledImage` don't support multiple instances

### 1.2 Use Cases Requiring Flexible I/O

1. **Encoder Pipeline**:
   - Input: RGBA (renderer)
   - Output 0: YCbCr optimal (encoder input)
   - Output 1: YCbCr linear (file dump)
   - Output 2: 2x2 subsampled Y (AQ)

2. **Decoder Post-Processing**:
   - Input 0: YCbCr decoded frame
   - Input 1: Previous frame (temporal filtering)
   - Output: RGBA for display

3. **Multi-format Transcoding**:
   - Input: YCbCr format A
   - Output 0: YCbCr format B (different subsampling)
   - Output 1: Linear copy for debug

---

## 2. Requirements

### 2.1 Functional Requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| FR-1 | Support 1-N input slots, each configurable as image or buffer | High |
| FR-2 | Support 1-M output slots, each configurable as image or buffer | High |
| FR-3 | Each slot has independent format, layout (optimal/linear), and tiling | High |
| FR-4 | Dynamic binding allocation based on slot configuration | High |
| FR-5 | Indexed naming for shader variables (input0, input1, output0, output1) | High |
| FR-6 | Subsampled outputs work correctly regardless of primary output format | High |
| FR-7 | Filter configuration describes all I/O at creation time | High |
| FR-8 | Maintain backward compatibility with existing API | Medium |

### 2.2 Non-Functional Requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| NFR-1 | Dynamic shader generation (no fixed shaders) | High |
| NFR-2 | Maintain three-stage pipeline: Normalize → Process → Denormalize | High |
| NFR-3 | All processing in normalized [0.0, 1.0] color space | High |
| NFR-4 | Test coverage for all input/output combinations | High |
| NFR-5 | Performance: no regression from current implementation | Medium |
| NFR-6 | Use VkCodecUtils infrastructure classes (no manual Vulkan allocations) | High |

### 2.4 VkCodecUtils Infrastructure Requirements

All implementations and test applications **MUST** use the existing VkCodecUtils classes from `vulkan-video-samples/common/libs/VkCodecUtils/`. No manual Vulkan object creation (images, buffers, memory) is allowed.

**Required Classes to Use:**

| Class | Purpose | Usage |
|-------|---------|-------|
| `VkImageResource` | Image creation with proper memory allocation | Input/output images for filter |
| `VkImageResourceView` | Image view creation with YCbCr conversion support | Plane views, display views |
| `VkBufferResource` | Buffer creation with host/device memory | Staging buffers, linear data |
| `VulkanVideoImagePool` | Pooled image management | Frame pools for encoder/decoder |
| `VulkanDeviceContext` | Device context with dispatch table | All Vulkan API calls |
| `VulkanSamplerYcbcrConversion` | YCbCr sampler/conversion management | Input sampling with color conversion |
| `VulkanCommandBufferPool` | Command buffer pool management | Recording filter commands |
| `VulkanFilter` | Base filter class | Inherit for new filter types |
| `VkThreadPool` | Thread pool for async operations | File dump workers |
| `VkThreadSafeQueue` | Thread-safe queue | Frame queuing |

**Class Diagram:**

```
VulkanFilter (base)
    ├── VulkanCommandBufferPool (inherited)
    │       └── manages VkCommandPool, VkCommandBuffer
    └── VulkanFilterYuvCompute (compute filter)
            ├── uses VkImageResource for I/O images
            ├── uses VkImageResourceView for plane views
            ├── uses VkBufferResource for staging/uniform buffers
            ├── uses VulkanSamplerYcbcrConversion for input sampling
            └── uses VulkanVideoImagePool for frame pooling

Test Application
    ├── uses VulkanDeviceContext for Vulkan context
    ├── uses VulkanVideoImagePool for test frame pools
    ├── uses VkImageResource for reference images
    ├── uses VkBufferResource for readback buffers
    └── uses VkThreadPool for parallel validation
```

**Key VkCodecUtils Files:**

| File | Classes/Functions |
|------|-------------------|
| `VkImageResource.h/cpp` | `VkImageResource`, `VkImageResourceView` |
| `VkBufferResource.h/cpp` | `VkBufferResource` |
| `VulkanVideoImagePool.h/cpp` | `VulkanVideoImagePool` |
| `VulkanDeviceContext.h/cpp` | `VulkanDeviceContext` |
| `VulkanSamplerYcbcrConversion.h/cpp` | `VulkanSamplerYcbcrConversion` |
| `VulkanCommandBufferPool.h/cpp` | `VulkanCommandBufferPool` |
| `VulkanFilter.h/cpp` | `VulkanFilter` base class |
| `VkThreadPool.h/cpp` | `VkThreadPool` |
| `VkThreadSafeQueue.h/cpp` | `VkThreadSafeQueue` |
| `VulkanFrame.h/cpp` | `VulkanFrame` (frame management pattern) |

### 2.3 Processing Pipeline (Existing - to be preserved)

```
┌─────────────────────────────────────────────────────────────────────┐
│                        COMPUTE SHADER                                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌──────────────┐    ┌──────────────────┐    ┌──────────────────┐  │
│  │   INPUT      │    │    PROCESSING    │    │     OUTPUT       │  │
│  │  STAGE       │───▶│     STAGE        │───▶│     STAGE        │  │
│  │              │    │                  │    │                  │  │
│  │ • Fetch      │    │ • Color convert  │    │ • Denormalize    │  │
│  │ • Normalize  │    │ • Filter         │    │ • Pack           │  │
│  │ • Unpack     │    │ • Downsample     │    │ • Write          │  │
│  │              │    │                  │    │                  │  │
│  │ Output:      │    │ Input/Output:    │    │ Input:           │  │
│  │ vec4 [0,1]   │    │ vec4 [0,1]       │    │ vec4 [0,1]       │  │
│  └──────────────┘    └──────────────────┘    └──────────────────┘  │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.4 Dispatch Grid Strategy

The compute dispatch grid is determined by the **lowest resolution output**. This ensures all outputs are correctly covered with minimal thread divergence.

#### 2.4.1 General Rule

```
dispatchWidth  = lowestOutputWidth
dispatchHeight = lowestOutputHeight
workgroupsX    = (dispatchWidth + workgroupSizeX - 1) / workgroupSizeX
workgroupsY    = (dispatchHeight + workgroupSizeY - 1) / workgroupSizeY
```

Each thread processes a **block of pixels** whose size depends on the ratio between the dispatch resolution and each output's resolution.

#### 2.4.2 With Y Subsampling Enabled (FLAG_ENABLE_Y_SUBSAMPLING)

When the 2x2 subsampled Y output (binding 9) is enabled for Adaptive Quantization, the dispatch grid is **always at half resolution** regardless of the main output format:

```
dispatchWidth  = (outputWidth + 1) / 2
dispatchHeight = (outputHeight + 1) / 2
```

The subsampled Y output has the lowest resolution (always 2x2 subsampled), so it determines the dispatch grid.

**Thread processing for different main output formats:**

| Main Output | Subsampled Y | Dispatch Grid         | Thread Block | Per-Thread Work                    |
|-------------|--------------|----------------------|--------------|-------------------------------------|
| 4:2:0       | 2x2          | (W/2) × (H/2)        | 2x2 luma     | 4 Y, 1 CbCr read; 4 Y, 1 CbCr write; 1 subsamp Y |
| 4:2:2       | 2x2          | (W/2) × (H/2)        | 2x2 luma     | 4 Y, 2 CbCr read; 4 Y, 2 CbCr write; 1 subsamp Y |
| 4:4:4       | 2x2          | (W/2) × (H/2)        | 2x2 luma     | 4 Y, 4 CbCr read; 4 Y, 4 CbCr write; 1 subsamp Y |

When the main output is 4:4:4 or 4:2:2, each thread writes **multiple chroma samples** to the main output (2 for 4:2:2, 4 for 4:4:4) while writing only one sample to binding 9.

**Implementation Status by Filter Type:**

| Filter      | Y Subsampling Support | Dispatch Strategy                           |
|-------------|----------------------|---------------------------------------------|
| YCBCRCOPY   | ✓ Supported          | Always 2x2 dispatch, writes multi-pixels    |
| YCBCRCLEAR  | ✓ Supported          | Uses output chroma ratio for dispatch       |
| RGBA2YCBCR  | ✗ NOT YET SUPPORTED  | Uses output chroma ratio (needs update)     |
| YCBCR2RGBA  | N/A                  | Output is RGBA, no Y subsampling needed     |

> **TODO**: RGBA2YCBCR needs to be updated to support FLAG_ENABLE_Y_SUBSAMPLING.
> When enabled, it should use 2x2 dispatch grid and write multiple pixels to 4:4:4/4:2:2 outputs.

#### 2.4.3 Without Y Subsampling (Standard Mode)

Without subsampled Y output, dispatch is based on the main output's chroma resolution:

| Output Format | Chroma Ratio | Dispatch Grid           | Thread Block | Notes |
|---------------|--------------|-------------------------|--------------|-------|
| 4:2:0 (NV12)  | 2x2          | (width/2) × (height/2)  | 2x2 luma     | Each thread: 4 Y + 1 CbCr |
| 4:2:2 (NV16)  | 2x1          | (width/2) × height      | 2x1 luma     | Each thread: 2 Y + 1 CbCr |
| 4:4:4 (YUV444)| 1x1          | width × height          | 1x1 luma     | Each thread: 1 Y + 1 CbCr |

#### 2.4.4 Multi-Pixel Processing Example

For 4:4:4 output with Y subsampling enabled:
- Dispatch: `(width/2) × (height/2)` threads
- Each thread at position `(chromaX, chromaY)` handles:
  - **Input reads**: 4 Y samples + 4 CbCr samples from positions:
    - `(2*chromaX, 2*chromaY)`, `(2*chromaX+1, 2*chromaY)`
    - `(2*chromaX, 2*chromaY+1)`, `(2*chromaX+1, 2*chromaY+1)`
  - **Main output writes** (4:4:4): 4 Y + 4 CbCr at the same 4 positions
  - **Subsampled Y write**: Average of 4 Y values → 1 sample at `(chromaX, chromaY)`

```glsl
// Shader logic for 4:4:4 with Y subsampling
void main() {
    ivec2 chromaPos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 lumaPos = chromaPos * 2;  // Top-left of 2x2 block
    
    // Read 2x2 block of Y and CbCr
    vec3 ycbcr00 = readYCbCr(lumaPos + ivec2(0, 0));
    vec3 ycbcr10 = readYCbCr(lumaPos + ivec2(1, 0));
    vec3 ycbcr01 = readYCbCr(lumaPos + ivec2(0, 1));
    vec3 ycbcr11 = readYCbCr(lumaPos + ivec2(1, 1));
    
    // Write all 4 pixels to 4:4:4 output
    writeYCbCr(lumaPos + ivec2(0, 0), ycbcr00);
    writeYCbCr(lumaPos + ivec2(1, 0), ycbcr10);
    writeYCbCr(lumaPos + ivec2(0, 1), ycbcr01);
    writeYCbCr(lumaPos + ivec2(1, 1), ycbcr11);
    
    // Compute 2x2 box filter for subsampled Y
    float subsampledY = (ycbcr00.x + ycbcr10.x + ycbcr01.x + ycbcr11.x) * 0.25;
    imageStore(subsampledImageY, chromaPos, vec4(subsampledY, 0, 0, 1));
}
```

---

## 3. Design

### 3.1 I/O Slot Descriptor

```cpp
/**
 * @brief Describes a single input or output slot for the filter
 */
struct FilterIOSlot {
    enum class Type {
        None,           // Slot not used
        Image,          // VkImage (storage image)
        Buffer,         // VkBuffer (storage buffer)
        SampledImage    // VkImage with YCbCr sampler (input only)
    };
    
    enum class SubsampleMode {
        None,           // No subsampling (match dispatch)
        Fixed2x2,       // Always 2x2 subsampled (for AQ)
        MatchChroma     // Match output chroma subsampling
    };
    
    uint32_t        index{0};           // Slot index (0, 1, 2, ...)
    Type            type{Type::None};
    VkFormat        format{VK_FORMAT_UNDEFINED};
    VkImageTiling   tiling{VK_IMAGE_TILING_OPTIMAL};
    SubsampleMode   subsample{SubsampleMode::None};
    bool            isYOnly{false};     // Y-plane only (for subsampled outputs)
    
    // Computed at configuration time
    uint32_t        baseBinding{0};     // Starting binding number
    uint32_t        numPlanes{0};       // Number of planes/bindings used
    VkImageAspectFlags aspects{0};      // Image aspects
};

/**
 * @brief Complete filter configuration with dynamic I/O
 */
struct FilterConfiguration {
    // Filter operation type
    FilterType filterType{RGBA2YCBCR};
    
    // Color space settings
    VkSamplerYcbcrModelConversion ycbcrModel{VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709};
    VkSamplerYcbcrRange ycbcrRange{VK_SAMPLER_YCBCR_RANGE_ITU_FULL};
    VkChromaLocation chromaLocation{VK_CHROMA_LOCATION_COSITED_EVEN};
    
    // Dynamic I/O slots
    std::vector<FilterIOSlot> inputs;   // Input slots (max 4 recommended)
    std::vector<FilterIOSlot> outputs;  // Output slots (max 4 recommended)
    
    // Processing options
    bool enableRowColumnReplication{false};
    uint32_t maxNumFrames{4};
    
    // Computed binding layout
    uint32_t totalBindings{0};
    uint32_t uniformBufferBinding{0};
};
```

### 3.2 Binding Layout Strategy

**Dynamic Binding Allocation:**

```
┌─────────────────────────────────────────────────────────────────────┐
│                      BINDING LAYOUT                                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  Inputs (contiguous):                                                │
│  ┌────────┬────────┬────────┬────────┐                              │
│  │ Input0 │ Input0 │ Input0 │ Input0 │  ← Up to 4 planes per slot   │
│  │ bind 0 │ bind 1 │ bind 2 │ bind 3 │                              │
│  └────────┴────────┴────────┴────────┘                              │
│  ┌────────┬────────┬────────┬────────┐                              │
│  │ Input1 │ Input1 │ Input1 │ Input1 │  ← Second input (if any)     │
│  │ bind 4 │ bind 5 │ bind 6 │ bind 7 │                              │
│  └────────┴────────┴────────┴────────┘                              │
│                                                                      │
│  Outputs (contiguous, after inputs):                                 │
│  ┌────────┬────────┬────────┬────────┐                              │
│  │Output0 │Output0 │Output0 │Output0 │  ← Primary output            │
│  │bind  8 │bind  9 │bind 10 │bind 11 │                              │
│  └────────┴────────┴────────┴────────┘                              │
│  ┌────────┬────────┬────────┬────────┐                              │
│  │Output1 │Output1 │Output1 │Output1 │  ← Secondary output          │
│  │bind 12 │bind 13 │bind 14 │bind 15 │                              │
│  └────────┴────────┴────────┴────────┘                              │
│  ┌────────┐                                                          │
│  │Output2 │  ← Subsampled Y (1 binding, Y-only)                     │
│  │bind 16 │                                                          │
│  └────────┘                                                          │
│                                                                      │
│  Uniform Buffer (last):                                              │
│  ┌────────┐                                                          │
│  │Uniform │  ← Parameters                                           │
│  │bind 17 │                                                          │
│  └────────┘                                                          │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.3 Shader Variable Naming Convention

**Current (fixed):**
```glsl
layout(binding = 0) uniform sampler2D inputImage;
layout(binding = 4) writeonly image2DArray outputImageY;
layout(binding = 9) writeonly image2DArray subsampledImageY;
```

**Proposed (indexed):**
```glsl
// Inputs with index
layout(binding = 0) uniform sampler2D input0_rgba;
layout(binding = 4) readonly image2DArray input1_Y;
layout(binding = 5) readonly image2DArray input1_CbCr;

// Outputs with index
layout(binding = 8) writeonly image2DArray output0_Y;
layout(binding = 9) writeonly image2DArray output0_CbCr;
layout(binding = 12) writeonly image2DArray output1_Y;  // Linear copy
layout(binding = 13) writeonly image2DArray output1_CbCr;
layout(binding = 16) writeonly image2DArray output2_Y;  // 2x2 subsampled
```

### 3.4 Dispatch and Subsampling Fix

**Problem:** Commit fe9187b changed dispatch to match output chroma, but 2x2 subsampled outputs need special handling.

**Solution:**

```cpp
// Dispatch resolution based on PRIMARY output chroma
uint32_t dispatchWidth = outputWidth / primaryChromaHorzRatio;
uint32_t dispatchHeight = outputHeight / primaryChromaVertRatio;

// Each output slot declares its own processing requirement
for (auto& output : outputs) {
    if (output.subsample == SubsampleMode::Fixed2x2) {
        // This output processes 2x2 blocks regardless of dispatch
        // Shader generates loop to handle multiple writes per thread
    } else if (output.subsample == SubsampleMode::MatchChroma) {
        // This output matches dispatch resolution
    }
}
```

**Shader Generation for Mixed Subsampling:**

```glsl
void main() {
    ivec2 chromaPos = ivec2(gl_GlobalInvocationID.xy);
    
    // Process primary output at dispatch resolution
    processAndWriteOutput0(chromaPos);  // e.g., 4:2:0
    
    // For 2x2 subsampled output when dispatch is 4:4:4:
    // Need to accumulate 2x2 block and write when at block boundary
    if (output2_subsample == FIXED_2X2) {
        if ((chromaPos.x % 2 == 1) && (chromaPos.y % 2 == 1)) {
            // We're at bottom-right of 2x2 block
            // Average the 4 values and write
            float subsampledY = (Y00 + Y01 + Y10 + Y11) * 0.25;
            ivec2 subsamplePos = chromaPos / 2;
            imageStore(output2_Y, ivec3(subsamplePos, layer), vec4(subsampledY, 0, 0, 1));
        }
    }
}
```

### 3.5 API Changes

```cpp
/**
 * @brief Create filter with flexible I/O configuration
 */
static VkResult Create(
    const VulkanDeviceContext* vkDevCtx,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    const FilterConfiguration& config,  // New: full configuration
    VkSharedBaseObj<VulkanFilter>& vulkanFilter
);

/**
 * @brief Record commands with dynamic output binding
 */
VkResult RecordCommandBuffer(
    VkCommandBuffer cmdBuf,
    uint32_t frameIdx,
    // Input bindings (array)
    uint32_t numInputs,
    const VkImageView* inputViews,      // Array of input views
    const VkExtent2D* inputExtents,     // Array of input extents
    // Output bindings (array)  
    uint32_t numOutputs,
    const VkImageView* outputViews,     // Array of output views (per-plane)
    const VkExtent2D* outputExtents,    // Array of output extents
    // Parameters
    uint32_t srcLayer = 0,
    uint32_t dstLayer = 0
);
```

---

## 4. Implementation Plan

### Phase 1: Fix Subsampled Output Dispatch (Critical Bug)

1. Identify dispatch resolution based on primary output
2. Generate shader code that handles Fixed2x2 outputs at different dispatch resolutions
3. Add `SubsampleMode` to output configuration

### Phase 2: Indexed Naming

1. Add `index` field to shader generation functions
2. Update `GenImageIoBindingLayout()` to use indexed names
3. Update `ShaderGeneratePlaneDescriptors()` to use indexed names

### Phase 3: Dynamic Binding Allocation

1. Implement `FilterConfiguration` and `FilterIOSlot` structures
2. Implement `computeBindingLayout()` to assign binding numbers
3. Update `InitDescriptorSetLayout()` to use dynamic configuration

### Phase 4: Multiple Output Support

1. Update shader generation to iterate over outputs
2. Update `RecordCommandBuffer()` to accept output arrays
3. Update descriptor writing for multiple outputs

### Phase 5: Backward Compatibility

1. Keep existing `Create()` overload
2. Map old parameters to new `FilterConfiguration`

---

## 5. Test Plan

### 5.1 Test Application Structure

The test application **MUST** use VkCodecUtils infrastructure classes. No manual Vulkan allocations.

```
vulkan-video-samples/
└── vk_filter_test/
    ├── CMakeLists.txt
    ├── src/
    │   ├── main.cpp
    │   ├── FilterTestApp.cpp          // Uses VulkanDeviceContext
    │   ├── FilterTestApp.h
    │   ├── TestImagePool.cpp          // Uses VulkanVideoImagePool
    │   ├── TestImagePool.h
    │   ├── TestCases.cpp              // Uses VkImageResource, VkBufferResource
    │   └── TestCases.h
    └── test_data/
        ├── reference_nv12.yuv
        ├── reference_rgba.raw
        └── reference_i420.yuv
```

### 5.2 Test Application VkCodecUtils Integration

The test application follows the initialization pattern from `vk_video_encoder/src/vulkan_video_encoder.cpp`:

```cpp
/**
 * @brief Test application using VkCodecUtils infrastructure
 * 
 * Initialization pattern (matches encoder sample):
 * 1. Add required instance layers (validation if verbose)
 * 2. Add required instance extensions (debug_report if verbose)
 * 3. Add required device extensions
 * 4. InitVulkanDevice() - creates VkInstance
 * 5. InitDebugReport() - if validation enabled
 * 6. InitPhysicalDevice() - selects GPU with required queue types
 * 7. CreateVulkanDevice() - creates VkDevice with queues
 */
class FilterTestApp {
public:
    // VulkanDeviceContext as member (not VkSharedBaseObj - follows encoder pattern)
    VulkanDeviceContext m_vkDevCtx;
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    
    /**
     * @brief Initialize following encoder sample pattern
     */
    VkResult init(bool verbose) {
        // Instance layers and extensions for validation
        static const char* const requiredInstanceLayers[] = {
            "VK_LAYER_KHRONOS_validation", nullptr
        };
        static const char* const requiredInstanceExtensions[] = {
            VK_EXT_DEBUG_REPORT_EXTENSION_NAME, nullptr
        };
        
        // Device extensions for compute filter
        static const char* const requiredDeviceExtensions[] = {
            VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
            nullptr
        };
        static const char* const optionalDeviceExtensions[] = {
            VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME,
            VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
            nullptr
        };
        
        // Add validation layers if verbose (like encoder does for validate flag)
        if (verbose) {
            m_vkDevCtx.AddReqInstanceLayers(requiredInstanceLayers);
            m_vkDevCtx.AddReqInstanceExtensions(requiredInstanceExtensions);
        }
        
        m_vkDevCtx.AddReqDeviceExtensions(requiredDeviceExtensions, verbose);
        m_vkDevCtx.AddOptDeviceExtensions(optionalDeviceExtensions, verbose);
        
        // Creates VkInstance
        VkResult result = m_vkDevCtx.InitVulkanDevice("VkFilterTest", VK_NULL_HANDLE, verbose);
        if (result != VK_SUCCESS) return result;
        
        // Setup debug callback
        m_vkDevCtx.InitDebugReport(verbose, verbose);
        
        // Select physical device with compute+transfer queues (no video queues needed)
        vk::DeviceUuidUtils deviceUuid;  // Empty = auto-select
        result = m_vkDevCtx.InitPhysicalDevice(
            -1,                                              // deviceId: auto-select
            deviceUuid,
            VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,    // requestQueueTypes
            nullptr,                                          // no WSI
            0, VK_VIDEO_CODEC_OPERATION_NONE_KHR,            // no decode
            0, VK_VIDEO_CODEC_OPERATION_NONE_KHR             // no encode
        );
        if (result != VK_SUCCESS) return result;
        
        // Create logical device with compute+transfer queues
        result = m_vkDevCtx.CreateVulkanDevice(
            0,                              // numDecodeQueues
            0,                              // numEncodeQueues
            VK_VIDEO_CODEC_OPERATION_NONE_KHR,
            true,                           // createTransferQueue
            false,                          // createGraphicsQueue
            false,                          // createPresentQueue
            true                            // createComputeQueue
        );
        if (result != VK_SUCCESS) return result;
        
        // Create command pool for compute queue
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = m_vkDevCtx.GetComputeQueueFamilyIdx();
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        return m_vkDevCtx.CreateCommandPool(m_vkDevCtx.getDevice(), &poolInfo, nullptr, &m_commandPool);
    }
    
    /**
     * @brief Create staging buffer for readback using VkBufferResource
     */
    VkResult createStagingBuffer(
        size_t size,
        VkSharedBaseObj<VkBufferResource>& outBuffer
    ) {
        // Use VkBufferResource::Create() - no manual vkCreateBuffer
        return VkBufferResource::Create(
            m_vkDevCtx,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            size,
            outBuffer
        );
    }
};
```

### 5.3 Test Cases

| Test ID | Description | Inputs | Outputs | Validation |
|---------|-------------|--------|---------|------------|
| TC-001 | Single RGBA to NV12 | RGBA image | NV12 image | Compare with reference |
| TC-002 | Single RGBA to I420 | RGBA image | I420 image | Compare with reference |
| TC-003 | Dual output (optimal + linear) | RGBA image | NV12 optimal, NV12 linear | Both match reference |
| TC-004 | Triple output with subsampled | RGBA image | NV12, NV12 linear, 2x2 Y | All match reference |
| TC-005 | Buffer input | RGBA buffer | NV12 image | Compare with reference |
| TC-006 | Buffer output | RGBA image | NV12 buffer | Compare with reference |
| TC-007 | YCbCr to RGBA | NV12 image | RGBA image | Compare with reference |
| TC-008 | YCbCr copy (format convert) | NV12 image | I420 image | Compare with reference |
| TC-009 | 10-bit formats | P010 image | P010 image | Bit-exact copy |
| TC-010 | 4:4:4 with 2x2 subsampled | RGBA image | YUV444, 2x2 Y | Subsampled Y correct |

### 5.4 Validation Method

```cpp
/**
 * @brief Validate output using VkBufferResource for readback
 */
bool validateOutput(
    const VkSharedBaseObj<VkImageResource>& outputImage,
    const VkSharedBaseObj<VkBufferResource>& stagingBuffer,
    const void* referenceData,
    size_t referenceSize,
    float tolerance = 0.01f
) {
    // 1. Copy image to staging buffer (using VkCodecUtils helpers)
    // 2. Map staging buffer (VkBufferResource::GetMappedData())
    // 3. Compare with reference
    // 4. For YCbCr: compare Y, Cb, Cr planes separately
    // 5. Report PSNR for quality metrics
}
```

---

## 6. Timeline

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| Phase 1 | 2 days | Fix subsampled dispatch bug |
| Phase 2 | 2 days | Indexed naming implementation |
| Phase 3 | 3 days | Dynamic binding allocation |
| Phase 4 | 3 days | Multiple output support |
| Phase 5 | 1 day | Backward compatibility |
| Testing | 3 days | Test application + validation |
| **Total** | **14 days** | Complete flexible I/O support |

---

## 7. Appendix: Current Binding Layout Reference

```
Current Fixed Layout:
=====================
Binding 0: Input RGBA/Combined or Y plane
Binding 1: Input Y plane (multi-planar)
Binding 2: Input CbCr or Cb plane
Binding 3: Input Cr plane (3-plane)
Binding 4: Output RGBA/Combined or Y plane
Binding 5: Output Y plane (multi-planar)
Binding 6: Output CbCr or Cb plane
Binding 7: Output Cr plane (3-plane)
Binding 8: Uniform buffer (parameters)
Binding 9: Subsampled Y output (AQ)

Proposed Dynamic Layout (Example with 2 outputs):
=================================================
Binding 0-3:  Input 0 (RGBA or YCbCr planes)
Binding 4-7:  Output 0 (Primary YCbCr, optimal)
Binding 8-11: Output 1 (Secondary YCbCr, linear)
Binding 12:   Output 2 (2x2 subsampled Y)
Binding 13:   Uniform buffer
```

---

## 8. Test Cases Reference

### 8.1 Test Case Summary Table

| ID | Name | Filter Type | Input | Output | Description |
|----|------|-------------|-------|--------|-------------|
| **RGBA → YCbCr (8 formats)** |||||
| TC001 | RGBA_to_NV12 | RGBA2YCBCR | RGBA8 Image | NV12 Image | 8-bit 4:2:0 2-plane |
| TC002 | RGBA_to_P010 | RGBA2YCBCR | RGBA8 Image | P010 Image | 10-bit 4:2:0 2-plane |
| TC003 | RGBA_to_P012 | RGBA2YCBCR | RGBA8 Image | P012 Image | 12-bit 4:2:0 2-plane |
| TC004 | RGBA_to_I420 | RGBA2YCBCR | RGBA8 Image | I420 Image | 8-bit 4:2:0 3-plane |
| TC005 | RGBA_to_NV16 | RGBA2YCBCR | RGBA8 Image | NV16 Image | 8-bit 4:2:2 2-plane |
| TC006 | RGBA_to_P210 | RGBA2YCBCR | RGBA8 Image | P210 Image | 10-bit 4:2:2 2-plane |
| TC007 | RGBA_to_YUV444 | RGBA2YCBCR | RGBA8 Image | YUV444 Image | 8-bit 4:4:4 3-plane |
| TC008 | RGBA_to_Y410 | RGBA2YCBCR | RGBA8 Image | Y410 Image | 10-bit 4:4:4 packed |
| **YCbCr → RGBA (8 formats)** |||||
| TC010 | NV12_to_RGBA | YCBCR2RGBA | NV12 Image | RGBA8 Image | 8-bit 4:2:0 2-plane |
| TC011 | P010_to_RGBA | YCBCR2RGBA | P010 Image | RGBA8 Image | 10-bit 4:2:0 2-plane |
| TC012 | P012_to_RGBA | YCBCR2RGBA | P012 Image | RGBA8 Image | 12-bit 4:2:0 2-plane |
| TC013 | I420_to_RGBA | YCBCR2RGBA | I420 Image | RGBA8 Image | 8-bit 4:2:0 3-plane |
| TC014 | NV16_to_RGBA | YCBCR2RGBA | NV16 Image | RGBA8 Image | 8-bit 4:2:2 2-plane |
| TC015 | P210_to_RGBA | YCBCR2RGBA | P210 Image | RGBA8 Image | 10-bit 4:2:2 2-plane |
| TC016 | YUV444_to_RGBA | YCBCR2RGBA | YUV444 Image | RGBA8 Image | 8-bit 4:4:4 3-plane |
| TC017 | Y410_to_RGBA | YCBCR2RGBA | Y410 Image | RGBA8 Image | 10-bit 4:4:4 packed |
| **Color Primaries Tests** |||||
| TC020 | RGBA_to_NV12_BT601 | RGBA2YCBCR | RGBA8 Image | NV12 Image | BT.601 primaries |
| TC021 | RGBA_to_NV12_BT709 | RGBA2YCBCR | RGBA8 Image | NV12 Image | BT.709 primaries |
| TC022 | RGBA_to_NV12_BT2020 | RGBA2YCBCR | RGBA8 Image | NV12 Image | BT.2020 primaries |
| TC023 | RGBA_to_P010_BT601 | RGBA2YCBCR | RGBA8 Image | P010 Image | BT.601, 10-bit |
| TC024 | RGBA_to_P010_BT709 | RGBA2YCBCR | RGBA8 Image | P010 Image | BT.709, 10-bit |
| TC025 | RGBA_to_P010_BT2020 | RGBA2YCBCR | RGBA8 Image | P010 Image | BT.2020, 10-bit |
| **Range Tests** |||||
| TC030 | RGBA_to_NV12_FullRange | RGBA2YCBCR | RGBA8 Image | NV12 Image | Full range [0-255] |
| TC031 | RGBA_to_NV12_LimitedRange | RGBA2YCBCR | RGBA8 Image | NV12 Image | Limited range [16-235] |
| TC032 | RGBA_to_P010_FullRange | RGBA2YCBCR | RGBA8 Image | P010 Image | Full range, 10-bit |
| TC033 | RGBA_to_P010_LimitedRange | RGBA2YCBCR | RGBA8 Image | P010 Image | Limited range, 10-bit |
| **YCbCr Copy Tests** |||||
| TC040 | YCbCrCopy_NV12 | YCBCRCOPY | NV12 Image | NV12 Image | Same format copy |
| TC041 | YCbCrCopy_P010 | YCBCRCOPY | P010 Image | P010 Image | 10-bit copy |
| TC042 | YCbCrCopy_I420 | YCBCRCOPY | I420 Image | I420 Image | 3-plane copy |
| TC043 | YCbCrCopy_NV16 | YCBCRCOPY | NV16 Image | NV16 Image | 4:2:2 copy |
| TC044 | YCbCrCopy_YUV444 | YCBCRCOPY | YUV444 Image | YUV444 Image | 4:4:4 copy |
| **YCbCr Clear Tests** |||||
| TC050 | YCbCrClear_NV12 | YCBCRCLEAR | (none) | NV12 Image | Clear to neutral |
| TC051 | YCbCrClear_P010 | YCBCRCLEAR | (none) | P010 Image | 10-bit clear |
| **YCbCr Format Conversion Tests** |||||
| TC060 | NV12_to_I420 | YCBCRCOPY | NV12 Image | I420 Image | 2-plane → 3-plane |
| TC061 | I420_to_NV12 | YCBCRCOPY | I420 Image | NV12 Image | 3-plane → 2-plane |
| TC062 | NV12_to_NV16 | YCBCRCOPY | NV12 Image | NV16 Image | 4:2:0 → 4:2:2 |
| TC063 | NV12_to_YUV444 | YCBCRCOPY | NV12 Image | YUV444 Image | 4:2:0 → 4:4:4 |
| TC064 | P010_to_NV12 | YCBCRCOPY | P010 Image | NV12 Image | 10-bit → 8-bit |
| TC065 | NV12_to_P010 | YCBCRCOPY | NV12 Image | P010 Image | 8-bit → 10-bit |
| **Buffer I/O Tests** |||||
| TC070 | RGBABuffer_to_NV12Image | RGBA2YCBCR | RGBA8 Buffer | NV12 Image | Buffer → Image |
| TC071 | RGBAImage_to_NV12Buffer | RGBA2YCBCR | RGBA8 Image | NV12 Buffer | Image → Buffer |
| TC072 | RGBABuffer_to_NV12Buffer | RGBA2YCBCR | RGBA8 Buffer | NV12 Buffer | Buffer → Buffer |
| TC073 | NV12Buffer_to_RGBAImage | YCBCR2RGBA | NV12 Buffer | RGBA8 Image | YCbCr Buffer → Image |
| TC074 | NV12Image_to_RGBABuffer | YCBCR2RGBA | NV12 Image | RGBA8 Buffer | Image → RGBA Buffer |
| TC075 | RGBABuffer_to_P010Buffer | RGBA2YCBCR | RGBA8 Buffer | P010 Buffer | 10-bit buffer output |
| TC076 | P010Buffer_to_RGBABuffer | YCBCR2RGBA | P010 Buffer | RGBA8 Buffer | 10-bit buffer input |
| **Linear Tiling Tests** |||||
| TC080 | RGBA_to_NV12_Linear | RGBA2YCBCR | RGBA8 Image (Optimal) | NV12 Image (Linear) | Linear output |
| TC081 | RGBA_to_P010_Linear | RGBA2YCBCR | RGBA8 Image (Optimal) | P010 Image (Linear) | 10-bit linear |
| TC082 | Linear_NV12_to_Optimal_NV12 | YCBCRCOPY | NV12 Image (Linear) | NV12 Image (Optimal) | Tiling conversion |
| TC083 | Optimal_NV12_to_Linear_NV12 | YCBCRCOPY | NV12 Image (Optimal) | NV12 Image (Linear) | Tiling conversion |
| **Multi-Output Tests (Future)** |||||
| TC090 | Dual_Output_Optimal_Linear | RGBA2YCBCR | RGBA8 Image | NV12 (Optimal) + NV12 (Linear) | Dual output |
| TC091 | Triple_Output_with_Subsampled | RGBA2YCBCR | RGBA8 Image | NV12 (Opt) + NV12 (Lin) + Y (AQ) | With subsampled Y |
| **Edge Case Tests** |||||
| TC100 | Small_Resolution_64x64 | RGBA2YCBCR | RGBA8 Image 64×64 | NV12 Image 64×64 | Small resolution |
| TC101 | Odd_Resolution_1921x1081 | RGBA2YCBCR | RGBA8 Image 1921×1081 | NV12 Image 1921×1081 | Odd dimensions |
| TC102 | 4K_Resolution_3840x2160 | RGBA2YCBCR | RGBA8 Image 3840×2160 | NV12 Image 3840×2160 | 4K resolution |
| TC103 | 8K_Resolution_7680x4320 | RGBA2YCBCR | RGBA8 Image 7680×4320 | NV12 Image 7680×4320 | 8K resolution |
| TC104 | Minimum_Resolution_2x2 | RGBA2YCBCR | RGBA8 Image 2×2 | NV12 Image 2×2 | Minimum size |

### 8.2 Format Reference

| Format | VkFormat | Bit Depth | Subsampling | Planes | Description |
|--------|----------|-----------|-------------|--------|-------------|
| RGBA8 | VK_FORMAT_R8G8B8A8_UNORM | 8 | 4:4:4 | 1 | Standard RGBA |
| NV12 | VK_FORMAT_G8_B8R8_2PLANE_420_UNORM | 8 | 4:2:0 | 2 | Y + interleaved CbCr |
| P010 | VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 | 10 | 4:2:0 | 2 | 10-bit NV12 |
| P012 | VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16 | 12 | 4:2:0 | 2 | 12-bit NV12 |
| I420 | VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM | 8 | 4:2:0 | 3 | Y + Cb + Cr separate |
| NV16 | VK_FORMAT_G8_B8R8_2PLANE_422_UNORM | 8 | 4:2:2 | 2 | Horizontal subsampling |
| P210 | VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16 | 10 | 4:2:2 | 2 | 10-bit 4:2:2 |
| YUV444 | VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM | 8 | 4:4:4 | 3 | No subsampling |
| Y410 | VK_FORMAT_A2R10G10B10_UNORM_PACK32 | 10 | 4:4:4 | 1 | Packed AYUV |

### 8.3 Filter Types

| Filter Type | Description | Input | Output |
|-------------|-------------|-------|--------|
| RGBA2YCBCR | RGB to YCbCr conversion | RGBA image/buffer | YCbCr image/buffer |
| YCBCR2RGBA | YCbCr to RGB conversion | YCbCr image/buffer | RGBA image/buffer |
| YCBCRCOPY | YCbCr format conversion/copy | YCbCr image/buffer | YCbCr image/buffer |
| YCBCRCLEAR | Clear YCbCr to neutral | (none) | YCbCr image/buffer |

### 8.4 Resource Types and Tiling

| Resource Type | Tiling Mode | Use Case |
|---------------|-------------|----------|
| Image (Optimal) | VK_IMAGE_TILING_OPTIMAL | GPU processing, encoding |
| Image (Linear) | VK_IMAGE_TILING_LINEAR | Host read-back, dumping |
| Buffer | N/A | Direct data transfer |

### 8.5 Smoke Tests (Quick Validation)

The following tests are run for smoke testing:
1. **TC001_RGBA_to_NV12** - Basic 8-bit 4:2:0 conversion
2. **TC002_RGBA_to_P010** - 10-bit format support
3. **TC005_RGBA_to_NV16** - 4:2:2 subsampling
4. **TC007_RGBA_to_YUV444** - 4:4:4 format
5. **TC040_YCbCrCopy_NV12** - Copy operation
6. **TC050_YCbCrClear_NV12** - Clear operation
7. **TC100_Small_Resolution_64x64** - Edge case handling

### 8.6 Known Issues

| Test | Issue | Status |
|------|-------|--------|
| TC010-TC017 (YCBCR2RGBA) | Shader generation type mismatch in `normalizeYCbCr`/`shiftCbCr` | Disabled in smoke tests |
| TC070-TC076 (Buffer I/O) | Buffer input/output not fully implemented | Disabled |
| TC090-TC091 (Multi-Output) | Requires flexible I/O implementation | Future work |
| TC103 (8K) | May exceed GPU memory on some devices | Commented out |

### 8.7 Running Tests

```bash
# Run smoke tests (default)
./vk_filter_test

# Run with validation layers
./vk_filter_test --verbose

# Run all standard tests
./vk_filter_test --all

# Run specific test
./vk_filter_test --test TC001_RGBA_to_NV12

# List available tests
./vk_filter_test --list
```

---

## 9. CPU Verification Reference Implementation

### 9.1 Overview

The test application includes a CPU reference implementation (`ColorConversion.h/cpp`) for validating GPU filter outputs. This enables bit-accurate comparison between GPU and CPU results.

### 9.2 Color Conversion Matrices

The reference implementation supports three color primaries standards:

| Standard | Kr | Kb | Kg | Use Case |
|----------|----|----|----|----|
| **BT.601** | 0.299 | 0.114 | 0.587 | SD video (NTSC/PAL) |
| **BT.709** | 0.2126 | 0.0722 | 0.7152 | HD video (1080p) |
| **BT.2020** | 0.2627 | 0.0593 | 0.678 | UHD/HDR video (4K/8K) |

**RGB to YCbCr conversion:**
```
Y  = Kr*R + Kg*G + Kb*B
Cb = (B - Y) / (2*(1-Kb))
Cr = (R - Y) / (2*(1-Kr))
```

**YCbCr to RGB conversion:**
```
R = Y + 2*(1-Kr)*Cr
G = Y - 2*Kb*(1-Kb)/Kg*Cb - 2*Kr*(1-Kr)/Kg*Cr
B = Y + 2*(1-Kb)*Cb
```

### 9.3 Range Handling

| Range | Y Range (8-bit) | CbCr Range (8-bit) | Description |
|-------|-----------------|--------------------|----|
| **Full** | [0, 255] | [0, 255] | Computer/PC range |
| **Limited** | [16, 235] | [16, 240] | Broadcast/TV range |

For higher bit depths:

| Bit Depth | Full Range Max | Y Limited | CbCr Limited |
|-----------|----------------|-----------|--------------|
| 8-bit | 255 | [16, 235] | [16, 240] |
| 10-bit | 1023 | [64, 940] | [64, 960] |
| 12-bit | 4095 | [256, 3760] | [256, 3840] |

### 9.4 API Reference

#### Conversion Functions

```cpp
// Normalized RGB <-> YCbCr (0-1 range)
YCbCrPixel rgbToYCbCr(const RGBPixel& rgb, ColorPrimaries primaries);
RGBPixel ycbcrToRgb(const YCbCrPixel& ycbcr, ColorPrimaries primaries);

// 8-bit integer conversion with range handling
void rgbToYCbCr8(uint8_t r, uint8_t g, uint8_t b,
                 ColorPrimaries primaries, ColorRange range,
                 uint8_t& y, uint8_t& cb, uint8_t& cr);

void ycbcrToRgb8(uint8_t y, uint8_t cb, uint8_t cr,
                 ColorPrimaries primaries, ColorRange range,
                 uint8_t& r, uint8_t& g, uint8_t& b);

// 16-bit integer conversion (for 10/12-bit content)
void rgbToYCbCr16(uint16_t r, uint16_t g, uint16_t b,
                  uint32_t bitDepth,
                  ColorPrimaries primaries, ColorRange range,
                  uint16_t& y, uint16_t& cb, uint16_t& cr);
```

#### Bulk Conversion Functions

```cpp
// RGBA to various YCbCr formats
void convertRGBAtoNV12(...);   // 4:2:0 2-plane
void convertRGBAtoP010(...);   // 4:2:0 10-bit
void convertRGBAtoI420(...);   // 4:2:0 3-plane
void convertRGBAtoNV16(...);   // 4:2:2 2-plane
void convertRGBAtoYUV444(...); // 4:4:4 3-plane

// YCbCr to RGBA
void convertNV12toRGBA(...);
```

### 9.5 Test Pattern Generation

| Pattern | Description | Use Case |
|---------|-------------|----------|
| **ColorBars** | SMPTE color bars | Visual verification |
| **Gradient** | Horizontal black-to-white | Ramp accuracy |
| **Checkerboard** | 8x8 pixel blocks | Edge handling |
| **Ramp** | All values 0-255 | Full range coverage |
| **Solid** | Uniform mid-gray | DC level verification |
| **Random** | Pseudo-random | Stress testing |

```cpp
void generateRGBATestPattern(TestPatternType type, 
                             uint32_t width, uint32_t height,
                             std::vector<uint8_t>& data);

void generateNV12TestPattern(TestPatternType type,
                             uint32_t width, uint32_t height,
                             ColorPrimaries primaries, ColorRange range,
                             std::vector<uint8_t>& yPlane,
                             std::vector<uint8_t>& uvPlane);
```

### 9.6 Validation Metrics

#### PSNR (Peak Signal-to-Noise Ratio)

```cpp
double calculatePSNR(const uint8_t* data1, const uint8_t* data2, 
                     size_t size, uint32_t maxValue = 255);
```

| PSNR Range | Quality |
|------------|---------|
| > 50 dB | Excellent (near lossless) |
| 40-50 dB | Very good |
| 30-40 dB | Acceptable |
| < 30 dB | Poor (visible artifacts) |

#### Comparison Functions

```cpp
ValidationResult compareNV12(const uint8_t* actualY, const uint8_t* actualUV,
                             const uint8_t* expectedY, const uint8_t* expectedUV,
                             uint32_t width, uint32_t height,
                             uint32_t tolerance = 2);

ValidationResult compareRGBA(const uint8_t* actual, const uint8_t* expected,
                             uint32_t width, uint32_t height,
                             uint32_t tolerance = 2);
```

#### ValidationResult Structure

```cpp
struct ValidationResult {
    bool passed;              // Overall pass/fail
    double psnrY, psnrCb, psnrCr;  // PSNR per channel
    double maxErrorY, maxErrorCb, maxErrorCr;  // Maximum error
    uint32_t errorCountY, errorCountCb, errorCountCr;  // Error counts
    std::string errorMessage;
};
```

### 9.7 Subsampled Output Verification

For 4:2:0 (NV12, P010, I420) and 4:2:2 (NV16, P210) formats, chroma subsampling uses box filter averaging:

**4:2:0 (2x2 block):**
```cpp
// Average 4 pixels for one chroma sample
Cb_out = (Cb[0,0] + Cb[0,1] + Cb[1,0] + Cb[1,1]) / 4
Cr_out = (Cr[0,0] + Cr[0,1] + Cr[1,0] + Cr[1,1]) / 4
```

**4:2:2 (2x1 block):**
```cpp
// Average 2 horizontal pixels
Cb_out = (Cb[0] + Cb[1]) / 2
Cr_out = (Cr[0] + Cr[1]) / 2
```

### 9.8 Buffer I/O Verification

For buffer inputs/outputs, verification compares raw byte data:

1. **Input Buffer:** Generate test pattern directly in host-visible buffer
2. **Output Buffer:** Map and read back for CPU comparison
3. **Format Layouts:**
   - NV12: Y plane (W×H), then UV plane (W/2 × H/2 × 2)
   - I420: Y plane, then U plane (W/2 × H/2), then V plane (W/2 × H/2)
   - P010: Same layout as NV12 but 16-bit per sample (MSB-aligned)

### 9.9 Tolerance Guidelines

| Test Type | Recommended Tolerance | Rationale |
|-----------|----------------------|-----------|
| Copy (same format) | 0 | Bit-exact expected |
| Clear | 0 | Bit-exact expected |
| RGB↔YCbCr (8-bit) | 2 | Rounding errors |
| RGB↔YCbCr (10-bit) | 1 (in 10-bit space) | Higher precision |
| With subsampling | 3 | Chroma averaging differences |
| Range conversion | 2 | Scaling precision |
