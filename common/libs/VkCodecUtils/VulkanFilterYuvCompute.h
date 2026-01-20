/*
* Copyright 2023 NVIDIA Corporation.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/


#ifndef _VULKANFILTERYUVCOMPUTE_H_
#define _VULKANFILTERYUVCOMPUTE_H_

#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanCommandBuffersSet.h"
#include "VkCodecUtils/VulkanSemaphoreSet.h"
#include "VkCodecUtils/VulkanFenceSet.h"
#include "VkCodecUtils/VulkanDescriptorSetLayout.h"
#include "VkCodecUtils/VulkanComputePipeline.h"
#include "VkCodecUtils/VulkanFilter.h"
#include "VkCodecUtils/VkImageResource.h"
#include "VkCodecUtils/VkBufferResource.h"
#include "nvidia_utils/vulkan/ycbcr_utils.h"
#include <array>

// =============================================================================
// Transfer Operation Types
// =============================================================================

/**
 * @brief Types of transfer operations for pre/post processing
 *
 * These transfer operations use Vulkan's built-in copy commands (vkCmdCopy*)
 * which are efficient but have strict requirements:
 *
 * **IMPORTANT: Transfer operations require matching formats and compatible layouts**
 *
 * Unlike compute-based operations (YCBCRCOPY, RGBA2YCBCR, etc.) which can perform
 * format and bit-depth conversions in the shader, transfer operations are essentially
 * memory copies and therefore:
 *
 * - Source and destination formats MUST be compatible (same format or compatible block size)
 * - Source and destination must have matching plane counts for multi-planar formats
 * - Buffer geometry (width, height, row pitch) must match the image dimensions
 * - For COPY_IMAGE_TO_IMAGE, both images must use compatible formats
 *
 * Transfer operations copy data plane-by-plane for multi-planar YCbCr formats:
 * - Y plane (PLANE_0_BIT) is copied with full resolution
 * - CbCr/Cb plane (PLANE_1_BIT) is copied at subsampled resolution
 * - Cr plane (PLANE_2_BIT, for 3-plane formats) is copied at subsampled resolution
 *
 * Use compute-based operations (YCBCRCOPY, YCBCR2RGBA, RGBA2YCBCR) when you need:
 * - Format conversion (e.g., NV12 to P010, 8-bit to 10-bit)
 * - Bit depth conversion with proper range scaling
 * - Layout changes that require data transformation
 * - Color space conversion (e.g., BT.601 to BT.709)
 *
 * @see YCBCRCOPY for compute-based copy with format/layout conversion support
 */
enum class TransferOpType : uint32_t {
    NONE = 0,                    ///< No transfer operation
    COPY_IMAGE_TO_BUFFER,        ///< vkCmdCopyImageToBuffer - copies image planes to buffer
    COPY_BUFFER_TO_IMAGE,        ///< vkCmdCopyBufferToImage - copies buffer data to image planes
    COPY_IMAGE_TO_IMAGE,         ///< vkCmdCopyImage - copies between images (tiling conversion)
};

/**
 * @brief Per-plane buffer geometry for multi-planar formats
 *
 * When using buffers to store multi-planar image data (NV12, P010, etc.),
 * each plane has its own offset, size, and row pitch within the buffer.
 */
struct PlaneBufferGeometry {
    VkDeviceSize        offset{0};       // Byte offset to plane start in buffer
    VkDeviceSize        size{0};         // Size of plane in bytes
    VkDeviceSize        rowPitch{0};     // Bytes per row (0 = tightly packed = width * bytesPerPixel)
    uint32_t            width{0};        // Width in pixels (for this plane, accounts for subsampling)
    uint32_t            height{0};       // Height in pixels (for this plane, accounts for subsampling)
    uint32_t            bytesPerPixel{1}; // Bytes per pixel for this plane format
};

/**
 * @brief Describes a resource (image or buffer) for transfer operations
 *
 * Supports both images (with VkSubresourceLayout for linear) and buffers
 * (with explicit per-plane geometry). Use factory methods to construct
 * from VkImageResource or VkBufferResource for automatic geometry extraction.
 */
struct TransferResource {
    enum class Type : uint8_t { 
        IMAGE = 0, 
        BUFFER = 1 
    };
    
    static constexpr uint32_t kMaxPlanes = 3;
    
    Type                type{Type::IMAGE};
    
    // For images
    VkImage             image{VK_NULL_HANDLE};
    VkImageLayout       currentLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageLayout       targetLayout{VK_IMAGE_LAYOUT_GENERAL};
    VkImageTiling       tiling{VK_IMAGE_TILING_OPTIMAL};
    VkFormat            format{VK_FORMAT_UNDEFINED};
    VkExtent3D          extent{0, 0, 1};       // Full image extent (not per-plane)
    uint32_t            arrayLayer{0};
    uint32_t            mipLevel{0};
    VkImageAspectFlags  aspectMask{VK_IMAGE_ASPECT_COLOR_BIT};
    
    // Per-plane layout for linear images (from vkGetImageSubresourceLayout)
    VkSubresourceLayout imageLayouts[kMaxPlanes]{};
    
    // For buffers - total buffer info
    VkBuffer            buffer{VK_NULL_HANDLE};
    VkDeviceSize        bufferTotalSize{0};    // Total buffer size
    
    // Per-plane geometry for buffers (explicit, must be set by caller)
    uint32_t            numPlanes{1};
    PlaneBufferGeometry planeGeometry[kMaxPlanes]{};
    
    // Helpers
    bool isImage() const { return type == Type::IMAGE; }
    bool isBuffer() const { return type == Type::BUFFER; }
    bool isLinear() const { return type == Type::IMAGE && tiling == VK_IMAGE_TILING_LINEAR; }
    bool isOptimal() const { return type == Type::IMAGE && tiling == VK_IMAGE_TILING_OPTIMAL; }
    bool isValid() const { 
        return (type == Type::IMAGE && image != VK_NULL_HANDLE) ||
               (type == Type::BUFFER && buffer != VK_NULL_HANDLE);
    }
    
    // Get plane offset for copy operations (works for both image and buffer)
    VkDeviceSize getPlaneOffset(uint32_t plane) const {
        if (type == Type::BUFFER) {
            return (plane < numPlanes) ? planeGeometry[plane].offset : 0;
        } else if (tiling == VK_IMAGE_TILING_LINEAR) {
            return (plane < kMaxPlanes) ? imageLayouts[plane].offset : 0;
        }
        return 0;  // Optimal tiling - offset is 0 (use VkImageSubresource)
    }
    
    // Get row pitch for copy operations (0 = tightly packed)
    VkDeviceSize getPlaneRowPitch(uint32_t plane) const {
        if (type == Type::BUFFER) {
            return (plane < numPlanes) ? planeGeometry[plane].rowPitch : 0;
        } else if (tiling == VK_IMAGE_TILING_LINEAR) {
            return (plane < kMaxPlanes) ? imageLayouts[plane].rowPitch : 0;
        }
        return 0;  // Optimal tiling - let driver compute
    }
    
    // Get plane width (accounting for subsampling)
    uint32_t getPlaneWidth(uint32_t plane) const {
        if (type == Type::BUFFER && plane < numPlanes) {
            return planeGeometry[plane].width;
        }
        // For images, need format info to compute subsampled dimensions
        return extent.width;  // Caller should use format utils for proper subsampling
    }
    
