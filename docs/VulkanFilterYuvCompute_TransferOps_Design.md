# VulkanFilterYuvCompute Transfer Operations Design

## 1. Overview

This document describes the enhancement of `VulkanFilterYuvCompute` to support pre-processing and post-processing transfer operations, enabling seamless data movement between different resource types (images, buffers) and tiling modes (optimal, linear) inline with compute operations.

## 2. Requirements

### 2.1 Functional Requirements

1. **Pre-Transfer Operations**: Before compute shader execution, transfer data:
   - Linear image → Optimal image (for GPU-friendly compute input)
   - Buffer → Image (for CPU-written data)
   - Image (any tiling) → Image (different tiling)

2. **Post-Transfer Operations**: After compute shader execution, transfer data:
   - Optimal image → Linear image (for CPU readback)
   - Image → Buffer (for dumping/encoding)
   - Image → Image (tiling conversion)

3. **Transfer-Only Mode**: New filter type `XFER` that performs only transfer operations:
   - `COPY_IMAGE_TO_BUFFER`
   - `COPY_BUFFER_TO_IMAGE`
   - `COPY_IMAGE_TO_IMAGE`

4. **Internal Synchronization**: Timeline semaphore synchronization between stages

5. **Queue Requirements**: Queue must support both `VK_QUEUE_COMPUTE_BIT` and `VK_QUEUE_TRANSFER_BIT`

### 2.2 Non-Functional Requirements

- Minimal overhead when transfers are not needed
- Reuse existing resource management infrastructure
- Compatible with existing `RecordCommandBuffer` API patterns

## 3. Architecture

### 3.1 Execution Pipeline

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Command Buffer Recording                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────┐     ┌──────────────┐     ┌───────────────┐                │
│  │ PRE-TRANSFER │ ──► │   COMPUTE    │ ──► │ POST-TRANSFER │                │
│  │   (optional) │     │  (optional)  │     │   (optional)  │                │
│  └──────────────┘     └──────────────┘     └───────────────┘                │
│         │                    │                     │                         │
│         ▼                    ▼                     ▼                         │
│  ┌──────────────┐     ┌──────────────┐     ┌───────────────┐                │
│  │   Barrier    │     │   Barrier    │     │    Barrier    │                │
│  │ XFER→COMPUTE │     │COMPUTE→XFER  │     │  XFER→HOST    │                │
│  └──────────────┘     └──────────────┘     └───────────────┘                │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Transfer Operation Types

```cpp
enum class TransferOpType {
    NONE = 0,
    COPY_IMAGE_TO_BUFFER,    // vkCmdCopyImageToBuffer
    COPY_BUFFER_TO_IMAGE,    // vkCmdCopyBufferToImage
    COPY_IMAGE_TO_IMAGE,     // vkCmdCopyImage or vkCmdBlitImage
};
```

### 3.3 Resource Descriptor

```cpp
/**
 * @brief Describes a resource (image or buffer) for transfer operations
 */
struct TransferResource {
    enum class Type { IMAGE, BUFFER };
    
    Type                type{Type::IMAGE};
    
    // For images
    VkImage             image{VK_NULL_HANDLE};
    VkImageLayout       currentLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageLayout       targetLayout{VK_IMAGE_LAYOUT_GENERAL};
    VkImageTiling       tiling{VK_IMAGE_TILING_OPTIMAL};
    VkFormat            format{VK_FORMAT_UNDEFINED};
    VkExtent3D          extent{0, 0, 1};
    uint32_t            arrayLayer{0};
    uint32_t            mipLevel{0};
    VkImageAspectFlags  aspectMask{VK_IMAGE_ASPECT_COLOR_BIT};
    
    // For buffers
    VkBuffer            buffer{VK_NULL_HANDLE};
    VkDeviceSize        offset{0};
    VkDeviceSize        size{0};
    VkDeviceSize        rowPitch{0};   // For image-like buffer layouts
    VkDeviceSize        depthPitch{0};
    
    // Helpers
    bool isImage() const { return type == Type::IMAGE; }
    bool isBuffer() const { return type == Type::BUFFER; }
    bool isLinear() const { return type == Type::IMAGE && tiling == VK_IMAGE_TILING_LINEAR; }
    bool isOptimal() const { return type == Type::IMAGE && tiling == VK_IMAGE_TILING_OPTIMAL; }
};
```

### 3.4 Transfer Operation Descriptor

```cpp
/**
 * @brief Describes a single transfer operation
 */
struct TransferOp {
    TransferOpType      opType{TransferOpType::NONE};
    TransferResource    src;
    TransferResource    dst;
    
    // For multi-plane images, specify per-plane regions
    uint32_t            numRegions{1};
    VkBufferImageCopy   bufferImageCopy[3];  // Max 3 planes
    VkImageCopy         imageCopy[3];
    
    bool isValid() const { return opType != TransferOpType::NONE; }
};
```

