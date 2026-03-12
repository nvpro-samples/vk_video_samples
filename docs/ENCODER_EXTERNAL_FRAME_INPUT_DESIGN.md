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
       |                           SIDE-EFFECTS:
       |                           - Assigns frameInputOrderNum = m_inputFrameNum++
       |                           - Sets lastFrame flag (is this the final frame?)
       |                           - Loads QP map from file if enableQpMap && qpMapFileHandler valid
       |                           - Acquires srcStagingImageView from m_linearInputImagePool
       v
  StageInputFrame()            <-- copies/filters from srcStagingImageView to srcEncodeImageResource
       |                           - If filter enabled: VulkanFilterYuvCompute dispatch
       |                           - If no filter: vkCmdCopyImage (linear -> optimal)
       |                           SIDE-EFFECTS:
       |                           - Acquires srcEncodeImageResource from m_inputImagePool
       |                           - Acquires inputCmdBuffer from m_inputCommandBufferPool
       |                           - Resets inputCmdBuffer (waits for its fence if in-flight)
       |                           - Records image layout transitions
       |                           - If filter: handles row/col replication padding
       |                           - If filter + AQ: acquires subsampledImageResource from pool
       |                           - If enableQpMap + optimal tiling: stages QP map in same cmd buf
       |                           - Ends command buffer recording
       |                           - Calls SubmitStagedInputFrame() internally
       |                           - Then calls EncodeFrameCommon() internally!
       v
  SubmitStagedInputFrame()     <-- submits staging command buffer to transfer/encode queue
       |                           SIDE-EFFECTS:
       |                           - Signals inputCmdBuffer's binary semaphore
       |                           - Signals inputCmdBuffer's fence (for CPU sync)
       |                           - Marks inputCmdBuffer as submitted
       |                           NOTE: Does NOT wait - non-blocking submit
       v
  EncodeFrameCommon()          <-- GOP position, DPB management, AQ, queue frame
       |                           SIDE-EFFECTS:
       |                           - Sets constQp from config
       |                           - Asserts srcEncodeImageResource is set
       |                           - Sets videoSession, videoSessionParameters refs
       |                           - Sets qualityLevel from config
       |                           - Assigns frameEncodeInputOrderNum = m_encodeInputFrameNum++
       |                           - Calls gopStructure.GetPositionInGOP() -> gopPosition
       |                           - Calls EncodeFrame() (codec-specific: fills DPB slots, refs)
       |                           - Calls HandleCtrlCmd() + CodecHandleRateControlCmd()
       |                           - If enableQpMap: ProcessQpMap()
       |                           - If enableIntraRefresh: FillIntraRefreshInfo()
       |                           - Acquires outputBitstreamBuffer from pool
       |                           - Sets encodeInfo.dstBuffer = outputBitstreamBuffer
       |                           - If AQ: acquires srcQpMapImageResource, runs ProcessAq()
       |                             with wait semaphore from inputCmdBuffer
       |                           - Calls EnqueueFrame() (B-frame reorder, thread queue)
       v
  RecordVideoCodingCmd()       <-- records vkCmdEncodeVideoKHR into encodeCmdBuffer
       |                           (called from EnqueueFrame or consumer thread)
       v
  SubmitVideoCodingCmds()      <-- submits to encode queue
       |                           SIDE-EFFECTS:
       |                           - Wait: inputCmdBuffer's semaphore (staging complete)
       |                           - Wait: AQ semaphore (if AQ enabled)
       |                           - Signal: encodeCmdBuffer's semaphore + fence
       v
  AssembleBitstreamData()      <-- SYNCHRONOUS: blocks on encodeCmdBuffer fence
       |                           - Resets inputCmdBuffer (waits fence: staging)
       |                           - Resets qpMapCmdBuffer (waits fence: qp map)
       |                           - Resets encodeCmdBuffer (waits fence: BLOCKS HERE)
       |                           - Writes non-VCL header data via fwrite()
       |                           - Reads bitstream size from query pool
       |                           - Maps outputBitstreamBuffer, reads data, fwrite()
       |                           - Returns outputBitstreamBuffer to pool
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