    // Get plane height (accounting for subsampling)
    uint32_t getPlaneHeight(uint32_t plane) const {
        if (type == Type::BUFFER && plane < numPlanes) {
            return planeGeometry[plane].height;
        }
        return extent.height;  // Caller should use format utils
    }
    
    // ==========================================================================
    // Static factory methods
    // ==========================================================================
    
    /**
     * @brief Create from raw VkImage with explicit parameters
     */
    static TransferResource fromImage(VkImage img, VkFormat fmt, VkExtent3D ext,
                                      VkImageTiling til = VK_IMAGE_TILING_OPTIMAL,
                                      VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL,
                                      uint32_t layer = 0) {
        TransferResource r{};
        r.type = Type::IMAGE;
        r.image = img;
        r.format = fmt;
        r.extent = ext;
        r.tiling = til;
        r.currentLayout = layout;
        r.targetLayout = layout;
        r.arrayLayer = layer;
        return r;
    }
    
    /**
     * @brief Create from VkImageResource (extracts format, extent, tiling, layouts)
     * @param imgRes     Image resource
     * @param layout     Current/target layout for the image
     * @param layer      Array layer to use
     */
    static TransferResource fromImageResource(const VkImageResource& imgRes,
                                              VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL,
                                              uint32_t layer = 0) {
        TransferResource r{};
        r.type = Type::IMAGE;
        r.image = imgRes.GetImage();
        
        const VkImageCreateInfo& info = imgRes.GetImageCreateInfo();
        r.format = info.format;
        r.extent = info.extent;
        r.tiling = info.tiling;
        r.arrayLayer = layer;
        r.currentLayout = layout;
        r.targetLayout = layout;
        
        // Copy per-plane layouts for linear images
        const VkSubresourceLayout* layouts = imgRes.GetSubresourceLayout();
        if (layouts) {
            for (uint32_t i = 0; i < kMaxPlanes; ++i) {
                r.imageLayouts[i] = layouts[i];
            }
        }
        
        return r;
    }
    
    /**
     * @brief Create from raw VkBuffer with explicit per-plane geometry
     * @param buf        Buffer handle
     * @param totalSize  Total buffer size
     * @param planes     Number of planes
     * @param geometry   Per-plane geometry array (must have 'planes' elements)
     */
    static TransferResource fromBuffer(VkBuffer buf, VkDeviceSize totalSize,
                                       uint32_t planes, const PlaneBufferGeometry* geometry) {
        TransferResource r{};
        r.type = Type::BUFFER;
        r.buffer = buf;
        r.bufferTotalSize = totalSize;
        r.numPlanes = planes;
        for (uint32_t i = 0; i < planes && i < kMaxPlanes; ++i) {
            r.planeGeometry[i] = geometry[i];
        }
        return r;
    }
    
    /**
     * @brief Create from VkBufferResource with explicit format and geometry
     *
     * Unlike images, buffers don't inherently know their image geometry.
     * Caller must specify format and per-plane layout.
     *
     * @param bufRes     Buffer resource
     * @param fmt        Format of image data in buffer
     * @param width      Image width in pixels
     * @param height     Image height in pixels
     * @param planes     Number of planes
     * @param planeOffsets   Per-plane byte offsets (or nullptr for auto-calculate)
     * @param planeRowPitches Per-plane row pitches (or nullptr for tightly packed)
     */
    static TransferResource fromBufferResource(const VkBufferResource& bufRes,
                                               VkFormat fmt,
                                               uint32_t width, uint32_t height,
                                               uint32_t planes = 1,
                                               const VkDeviceSize* planeOffsets = nullptr,
                                               const VkDeviceSize* planeRowPitches = nullptr);
    
    /**
     * @brief Create simple single-plane buffer (RGBA, grayscale, etc.)
     */
    static TransferResource fromBufferSimple(VkBuffer buf, VkDeviceSize totalSize,
                                             uint32_t width, uint32_t height,
                                             uint32_t bytesPerPixel,
                                             VkDeviceSize rowPitch = 0) {
        TransferResource r{};
        r.type = Type::BUFFER;
        r.buffer = buf;
        r.bufferTotalSize = totalSize;
        r.numPlanes = 1;
        r.extent = {width, height, 1};
        
        r.planeGeometry[0].offset = 0;
        r.planeGeometry[0].width = width;
        r.planeGeometry[0].height = height;
        r.planeGeometry[0].bytesPerPixel = bytesPerPixel;
        r.planeGeometry[0].rowPitch = (rowPitch > 0) ? rowPitch : (width * bytesPerPixel);
        r.planeGeometry[0].size = r.planeGeometry[0].rowPitch * height;
        
        return r;
    }
};

/**
 * @brief Describes a single transfer operation
 */
struct TransferOp {
    TransferOpType      opType{TransferOpType::NONE};
    TransferResource    src;
    TransferResource    dst;
    
    // For multi-plane images, specify per-plane regions (max 3 planes)
    uint32_t            numRegions{0};
    VkBufferImageCopy   bufferImageCopy[3];
    VkImageCopy         imageCopy[3];
    
    bool isValid() const { return opType != TransferOpType::NONE; }
};

/**
 * @brief Extended I/O slot with optional pre/post transfer operations
 * 
 * Describes a primary resource used by the compute shader, with optional
 * pre-transfer (data staged into primary before compute) and post-transfer
 * (data copied out from primary after compute).
 *
 * **Example use cases:**
 *
 * 1. **Input from linear image (for CPU access):**
 *    - preTransferSource: Linear image with host-visible memory
 *    - primary: Optimal image for compute shader
 *    - preTransferOp: COPY_IMAGE_TO_IMAGE (linear → optimal)
 *
 * 2. **Output to buffer (for file dumping):**
 *    - primary: Optimal image from compute shader
 *    - postTransferDest: Host-visible buffer
 *    - postTransferOp: COPY_IMAGE_TO_BUFFER
 *
 * 3. **Input from buffer (raw YUV file):**
 *    - preTransferSource: Buffer with loaded YUV data
 *    - primary: Image for compute shader
 *    - preTransferOp: COPY_BUFFER_TO_IMAGE
 *
 * **IMPORTANT:** Pre/post transfer operations are raw memory copies.
 * For format conversion, use the compute shader (YCBCRCOPY, RGBA2YCBCR, etc.)
 * by setting different input/output formats in the filter creation.
 */
struct FilterIOSlot {
    /// Primary resource used by the compute shader (must be an image for compute)
    TransferResource    primary;
    
    /// Optional pre-transfer: source data to stage into primary before compute
    /// The operation copies: preTransferSource → primary
    TransferResource    preTransferSource;
    TransferOpType      preTransferOp{TransferOpType::NONE};
    
    /// Optional post-transfer: destination for results after compute
    /// The operation copies: primary → postTransferDest
    TransferResource    postTransferDest;
    TransferOpType      postTransferOp{TransferOpType::NONE};
    
    /// @return true if a pre-transfer operation is configured
    bool hasPreTransfer() const { return preTransferOp != TransferOpType::NONE; }
    
    /// @return true if a post-transfer operation is configured
    bool hasPostTransfer() const { return postTransferOp != TransferOpType::NONE; }
    
    /// @return true if the primary resource is valid
    bool isValid() const { return primary.isValid(); }
};

/**
 * @brief Maximum number of input/output slots
 */
static constexpr uint32_t kMaxFilterIOSlots = 4;