### 3.5 Enhanced Filter Configuration

```cpp
/**
 * @brief Extended filter type enum
 */
enum FilterType {
    YCBCRCOPY,      // Existing: compute-based copy
    YCBCRCLEAR,     // Existing: compute-based clear
    YCBCR2RGBA,     // Existing: YCbCr → RGBA conversion
    RGBA2YCBCR,     // Existing: RGBA → YCbCr conversion
    
    // New transfer-only types
    XFER_IMAGE_TO_BUFFER,   // Transfer only: image → buffer
    XFER_BUFFER_TO_IMAGE,   // Transfer only: buffer → image
    XFER_IMAGE_TO_IMAGE,    // Transfer only: image → image (tiling conversion)
};

/**
 * @brief Extended filter flags
 */
enum FilterFlags : uint32_t {
    // Existing flags...
    FLAG_NONE                               = 0,
    FLAG_INPUT_MSB_TO_LSB_SHIFT             = (1 << 0),
    FLAG_OUTPUT_LSB_TO_MSB_SHIFT            = (1 << 1),
    FLAG_ENABLE_Y_SUBSAMPLING               = (1 << 2),
    FLAG_ENABLE_ROW_COLUMN_REPLICATION_ONE  = (1 << 3),
    FLAG_ENABLE_ROW_COLUMN_REPLICATION_ALL  = (1 << 4),
    
    // New transfer flags
    FLAG_PRE_TRANSFER_ENABLED               = (1 << 8),   // Enable pre-transfer stage
    FLAG_POST_TRANSFER_ENABLED              = (1 << 9),   // Enable post-transfer stage
    FLAG_SKIP_COMPUTE                       = (1 << 10),  // Skip compute (transfer-only mode)
};
```

### 3.6 I/O Slot with Transfer Support

```cpp
/**
 * @brief Extended I/O slot with optional transfer operations
 */
struct FilterIOSlot {
    // Primary resource (used by compute shader)
    TransferResource    primary;
    
    // Optional pre-transfer: source for pre-transfer → primary
    TransferResource    preTransferSource;
    TransferOpType      preTransferOp{TransferOpType::NONE};
    
    // Optional post-transfer: primary → destination
    TransferResource    postTransferDest;
    TransferOpType      postTransferOp{TransferOpType::NONE};
    
    bool hasPreTransfer() const { return preTransferOp != TransferOpType::NONE; }
    bool hasPostTransfer() const { return postTransferOp != TransferOpType::NONE; }
};

/**
 * @brief Complete filter execution descriptor
 */
struct FilterExecutionDesc {
    // Input slots (up to 4 for flexibility)
    std::array<FilterIOSlot, 4> inputs;
    uint32_t numInputs{1};
    
    // Output slots (up to 4 for encoder, linear dump, display, etc.)
    std::array<FilterIOSlot, 4> outputs;
    uint32_t numOutputs{1};
    
    // Compute shader parameters
    uint32_t srcLayer{0};
    uint32_t dstLayer{0};
    
    // Synchronization
    VkSemaphore         waitSemaphore{VK_NULL_HANDLE};
    uint64_t            waitValue{0};
    VkSemaphore         signalSemaphore{VK_NULL_HANDLE};
    uint64_t            signalValue{0};
};
```

## 4. API Design

### 4.1 New RecordCommandBuffer Overload

```cpp
/**
 * @brief Record command buffer with full transfer support
 * 
 * Records: [pre-transfers] → [barriers] → [compute] → [barriers] → [post-transfers]
 * 
 * @param cmdBuf        Command buffer to record into
 * @param bufferIdx     Frame buffer index
 * @param execDesc      Execution descriptor with all I/O and transfer specs
 * @return VK_SUCCESS on success
 */
VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                             uint32_t bufferIdx,
                             const FilterExecutionDesc& execDesc);
```

### 4.2 Transfer-Only Creation

```cpp
/**
 * @brief Create a transfer-only filter instance
 */
static VkResult CreateTransfer(
    const VulkanDeviceContext* vkDevCtx,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    TransferOpType transferType,
    VkSharedBaseObj<VulkanFilter>& vulkanFilter);
```

### 4.3 Helper Methods

```cpp
/**
 * @brief Record pre-transfer operations
 */
void RecordPreTransfers(VkCommandBuffer cmdBuf,
                        const FilterExecutionDesc& execDesc);

/**
 * @brief Record post-transfer operations
 */
void RecordPostTransfers(VkCommandBuffer cmdBuf,
                         const FilterExecutionDesc& execDesc);

/**
 * @brief Record pipeline barrier between stages
 */
void RecordStageBarrier(VkCommandBuffer cmdBuf,
                        VkPipelineStageFlags srcStage,
                        VkPipelineStageFlags dstStage,
                        const TransferResource& resource,
                        VkImageLayout oldLayout,
                        VkImageLayout newLayout);

/**
 * @brief Calculate buffer-image copy regions for multi-plane formats
 */
void CalculateBufferImageCopyRegions(
    const TransferResource& image,
    const TransferResource& buffer,
    VkBufferImageCopy regions[3],
    uint32_t& numRegions);
```