## Common Operations the New Interface Must Handle

The stages above embed common operations that are **NOT** related to file I/O or staging. These must still be performed in the new external frame interface regardless of which input path is taken:

### From LoadNextFrame() — must replicate:

| Operation | What | When |
|-----------|------|------|
| `frameInputOrderNum = m_inputFrameNum++` | Assigns monotonic input sequence number | Always |
| `lastFrame` flag | Marks the final frame (for flush/EOS) | Always |
| QP map loading | `LoadNextQpMapFrameFromFile()` | Only if QP map from file; external path provides QP map via IPC instead |
| Pool acquisition: `srcStagingImageView` | Gets linear image from `m_linearInputImagePool` | Only for Paths B/C (external linear input) |

### From StageInputFrame() — must replicate:

| Operation | What | When |
|-----------|------|------|
| Pool acquisition: `srcEncodeImageResource` | Gets optimal image from `m_inputImagePool` | Paths B/C (need internal optimal); Path A skips this |
| Pool acquisition: `inputCmdBuffer` | Gets command buffer from `m_inputCommandBufferPool` | Paths B/C; Path A needs a different approach (no staging cmd) |
| Command buffer reset + begin | Waits fence, resets, begins recording | Paths B/C |
| Image layout transitions | `TransitionImageLayout()` for src and dst | Paths B/C |
| Row/column replication padding | Via filter `enablePictureRowColReplication` | Only if filter + config enables it |
| AQ subsampled image acquisition | `m_inputSubsampledImagePool->GetAvailableImage()` | Only if AQ enabled |
| QP map staging | `StageInputFrameQpMap()` in same cmd buffer | Only if QP map + optimal tiling |
| Call `SubmitStagedInputFrame()` | Submits staging to queue | Paths B/C |
| **Call `EncodeFrameCommon()`** | This is called at the end of `StageInputFrame()`! | Always |

**Critical:** `StageInputFrame()` calls `EncodeFrameCommon()` at line 519 — it's not just staging, it also triggers the entire encode pipeline. The new interface must replicate this call sequence.

### From EncodeFrameCommon() — must replicate:

| Operation | What | When |
|-----------|------|------|
| `constQp` assignment | From config | Always |
| `videoSession/Parameters` refs | Set from encoder's session | Always |
| `qualityLevel` assignment | From config | Always |
| `frameEncodeInputOrderNum = m_encodeInputFrameNum++` | Encode-order sequence number | Always |
| `gopStructure.GetPositionInGOP()` | I/P/B frame type decision | Always (but external can override with forceIDR) |
| `EncodeFrame()` (codec-specific) | Fills DPB slots, reference management | Always |
| `HandleCtrlCmd()` + `CodecHandleRateControlCmd()` | Rate control commands | Always |
| `ProcessQpMap()` | Process QP map data | If enableQpMap |
| `FillIntraRefreshInfo()` | Intra-refresh | If enableIntraRefresh |
| `GetBitstreamBuffer()` | Acquire output bitstream VkBuffer | Always |
| AQ processing | `FindFreeAqProcessorSlot()` + `ProcessAq()` | If AQ enabled |
| `EnqueueFrame()` | B-frame reorder + submit to thread queue | Always |

### From SubmitStagedInputFrame() — must replicate:

| Operation | What | When |
|-----------|------|------|
| Binary semaphore signal | `inputCmdBuffer->GetSemaphore()` | Paths B/C |
| Timeline semaphore signal (if added) | For external frame release | All paths (new) |
| Fence signal | `inputCmdBuffer->GetFence()` | Paths B/C |
| Queue submit | `MultiThreadedQueueSubmit()` to encode/transfer queue | Paths B/C |
| **External wait semaphores** | Must be injected into `VkSubmitInfo2KHR` | All paths (new) |
| **External signal semaphores** | Must be injected into `VkSubmitInfo2KHR` | All paths (new) |

### What the New Interface Must Replicate