/**
 * @brief Complete filter execution descriptor
 * 
 * Describes all inputs, outputs, transfer operations, and synchronization
 * for a single filter invocation. This is the primary configuration for
 * the unified RecordCommandBuffer() API.
 *
 * **Typical configurations:**
 *
 * 1. **Simple compute (no transfers):**
 *    - inputs[0].primary = input image
 *    - outputs[0].primary = output image
 *    - No pre/post transfers
 *
 * 2. **Compute with output to buffer (file dump):**
 *    - inputs[0].primary = input image
 *    - outputs[0].primary = optimal output image (for display/encoder)
 *    - outputs[0].postTransferDest = host-visible buffer
 *    - outputs[0].postTransferOp = COPY_IMAGE_TO_BUFFER
 *
 * 3. **Compute with dual output (display + encoder):**
 *    - inputs[0].primary = input image
 *    - outputs[0].primary = display image (optimal, displayable format)
 *    - outputs[1].primary = encoder image (optimal, encoder-compatible format)
 *
 * 4. **Input from linear image:**
 *    - inputs[0].preTransferSource = linear image (host-written)
 *    - inputs[0].primary = optimal image (for compute)
 *    - inputs[0].preTransferOp = COPY_IMAGE_TO_IMAGE
 *
 * **Multi-planar YCbCr:** All transfer operations automatically handle
 * multi-planar formats (NV12, P010, I420, etc.) by copying plane-by-plane.
 */
struct FilterExecutionDesc {
    /// Input slots (typically 1, but supports multiple for multi-input filters)
    std::array<FilterIOSlot, kMaxFilterIOSlots> inputs;
    uint32_t numInputs{1};
    
    /// Output slots (supports dual/multi output for encoder, display, dump, etc.)
    std::array<FilterIOSlot, kMaxFilterIOSlots> outputs;
    uint32_t numOutputs{1};
    
    /// Source image layer (for array images)
    uint32_t srcLayer{0};
    
    /// Destination image layer (for array images)
    uint32_t dstLayer{0};
    
    // =========================================================================
    // Optional external synchronization (timeline semaphores)
    // =========================================================================
    
    /// Semaphore to wait on before starting operations
    VkSemaphore         waitSemaphore{VK_NULL_HANDLE};
    
    /// Timeline value to wait for (0 for binary semaphores)
    uint64_t            waitValue{0};
    
    /// Pipeline stage at which to wait
    VkPipelineStageFlags waitDstStageMask{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    
    /// Semaphore to signal after all operations complete
    VkSemaphore         signalSemaphore{VK_NULL_HANDLE};
    
    /// Timeline value to signal (0 for binary semaphores)
    uint64_t            signalValue{0};
    
    // =========================================================================
    // Helper methods
    // =========================================================================
    
    /// @return true if any input slot has a pre-transfer operation configured
    bool hasAnyPreTransfer() const {
        for (uint32_t i = 0; i < numInputs; ++i) {
            if (inputs[i].hasPreTransfer()) return true;
        }
        return false;
    }
    
    /// @return true if any output slot has a post-transfer operation configured
    bool hasAnyPostTransfer() const {
        for (uint32_t i = 0; i < numOutputs; ++i) {
            if (outputs[i].hasPostTransfer()) return true;
        }
        return false;
    }
};

// =============================================================================
// VulkanFilterYuvCompute Class
// =============================================================================

class VulkanFilterYuvCompute : public VulkanFilter
{
public:

    /**
     * @brief Filter operation types
     *
     * The filter supports two categories of operations:
     *
     * ## Compute-based Operations (YCBCRCOPY, YCBCRCLEAR, YCBCR2RGBA, RGBA2YCBCR)
     *
     * These operations use a compute shader for processing and can handle:
     * - **Different input/output formats**: e.g., NV12 input → P010 output
     * - **Different bit depths**: e.g., 8-bit → 10-bit with proper range scaling
     * - **Different tiling modes**: linear ↔ optimal (via shader read/write)
     * - **Different layouts**: 2-plane (NV12) ↔ 3-plane (I420)
     * - **Chroma subsampling changes**: 4:4:4 → 4:2:0, 4:2:2 → 4:2:0, etc.
     * - **Color space conversions**: BT.601 ↔ BT.709 ↔ BT.2020
     * - **Range conversions**: limited range ↔ full range
     *
     * The compute shader normalizes input data, processes it, and denormalizes
     * to the output format, ensuring correct color representation across formats.
     *
     * ## Transfer-only Operations (XFER_*)
     *
     * These operations use Vulkan's vkCmdCopy* commands for efficient memory copies.
     * They are faster but have strict requirements:
     * - **Source and destination formats MUST match** (or be compatible)
     * - **Source and destination layouts MUST be compatible**
     * - No format conversion, bit depth conversion, or color space conversion
     * - Useful for: tiling mode changes (linear ↔ optimal), GPU-to-host readback
     *
     * For multi-planar YCbCr formats, transfer operations copy plane-by-plane,
     * with each plane handled separately using the appropriate aspect mask.
     */
    enum FilterType { 
        // =====================================================================
        // Compute-based operations (use compute shader, support format conversion)
        // =====================================================================
        
        /**
         * @brief Copy YCbCr data with optional format/layout conversion
         *
         * Uses a compute shader to copy YCbCr data, supporting:
         * - Different input/output formats (NV12 → P010, I420 → NV12, etc.)
         * - Different bit depths (8-bit ↔ 10-bit ↔ 12-bit) with proper scaling
         * - Different plane layouts (2-plane ↔ 3-plane)
         * - Linear ↔ optimal tiling (via shader image load/store)
         * - Optional range conversion (limited ↔ full range)
         * - Optional LSB/MSB bit alignment adjustments
         *
         * This is the most flexible copy operation for YCbCr data.
         */
        YCBCRCOPY,
        
        /**
         * @brief Clear YCbCr image to a constant value
         *
         * Uses a compute shader to fill YCbCr planes with constant values.
         * Typically used to initialize frames to "black" (Y=16/64/256, CbCr=128/512/2048).
         */
        YCBCRCLEAR,
        
        /**
         * @brief Convert YCbCr to RGBA
         *
         * Uses a compute shader to perform color space conversion from YCbCr to RGBA.
         * Handles all YCbCr formats (NV12, P010, I420, etc.) and color primaries
         * (BT.601, BT.709, BT.2020).
         */
        YCBCR2RGBA,
        
        /**
         * @brief Convert RGBA to YCbCr
         *
         * Uses a compute shader to perform color space conversion from RGBA to YCbCr.
         * Supports all output formats (NV12, P010, I420, NV16, YUV444, etc.) with
         * proper chroma downsampling for subsampled formats.
         */
        RGBA2YCBCR,
        
        // =====================================================================
        // Transfer-only operations (use vkCmdCopy*, no format conversion)
        // =====================================================================
        
        /**
         * @brief Transfer image data to buffer (no format conversion)
         *
         * Uses vkCmdCopyImageToBuffer to copy image planes to a buffer.
         * For multi-planar formats, each plane is copied as a separate region.
         *
         * **Requirements:**
         * - Buffer must have sufficient size for all planes
         * - Buffer geometry (PlaneBufferGeometry) must match image dimensions
         * - No format conversion is performed - data is copied as-is
         *
         * Common use case: GPU-to-host readback for file dumping, CPU processing
         */
        XFER_IMAGE_TO_BUFFER,
        
