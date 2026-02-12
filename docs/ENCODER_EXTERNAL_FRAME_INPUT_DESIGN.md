# Encoder Extended Interface: External Frame Input with Synchronization

## Requirements

1. Accept **ref-counted images** (`VkSharedBaseObj<VkImageResource>` / `VkSharedBaseObj<VulkanVideoImagePoolNode>`) as input instead of file-based I/O
2. Support **synchronization** via wait/signal semaphore arrays (timeline or binary) on both input and output
3. **Slice through** the existing `LoadNextFrame` -> `StageInputFrame` -> `EncodeFrameCommon` pipeline:
   - **Optimal YCbCr** (NV12, P010): inject directly into `srcEncodeImageResource`, skip staging and filter
   - **Linear YCbCr**: inject into `srcStagingImageView`, upload to optimal via existing staging path, skip file load
   - **RGBA (any tiling)**: inject into `srcStagingImageView` or `srcEncodeImageResource` depending on tiling, enable filter (RGBA->YCbCr)
4. The **filter is optional** - only used when the input format doesn't match what the encoder accepts
5. **Asynchronous bitstream retrieval**: request a bitstream buffer for a frame ID, get it back with a fence/semaphore when the encode completes
6. Future: **pool of threads** for bitstream assembly (currently synchronous `AssembleBitstreamData`)

## Current Frame Path Analysis

```
Current (file-based):

  File (YUV/Y4M)
       |
       v
  LoadNextFrame()              <-- reads from m_inputFileHandler, copies to srcStagingImageView
       |                           (linear, CPU-accessible, VkImageResource)
       v
  StageInputFrame()            <-- copies/filters from srcStagingImageView to srcEncodeImageResource
       |                           - If filter enabled: VulkanFilterYuvCompute dispatch
       |                           - If no filter: vkCmdCopyImage (linear -> optimal)
       |                           - Submits staging command buffer, waits via fence
       v
  SubmitStagedInputFrame()     <-- waits for staging fence to complete
       |
       v
  EncodeFrameCommon()          <-- GOP position, DPB management, AQ, queue frame
       |
       v
  RecordVideoCodingCmd()       <-- records vkCmdEncodeVideoKHR into encodeCmdBuffer
       |
       v
  SubmitVideoCodingCmds()      <-- submits to encode queue, signals encode fence
       |
       v
  AssembleBitstreamData()      <-- SYNCHRONOUS: waits for encode fence, reads bitstream,
                                   writes to file via fwrite()
```

### Key Fields in VkVideoEncodeFrameInfo

| Field | Type | Role |
|-------|------|------|
| `srcStagingImageView` | `VkSharedBaseObj<VulkanVideoImagePoolNode>` | Linear staging image (CPU-accessible) |
| `srcEncodeImageResource` | `VkSharedBaseObj<VulkanVideoImagePoolNode>` | Optimal encode input image |
| `outputBitstreamBuffer` | `VkSharedBaseObj<VulkanBitstreamBuffer>` | Output bitstream VkBuffer |
| `inputCmdBuffer` | `VkSharedBaseObj<VulkanCommandBufferPool::PoolNode>` | Command buffer for staging |
| `encodeCmdBuffer` | `VkSharedBaseObj<VulkanCommandBufferPool::PoolNode>` | Command buffer for encode |
| `inputTimeStamp` | `uint64_t` | PTS |
| `frameInputOrderNum` | `uint64_t` | Input sequence number |
| `gopPosition` | `VkVideoGopStructure::GopPosition` | I/P/B decision |

### Image Pools (allocated at init)

| Pool | Variable | Content |
|------|----------|---------|
| `m_linearInputImagePool` | `VulkanVideoImagePool` | Linear staging images |
| `m_inputImagePool` | `VulkanVideoImagePool` | Optimal encode input images |
| `m_dpbImagePool` | `VulkanVideoImagePool` | DPB reference frames |
| `m_bitstreamBufferPool` | `VulkanBitstreamBufferPool` | Output bitstream VkBuffers |

## Extended Frame Path Design

### Three Input Paths