## 5. Command Buffer Recording Sequence

### 5.1 Full Pipeline (Pre + Compute + Post)

```cpp
void RecordFullPipeline(VkCommandBuffer cmdBuf,
                        uint32_t bufferIdx,
                        const FilterExecutionDesc& execDesc)
{
    // ═══════════════════════════════════════════════════════════════════════
    // Stage 1: Pre-Transfer Operations
    // ═══════════════════════════════════════════════════════════════════════
    for (uint32_t i = 0; i < execDesc.numInputs; i++) {
        const auto& slot = execDesc.inputs[i];
        if (slot.hasPreTransfer()) {
            // Transition source to TRANSFER_SRC_OPTIMAL
            RecordLayoutTransition(cmdBuf, slot.preTransferSource,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            
            // Transition primary (dest) to TRANSFER_DST_OPTIMAL
            RecordLayoutTransition(cmdBuf, slot.primary,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            
            // Record transfer command
            RecordTransferOp(cmdBuf, slot.preTransferOp,
                            slot.preTransferSource, slot.primary);
        }
    }
    
    // Barrier: TRANSFER → COMPUTE
    if (hasAnyPreTransfer(execDesc)) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmdBuf,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    
    // ═══════════════════════════════════════════════════════════════════════
    // Stage 2: Compute Operation (existing logic)
    // ═══════════════════════════════════════════════════════════════════════
    if (!(m_filterFlags & FLAG_SKIP_COMPUTE)) {
        // Transition inputs to GENERAL for compute read
        for (uint32_t i = 0; i < execDesc.numInputs; i++) {
            RecordLayoutTransition(cmdBuf, execDesc.inputs[i].primary,
                                   VK_IMAGE_LAYOUT_GENERAL);
        }
        
        // Transition outputs to GENERAL for compute write
        for (uint32_t i = 0; i < execDesc.numOutputs; i++) {
            RecordLayoutTransition(cmdBuf, execDesc.outputs[i].primary,
                                   VK_IMAGE_LAYOUT_GENERAL);
        }
        
        // Bind pipeline, descriptors, dispatch (existing code)
        RecordComputeDispatch(cmdBuf, bufferIdx, execDesc);
    }
    
    // Barrier: COMPUTE → TRANSFER
    if (hasAnyPostTransfer(execDesc)) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmdBuf,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    
    // ═══════════════════════════════════════════════════════════════════════
    // Stage 3: Post-Transfer Operations
    // ═══════════════════════════════════════════════════════════════════════
    for (uint32_t i = 0; i < execDesc.numOutputs; i++) {
        const auto& slot = execDesc.outputs[i];
        if (slot.hasPostTransfer()) {
            // Transition primary (source) to TRANSFER_SRC_OPTIMAL
            RecordLayoutTransition(cmdBuf, slot.primary,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            
            // Record transfer command
            RecordTransferOp(cmdBuf, slot.postTransferOp,
                            slot.primary, slot.postTransferDest);
        }
    }
    
    // Final barrier if needed for host readback
    if (hasHostVisiblePostTransfer(execDesc)) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmdBuf,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT,
                            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
}
```

## 6. Use Cases

### 6.1 RGBA → YCbCr with Linear Output for Dumping

```cpp
FilterExecutionDesc execDesc;
execDesc.numInputs = 1;
execDesc.numOutputs = 1;

// Input: RGBA optimal image (no pre-transfer)
execDesc.inputs[0].primary = {
    .type = TransferResource::Type::IMAGE,
    .image = rgbaImage,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .format = VK_FORMAT_R8G8B8A8_UNORM,
    .extent = {width, height, 1}
};

// Output: YCbCr optimal (primary) with post-transfer to linear
execDesc.outputs[0].primary = {
    .type = TransferResource::Type::IMAGE,
    .image = ycbcrOptimalImage,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
    .extent = {width, height, 1}
};
execDesc.outputs[0].postTransferDest = {
    .type = TransferResource::Type::IMAGE,
    .image = ycbcrLinearImage,
    .tiling = VK_IMAGE_TILING_LINEAR,
    .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
    .extent = {width, height, 1}
};
execDesc.outputs[0].postTransferOp = TransferOpType::COPY_IMAGE_TO_IMAGE;

filter->RecordCommandBuffer(cmdBuf, frameIdx, execDesc);
```

### 6.2 Dual Output: Encoder (Optimal) + Dump (Buffer)