        /**
         * @brief Transfer buffer data to image (no format conversion)
         *
         * Uses vkCmdCopyBufferToImage to copy buffer data to image planes.
         * For multi-planar formats, each plane is copied as a separate region.
         *
         * **Requirements:**
         * - Buffer geometry must match image format and dimensions
         * - No format conversion is performed - data is copied as-is
         *
         * Common use case: CPU-generated data upload, loading raw YUV files
         */
        XFER_BUFFER_TO_IMAGE,
        
        /**
         * @brief Transfer between images with different tiling (no format conversion)
         *
         * Uses vkCmdCopyImage to copy between images.
         * For multi-planar formats, each plane is copied as a separate region.
         *
         * **Requirements:**
         * - Source and destination formats MUST be identical (or compatible)
         * - Both images must have matching dimensions
         * - No format conversion is performed
         *
         * Common use case: Tiling mode conversion (linear → optimal for display,
         * optimal → linear for host access)
         */
        XFER_IMAGE_TO_IMAGE,
    };
    
    // =========================================================================
    // Dispatch Grid Strategy
    // =========================================================================
    
    /**
     * @brief Dispatch Grid Determination
     *
     * The compute dispatch grid is determined by the **lowest resolution output**.
     * This ensures all outputs are correctly covered with minimal thread divergence.
     *
     * ## General Rule
     *
     * ```
     * dispatchWidth  = lowestOutputWidth
     * dispatchHeight = lowestOutputHeight
     * workgroupsX    = (dispatchWidth + workgroupSizeX - 1) / workgroupSizeX
     * workgroupsY    = (dispatchHeight + workgroupSizeY - 1) / workgroupSizeY
     * ```
     *
     * Each thread processes a **block of pixels** whose size depends on the ratio
     * between the dispatch resolution and each output's resolution.
     *
     * ## With Y Subsampling Enabled (FLAG_ENABLE_Y_SUBSAMPLING)
     *
     * When the 2x2 subsampled Y output (binding 9) is enabled for Adaptive Quantization,
     * the dispatch grid is **always at half resolution** regardless of the main output format:
     *
     * ```
     * dispatchWidth  = (outputWidth + 1) / 2
     * dispatchHeight = (outputHeight + 1) / 2
     * ```
     *
     * This is because the subsampled Y output has the lowest resolution (always 2x2 subsampled).
     *
     * **Thread processing for different main output formats:**
     *
     * | Main Output | Subsampled Y | Thread Block | Per-Thread Work                    |
     * |-------------|--------------|--------------|-------------------------------------|
     * | 4:2:0       | 2x2          | 2x2 luma     | 4 Y reads, 1 CbCr read, 4 Y writes, 1 CbCr write, 1 subsamp Y |
     * | 4:2:2       | 2x2          | 2x2 luma     | 4 Y reads, 2 CbCr reads, 4 Y writes, 2 CbCr writes, 1 subsamp Y |
     * | 4:4:4       | 2x2          | 2x2 luma     | 4 Y reads, 4 CbCr reads, 4 Y writes, 4 CbCr writes, 1 subsamp Y |
     *
     * **Implementation Status by Filter Type:**
     *
     * | Filter      | Y Subsampling Support | Dispatch Strategy                           |
     * |-------------|----------------------|---------------------------------------------|
     * | YCBCRCOPY   | ✓ Supported          | Always 2x2 dispatch, writes multi-pixels    |
     * | YCBCRCLEAR  | ✓ Supported          | Uses output chroma ratio for dispatch       |
     * | RGBA2YCBCR  | ✗ NOT YET SUPPORTED  | Uses output chroma ratio (needs update)     |
     * | YCBCR2RGBA  | N/A                  | Output is RGBA, no Y subsampling needed     |
     *
     * ## Without Y Subsampling (Standard Mode)
     *
     * Without subsampled Y output, dispatch is based on the main output's chroma resolution:
     *
     * | Output Format | Chroma Ratio | Dispatch Grid           | Thread Block |
     * |---------------|--------------|-------------------------|--------------|
     * | 4:2:0 (NV12)  | 2x2          | (width/2) × (height/2)  | 2x2 luma     |
     * | 4:2:2 (NV16)  | 2x1          | (width/2) × height      | 2x1 luma     |
     * | 4:4:4 (YUV444)| 1x1          | width × height          | 1x1 luma     |
     *
     * ## Multi-Pixel Processing Example
     *
     * For 4:4:4 output with Y subsampling enabled:
     * - Dispatch: (width/2) × (height/2) threads
     * - Each thread at position (chromaX, chromaY) handles:
     *   - Luma positions: (2*chromaX, 2*chromaY), (2*chromaX+1, 2*chromaY),
     *                     (2*chromaX, 2*chromaY+1), (2*chromaX+1, 2*chromaY+1)
     *   - Reads 4 Y + 4 CbCr from input
     *   - Writes 4 Y + 4 CbCr to main output (4:4:4)
     *   - Computes average of 4 Y values
     *   - Writes 1 subsampled Y to binding 9
     */
    
    /**
     * @brief Filter configuration flags (bitmask)
     */
    enum FilterFlags : uint32_t {
        FLAG_NONE                               = 0,
        
        /// Enable MSB-to-LSB shift for high bit-depth input (10/12-bit in 16-bit container)
        FLAG_INPUT_MSB_TO_LSB_SHIFT             = (1 << 0),
        
        /// Enable LSB-to-MSB shift for high bit-depth output (10/12-bit in 16-bit container)
        FLAG_OUTPUT_LSB_TO_MSB_SHIFT            = (1 << 1),
        
        /**
         * @brief Enable 2x2 Y subsampling output (binding 9)
         *
         * When enabled, produces an additional output with 2x2 box-filtered luma values.
         * Used for Adaptive Quantization (AQ) algorithms that need downsampled luma.
         *
         * **IMPORTANT: This affects the dispatch grid!**
         * When enabled, dispatch is ALWAYS at half resolution (width/2 × height/2),
         * regardless of the main output format. Each thread processes a 2x2 luma block.
         *
         * For 4:4:4 or 4:2:2 main output formats, this means each thread writes
         * multiple pixels to the main output while writing one pixel to binding 9.
         *
         * @see "Dispatch Grid Strategy" documentation above
         */
        FLAG_ENABLE_Y_SUBSAMPLING               = (1 << 2),
        
        /// Replicate edge pixels when reading beyond input bounds (single row/column)
        FLAG_ENABLE_ROW_COLUMN_REPLICATION_ONE  = (1 << 3),
        
        /// Replicate edge pixels for all out-of-bounds reads
        FLAG_ENABLE_ROW_COLUMN_REPLICATION_ALL  = (1 << 4),
        
        // Transfer operation flags
        FLAG_PRE_TRANSFER_ENABLED               = (1 << 8),  ///< Enable pre-transfer stage
        FLAG_POST_TRANSFER_ENABLED              = (1 << 9),  ///< Enable post-transfer stage
        FLAG_SKIP_COMPUTE                       = (1 << 10), ///< Skip compute (transfer-only mode)
        
        // Aliases for convenience
        FLAG_PRE_TRANSFER                       = FLAG_PRE_TRANSFER_ENABLED,
        FLAG_POST_TRANSFER                      = FLAG_POST_TRANSFER_ENABLED,
    };
    
    static constexpr uint32_t maxNumComputeDescr = 10;

    static constexpr VkImageAspectFlags validPlaneAspects = VK_IMAGE_ASPECT_PLANE_0_BIT |
                                                            VK_IMAGE_ASPECT_PLANE_1_BIT |
                                                            VK_IMAGE_ASPECT_PLANE_2_BIT;