```
Path A: Optimal YCbCr (NV12/P010) -- ZERO COPY
  External VkImage (optimal, directly encodable format)
       |
       v
  [Skip LoadNextFrame, Skip StageInputFrame]
  Inject into encodeFrameInfo->srcEncodeImageResource directly
       |
       v
  EncodeFrameCommon() -> RecordVideoCodingCmd() -> SubmitVideoCodingCmds()
       |
       v
  Async bitstream retrieval


Path B: Linear YCbCr -- UPLOAD ONLY
  External VkImage (linear, YCbCr)
       |
       v
  [Skip LoadNextFrame]
  Inject into encodeFrameInfo->srcStagingImageView
       |
       v
  StageInputFrame()  <-- copies linear -> optimal (no filter needed)
       |
       v
  EncodeFrameCommon() -> RecordVideoCodingCmd() -> SubmitVideoCodingCmds()
       |
       v
  Async bitstream retrieval


Path C: RGBA (any tiling) -- FILTER REQUIRED
  External VkImage (RGBA, linear or optimal)
       |
       v
  [Skip LoadNextFrame]
  Inject into srcStagingImageView (linear) or srcEncodeImageResource (optimal)
       |
       v
  StageInputFrame()  <-- VulkanFilterYuvCompute: RGBA -> NV12/P010
       |
       v
  EncodeFrameCommon() -> RecordVideoCodingCmd() -> SubmitVideoCodingCmds()
       |
       v
  Async bitstream retrieval
```

### Path Selection Logic

```cpp
if (inputFormat is directly encodable && inputTiling is optimal) {
    // Path A: inject into srcEncodeImageResource, skip staging entirely
    needsStaging = false;
    needsFilter = false;
} else if (inputFormat is YCbCr && inputTiling is linear) {
    // Path B: inject into srcStagingImageView, use existing linear->optimal copy
    needsStaging = true;
    needsFilter = false;
} else {
    // Path C: inject into staging, run filter (RGBA->YCbCr)
    needsStaging = true;
    needsFilter = true;
}
```

## Extended Internal Interface (VkVideoEncoder additions)

### New Methods on VkVideoEncoder

```cpp
class VkVideoEncoder {
    // ... existing methods ...

    // === External frame input ===

    // Accept a ref-counted external image as the encode input.
    // This replaces LoadNextFrame() for external sources.
    //
    // externalImage: ref-counted image resource (optimal or linear)
    // format: VkFormat of the image
    // tiling: VK_IMAGE_TILING_OPTIMAL or VK_IMAGE_TILING_LINEAR
    // frameId: unique frame identifier (for async bitstream retrieval)
    // pts: presentation timestamp
    //
    // Wait semaphores are submitted as part of the staging/encode command buffer.
    // Signal semaphores are submitted after the encode command completes.
    VkResult SetExternalInputFrame(
        VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
        VkSharedBaseObj<VkImageResource>& externalImage,
        VkFormat format,
        VkImageTiling tiling,
        uint32_t width, uint32_t height,
        uint64_t frameId,
        uint64_t pts,
        uint32_t waitSemaphoreCount,
        const VkSemaphore* pWaitSemaphores,
        const uint64_t* pWaitSemaphoreValues,
        uint32_t signalSemaphoreCount,
        const VkSemaphore* pSignalSemaphores,
        const uint64_t* pSignalSemaphoreValues);

    // Alternative: accept raw VkImage + VkDeviceMemory (for imported DMA-BUF images
    // that aren't wrapped in VkImageResource). Creates a temporary wrapper.
    VkResult SetExternalInputImage(
        VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
        VkImage image,
        VkImageView imageView,       // VK_NULL_HANDLE = create internally
        VkDeviceMemory memory,
        VkFormat format,
        VkImageTiling tiling,
        uint32_t width, uint32_t height,
        uint64_t frameId,
        uint64_t pts,
        uint32_t waitSemaphoreCount,
        const VkSemaphore* pWaitSemaphores,
        const uint64_t* pWaitSemaphoreValues,
        uint32_t signalSemaphoreCount,
        const VkSemaphore* pSignalSemaphores,
        const uint64_t* pSignalSemaphoreValues);

    // === Async bitstream retrieval ===

    // Request a bitstream buffer handle for a frame ID.
    // Returns immediately. The bitstream may not be ready yet.
    //
    // bitstreamBuffer: [out] ref-counted bitstream buffer
    // fence: [out] fence that will be signaled when the bitstream is ready
    // semaphore: [out] semaphore that will be signaled (optional, can be VK_NULL_HANDLE)
    //
    // The caller must wait on the fence/semaphore before reading the bitstream.
    // After reading, release the bitstreamBuffer ref to return it to the pool.
    VkResult RequestBitstreamBuffer(
        uint64_t frameId,
        VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer,
        VkFence& fence,
        VkSemaphore semaphore = VK_NULL_HANDLE);

    // Poll: check if a bitstream is ready for a frame ID.
    // Returns VK_SUCCESS if ready, VK_NOT_READY if still encoding.
    VkResult PollBitstreamReady(uint64_t frameId);

    // Get the bitstream data pointer and size after the fence is signaled.
    // bitstreamBuffer must have been obtained from RequestBitstreamBuffer().
    VkResult GetBitstreamData(
        VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer,
        const uint8_t** ppData,
        uint32_t* pSize);
};
```