`EncodeFrameCommon()` is **not a problem** — it's the common tail that both file-based and external paths converge into unchanged. The problem is the side-effects embedded in `LoadNextFrame()` and `StageInputFrame()` that happen *before* `EncodeFrameCommon()` runs.

The new `SetExternalInputFrame()` must replicate these side-effects, then hand off to the existing `StageInputFrame()` (Paths B/C) or `EncodeFrameCommon()` (Path A).

### Side-effects from LoadNextFrame() that SetExternalInputFrame must replicate:

```cpp
// These are mandatory bookkeeping that LoadNextFrame() performs.
// Without them, EncodeFrameCommon() will malfunction.

encodeFrameInfo->frameInputOrderNum = m_inputFrameNum++;   // CRITICAL: GOP uses this
encodeFrameInfo->lastFrame = isLastFrame;                   // CRITICAL: EOS signaling

// QP map: LoadNextFrame calls LoadNextQpMapFrameFromFile().
// For external path: QP map comes from IPC, not file.
// If external QP map provided:
//   - Acquire srcQpMapStagingResource from m_linearQpMapImagePool
//   - Copy QP map data into the linear staging image
//   - (StageInputFrame will handle the staging copy to optimal)
// If no external QP map: skip (same as file path with no QP map file)

// Pool acquisition: LoadNextFrame acquires srcStagingImageView.
// For external path:
//   - Path A (optimal direct): NOT needed (no staging)
//   - Path B/C (linear or RGBA): srcStagingImageView must be set
//     to a wrapper around the external image, NOT acquired from pool.
//     The external image IS the staging source.
```

### Side-effects from StageInputFrame() that SetExternalInputFrame must handle:

```cpp
// StageInputFrame does these things BEFORE calling EncodeFrameCommon():

// 1. Acquire srcEncodeImageResource from m_inputImagePool
//    - Path A: NOT needed — external optimal image IS the encode input
//    - Path B/C: needed — this is the internal optimal image the encoder reads
//    StageInputFrame already does this (line 404-413), so for B/C
//    we can just call StageInputFrame() with our external staging image set.

// 2. Acquire inputCmdBuffer from m_inputCommandBufferPool
//    - Path A: NOT needed — no staging command buffer
//    - Path B/C: needed — StageInputFrame already does this (line 415)

// 3. External wait semaphores injection
//    - Path B/C: must be added to SubmitStagedInputFrame()'s VkSubmitInfo2KHR
//      as additional wait semaphores (graph semaphore from producer)
//    - Path A: must be added to SubmitVideoCodingCmds()'s VkSubmitInfo2KHR
//      since there's no staging submit

// 4. External signal semaphores injection
//    - All paths: must be signaled when the encoder no longer needs the
//      external image. For Path B/C this is after the staging copy.
//      For Path A this is after the encode uses the image (srcEncodeImageResource
//      is consumed by vkCmdEncodeVideoKHR).
//    - Path B/C: add to SubmitStagedInputFrame() signal semaphores
//    - Path A: add to SubmitVideoCodingCmds() signal semaphores
//      BUT: the encode may reference the image across multiple frames (DPB).
//      For external input that is NOT a reference frame, signal immediately.
//      For external input used as reference: signal when DPB slot is released.
//      SIMPLIFICATION: for now, always copy external to internal encode image
//      (even Path A) so the external image can be released immediately after copy.
```

### Summary: SetExternalInputFrame Implementation