    static constexpr VkImageAspectFlags validAspects = VK_IMAGE_ASPECT_COLOR_BIT | validPlaneAspects;

    static uint32_t GetPlaneIndex(VkImageAspectFlagBits planeAspect);

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           uint32_t queueFamilyIndex,
                           uint32_t queueIndex,
                           FilterType flterType,
                           uint32_t maxNumFrames,
                           VkFormat inputFormat,
                           VkFormat outputFormat,
                           uint32_t filterFlags,  // FilterFlags bitmask
                           const VkSamplerYcbcrConversionCreateInfo* pYcbcrConversionCreateInfo,
                           const YcbcrPrimariesConstants* pYcbcrPrimariesConstants,
                           const VkSamplerCreateInfo* pSamplerCreateInfo,
                           VkSharedBaseObj<VulkanFilter>& vulkanFilter);

    /**
     * @brief Create a transfer-only filter instance
     * 
     * Creates a filter that only performs transfer operations (no compute shader).
     * Useful for efficient memory copies when no format conversion is needed.
     *
     * **When to use transfer-only vs compute-based:**
     *
     * Use transfer-only (CreateTransfer) when:
     * - Source and destination have the same format
     * - You need maximum copy performance
     * - Converting between tiling modes (linear ↔ optimal)
     * - Copying to/from host-accessible buffers for readback
     *
     * Use compute-based (Create with YCBCRCOPY) when:
     * - Source and destination have different formats (NV12 → P010)
     * - Bit depth conversion is needed (8-bit → 10-bit)
     * - Color space or range conversion is needed
     * - Plane layout conversion is needed (2-plane ↔ 3-plane)
     *
     * @param vkDevCtx         Device context
     * @param queueFamilyIndex Queue family (must support VK_QUEUE_TRANSFER_BIT)
     * @param queueIndex       Queue index within the family
     * @param transferType     Type of transfer operation (COPY_IMAGE_TO_BUFFER, etc.)
     * @param vulkanFilter     Output filter object
     * @return VK_SUCCESS on success
     *
     * @see TransferOpType for the specific transfer operation requirements
     * @see YCBCRCOPY for compute-based copy with format conversion support
     */
    static VkResult CreateTransfer(const VulkanDeviceContext* vkDevCtx,
                                   uint32_t queueFamilyIndex,
                                   uint32_t queueIndex,
                                   TransferOpType transferType,
                                   VkSharedBaseObj<VulkanFilter>& vulkanFilter);

    VulkanFilterYuvCompute(const VulkanDeviceContext* vkDevCtx,
                           uint32_t queueFamilyIndex,
                           uint32_t queueIndex,
                           FilterType filterType,
                           uint32_t maxNumFrames,
                           VkFormat inputFormat,
                           VkFormat outputFormat,
                           uint32_t filterFlags,  // FilterFlags bitmask
                           const YcbcrPrimariesConstants* pYcbcrPrimariesConstants)
        : VulkanFilter(vkDevCtx, queueFamilyIndex, queueIndex)
        , m_filterType(filterType)
        , m_inputFormat(inputFormat)
        , m_outputFormat(outputFormat)
        , m_workgroupSizeX(16)
        , m_workgroupSizeY(16)
        , m_maxNumFrames(maxNumFrames)
        , m_ycbcrPrimariesConstants (pYcbcrPrimariesConstants ?
                                        *pYcbcrPrimariesConstants :
                                        YcbcrPrimariesConstants{0.0, 0.0})
        , m_inputImageAspects(  VK_IMAGE_ASPECT_COLOR_BIT |
                                VK_IMAGE_ASPECT_PLANE_0_BIT |
                                VK_IMAGE_ASPECT_PLANE_1_BIT |
                                VK_IMAGE_ASPECT_PLANE_2_BIT)
        , m_outputImageAspects( VK_IMAGE_ASPECT_COLOR_BIT |
                                VK_IMAGE_ASPECT_PLANE_0_BIT |
                                VK_IMAGE_ASPECT_PLANE_1_BIT |
                                VK_IMAGE_ASPECT_PLANE_2_BIT)
        , m_filterFlags(filterFlags)
        , m_inputEnableMsbToLsbShift((filterFlags & FLAG_INPUT_MSB_TO_LSB_SHIFT) != 0)
        , m_outputEnableLsbToMsbShift((filterFlags & FLAG_OUTPUT_LSB_TO_MSB_SHIFT) != 0)
        , m_enableRowAndColumnReplication((filterFlags & (FLAG_ENABLE_ROW_COLUMN_REPLICATION_ONE | FLAG_ENABLE_ROW_COLUMN_REPLICATION_ALL)) != 0)
        , m_inputIsBuffer(false)
        , m_outputIsBuffer(false)
        , m_enableYSubsampling((filterFlags & FLAG_ENABLE_Y_SUBSAMPLING) != 0)
        , m_skipCompute((filterFlags & FLAG_SKIP_COMPUTE) != 0 || 
                        filterType == XFER_IMAGE_TO_BUFFER ||
                        filterType == XFER_BUFFER_TO_IMAGE ||
                        filterType == XFER_IMAGE_TO_IMAGE)
    {
    }

    VkResult Init(const VkSamplerYcbcrConversionCreateInfo* pYcbcrConversionCreateInfo,
                  const VkSamplerCreateInfo* pSamplerCreateInfo);

    virtual ~VulkanFilterYuvCompute() {
        assert(m_vkDevCtx != nullptr);
    }

    uint32_t UpdateBufferDescriptorSets(const VkBuffer*            vkBuffers,
                                        uint32_t                   numVkBuffers,
                                        const VkSubresourceLayout* vkBufferSubresourceLayout,
                                        uint32_t                   numPlanes,
                                        VkImageAspectFlags         validImageAspects,
                                        uint32_t&                  descrIndex,
                                        uint32_t&                  baseBinding,
                                        VkDescriptorType           descriptorType, // Ex: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                        VkDescriptorBufferInfo bufferDescriptors[maxNumComputeDescr],
                                        std::array<VkWriteDescriptorSet, maxNumComputeDescr>& writeDescriptorSets,
                                        const uint32_t maxDescriptors = maxNumComputeDescr);

    uint32_t  UpdateImageDescriptorSets(const VkImageResourceView* inputImageView,
                                        VkImageAspectFlags         validImageAspects,
                                        VkSampler                  convSampler,
                                        VkImageLayout              imageLayout,
                                        uint32_t&                  descrIndex,
                                        uint32_t&                  baseBinding,
                                        VkDescriptorType           descriptorType, // Ex: VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                        VkDescriptorImageInfo      imageDescriptors[maxNumComputeDescr],
                                        std::array<VkWriteDescriptorSet, maxNumComputeDescr>& writeDescriptorSets,
                                        const uint32_t maxDescriptors = maxNumComputeDescr);