### New Fields on VkVideoEncodeFrameInfo

```cpp
struct VkVideoEncodeFrameInfo : public VkVideoRefCountBase {
    // ... existing fields ...

    // === External input support ===

    // When set, this image is used directly as the encode input
    // instead of the internally-allocated pool image.
    // The VkSharedBaseObj ref-count keeps the image alive until encode completes.
    VkSharedBaseObj<VkImageResource> externalInputImage;

    // True if the input was provided externally (not loaded from file)
    bool isExternalInput{false};

    // Tiling of the external input (determines which path to take)
    VkImageTiling externalInputTiling{VK_IMAGE_TILING_OPTIMAL};

    // === Input synchronization ===

    // Wait semaphores for the staging/encode command buffer.
    // These are waited on before the encoder accesses the input image.
    std::vector<VkSemaphore> inputWaitSemaphores;
    std::vector<uint64_t>    inputWaitSemaphoreValues;
    std::vector<VkPipelineStageFlags> inputWaitDstStageMasks;

    // Signal semaphores after the input image is no longer needed.
    // The encoder signals these after the staging copy (if any) completes.
    // This tells the producer the frame slot can be reused.
    std::vector<VkSemaphore> inputSignalSemaphores;
    std::vector<uint64_t>    inputSignalSemaphoreValues;

    // === Async bitstream retrieval ===

    // Fence signaled when the encode command completes and bitstream is ready.
    // Currently the encodeCmdBuffer already has a fence - reuse it.
    // For async retrieval, the caller can poll/wait on this fence
    // without blocking the encode pipeline.
    // (encodeCmdBuffer->GetFence() already exists)
};
```

## Synchronization Flow

### Per-Frame Timeline Semaphore Usage

```
Producer                              Encoder Service
========                              ===============

Render frame N
    |
    v
Signal graph semaphore                Wait graph semaphore
  value = (N << 3) | stage            value = (N << 3) | stage
    |                                      |
    v                                      v
Send DMA-BUF FD                       Import DMA-BUF -> VkImage
    |                                      |
    |                                      v
    |                                 SetExternalInputFrame()
    |                                   (with waitSemaphores = [graphSem])
    |                                   (with signalSemaphores = [releaseSem])
    |                                      |
    |                                      v
    |                                 StageInputFrame() / direct inject
    |                                      |
    |                                      v
    |                                 SubmitVideoCodingCmds()
    |                                   GPU: wait graphSem, encode, signal releaseSem
    |                                      |
    |                                      v
Wait release semaphore                Signal release semaphore
  value = N                             value = N
    |                                      |
    v                                      v
Reuse frame slot N                    RequestBitstreamBuffer(N)
                                           |
                                           v
                                      [Poll/wait fence]
                                           |
                                           v
                                      GetBitstreamData() -> write to file/IPC
                                           |
                                           v
                                      Release bitstreamBuffer ref -> back to pool
```

### Command Buffer Semaphore Injection

The wait/signal semaphores from `SetExternalInputFrame()` are injected into the `VkSubmitInfo` for the appropriate command buffer:

**Staging command buffer** (if staging needed):
- Wait: `inputWaitSemaphores` (graph semaphore)
- Signal: internal staging-complete semaphore

**Encode command buffer**:
- Wait: staging-complete semaphore (if staging) OR `inputWaitSemaphores` (if direct inject)
- Signal: `inputSignalSemaphores` (release semaphore) + internal encode fence

This means the existing `SubmitStagedInputFrame()` and `SubmitVideoCodingCmds()` need to accept additional semaphores in their `VkSubmitInfo`.

## Async Bitstream Retrieval Design

### Current (Synchronous)

```cpp
// AssembleBitstreamData() blocks:
encodeCmdBuffer->ResetCommandBuffer(true, "encoderEncodeFence");  // BLOCKS on fence
// ... read bitstream, fwrite() to file ...
```

### New (Asynchronous)

```cpp
// 1. Submit encode (non-blocking)
SubmitVideoCodingCmds();  // Returns immediately

// 2. Request bitstream handle (non-blocking)
VkSharedBaseObj<VulkanBitstreamBuffer> bsBuf;
VkFence fence;
RequestBitstreamBuffer(frameId, bsBuf, fence);  // Returns immediately

// 3. Later: check if ready (non-blocking)
if (PollBitstreamReady(frameId) == VK_SUCCESS) {
    // 4. Read the data
    const uint8_t* data;
    uint32_t size;
    GetBitstreamData(bsBuf, &data, &size);
    
    // 5. Write/send the bitstream
    fwrite(data, 1, size, outputFile);
    // or: send via IPC to parent process
    
    // 6. Release buffer back to pool (ref-count drops)
    bsBuf = nullptr;
}
```