```cpp
VkResult SetExternalInputFrame(...) {

    // =============================================
    // 1. Replicate LoadNextFrame() bookkeeping
    // =============================================
    encodeFrameInfo->frameInputOrderNum = m_inputFrameNum++;
    encodeFrameInfo->lastFrame = isLastFrame;
    encodeFrameInfo->inputTimeStamp = pts;

    // QP map from external source (not file)
    if (hasExternalQpMap) {
        acquireAndFillQpMapStaging(encodeFrameInfo, qpMapData, qpMapWidth, qpMapHeight);
    }

    // =============================================
    // 2. Store external sync info (new fields)
    // =============================================
    encodeFrameInfo->inputWaitSemaphores = {waitSemaphores...};
    encodeFrameInfo->inputWaitSemaphoreValues = {waitValues...};
    encodeFrameInfo->inputSignalSemaphores = {signalSemaphores...};
    encodeFrameInfo->inputSignalSemaphoreValues = {signalValues...};
    encodeFrameInfo->isExternalInput = true;

    // =============================================
    // 3. Path selection and image injection
    // =============================================
    bool needsStaging = false;
    bool needsFilter = false;

    if (isDirectlyEncodable(format) && tiling == VK_IMAGE_TILING_OPTIMAL) {
        // Path A: set srcStagingImageView to external image.
        // StageInputFrame will copy it to srcEncodeImageResource (internal pool).
        // This ensures the external image is released after the copy,
        // not held for the duration of DPB reference lifetime.
        encodeFrameInfo->srcStagingImageView = wrapExternalAsPoolNode(image);
        needsStaging = true;
        needsFilter = false;
        // NOTE: We still go through StageInputFrame for the copy,
        // but the filter is disabled (format is already encodable).
    } else if (isYCbCrFormat(format) && tiling == VK_IMAGE_TILING_LINEAR) {
        // Path B: linear YCbCr -> upload to optimal
        encodeFrameInfo->srcStagingImageView = wrapExternalAsPoolNode(image);
        needsStaging = true;
        needsFilter = false;
    } else {
        // Path C: RGBA or unsupported -> needs filter
        encodeFrameInfo->srcStagingImageView = wrapExternalAsPoolNode(image);
        needsStaging = true;
        needsFilter = true;
        // Ensure m_inputComputeFilter is initialized for RGBA->YCbCr
    }

    // =============================================
    // 4. Call StageInputFrame() which chains into EncodeFrameCommon()
    // =============================================
    // StageInputFrame() will:
    //   - Acquire srcEncodeImageResource from pool (internal optimal image)
    //   - Acquire inputCmdBuffer, record copy/filter, submit
    //   - Inject our external wait/signal semaphores into the submit
    //   - Call EncodeFrameCommon() at the end
    //
    // The only modification needed to StageInputFrame() is:
    //   - In SubmitStagedInputFrame(): add encodeFrameInfo->inputWaitSemaphores
    //     as additional wait semaphores in VkSubmitInfo2KHR
    //   - In SubmitStagedInputFrame(): add encodeFrameInfo->inputSignalSemaphores
    //     as additional signal semaphores in VkSubmitInfo2KHR
    //
    // This means ALL paths go through StageInputFrame(). Path A just does a
    // format-matching copy (optimal->optimal of the same format), Path B does
    // linear->optimal copy, Path C runs the filter.

    return StageInputFrame(encodeFrameInfo);
}
```

### Key Insight: Path A (Direct Optimal YCbCr) Skips StageInputFrame Entirely

For Phase 1 (direct optimal YCbCr input), the external image goes directly to
`srcEncodeImageResource` and we call `EncodeFrameCommon()` without any staging.
This is correct because:

1. **Input and DPB are separate**: The encoder's DPB (reconstructed reference
   frames) is managed internally in `dpbImageResources[]` / `setupImageResource`.
   These are separate allocations. The input image (`srcEncodeImageResource`) is
   only read once by `vkCmdEncodeVideoKHR` as the source picture, then the HW
   encoder reconstructs the reference into its own DPB slot. The input is free
   to reuse after `vkCmdEncodeVideoKHR` completes.

2. **Zero-copy**: No staging copy, no filter. The imported DMA-BUF VkImage goes
   directly to the encode HW.

3. **Semaphore injection**: Wait semaphores (graph) are injected into
   `SubmitVideoCodingCmds()` for the encode submit. The encode command buffer's
   binary semaphore signals when the encode HW is done reading the input.

Paths B and C (linear or RGBA) still go through `StageInputFrame()` because
they need a copy or filter. But Phase 1 only uses Path A.

### Synchronization for Path A (Direct Encode + Display)