    // Image input -> Image output + optional subsampled Y output (overload for AQ)
    VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                 uint32_t bufferIdx,
                                 const VkImageResourceView* inputImageView,
                                 const VkVideoPictureResourceInfoKHR * inputImageResourceInfo,
                                 const VkImageResourceView* outputImageView,
                                 const VkVideoPictureResourceInfoKHR * outputImageResourceInfo,
                                 const VkImageResourceView* subsampledImageView = nullptr,
                                 const VkVideoPictureResourceInfoKHR * subsampledImageResourceInfo = nullptr);

    // Buffer input -> Image output
    VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                 uint32_t bufferIdx,
                                 const VkBuffer*            inBuffers,     // with size numInBuffers
                                 uint32_t                   numInBuffers,
                                 const VkFormat*            inBufferFormats, // with size inBufferNumPlanes
                                 const VkSubresourceLayout* inBufferSubresourceLayouts, // with size inBufferNumPlanes
                                 uint32_t                   inBufferNumPlanes,
                                 const VkImageResourceView* outImageView,
                                 const VkVideoPictureResourceInfoKHR* outImageResourceInfo,
                                 const VkBufferImageCopy*   pBufferImageCopy,
                                 const VkImageResourceView* subsampledYImageView = nullptr,
                                 const VkVideoPictureResourceInfoKHR * subsampledImageResourceInfo = nullptr);

    // Image input -> Buffer output
    VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                 uint32_t bufferIdx,
                                 const VkImageResourceView* inImageView,
                                 const VkVideoPictureResourceInfoKHR* inImageResourceInfo,
                                 const VkBuffer*            outBuffers,        // with size numOutBuffers
                                 uint32_t                   numOutBuffers,
                                 const VkFormat*            inBufferFormats,   // with size outBufferNumPlanes
                                 const VkSubresourceLayout* outBufferSubresourceLayouts, // with size outBufferNumPlanes
                                 uint32_t                   outBufferNumPlanes,
                                 const VkBufferImageCopy*   pBufferImageCopy,
                                 const VkImageResourceView* subsampledYImageView = nullptr,
                                 const VkVideoPictureResourceInfoKHR * subsampledImageResourceInfo = nullptr);

    // Buffer input -> Buffer output
    VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                 uint32_t bufferIdx,
                                 const VkBuffer*            inBuffers,       // with size numInBuffers
                                 uint32_t                   numInBuffers,
                                 const VkFormat*            inBufferFormats, // with size inBufferNumPlanes
                                 const VkSubresourceLayout* inBufferSubresourceLayouts, // with size inBufferNumPlanes
                                 uint32_t                   inBufferNumPlanes,
                                 const VkExtent3D&          inBufferExtent,
                                 const VkBuffer*            outBuffers,        // with size numOutBuffers
                                 uint32_t                   numOutBuffers,
                                 const VkFormat*            outBufferFormats,   // with size outBufferNumPlanes
                                 const VkSubresourceLayout* outBufferSubresourceLayouts, // with size outBufferNumPlanes
                                 uint32_t                   outBufferNumPlanes,
                                 const VkExtent3D&          outBufferExtent,
                                 const VkImageResourceView* subsampledYImageView = nullptr,
                                 const VkVideoPictureResourceInfoKHR * subsampledImageResourceInfo = nullptr);

    // =========================================================================
    // Unified API with integrated transfer support
    // =========================================================================
    
    /**
     * @brief Record command buffer with full transfer support
     * 
     * Records the complete execution pipeline with all stages:
     *
     * ```
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ Stage 1: PRE-TRANSFERS (optional)                                   │
     * │   - Transition preTransferSource → TRANSFER_SRC_OPTIMAL             │
     * │   - Transition primary input → TRANSFER_DST_OPTIMAL                 │
     * │   - Copy: preTransferSource → primary input                         │
     * │   - Barrier: TRANSFER → COMPUTE                                     │
     * ├─────────────────────────────────────────────────────────────────────┤
     * │ Stage 2: COMPUTE (skipped for XFER_* filter types)                  │
     * │   - Transition inputs → GENERAL (for shader read)                   │
     * │   - Transition outputs → GENERAL (for shader write)                 │
     * │   - Bind pipeline, descriptors, push constants                      │
     * │   - Dispatch compute shader                                         │
     * ├─────────────────────────────────────────────────────────────────────┤
     * │ Stage 3: POST-TRANSFERS (optional)                                  │
     * │   - Barrier: COMPUTE → TRANSFER                                     │
     * │   - Transition primary output → TRANSFER_SRC_OPTIMAL                │
     * │   - Transition postTransferDest → TRANSFER_DST_OPTIMAL              │
     * │   - Copy: primary output → postTransferDest                         │
     * │   - Barrier: TRANSFER → HOST (if dest is host-accessible)           │
     * └─────────────────────────────────────────────────────────────────────┘
     * ```
     *
     * **Multi-planar YCbCr handling:**
     * Transfer operations copy plane-by-plane:
     * - Y plane at full resolution (VK_IMAGE_ASPECT_PLANE_0_BIT)
     * - CbCr/Cb plane at chroma resolution (VK_IMAGE_ASPECT_PLANE_1_BIT)
     * - Cr plane at chroma resolution (VK_IMAGE_ASPECT_PLANE_2_BIT, 3-plane formats)
     *
     * **IMPORTANT: Transfer vs Compute format requirements:**
     * - Transfer operations (pre/post-transfer, XFER_* types): formats MUST match
     * - Compute operations (YCBCRCOPY, RGBA2YCBCR, etc.): formats can differ
     *
     * @param cmdBuf        Command buffer to record into (must be in recording state)
     * @param bufferIdx     Frame buffer index (for descriptor set management)
     * @param execDesc      Execution descriptor with all I/O and transfer specifications
     * @return VK_SUCCESS on success
     *
     * @see FilterExecutionDesc for input/output slot configuration
     * @see FilterIOSlot for pre/post-transfer configuration
     */
    VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                 uint32_t bufferIdx,
                                 const FilterExecutionDesc& execDesc);

    /**
     * @brief Check if this filter is transfer-only (no compute shader)
     */
    bool isTransferOnly() const {
        return m_filterType == XFER_IMAGE_TO_BUFFER ||
               m_filterType == XFER_BUFFER_TO_IMAGE ||
               m_filterType == XFER_IMAGE_TO_IMAGE;
    }

