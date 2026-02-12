/*
 * Copyright 2024-2025 NVIDIA Corporation.
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

#ifndef _VULKAN_VIDEO_ENCODER_EXT_H_
#define _VULKAN_VIDEO_ENCODER_EXT_H_

#include "vulkan_video_encoder.h"
#include <vulkan/vulkan.h>

//=============================================================================
// External Frame Input with Synchronization
//
// Extends VulkanVideoEncoder for frame-at-a-time operation with
// externally-provided VkImages and timeline semaphore synchronization.
// This is the interface for cross-process encoder services.
//
// Usage flow:
//   1. CreateVulkanVideoEncoderExt() to create the encoder
//   2. InitializeExt() with structured config (not argc/argv)
//   3. For each frame:
//      a. SubmitExternalFrame() with imported VkImage + sync info
//      b. Poll GetEncodedFrame() for completed bitstream
//   4. Flush() to drain pending frames
//=============================================================================

//=============================================================================
// Encoder Configuration (structured, not argc/argv)
//=============================================================================
struct VkVideoEncoderConfig {
    // Codec
    VkVideoCodecOperationFlagBitsKHR codec;

    // Encode output resolution
    uint32_t encodeWidth;
    uint32_t encodeHeight;

    // Input format (what the external frames will be)
    VkFormat inputFormat;
    uint32_t inputWidth;
    uint32_t inputHeight;

    // Rate control
    // 0 = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR
    // 1 = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_KHR (constant QP)
    // 2 = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_KHR
    // 3 = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_KHR
    uint32_t rateControlMode;
    uint32_t averageBitrate;    // bits/sec
    uint32_t maxBitrate;        // bits/sec (VBR)
    uint32_t vbvBufferSize;     // bits (0 = default)

    // Constant QP (when rateControlMode == DISABLED)
    int32_t constQpI;
    int32_t constQpP;
    int32_t constQpB;
    int32_t minQp;
    int32_t maxQp;

    // GOP structure
    uint32_t gopLength;         // Frames per GOP
    uint32_t consecutiveBFrames;// B-frames between I/P (0 = no B-frames)
    uint32_t idrPeriod;         // 0 = every GOP starts with IDR
    VkBool32 closedGop;

    // Frame rate
    uint32_t frameRateNum;
    uint32_t frameRateDen;

    // Quality
    uint32_t qualityLevel;      // 0 = default

    // Color info (VUI)
    uint8_t colourPrimaries;
    uint8_t transferCharacteristics;
    uint8_t matrixCoefficients;
    VkBool32 videoFullRange;

    // Enable the built-in compute filter for input preprocessing
    // Set to VK_TRUE if input format may not be directly encodable
    // (e.g. RGBA input that needs RGBA->NV12 conversion)
    VkBool32 enablePreprocessFilter;

    // Device selection (-1 = auto, matches first discrete GPU)
    int32_t deviceId;
    uint8_t gpuUUID[VK_UUID_SIZE]; // Preferred GPU UUID (all zeros = auto)

    // Debug
    VkBool32 verbose;
    VkBool32 validate;          // Vulkan validation layers
};

//=============================================================================
// External Frame Descriptor
//
// Describes a frame to encode that was allocated externally
// (e.g. imported from DMA-BUF in a cross-process encoder service).
//=============================================================================
struct VkVideoEncodeInputFrame {
    // The VkImage to encode (must be on the same device as the encoder)
    VkImage  image;
    VkImageView imageView;      // Can be VK_NULL_HANDLE if not needed

    // Image properties (must match the actual image)
    VkFormat format;
    uint32_t width;
    uint32_t height;
    VkImageLayout currentLayout; // Current layout of the image

    // Frame identification
    uint64_t frameId;           // Unique frame identifier
    uint64_t pts;               // Presentation timestamp (90kHz or custom)

    // Frame type overrides (0 = let encoder decide via GOP structure)
    VkBool32 forceIDR;
    VkBool32 forceIntra;

    // Per-frame QP override (-1 = use session default)
    int32_t qpOverride;

    // Synchronization: wait semaphores
    // The encoder will wait on these before accessing the image.
    // Typically this is the producer's graph timeline semaphore.
    uint32_t    waitSemaphoreCount;
    VkSemaphore* pWaitSemaphores;    // Array of semaphores to wait on
    uint64_t*    pWaitSemaphoreValues; // Timeline values (0 for binary semaphores)

    // Synchronization: signal semaphores
    // The encoder will signal these after the image is no longer needed.
    // Typically this is the consumer's release timeline semaphore.
    uint32_t    signalSemaphoreCount;
    VkSemaphore* pSignalSemaphores;    // Array of semaphores to signal
    uint64_t*    pSignalSemaphoreValues; // Timeline values (0 for binary)
};

//=============================================================================
// Encoded Frame Result
//
// Returned by GetEncodedFrame() after encoding completes.
//=============================================================================
struct VkVideoEncodeResult {
    uint64_t frameId;           // Matches VkVideoEncodeInputFrame::frameId
    uint64_t pts;               // Pass-through from input
    uint64_t dts;               // Decode timestamp (encoder-assigned)

    // Bitstream
    const uint8_t* pBitstreamData; // Pointer to encoded data (valid until next GetEncodedFrame)
    uint32_t bitstreamSize;     // Size in bytes

    // Frame info
    uint32_t pictureType;       // 0=I, 1=P, 2=B
    VkBool32 isIDR;
    uint32_t temporalLayerId;

    // Encode status
    VkResult status;            // VK_SUCCESS or error
};

//=============================================================================
// Extended Encoder Interface
//
// Extends VulkanVideoEncoder with external frame input and sync support.
// The base VulkanVideoEncoder methods (Initialize, EncodeNextFrame, etc.)
// remain for backward compatibility with file-based encoding.
//=============================================================================
class VulkanVideoEncoderExt : public VulkanVideoEncoder {
public:
    // Initialize with structured config (alternative to argc/argv)
    virtual VkResult InitializeExt(const VkVideoEncoderConfig& config) = 0;

    // Submit an externally-provided frame for encoding.
    // The encoder will:
    //   1. Wait on the input frame's wait semaphores
    //   2. If format conversion is needed, run the compute filter
    //   3. Encode the frame
    //   4. Signal the input frame's signal semaphores (frame consumed)
    //
    // This is non-blocking: the frame is queued for encoding.
    // Call GetEncodedFrame() to retrieve the bitstream.
    //
    // Returns VK_SUCCESS if the frame was accepted for encoding.
    // Returns VK_NOT_READY if the encoder's internal queue is full (try again later).
    virtual VkResult SubmitExternalFrame(const VkVideoEncodeInputFrame& frame) = 0;

    // Retrieve the next completed encoded frame.
    // Returns VK_SUCCESS and fills 'result' if a frame is ready.
    // Returns VK_NOT_READY if no frames are ready yet.
    //
    // The pBitstreamData pointer in result is valid until the next call
    // to GetEncodedFrame() or Flush().
    virtual VkResult GetEncodedFrame(VkVideoEncodeResult& result) = 0;

    // Flush: encode all pending frames and make their bitstreams available.
    // Blocks until all pending frames are encoded.
    virtual VkResult Flush() = 0;

    // Dynamic reconfiguration (e.g. bitrate change mid-stream).
    // Only rate control parameters can be changed without session reset.
    virtual VkResult Reconfigure(const VkVideoEncoderConfig& config) = 0;

    // Query encoder capabilities for the configured codec.
    // Can be called before InitializeExt() to check support.
    virtual VkBool32 SupportsFormat(VkFormat inputFormat) const = 0;
    virtual uint32_t GetMaxWidth() const = 0;
    virtual uint32_t GetMaxHeight() const = 0;
};

// Factory function for the extended encoder interface
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult CreateVulkanVideoEncoderExt(
    VkSharedBaseObj<VulkanVideoEncoderExt>& vulkanVideoEncoder);

#endif /* _VULKAN_VIDEO_ENCODER_EXT_H_ */