```
Encoder (encode queue):
  wait:   graphSemaphore (value = graphWaitValue)  ← renderer done
  cmd:    vkCmdEncodeVideoKHR (reads imported image as source picture)
  signal: encodeInputDoneSem (binary)               ← input image free

Display (graphics queue, chained after encode):
  wait:   encodeInputDoneSem                        ← encoder released input
  wait:   imageAvailableSem                         ← swapchain
  cmd:    blit imported image → swapchain
  signal: releaseSemaphore (value = frameId)        ← consumer done
  signal: renderFinishedSem                         ← for present
```

The release semaphore fires from the display submit — after BOTH the encode
and display are done reading the imported image. The `encodeInputDoneSem` is
the binary semaphore from `SubmitVideoCodingCmds()` (the encode command
buffer's semaphore), returned to the encoder service via
`SubmitExternalFrame(pStagingCompleteSemaphore)`.

For Paths B/C that go through `StageInputFrame()`, the semaphore injection
is in `SubmitStagedInputFrame()`:

```cpp
VkResult VkVideoEncoder::SubmitStagedInputFrame(...) {
    // ... existing code ...

    // NEW: Add external wait semaphores from the frame info
    for (auto& sem : encodeFrameInfo->inputWaitSemaphores) {
        waitSemaphoreInfos[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        waitSemaphoreInfos[waitCount].semaphore = sem;
        waitSemaphoreInfos[waitCount].value = encodeFrameInfo->inputWaitSemaphoreValues[i];
        waitSemaphoreInfos[waitCount].stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        waitCount++;
    }

    // NEW: Add external signal semaphores
    for (auto& sem : encodeFrameInfo->inputSignalSemaphores) {
        signalSemaphoreInfos[signalCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        signalSemaphoreInfos[signalCount].semaphore = sem;
        signalSemaphoreInfos[signalCount].value = encodeFrameInfo->inputSignalSemaphoreValues[i];
        signalSemaphoreInfos[signalCount].stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        signalCount++;
    }

    // ... rest of existing submit code ...
}
```

### Helper: wrapExternalAsPoolNode()

Need a utility to wrap an external `VkImage` + `VkDeviceMemory` in a `VulkanVideoImagePoolNode` so it can be set as `srcStagingImageView`. This wrapper:
- Creates a `VkImageResource` wrapping the external `VkImage`/`VkDeviceMemory` **without** owning them (no destroy on release)
- Creates a `VkImageResourceView` for the image
- Wraps in a `VulkanVideoImagePoolNode` with ref-counting
- The ref-count keeps the wrapper alive until `StageInputFrame()` completes the copy, then drops naturally

```cpp
VkSharedBaseObj<VulkanVideoImagePoolNode> VkVideoEncoder::WrapExternalImage(
    VkImage image, VkDeviceMemory memory, VkImageView view,
    VkFormat format, uint32_t width, uint32_t height,
    VkImageTiling tiling)
{
    // Create non-owning VkImageResource (doesn't destroy the VkImage on release)
    VkSharedBaseObj<VkImageResource> imageResource;
    VkImageResource::CreateFromExternalImage(m_vkDevCtx, image, memory,
                                              format, {width, height, 1},
                                              tiling, false /*ownsImage*/,
                                              imageResource);

    // Create view (may already have one from the caller)
    VkSharedBaseObj<VkImageResourceView> imageView;
    if (view != VK_NULL_HANDLE) {
        VkImageResourceView::CreateFromExternalView(m_vkDevCtx, view, imageResource,
                                                     false /*ownsView*/, imageView);
    } else {
        VkImageResourceView::Create(m_vkDevCtx, imageResource, imageView);
    }

    // Wrap in pool node (non-owning: doesn't return to any pool on release)
    VkSharedBaseObj<VulkanVideoImagePoolNode> node;
    VulkanVideoImagePoolNode::CreateExternal(m_vkDevCtx, imageResource, imageView, node);
    return node;
}
```

**NOTE**: `VkImageResource::CreateFromExternalImage()` and `VulkanVideoImagePoolNode::CreateExternal()` are new methods that need to be added to the VkCodecUtils infrastructure. They create non-owning wrappers around externally-managed resources.

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