private:
    // =========================================================================
    // Transfer operation helpers
    // =========================================================================
    
    /**
     * @brief Record layout transition for an image resource
     *
     * Inserts a pipeline barrier to transition an image between layouts.
     * Used before/after transfer operations to ensure correct access patterns.
     *
     * @param cmdBuf      Command buffer to record into
     * @param resource    Image resource to transition (no-op for buffers or if already in newLayout)
     * @param newLayout   Target layout for the image
     * @param srcStage    Source pipeline stage (when transition can begin)
     * @param dstStage    Destination pipeline stage (when transition must complete)
     * @param srcAccess   Source access mask (previous access type)
     * @param dstAccess   Destination access mask (new access type)
     */
    void RecordLayoutTransition(VkCommandBuffer cmdBuf,
                                const TransferResource& resource,
                                VkImageLayout newLayout,
                                VkPipelineStageFlags srcStage,
                                VkPipelineStageFlags dstStage,
                                VkAccessFlags srcAccess,
                                VkAccessFlags dstAccess);
    
    /**
     * @brief Record pre-transfer operations for all inputs
     *
     * Iterates through all input slots in the execution descriptor and records
     * any pre-transfer operations that stage data into the primary compute resource.
     *
     * The sequence for each input with pre-transfer:
     * 1. Transition preTransferSource → TRANSFER_SRC_OPTIMAL
     * 2. Transition primary → TRANSFER_DST_OPTIMAL
     * 3. Record copy (preTransferSource → primary)
     *
     * After all pre-transfers, a barrier is inserted: TRANSFER → COMPUTE
     *
     * @param cmdBuf      Command buffer to record into
     * @param execDesc    Execution descriptor with input slot configurations
     */
    void RecordPreTransfers(VkCommandBuffer cmdBuf,
                           const FilterExecutionDesc& execDesc);
    
    /**
     * @brief Record post-transfer operations for all outputs
     *
     * Iterates through all output slots in the execution descriptor and records
     * any post-transfer operations that copy computed results to secondary destinations.
     *
     * The sequence for each output with post-transfer:
     * 1. Barrier: COMPUTE → TRANSFER (inserted once before all post-transfers)
     * 2. Transition primary → TRANSFER_SRC_OPTIMAL
     * 3. Transition postTransferDest → TRANSFER_DST_OPTIMAL (if image)
     * 4. Record copy (primary → postTransferDest)
     *
     * If any post-transfer destination is a buffer or linear image (host-accessible),
     * a final barrier is inserted: TRANSFER → HOST
     *
     * @param cmdBuf      Command buffer to record into
     * @param execDesc    Execution descriptor with output slot configurations
     */
    void RecordPostTransfers(VkCommandBuffer cmdBuf,
                            const FilterExecutionDesc& execDesc);
    
    /**
     * @brief Record a single transfer operation between resources
     *
     * Records the appropriate vkCmdCopy* command based on the operation type.
     * Automatically handles multi-planar YCbCr formats by copying each plane
     * as a separate region with the correct aspect mask and subsampled dimensions.
     *
     * **IMPORTANT:** Transfer operations require matching formats between source
     * and destination. No format conversion is performed. For format conversion,
     * use compute-based operations (YCBCRCOPY, YCBCR2RGBA, RGBA2YCBCR).
     *
     * @param cmdBuf      Command buffer to record into
     * @param opType      Type of transfer operation
     * @param src         Source resource (must match opType requirements)
     * @param dst         Destination resource (must match opType requirements)
     *
     * @see TransferOpType for valid source/destination combinations
     */
    void RecordTransferOp(VkCommandBuffer cmdBuf,
                         TransferOpType opType,
                         const TransferResource& src,
                         const TransferResource& dst);
    
    /**
     * @brief Calculate buffer-image copy regions for multi-planar formats
     *
     * Generates VkBufferImageCopy regions for copying between a buffer and
     * a multi-planar YCbCr image. Each plane is handled as a separate region.
     *
     * **Plane-by-plane copy strategy:**
     * - Plane 0 (Y): Full resolution, VK_IMAGE_ASPECT_PLANE_0_BIT
     * - Plane 1 (CbCr or Cb): Subsampled resolution, VK_IMAGE_ASPECT_PLANE_1_BIT
     * - Plane 2 (Cr, 3-plane only): Subsampled resolution, VK_IMAGE_ASPECT_PLANE_2_BIT
     *
     * Buffer geometry is obtained from:
     * 1. Explicit PlaneBufferGeometry (preferred, set via fromBufferResource())
     * 2. Format-based calculation using YcbcrVkFormatInfo() as fallback
     *
     * @param image       Image resource (provides format, extent, array layer, mip level)
     * @param buffer      Buffer resource (provides plane offsets, row pitches, dimensions)
     * @param regions     Output array of copy regions (max 3 for multi-planar)
     * @param numRegions  Output: number of regions populated
     *
     * @note For single-plane formats (RGBA, R8, etc.), only one region is generated.
     */
    void CalculateBufferImageCopyRegions(const TransferResource& image,
                                         const TransferResource& buffer,
                                         VkBufferImageCopy regions[3],
                                         uint32_t& numRegions);
    
    /**
     * @brief Calculate image-to-image copy regions for multi-planar formats
     *
     * Generates VkImageCopy regions for copying between two images.
     * Both images must have the same (or compatible) format.
     * Each plane is handled as a separate region.
     *
     * **Plane-by-plane copy strategy:**
     * - Plane 0 (Y): Full resolution, VK_IMAGE_ASPECT_PLANE_0_BIT
     * - Plane 1 (CbCr or Cb): Subsampled resolution, VK_IMAGE_ASPECT_PLANE_1_BIT
     * - Plane 2 (Cr, 3-plane only): Subsampled resolution, VK_IMAGE_ASPECT_PLANE_2_BIT
     *
     * Subsampled plane dimensions are calculated from the source image format.
     *
     * @param srcImage    Source image resource
     * @param dstImage    Destination image resource (must have compatible format)
     * @param regions     Output array of copy regions (max 3 for multi-planar)
     * @param numRegions  Output: number of regions populated
     *
     * @note For single-plane formats (RGBA, R8, etc.), only one region is generated.
     * @note Unlike YCBCRCOPY, this does NOT perform format conversion - formats must match.
     */
    void CalculateImageCopyRegions(const TransferResource& srcImage,
                                   const TransferResource& dstImage,
                                   VkImageCopy regions[3],
                                   uint32_t& numRegions);
    
    /**
     * @brief Record the compute dispatch portion
     *
     * Records the compute shader dispatch after descriptor binding.
     * Used internally by the unified RecordCommandBuffer(execDesc) overload.
     *
     * @param cmdBuf      Command buffer to record into
     * @param bufferIdx   Frame buffer index for descriptor management
     * @param execDesc    Execution descriptor with I/O specifications
     * @return VK_SUCCESS on success
     */
    VkResult RecordComputeDispatch(VkCommandBuffer cmdBuf,
                                  uint32_t bufferIdx,
                                  const FilterExecutionDesc& execDesc);

    // =========================================================================
    // Existing private members
    // =========================================================================

    VkResult InitDescriptorSetLayout(uint32_t maxNumFrames);

    /**
     * @brief Generates GLSL image descriptor bindings for shader input/output
     *
     * Creates appropriate GLSL image binding declarations based on the input/output format.
     * Handles different YUV formats like single-plane (RGBA), 2-plane (NV12/NV21), and 3-plane (I420, etc).
     *
     * @param computeShader Output stringstream for shader code
     * @param imageAspects Output parameter to store the image aspect flags used
     * @param imageName Base image variable name
     * @param imageFormat Vulkan format of the image
     * @param isInput Whether this is an input or output resource
     * @param startBinding Starting binding number in the descriptor set
     * @param set Descriptor set number
     * @param imageArray Whether to use image2DArray or image2D
     * @return The next available binding number after all descriptors are created
     */
    uint32_t ShaderGenerateImagePlaneDescriptors(std::stringstream& computeShader,
                                                 VkImageAspectFlags& imageAspects,
                                                 const char *imageName,
                                                 VkFormat    imageFormat,
                                                 bool isInput,
                                                 uint32_t startBinding = 0,
                                                 uint32_t set = 0,
                                                 bool imageArray = true);

    /**
     * @brief Generates GLSL buffer descriptor bindings for shader input/output
     *
     * Creates appropriate GLSL buffer binding declarations based on the input/output format.
     * Handles different YUV buffer layouts matching single-plane, 2-plane, or 3-plane formats.
     *
     * @param shaderStr Output stringstream for shader code
     * @param imageAspects Output parameter to store the image aspect flags used
     * @param bufferName Base buffer variable name
     * @param bufferFormat Vulkan format of the buffer data
     * @param isInput Whether this is an input or output resource
     * @param startBinding Starting binding number in the descriptor set
     * @param set Descriptor set number
     * @param bufferType The Vulkan descriptor type to use for the buffer
     * @return The next available binding number after all descriptors are created
     */
    uint32_t ShaderGenerateBufferPlaneDescriptors(std::stringstream& shaderStr,
                                                  VkImageAspectFlags& imageAspects,
                                                  const char *bufferName,
                                                  VkFormat    bufferFormat,
                                                  bool isInput,
                                                  uint32_t startBinding = 0,
                                                  uint32_t set = 0,
                                                  VkDescriptorType bufferType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    /**
     * @brief Unified descriptor generation for either buffer or image resources
     *
     * Delegates to either ShaderGenerateImagePlaneDescriptors or ShaderGenerateBufferPlaneDescriptors
     * based on the resource type (image or buffer) needed for input/output.
     *
     * @param shaderStr Output stringstream for shader code
     * @param isInput Whether this is an input or output resource
     * @param startBinding Starting binding number in the descriptor set
     * @param set Descriptor set number
     * @param imageArray Whether to use image2DArray or image2D (for image resources)
     * @param bufferType The Vulkan descriptor type to use for buffer resources
     * @return The next available binding number after all descriptors are created
     */
protected:
    uint32_t ShaderGeneratePlaneDescriptors(std::stringstream& shaderStr,
                                            bool isInput,
                                            uint32_t startBinding,
                                            uint32_t set,
                                            bool imageArray,
                                            VkDescriptorType bufferType);

    /**
     * @brief Initializes GLSL shader for YCbCr copy operation
     *
     * Generates a compute shader that copies YCbCr data from input to output
     * with optional format, bit depth, and layout conversion.
     *
     * **Unlike transfer operations (XFER_*), YCBCRCOPY supports:**
     *
     * - **Different formats:** NV12 → P010, I420 → NV12, NV12 → YUV444, etc.
     * - **Different bit depths:** 8-bit ↔ 10-bit ↔ 12-bit with proper range scaling
     * - **Different plane layouts:** 2-plane (NV12) ↔ 3-plane (I420)
     * - **Different tiling modes:** linear ↔ optimal (via shader image load/store)
     * - **Bit alignment conversion:** MSB ↔ LSB alignment for 10/12-bit formats
     * - **Range conversion:** limited range ↔ full range
     * - **Buffer ↔ Image:** input/output can be images or buffers independently
     *
     * **Processing pipeline:**
     * 1. Read input (image or buffer), normalize to [0.0, 1.0] float
     * 2. Apply format/range conversion in normalized space
     * 3. Denormalize to output bit depth and range
     * 4. Write output (image or buffer)
     *
     * The shader handles chroma subsampling differences automatically.
     * One thread typically processes a 2x2 luma block with 1 chroma sample.
     *
     * **When to use YCBCRCOPY vs XFER_* transfers:**
     * - Use YCBCRCOPY when input and output formats differ
     * - Use XFER_* when formats match and you need maximum copy performance
     *
     * @param computeShader Output string for the complete GLSL shader code
     * @return Size of the generated shader code in bytes
     */
    size_t InitYCBCRCOPY(std::string& computeShader);

    /**
     * @brief Initializes GLSL shader for YCbCr clear operation
     *
     * Generates a compute shader that clears/fills YCbCr data in the output
     * resource with constant values.
     *
     * @param computeShader Output string for the complete GLSL shader code
     * @return Size of the generated shader code in bytes
     */
    size_t InitYCBCRCLEAR(std::string& computeShader);

    /**
     * @brief Initializes GLSL shader for YCbCr to RGBA conversion
     *
     * Generates a compute shader that converts YCbCr input to RGBA output
     * using the appropriate color space conversion matrix.
     *
     * @param computeShader Output string for the complete GLSL shader code
     * @return Size of the generated shader code in bytes
     */
    size_t InitYCBCR2RGBA(std::string& computeShader);

    /**
     * @brief Initializes GLSL shader for RGBA to YCbCr conversion
     *
     * Generates a compute shader that converts RGBA input to YCbCr output
     * using the appropriate color space conversion matrix.
     * Supports all chroma subsampling formats (4:4:4, 4:2:2, 4:2:0).
     *
     * Architecture:
     * 1. Input handling: Read RGB pixels, normalize to [0.0, 1.0]
     * 2. Processing: RGB→YCbCr matrix conversion + chroma downsampling
     * 3. Output handling: Denormalize to target bit depth, write to planes
     *
     * @param computeShader Output string for the complete GLSL shader code
     * @return Size of the generated shader code in bytes
     */
    size_t InitRGBA2YCBCR(std::string& computeShader);

    const FilterType                         m_filterType;
    VkFormat                                 m_inputFormat;
    VkFormat                                 m_outputFormat;
    uint32_t                                 m_workgroupSizeX; // usually 16
    uint32_t                                 m_workgroupSizeY; // usually 16
    uint32_t                                 m_maxNumFrames;
    const YcbcrPrimariesConstants            m_ycbcrPrimariesConstants;
    VulkanSamplerYcbcrConversion             m_samplerYcbcrConversion;
    VulkanDescriptorSetLayout                m_descriptorSetLayout;
    VulkanComputePipeline                    m_computePipeline;
    VkImageAspectFlags                       m_inputImageAspects;
    VkImageAspectFlags                       m_outputImageAspects;
    uint32_t                                 m_filterFlags;  // FilterFlags bitmask
    uint32_t                                 m_inputEnableMsbToLsbShift : 1;
    uint32_t                                 m_outputEnableLsbToMsbShift : 1;
    uint32_t                                 m_enableRowAndColumnReplication : 1;
    uint32_t                                 m_inputIsBuffer : 1;
    uint32_t                                 m_outputIsBuffer : 1;
    uint32_t                                 m_enableYSubsampling : 1; // Enable 2x2 Y subsampling output
    uint32_t                                 m_skipCompute : 1;        // Skip compute (transfer-only mode)

    struct PushConstants {
        uint32_t srcLayer;         // src image layer to use
        uint32_t dstLayer;         // dst image layer to use
        uint32_t inputWidth;       // input image or buffer width
        uint32_t inputHeight;      // input image or buffer height
        uint32_t outputWidth;      // output image or buffer width
        uint32_t outputHeight;     // output image or buffer height
        uint32_t halfInputWidth;   // (inputWidth + 1) / 2 - precomputed for 2x2 blocks
        uint32_t halfInputHeight;  // (inputHeight + 1) / 2
        uint32_t halfOutputWidth;  // (outputWidth + 1) / 2 - precomputed for 2x2 blocks
        uint32_t halfOutputHeight; // (outputHeight + 1) / 2
        uint32_t inYOffset;        // input buffer Y plane offset
        uint32_t inCbOffset;       // input buffer Cb plane offset
        uint32_t inCrOffset;       // input buffer Cr plane offset
        uint32_t inYPitch;         // input buffer Y plane pitch
        uint32_t inCbPitch;        // input buffer Cb plane pitch
        uint32_t inCrPitch;        // input buffer Cr plane pitch
        uint32_t outYOffset;       // output buffer Y plane offset
        uint32_t outCbOffset;      // output buffer Cb plane offset
        uint32_t outCrOffset;      // output buffer Cr plane offset
        uint32_t outYPitch;        // output buffer Y plane pitch
        uint32_t outCbPitch;       // output buffer Cb plane pitch
        uint32_t outCrPitch;       // output buffer Cr plane pitch
    };
};

#endif /* _VULKANFILTERYUVCOMPUTE_H_ */