### Thread Pool (Future)

```
Encode threads (existing):        Assembly thread pool (future):
  SubmitVideoCodingCmds()            WaitForFence(encodeFence)
       |                                  |
       v                                  v
  Push to completion queue          GetBitstreamData()
       |                                  |
       |                                  v
       v                            Write to file / send IPC
  Continue encoding                      |
  next frame                              v
                                    Release bitstreamBuffer
```

The completion queue is a thread-safe `std::queue<CompletionEntry>` where:
```cpp
struct CompletionEntry {
    uint64_t frameId;
    VkSharedBaseObj<VkVideoEncodeFrameInfo> encodeFrameInfo;
    VkSharedBaseObj<VulkanBitstreamBuffer> bitstreamBuffer;
    VkFence fence;  // From encodeCmdBuffer
};
```

A pool of N assembly threads (1 per encode queue, typically 1-2) drains this queue, waits on fences, reads bitstream data, and writes output.

## Implementation Plan

### Phase 1: External Frame Input (Internal API)

**Changes to `VkVideoEncoder.h/cpp`:**

1. Add `externalInputImage`, `isExternalInput`, `externalInputTiling` fields to `VkVideoEncodeFrameInfo`
2. Add `inputWaitSemaphores/Values`, `inputSignalSemaphores/Values` fields
3. Implement `SetExternalInputFrame()`:
   - Detect input path (A/B/C) based on format and tiling
   - Path A: set `srcEncodeImageResource` from external image, skip staging
   - Path B: set `srcStagingImageView` from external image
   - Path C: set `srcStagingImageView`, enable filter
4. Modify `StageInputFrame()` to pass through external semaphores to `VkSubmitInfo`
5. Modify `SubmitVideoCodingCmds()` to include signal semaphores in `VkSubmitInfo`

**Changes to `VulkanCommandBufferPool`:**
- Add support for additional wait/signal semaphores in `SubmitCommandBuffer()`

### Phase 2: Async Bitstream Retrieval

**Changes to `VkVideoEncoder.h/cpp`:**

1. Implement `RequestBitstreamBuffer()`: returns the `outputBitstreamBuffer` and the encode fence from `encodeCmdBuffer`
2. Implement `PollBitstreamReady()`: calls `vkGetFenceStatus()` on the encode fence
3. Implement `GetBitstreamData()`: maps the bitstream VkBuffer and returns pointer + size
4. Remove the synchronous `fwrite()` from `AssembleBitstreamData()`
5. Add an async variant: `AssembleBitstreamDataAsync()` that doesn't block

### Phase 3: Public Interface (vulkan_video_encoder_ext.h)

**Update `VulkanVideoEncoderExt`:**

1. `SubmitExternalFrame()` calls `GetAvailablePoolNode()` + `SetExternalInputFrame()` + `EncodeFrameCommon()` internally
2. `GetEncodedFrame()` calls `PollBitstreamReady()` + `GetBitstreamData()` for the oldest pending frame
3. `Flush()` waits on all pending encode fences

### Phase 4: Thread Pool for Bitstream Assembly (Future)

1. Create `BitstreamAssemblyPool` class with configurable thread count
2. `SubmitVideoCodingCmds()` pushes `CompletionEntry` to assembly queue instead of calling `AssembleBitstreamData()`
3. Assembly threads wait on fences, read bitstream, invoke callback
4. Callback writes to file or sends via IPC to encoder service

## Files Modified

| File | Changes |
|------|---------|
| `VkVideoEncoder.h` | Add external input fields to `VkVideoEncodeFrameInfo`, add new methods |
| `VkVideoEncoder.cpp` | Implement `SetExternalInputFrame()`, `RequestBitstreamBuffer()`, modify `StageInputFrame()` and `SubmitVideoCodingCmds()` for semaphore injection |
| `vulkan_video_encoder_ext.h` | Update public interface with refined async design |
| `VulkanVideoImagePool.h` | Add `CreateFromExternalImage()` for wrapping external VkImage in pool node |
| `VulkanCommandBufferPool.h/cpp` | Add semaphore arrays to `SubmitCommandBuffer()` |
| `VulkanBitstreamBufferImpl.h/cpp` | Add `MapData()` / `UnmapData()` for async read |

## Backward Compatibility

All existing code paths remain unchanged:

- `LoadNextFrame()` still works for file-based input
- `StageInputFrame()` detects `isExternalInput` and adjusts behavior
- `AssembleBitstreamData()` remains for synchronous mode
- `EncodeNextFrame()` (public API) continues to call the full file-based pipeline
- The new `SubmitExternalFrame()` / `GetEncodedFrame()` are additive, not replacing

The existing test app (`vulkan-video-enc-test`) and demo apps continue to work unchanged.