```cpp
FilterExecutionDesc execDesc;
execDesc.numInputs = 1;
execDesc.numOutputs = 2;

// Input: RGBA
execDesc.inputs[0].primary = {...};

// Output 0: For encoder (optimal, no post-transfer)
execDesc.outputs[0].primary = {
    .type = TransferResource::Type::IMAGE,
    .image = encoderInputImage,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
};

// Output 1: For dump (optimal primary, post-transfer to buffer)
execDesc.outputs[1].primary = {
    .type = TransferResource::Type::IMAGE,
    .image = dumpOptimalImage,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
};
execDesc.outputs[1].postTransferDest = {
    .type = TransferResource::Type::BUFFER,
    .buffer = dumpStagingBuffer,
    .size = bufferSize,
    .rowPitch = width
};
execDesc.outputs[1].postTransferOp = TransferOpType::COPY_IMAGE_TO_BUFFER;
```

### 6.3 Transfer-Only: Linear to Optimal (Pre-encode)

```cpp
// Create transfer-only filter
VkSharedBaseObj<VulkanFilter> xferFilter;
VulkanFilterYuvCompute::CreateTransfer(
    vkDevCtx, queueFamily, queueIndex,
    TransferOpType::COPY_IMAGE_TO_IMAGE,
    xferFilter);

FilterExecutionDesc execDesc;
execDesc.numInputs = 1;
execDesc.numOutputs = 1;

// For transfer-only, use preTransfer fields on input
execDesc.inputs[0].preTransferSource = {
    .type = TransferResource::Type::IMAGE,
    .image = linearImage,
    .tiling = VK_IMAGE_TILING_LINEAR
};
execDesc.inputs[0].primary = {
    .type = TransferResource::Type::IMAGE,
    .image = optimalImage,
    .tiling = VK_IMAGE_TILING_OPTIMAL
};
execDesc.inputs[0].preTransferOp = TransferOpType::COPY_IMAGE_TO_IMAGE;

xferFilter->RecordCommandBuffer(cmdBuf, 0, execDesc);
```

## 7. Implementation Plan

### Phase 1: Core Infrastructure
1. Add `TransferResource`, `TransferOp`, `FilterIOSlot` structs
2. Add `FilterExecutionDesc` struct
3. Add new `FilterFlags` for transfer operations
4. Implement `RecordLayoutTransition()` helper

### Phase 2: Transfer Recording
1. Implement `RecordPreTransfers()`
2. Implement `RecordPostTransfers()`
3. Implement `RecordTransferOp()` for each transfer type
4. Implement `CalculateBufferImageCopyRegions()` for multi-plane

### Phase 3: New RecordCommandBuffer Overload
1. Implement unified `RecordCommandBuffer(cmdBuf, bufferIdx, execDesc)`
2. Integrate pre/post transfers with existing compute logic
3. Add proper barrier insertion

### Phase 4: Transfer-Only Filter
1. Add `XFER_*` filter types
2. Implement `CreateTransfer()` factory method
3. Handle `FLAG_SKIP_COMPUTE` in command recording

### Phase 5: Testing
1. Add test cases for pre-transfer scenarios
2. Add test cases for post-transfer scenarios
3. Add test cases for dual-output scenarios
4. Add test cases for transfer-only mode

## 8. Synchronization Details

### 8.1 Internal Timeline Semaphore (Optional)

For complex multi-output scenarios where outputs have different consumers:

```cpp
struct FilterSyncInfo {
    // Per-output completion signaling
    struct OutputSync {
        VkSemaphore     semaphore{VK_NULL_HANDLE};
        uint64_t        signalValue{0};
    };
    std::array<OutputSync, 4> outputSync;
};
```

### 8.2 Queue Family Ownership Transfer

For scenarios where transfer and compute use different queue families:

```cpp
struct QueueTransferInfo {
    uint32_t srcQueueFamilyIndex{VK_QUEUE_FAMILY_IGNORED};
    uint32_t dstQueueFamilyIndex{VK_QUEUE_FAMILY_IGNORED};
    bool     releaseRequired{false};
    bool     acquireRequired{false};
};
```

## 9. Validation Considerations

1. **Queue Capability Check**: Verify queue supports both compute and transfer
2. **Format Compatibility**: Ensure source/dest formats are compatible for copy
3. **Tiling Support**: Verify linear tiling is supported for the format if used
4. **Size Matching**: Validate buffer sizes match image requirements
5. **Layout Tracking**: Track and validate image layouts through the pipeline

## 10. Performance Considerations

1. **Minimize Barriers**: Combine barriers where possible
2. **Async Transfer**: Use dedicated transfer queue when available
3. **Memory Aliasing**: Consider memory aliasing for intermediate resources
4. **Batch Transfers**: Group multiple plane copies into single command

---

*Document Version: 1.0*
*Last Updated: 2026-01-23*
