/*
 * Copyright 2022 NVIDIA Corporation.
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

#include <functional>
#include "VkCodecUtils/VkEncoderStdioLatch.h"
#include <atomic>
#include <vector>
#include <cmath>
#include <cinttypes>  // For PRIu64, PRId64
#include <cstdio>
#include <fstream>
#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkVideoEncoder/VkVideoEncoderPsnr.h"
#include "VkVideoCore/VulkanVideoCapabilities.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"
#include "VkVideoEncoder/VkEncoderConfigH264.h"
#include "VkVideoEncoder/VkEncoderConfigH265.h"
#include "VkVideoEncoder/VkEncoderConfigAV1.h"
#include "VkVideoEncoder/VkVideoEncoderOsAdapterLinux.h"
#include "VkCodecUtils/YCbCrConvUtilsCpu.h"
#include "VkCodecUtils/VkVideoCrc.h"
#ifdef NV_AQ_GPU_LIB_SUPPORTED
#include "VulkanAqProcessor.h"
#endif // NV_AQ_GPU_LIB_SUPPORTED

VkResult VkVideoEncoder::RequestRateControlUpdate(uint64_t averageBitrate,
                                                  uint64_t maxBitrate,
                                                  uint32_t frameRateNumerator,
                                                  uint32_t frameRateDenominator)
{
    // Producer side of Reconfigure: callable from any thread (the ext
    // Reconfigure entry runs on the caller's sequence). Values are folded
    // into the live rate-control state on the encoder thread.
    if (averageBitrate == 0) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    std::lock_guard<std::mutex> lock(m_pendingRateControlMutex);
    m_pendingRateControlUpdate.averageBitrate = averageBitrate;
    m_pendingRateControlUpdate.maxBitrate =
        (maxBitrate != 0) ? maxBitrate : averageBitrate;
    m_pendingRateControlUpdate.frameRateNumerator = frameRateNumerator;
    m_pendingRateControlUpdate.frameRateDenominator = frameRateDenominator;
    m_pendingRateControlArmed = true;
    return VK_SUCCESS;
}

void VkVideoEncoder::ApplyPendingRateControlUpdate()
{
    PendingRateControlUpdate update;
    {
        std::lock_guard<std::mutex> lock(m_pendingRateControlMutex);
        if (!m_pendingRateControlArmed) {
            return;
        }
        update = m_pendingRateControlUpdate;
        m_pendingRateControlArmed = false;
    }
    for (uint32_t i = 0; i < ARRAYSIZE(m_rateControlLayersInfo); i++) {
        m_rateControlLayersInfo[i].averageBitrate = update.averageBitrate;
        m_rateControlLayersInfo[i].maxBitrate = update.maxBitrate;
        if (update.frameRateNumerator != 0) {
            m_rateControlLayersInfo[i].frameRateNumerator =
                update.frameRateNumerator;
            m_rateControlLayersInfo[i].frameRateDenominator =
                (update.frameRateDenominator != 0)
                    ? update.frameRateDenominator
                    : 1;
        }
    }
    // The next frame-record emits VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL
    // with the refreshed values (the consume immediately follows this call).
    m_sendRateControlCmd = true;
}

// Backpressure bound for un-drained captured bitstreams; generous relative
// to the 8-deep assembly pipeline, so it only fires when the consumer has
// genuinely stopped draining.
static constexpr size_t kMaxUnclaimedCapturedBitstreams = 64;

bool VkVideoEncoder::CanAcceptNewInputFrame() const
{
    if (m_asyncAssemblyEnabled && (m_assemblyQueueCapacity > 0)) {
        // One submit can flush a whole reordered run into the assembly queue;
        // leave room for the burst so the producer-side Push never reaches its
        // condition-variable wait. NOTE the qualifier: that promise holds for
        // the EXT path, which gates on this function. The CLI/file path calls
        // EnqueueFrame directly with no admission check and can still block in
        // VkThreadSafeQueue's producer wait -- it has no non-blocking contract.
        //
        // GetMaxAssemblyBurst() is virtual because AV1 splices an extra
        // show_existing_frame node per reordered insert.
        const size_t burst = GetMaxAssemblyBurst();
        if ((m_assemblyQueue.Size() + burst) > m_assemblyQueueCapacity) {
            return false;
        }
    }
    {
        // Bound instead of unbounded deque growth when the consumer stops
        // draining captured bitstreams.
        std::lock_guard<std::mutex> lock(m_capturedBitstreamsMutex);
        if (m_capturedBitstreams.size() >= kMaxUnclaimedCapturedBitstreams) {
            return false;
        }
    }
    return true;
}

VkResult VkVideoEncoder::CreateVideoEncoder(const VulkanDeviceContext* vkDevCtx,
                                            VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                            VkSharedBaseObj<VkVideoEncoder>& encoder)
{
    if (encoderConfig->codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) {
        return CreateVideoEncoderH264(vkDevCtx, encoderConfig, encoder);
    } else if (encoderConfig->codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR) {
        return CreateVideoEncoderH265(vkDevCtx, encoderConfig, encoder);
    } else if (encoderConfig->codec == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) {
        return CreateVideoEncoderAV1(vkDevCtx, encoderConfig, encoder);
    }
    return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
}

const uint8_t* VkVideoEncoder::setPlaneOffset(const uint8_t* pFrameData, size_t bufferSize, size_t &currentReadOffset)
{
    const uint8_t* buf = pFrameData + currentReadOffset;
    currentReadOffset += bufferSize;
    return buf;
}

VkResult VkVideoEncoder::SelectDrmFormatModifier(
    VkSharedBaseObj<EncoderConfig>& encoderConfig,
    VkFormat format, VkImageUsageFlags usage, const VkExtent2D& imageExtent)
{
    // The modifier machinery is OS-conditional, so it lives in the
    // separately-compiled OS adapter -- this file carries no OS-specific
    // code. A platform with no adapter arm reports
    // VK_ERROR_FEATURE_NOT_PRESENT rather than selecting anything.
    (void)usage; (void)imageExtent;
    uint64_t selected = 0;
    VkResult result = vkenc::OsSelectDrmFormatModifier(
        m_vkDevCtx, format, encoderConfig->drmFormatModifierIndex, &selected);
    if (result != VK_SUCCESS) {
        return result;
    }
    encoderConfig->selectedDrmFormatModifier = selected;
    return VK_SUCCESS;
}

VkResult VkVideoEncoder::LoadNextQpMapFrameFromFile(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    if ((m_encoderConfig->enableQpMap == VK_FALSE) || (!m_encoderConfig->qpMapFileHandler.HandleIsValid()))  {
        return VK_SUCCESS;
    }

    VkSharedBaseObj<VulkanVideoImagePoolNode>& srcQpMapResource = ((m_qpMapTiling != VK_IMAGE_TILING_LINEAR)) ?
                                                                    encodeFrameInfo->srcQpMapStagingResource :
                                                                    encodeFrameInfo->srcQpMapImageResource;

    VkSharedBaseObj<VulkanVideoImagePool>& qpMapImagePool = ((m_qpMapTiling != VK_IMAGE_TILING_LINEAR)) ?
                                                               m_linearQpMapImagePool : m_qpMapImagePool;

    // If srcQpMapStagingImageView is valid at this point, it means that the client had provided
    // the QpMap image.
    if (srcQpMapResource == nullptr) {
        bool success = qpMapImagePool->GetAvailableImage(srcQpMapResource,
                                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        assert(success);
        if (!success) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        assert(srcQpMapResource != nullptr);

        VkSharedBaseObj<VkImageResourceView> linearQpMapImageView;
        srcQpMapResource->GetImageView(linearQpMapImageView);

        const VkSharedBaseObj<VkImageResource>& dstQpMapImageResource = linearQpMapImageView->GetImageResource();
        VkSharedBaseObj<VulkanDeviceMemoryImpl> srcQpMapImageDeviceMemory(dstQpMapImageResource->GetMemory());

        // Map the image and read the image data.
        VkDeviceSize qpMapImageOffset = dstQpMapImageResource->GetImageDeviceMemoryOffset();
        VkDeviceSize qpMapMaxSize = 0;
        uint8_t* writeQpMapImagePtr = srcQpMapImageDeviceMemory->GetDataPtr(qpMapImageOffset, qpMapMaxSize);
        assert(writeQpMapImagePtr != nullptr);

        const VkFormatDesc* pFormatDesc = vkFormatLookUp(m_imageQpMapFormat);
        size_t formatTexelSize = (pFormatDesc != nullptr) ? pFormatDesc->numberOfBytes : 1;
        uint32_t inputQpMapWidth = (m_encoderConfig->input.width + m_qpMapTexelSize.width - 1) / m_qpMapTexelSize.width;
        uint32_t qpMapWidth = (m_encoderConfig->encodeWidth + m_qpMapTexelSize.width - 1) / m_qpMapTexelSize.width;
        uint32_t qpMapHeight = (m_encoderConfig->encodeHeight + m_qpMapTexelSize.height - 1) / m_qpMapTexelSize.height;
        uint64_t qpMapFileOffset = qpMapWidth * qpMapHeight * encodeFrameInfo->frameInputOrderNum * formatTexelSize;
        const uint8_t* pQpMapData = m_encoderConfig->qpMapFileHandler.GetMappedPtr(qpMapFileOffset);

        const VkSubresourceLayout* dstQpMapSubresourceLayout = dstQpMapImageResource->GetSubresourceLayout();

        for (uint32_t j = 0; j < qpMapHeight; j++) {
            memcpy(writeQpMapImagePtr + (dstQpMapSubresourceLayout[0].offset + j * dstQpMapSubresourceLayout[0].rowPitch),
                   pQpMapData + j * inputQpMapWidth * formatTexelSize, qpMapWidth * formatTexelSize);
        }
    }

    return VK_SUCCESS;
}

// 1. Load current input frame from file
// 2. Convert yuv image to nv12 (TODO: switch to Vulkan compute next, instead of using the CPU for that)
// 3. Copy the nv12 input linear image to the optimal input image
// 4. Load qp map from file
// 5. Copy linear image to the optimal image
VkResult VkVideoEncoder::LoadNextFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    assert(encodeFrameInfo);

    encodeFrameInfo->frameInputOrderNum = m_inputFrameNum++;
    encodeFrameInfo->lastFrame = !(encodeFrameInfo->frameInputOrderNum < (m_encoderConfig->numFrames - 1));

    if ((m_encoderConfig->enableQpMap == VK_TRUE) && m_encoderConfig->qpMapFileHandler.HandleIsValid()) {

        VkResult result = LoadNextQpMapFrameFromFile(encodeFrameInfo);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    if (encodeFrameInfo->srcStagingImageView == nullptr) {
        bool success = m_linearInputImagePool->GetAvailableImage(encodeFrameInfo->srcStagingImageView,
                                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        assert(success);
        if (!success) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        assert(encodeFrameInfo->srcStagingImageView != nullptr);
    }

    VkSharedBaseObj<VkImageResourceView> linearInputImageView;
    encodeFrameInfo->srcStagingImageView->GetImageView(linearInputImageView);

    const VkSharedBaseObj<VkImageResource>& dstImageResource = linearInputImageView->GetImageResource();
    VkSharedBaseObj<VulkanDeviceMemoryImpl> srcImageDeviceMemory(dstImageResource->GetMemory());

    // Map the image and read the image data.
    VkDeviceSize imageOffset = dstImageResource->GetImageDeviceMemoryOffset();
    VkDeviceSize maxSize = 0;

    uint8_t* writeImagePtr = srcImageDeviceMemory->GetDataPtr(imageOffset, maxSize);
    assert(writeImagePtr != nullptr);

    // AdvanceFrameOffset() assumes we increment the frame counter, i.e. m_inputFrameNum++
    const size_t frameOffset = m_encoderConfig->inputFileHandler.GetCurrFrameOffset();
    m_encoderConfig->inputFileHandler.AdvanceFrameOffset(frameOffset);
    const uint8_t* pInputFrameData = m_encoderConfig->inputFileHandler.GetMappedPtr(frameOffset);

    // NOTE: Get image layout
    const VkSubresourceLayout* dstSubresourceLayout = dstImageResource->GetSubresourceLayout();

    // Direct plane copy - no color space conversion needed
    CopyYCbCrPlanesDirectCPU(
            pInputFrameData,                                               // Source buffer
            m_encoderConfig->input.planeLayouts,                           // Source layouts
            writeImagePtr,                                                 // Destination buffer
            dstSubresourceLayout,                                          // Destination layouts
            std::min(m_encoderConfig->encodeWidth, m_encoderConfig->input.width),    // Width
            std::min(m_encoderConfig->encodeHeight, m_encoderConfig->input.height),  // Height
            m_encoderConfig->input.numPlanes,                              // Number of planes
            m_encoderConfig->input.vkFormat);                              // Format for subsampling detection

    if (m_psnr && m_psnr->Enabled()) {
        m_psnr->CaptureInput(encodeFrameInfo.get(), pInputFrameData);
    }

    // Now stage the input frame for the encoder video input
    return StageInputFrame(encodeFrameInfo);
}

VkResult VkVideoEncoder::StageInputFrameQpMap(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                              VkCommandBuffer cmdBuf)
{

    if (m_encoderConfig->enableQpMap == VK_FALSE) {
        return VK_SUCCESS;
    }

    const bool useDedicatedCommandBuf = (cmdBuf == VK_NULL_HANDLE);

    if (encodeFrameInfo->srcQpMapImageResource == nullptr) {
        bool success = m_qpMapImagePool->GetAvailableImage(encodeFrameInfo->srcQpMapImageResource,
                                                           VK_IMAGE_LAYOUT_VIDEO_ENCODE_QUANTIZATION_MAP_KHR);
        assert(success);
        assert(encodeFrameInfo->srcQpMapImageResource != nullptr);
        if (!success || encodeFrameInfo->srcQpMapImageResource == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    if (useDedicatedCommandBuf) {
        assert(m_inputCommandBufferPool != nullptr);
        m_inputCommandBufferPool->GetAvailablePoolNode(encodeFrameInfo->qpMapCmdBuffer);
        assert(encodeFrameInfo->qpMapCmdBuffer != nullptr);

        // Make sure command buffer is not in use anymore and reset
        encodeFrameInfo->qpMapCmdBuffer->ResetCommandBuffer(true, "encoderStagedInputFence");

        // Begin command buffer
        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        cmdBuf = encodeFrameInfo->qpMapCmdBuffer->BeginCommandBufferRecording(beginInfo);
    }

    assert(cmdBuf != VK_NULL_HANDLE);

    VkSharedBaseObj<VkImageResourceView> linearQpMapImageView;
    encodeFrameInfo->srcQpMapStagingResource->GetImageView(linearQpMapImageView);

    VkSharedBaseObj<VkImageResourceView> srcQpMapImageView;
    encodeFrameInfo->srcQpMapImageResource->GetImageView(srcQpMapImageView);

    VkImageLayout linearQpMapImgNewLayout = TransitionImageLayout(cmdBuf, linearQpMapImageView, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageLayout srcQpMapImgNewLayout = TransitionImageLayout(cmdBuf, srcQpMapImageView, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    (void)linearQpMapImgNewLayout;
    (void)srcQpMapImgNewLayout;

    VkExtent2D copyImageExtent {
        (std::min(m_encoderConfig->encodeWidth,  m_encoderConfig->input.width) + m_qpMapTexelSize.width - 1) / m_qpMapTexelSize.width,
        (std::min(m_encoderConfig->encodeHeight, m_encoderConfig->input.height) + m_qpMapTexelSize.height - 1) / m_qpMapTexelSize.height
    };

    CopyLinearToLinearImage(cmdBuf, linearQpMapImageView, srcQpMapImageView, copyImageExtent);

    if (useDedicatedCommandBuf) {
        VkResult result = VK_SUCCESS;
        result = encodeFrameInfo->qpMapCmdBuffer->EndCommandBufferRecording(cmdBuf);
        if (result != VK_SUCCESS) {
            return result;
        }

        // Now submit the staged input to the queue
        return SubmitStagedQpMap(encodeFrameInfo);
    }

    return VK_SUCCESS;
}

VkResult VkVideoEncoder::EncodeFrameCommon(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    encodeFrameInfo->constQp = m_encoderConfig->constQp;

    // A per-frame quantizer replaces the session's constant QP for this frame
    // only. This has to happen after the copy above, which is unconditional --
    // a value written anywhere earlier would be silently overwritten, which is
    // exactly how this field came to be dead.
    //
    // Refused outside constant-QP mode: in CBR/VBR the rate controller owns
    // QP, and an override there yields a stream fighting its own bitrate
    // target rather than the one the caller asked for.
    if (encodeFrameInfo->qpOverrideOnInput >= 0) {
        if (m_encoderConfig->rateControlMode ==
            VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
            const int32_t qp = encodeFrameInfo->qpOverrideOnInput;
            // One value names the frame, so it applies whichever slice type
            // this frame turns out to be.
            encodeFrameInfo->constQp.qpIntra  = qp;
            encodeFrameInfo->constQp.qpInterP = qp;
            encodeFrameInfo->constQp.qpInterB = qp;
        } else {
            // ONCE PER PROCESS, and "once" has to be true rather
            // than likely: independent sessions reach this branch
            // concurrently, and a plain check-then-store lets two
            // of them both read false and both print. The flag
            // arbitrates emission and publishes nothing else, so
            // relaxed ordering is the entire requirement. It does
            // not rely on stdio locking, on per-session
            // serialization, or on the output being suppressed.
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true, std::memory_order_relaxed)) {
                VkEncErr() << "[Encoder] per-frame qpOverride ignored: this "
                              "session's rate-control mode is not DISABLED, "
                              "so the encoder owns QP." << std::endl;
            }
        }
    }

    assert(encodeFrameInfo);
    assert(m_encoderConfig);
    assert(encodeFrameInfo->srcEncodeImageResource);

    encodeFrameInfo->videoSession = m_videoSession;
    encodeFrameInfo->videoSessionParameters = m_videoSessionParameters;
    encodeFrameInfo->qualityLevel = m_encoderConfig->qualityLevel;

    encodeFrameInfo->frameEncodeInputOrderNum = m_encodeInputFrameNum++;

    // GetPositionInGOP() method returns display position of the picture relative to last key frame picture.
    // A caller-forced mid-stream IDR (forceIdrOnInput) takes the same
    // "start a new IDR sequence" branch as the first frame / a periodic
    // idrPeriod boundary: pictureType becomes FRAME_TYPE_IDR and the GOP
    // state machine restarts at this frame, so all downstream IDR handling
    // (DPB flush, idr_pic_id, deferred-queue preflush, header emission)
    // follows the normal IDR path.
    const bool startNewIdrSequence =
        (encodeFrameInfo->frameEncodeInputOrderNum == 0) ||
        encodeFrameInfo->forceIdrOnInput;
    const bool isIdr = m_encoderConfig->gopStructure.GetPositionInGOP(m_gopState,
                                                                encodeFrameInfo->gopPosition,
                                                                startNewIdrSequence,
                                                                uint32_t(m_encoderConfig->numFrames - encodeFrameInfo->frameEncodeInputOrderNum));
    if (isIdr) {
        assert(encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR);
    }
    const bool isReference = m_encoderConfig->gopStructure.IsFrameReference(encodeFrameInfo->gopPosition);

    // and encode the input frame with the encoder next
    VkResult result = EncodeFrame(encodeFrameInfo);
    if (result != VK_SUCCESS) {
        assert(!"EncodeFrame error!!!");
        return result;
    }

    // Handles the generic portion of the control command, if enabled
    if (HandleCtrlCmd(encodeFrameInfo)) {
        // Handles the codec-specific rate control command if enabled
        CodecHandleRateControlCmd(encodeFrameInfo);
    }

    if (m_encoderConfig->enableQpMap) {
        ProcessQpMap(encodeFrameInfo);
    }

    const bool isIntraRefreshFrame = m_encoderConfig->gopStructure.IsIntraRefreshFrame(encodeFrameInfo->gopPosition);
    if (m_encoderConfig->enableIntraRefresh && isIntraRefreshFrame) {
        FillIntraRefreshInfo(encodeFrameInfo);
    }

    // NOTE: dstBuffer resource acquisition can be deferred at the last moment before submit
    VkDeviceSize size = GetBitstreamBuffer(encodeFrameInfo->outputBitstreamBuffer);
    assert((size > 0) && (encodeFrameInfo->outputBitstreamBuffer != nullptr));
    if ((size == 0) || (encodeFrameInfo->outputBitstreamBuffer == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    encodeFrameInfo->encodeInfo.dstBuffer = encodeFrameInfo->outputBitstreamBuffer->GetBuffer();
    encodeFrameInfo->encodeInfo.dstBufferOffset = 0;
    // Frame infos are POOL-RECYCLED (m_frameInfoBuffersQueue)
    // and Reset() clears bitstreamHeaderBufferSize but NOT this encodeInfo
    // field -- without this re-zero, a non-IDR frame recycled through a
    // pool node that previously carried an IDR would keep debiting the
    // stale header bytes from the RC budget with zero actual prepended
    // bytes. Zero it unconditionally beside dstBufferOffset; the gate
    // below re-fills both for real capture-mode IDRs.
    encodeFrameInfo->encodeInfo.precedingExternallyEncodedBytes = 0;

    // Local patch; not in upstream vk_video_samples. Stop lying to the
    // driver's rate controller about the app-prepended per-IDR headers. In
    // capture mode (disableFileOutput -- the Chromium in-memory bitstream
    // path) the codec-specific EncodeFrame() above filled
    // bitstreamHeaderBuffer with SPS/PPS (H.264) / VPS/SPS/PPS (H.265) for
    // EVERY IDR, and WriteBitstreamToFile() prepends those bytes to the
    // emitted chunk CPU-side -- but the RC never saw them, so every IDR
    // overshoots its frame budget by the header size (~40-60 B/IDR,
    // compounded by short GOPs). Therefore:
    //   * reserve the header bytes in the bitstream buffer via
    //     dstBufferOffset (aligned up to the driver's
    //     minBitstreamBufferOffsetAlignment), and
    //   * report them via precedingExternallyEncodedBytes so the RC debits
    //     this frame's budget.
    // The encode-feedback query's bitstreamStartOffset is defined RELATIVE
    // to dstBufferOffset, so every readback site adds
    // encodeInfo.dstBufferOffset back (no-op while the offset is 0).
    // Scope nuance: this reservation covers
    // app-prepended H.264/H.265 headers only -- AV1's 2-byte temporal
    // delimiter is outside it and rides its own codec-specific
    // assembly path, so AV1 is deliberately NOT gated here.
    if ((m_encoderConfig->disableFileOutput != 0) &&
        (encodeFrameInfo->bitstreamHeaderBufferSize > 0) &&
        ((m_encoderConfig->codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) ||
         (m_encoderConfig->codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR))) {
        const VkDeviceSize headerBytes = encodeFrameInfo->bitstreamHeaderBufferSize;
        VkDeviceSize offsetAlignment =
            m_encoderConfig->videoCapabilities.minBitstreamBufferOffsetAlignment;
        if (offsetAlignment == 0) {
            offsetAlignment = 1;
        }
        encodeFrameInfo->encodeInfo.dstBufferOffset =
            ((headerBytes + offsetAlignment - 1) / offsetAlignment) * offsetAlignment;
        encodeFrameInfo->encodeInfo.precedingExternallyEncodedBytes =
            (uint32_t)headerBytes;
    }

#ifdef NV_AQ_GPU_LIB_SUPPORTED
    if (m_aqAnalyzes) {
        // Use the interface directly - no need for casting
        const AqHwConfig* pCtxConfig = static_cast<const AqHwConfig*>(m_aqAnalyzes->GetConfig());

        uint32_t prepareFlags = 0; /* AqProcessor::SLOT_PREPARE_CPU_UPLOAD |
                                      AqProcessor::SLOT_PREPARE_GPU_PREPROCESS |
                                      AqProcessor::SLOT_PREPARE_SUBSAMPLING; */
        if (pCtxConfig->enableTemporalAQ) {
            prepareFlags |= AqProcessor::SLOT_PREPARE_TEMPORAL;

            // Add reference requirements based on frame type
            if (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_P) {
                // P-frame needs previous reference
                prepareFlags |= AqProcessor::SLOT_PREPARE_NEEDS_PREV_REF;
            } else if (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B) {
                // B-frame needs both
                prepareFlags |= AqProcessor::SLOT_PREPARE_NEEDS_PREV_REF | AqProcessor::SLOT_PREPARE_NEEDS_NEXT_REF;
            }
        }
        if (pCtxConfig->enableSpatialAQ) {
            prepareFlags |= AqProcessor::SLOT_PREPARE_SPATIAL;
        }

        // The encoder already keeps a reference to aqPendingTemporalBiDiSlot in the context of the previous frame.
        std::shared_ptr<AqProcessor> aqPendingTemporalBiDiSlot;
        // Check if we have a pending slot from a previous frame that needs deferred temporal processing
        VkEncPrintfOut("[ProcessFrame] Calling FindFreeBuffer with flags=0x%x\n", prepareFlags);
        encodeFrameInfo->aqProcessorSlot =
                m_aqAnalyzes->FindFreeAqProcessorSlot(prepareFlags,
                                                      pCtxConfig->codecType,
                                                      pCtxConfig->width,
                                                      pCtxConfig->height,
                                                      pCtxConfig->bitDepth,
                                                      pCtxConfig->chromaFormat,
                                                      ((m_encoderConfig->numFrames -
                                                              encodeFrameInfo->frameEncodeInputOrderNum) == 1),
                                                      aqPendingTemporalBiDiSlot);

        if (encodeFrameInfo->aqProcessorSlot == nullptr) {
            VkEncPrintfOut("[ProcessFrame] ERROR: FindFreeBuffer returned nullptr\n");
            return VK_ERROR_OUT_OF_POOL_MEMORY;
        }
        VkEncPrintfOut("[ProcessFrame] Slot allocated, %p\n", encodeFrameInfo->aqProcessorSlot.get());

        encodeFrameInfo->aqProcessorSlot->UpdateGop(encodeFrameInfo->frameEncodeInputOrderNum, encodeFrameInfo->gopPosition, isIdr);

        VkSharedBaseObj<VulkanVideoImagePoolNode> videoInputImage; // not needed
        VkSemaphoreSubmitInfoKHR* pWaitSemaphoreInfo = nullptr;
        uint32_t waitSemaphoreInfoCount = 0;
        VkSemaphoreSubmitInfoKHR waitSemaphoreInfo;
        // wait on the input filter
        if (encodeFrameInfo->inputCmdBuffer) {
            waitSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
            waitSemaphoreInfo.semaphore = encodeFrameInfo->inputCmdBuffer->GetSemaphore();
            waitSemaphoreInfo.value = 0; // Binary semaphore
            // Use transfer bit since these semaphores come from transfer operations
            waitSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
            waitSemaphoreInfo.deviceIndex = 0;
            waitSemaphoreInfoCount++;
            pWaitSemaphoreInfo = &waitSemaphoreInfo;
        }

        if (encodeFrameInfo->srcQpMapImageResource == nullptr) {
            bool success = m_qpMapImagePool->GetAvailableImage(encodeFrameInfo->srcQpMapImageResource,
                                                               VK_IMAGE_LAYOUT_VIDEO_ENCODE_QUANTIZATION_MAP_KHR);
            assert(success);
            assert(encodeFrameInfo->srcQpMapImageResource != nullptr);
            if (!success || encodeFrameInfo->srcQpMapImageResource == nullptr) {
                assert(!"Can't get get srcQpMapImageResource!");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        const VkSemaphoreSubmitInfoKHR* pSignalSemaphoreInfos = nullptr; // not needed
        uint32_t signalSemaphoreInfosCount = 0; // not needed
        VkFence* pSignalFence = nullptr; // not needed if no debugging is needed.

        VulkanAqProcessor* pVulkanAqProcessorSlot = (VulkanAqProcessor*)encodeFrameInfo->aqProcessorSlot.get();
        int ret = pVulkanAqProcessorSlot->ProcessAq(nullptr, // Don't copy the data to srcStagingImageView
                                                    encodeFrameInfo->frameEncodeInputOrderNum,
                                                    aqPendingTemporalBiDiSlot,
                                                    encodeFrameInfo->srcStagingImageView, // for debugging only
                                                    videoInputImage,
                                                    encodeFrameInfo->subsampledImageResource,
                                                    encodeFrameInfo->srcQpMapImageResource,
                                                    pWaitSemaphoreInfo,
                                                    waitSemaphoreInfoCount,
                                                    pSignalSemaphoreInfos,
                                                    signalSemaphoreInfosCount,
                                                    pSignalFence);
        if (ret != 0) {
            assert(!"Failed ProcessAq()!!!");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
#endif // NV_AQ_GPU_LIB_SUPPORTED

    // The frame is recorded and submitted inside this call whenever the
    // enqueue flushes, so the enqueue's result is this function's result.
    // Answering VK_SUCCESS regardless would leave a frame that never became
    // bitstream indistinguishable from one that did.
    return EnqueueFrame(encodeFrameInfo, isIdr, isReference);
}

VkResult VkVideoEncoder::WrapExternalImage(
    VkImage image, VkDeviceMemory memory,
    VkFormat format, uint32_t width, uint32_t height,
    VkImageTiling tiling,
    VkSharedBaseObj<VulkanVideoImagePoolNode>& outNode)
{
    VkImageCreateInfo imageCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCI.imageType = VK_IMAGE_TYPE_2D;
    imageCI.format = format;
    imageCI.extent = {width, height, 1};
    imageCI.mipLevels = 1;
    imageCI.arrayLayers = 1;
    imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCI.tiling = tiling;
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);
    // LINEAR imports are never directly encodable (SetExternalInputFrame
    // routes them through the staging copy path): the only operation the
    // encoder performs on them is a vkCmdCopyImage to an OPTIMAL pool image.
    // Describe the wrapper with the usage/flags a minimal staging source is
    // actually created with (plain TRANSFER_SRC, no create flags). The
    // fabricated encode/storage usage + MUTABLE_FORMAT flags below would
    // make the views violate VUID-VkImageViewCreateInfo-image-04441,
    // VUID-VkImageViewCreateInfo-pNext-02662 and
    // VUID-VkImageViewCreateInfo-usage-08336, because LINEAR YCbCr formats
    // do not expose the VIDEO_ENCODE_SRC / STORAGE format features, and the
    // external allocator did not set VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT.
    const bool isLinearStagingSource = (tiling == VK_IMAGE_TILING_LINEAR);
    if (isLinearStagingSource) {
        imageCI.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    } else {
        imageCI.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                      | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                      | VK_IMAGE_USAGE_SAMPLED_BIT
                      | VK_IMAGE_USAGE_STORAGE_BIT
                      | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
        if (mpInfo) {
            imageCI.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
                          | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
                          | VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
        }
    }

    VkSharedBaseObj<VkImageResource> imageResource;
    VkResult result = VkImageResource::CreateFromExternal(m_vkDevCtx, image, memory,
                                                          &imageCI, imageResource);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkImageSubresourceRange subresRange{};
    subresRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresRange.levelCount = 1;
    subresRange.layerCount = 1;

    VkSharedBaseObj<VkImageResourceView> imageView;
    if (isLinearStagingSource) {
        // Transfer-only staging source: an image with only TRANSFER usage is
        // not view-compatible (VUID-VkImageViewCreateInfo-image-04441), and
        // nothing in the staging copy path (TransitionImageLayout +
        // CopyLinearToOptimalImage) consumes a VkImageView -- both use the
        // raw VkImage handle. Create a view-less wrapper.
        result = VkImageResourceView::Create(
            m_vkDevCtx, imageResource, subresRange, imageView);
        if (result != VK_SUCCESS) {
            VkEncErr() << "[WrapExternalImage] view creation failed: "
                       << result << std::endl;
            return result;
        }
        return VulkanVideoImagePoolNode::CreateExternal(
            m_vkDevCtx, imageView, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, outNode);
    }
    if (mpInfo) {
        // Multiplanar: the combined NV12 view needs VIDEO_ENCODE_SRC + TRANSFER
        // (no SAMPLED — that would require a YCbCr conversion).
        // Per-plane views need STORAGE + SAMPLED + TRANSFER for compute.
        VkImageUsageFlags planeUsage = VK_IMAGE_USAGE_STORAGE_BIT
                                     | VK_IMAGE_USAGE_SAMPLED_BIT
                                     | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImageUsageFlags combinedUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR
                                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        result = VkImageResourceView::Create(m_vkDevCtx, imageResource, subresRange,
                                             planeUsage, VK_NULL_HANDLE,
                                             combinedUsage, imageView);
    } else {
        result = VkImageResourceView::Create(m_vkDevCtx, imageResource, subresRange, imageView);
    }
    if (result != VK_SUCCESS) {
        VkEncPrintfErr("[WrapExternalImage] VkImageResourceView::Create failed: %d\n", result);
        return result;
    }
    if (result != VK_SUCCESS) {
        return result;
    }

    result = VulkanVideoImagePoolNode::CreateExternal(
        m_vkDevCtx, imageView, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, outNode);

    return result;
}

void VkVideoEncoder::StampExternalFrameInfo(
    VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
    VkImageLayout srcImageCurrentLayout,
    uint64_t frameId,
    uint64_t pts,
    bool isLastFrame,
    bool forceIdr,
    int32_t qpOverride,
    ExternalInputResidency residency,
    uint32_t waitSemaphoreCount,
    const VkSemaphore* pWaitSemaphores,
    const uint64_t* pWaitSemaphoreValues,
    const VkPipelineStageFlags2* pWaitDstStageMasks,
    uint32_t signalSemaphoreCount,
    const VkSemaphore* pSignalSemaphores,
    const uint64_t* pSignalSemaphoreValues)
{
    // =============================================
    // 1. Replicate LoadNextFrame() bookkeeping
    // =============================================
    encodeFrameInfo->frameInputOrderNum = m_inputFrameNum++;
    encodeFrameInfo->lastFrame = isLastFrame;
    encodeFrameInfo->inputTimeStamp = pts;

    // =============================================
    // 2. Store external sync info and source layout
    // =============================================
    encodeFrameInfo->isExternalInput = true;
    encodeFrameInfo->srcExternalImageLayout = srcImageCurrentLayout;
    // Keep the CALLER's frame id on the node; the captured-bitstream
    // FIFO is keyed by it (see WriteBitstreamToFile), not by the internal
    // encode-input counter, so a partially failed submission or a future
    // reordering GOP cannot desynchronize capture routing.
    encodeFrameInfo->externalFrameId = frameId;
    // Latch the caller's mid-stream IDR request for EncodeFrameCommon.
    encodeFrameInfo->forceIdrOnInput = forceIdr;
    encodeFrameInfo->qpOverrideOnInput = qpOverride;
    // Latch the caller-declared queue-family ownership for
    // StageInputFrame's barrier construction.
    encodeFrameInfo->externalInputResidency = residency;

    encodeFrameInfo->inputWaitSemaphores.clear();
    encodeFrameInfo->inputWaitSemaphoreValues.clear();
    // With no caller-provided masks this vector stays EMPTY, and each
    // submission that injects these waits falls back to the stage of its
    // own consuming operation: TRANSFER on the staging-copy submit
    // (SubmitStagedInputFrame), VIDEO_ENCODE on the direct encode submit
    // (SubmitVideoCodingCmds). A single stored default cannot be right for
    // both -- TRANSFER on the encode submit leaves vkCmdEncodeVideoKHR
    // outside the wait's scope, so the encode could read the input before
    // the producer signaled, and VIDEO_ENCODE is not supported on a staging
    // submit routed to a dedicated TRANSFER queue.
    encodeFrameInfo->inputWaitDstStageMasks.clear();
    for (uint32_t i = 0; i < waitSemaphoreCount; i++) {
        encodeFrameInfo->inputWaitSemaphores.push_back(pWaitSemaphores[i]);
        encodeFrameInfo->inputWaitSemaphoreValues.push_back(
            pWaitSemaphoreValues ? pWaitSemaphoreValues[i] : 0);
        if (pWaitDstStageMasks != nullptr) {
            encodeFrameInfo->inputWaitDstStageMasks.push_back(
                pWaitDstStageMasks[i]);
        }
    }

    encodeFrameInfo->inputSignalSemaphores.clear();
    encodeFrameInfo->inputSignalSemaphoreValues.clear();
    for (uint32_t i = 0; i < signalSemaphoreCount; i++) {
        encodeFrameInfo->inputSignalSemaphores.push_back(pSignalSemaphores[i]);
        encodeFrameInfo->inputSignalSemaphoreValues.push_back(
            pSignalSemaphoreValues ? pSignalSemaphoreValues[i] : 0);
    }
}

VkResult VkVideoEncoder::SetExternalInputFrame(
    VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
    VkImage externalImage,
    VkDeviceMemory externalMemory,
    VkFormat format,
    uint32_t width, uint32_t height,
    VkImageTiling tiling,
    VkImageLayout srcImageCurrentLayout,
    uint64_t frameId,
    uint64_t pts,
    bool isLastFrame,
    bool forceIdr,
    int32_t qpOverride,
    ExternalInputResidency residency,
    uint32_t waitSemaphoreCount,
    const VkSemaphore* pWaitSemaphores,
    const uint64_t* pWaitSemaphoreValues,
    const VkPipelineStageFlags2* pWaitDstStageMasks,
    uint32_t signalSemaphoreCount,
    const VkSemaphore* pSignalSemaphores,
    const uint64_t* pSignalSemaphoreValues)
{
    assert(encodeFrameInfo);

    StampExternalFrameInfo(encodeFrameInfo, srcImageCurrentLayout, frameId,
                           pts, isLastFrame, forceIdr, qpOverride, residency,
                           waitSemaphoreCount, pWaitSemaphores,
                           pWaitSemaphoreValues, pWaitDstStageMasks,
                           signalSemaphoreCount, pSignalSemaphores,
                           pSignalSemaphoreValues);

    // =============================================
    // 3. Determine input path
    // =============================================
    // Path A: Optimal YCbCr that's directly encodable → set as
    //         srcEncodeImageResource, skip staging, go to EncodeFrameCommon.
    //         The encoder reads the input once via vkCmdEncodeVideoKHR,
    //         reconstructed DPB frames are separate internal allocations.
    //         Input is free after encode reads it.
    //
    // Path B/C: Linear YCbCr or RGBA → needs staging copy or filter.
    //           Set as srcStagingImageView, go through StageInputFrame().

    bool isDirectlyEncodable = false;
    // Path A (direct encode, zero-copy) requires:
    //   - OPTIMAL or DRM_FORMAT_MODIFIER tiling with an encodable YCbCr format
    //   - NOT VK_IMAGE_TILING_LINEAR (no GPU encode from linear memory)
    // DRM modifier block-linear images have the same physical memory layout as
    // OPTIMAL on NVIDIA. The driver supports VIDEO_ENCODE_SRC on DRM modifier
    // images (with non-zero block height). LINEAR is the only tiling rejected.
    if (tiling == VK_IMAGE_TILING_OPTIMAL ||
        tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        switch (format) {
            case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:                       // NV12
            case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:      // P010
            // 2-plane (semi-planar) 4:4:4 -- NV24 / S410. Already advertised by
            // the NVIDIA driver for VIDEO_ENCODE_SRC and native NVENC on every
            // chip for H.264/HEVC, so an externally-imported OPTIMAL or
            // DRM-modifier image in either format is directly encodable with no
            // driver change.
            case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM:                       // NV24
            case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16:      // S410
                isDirectlyEncodable = true;
                break;
            // Packed 4:4:4 (AYUV / Y410) on their RGBA aliases. Gate on the SESSION
            // format, not on the enum: these same VkFormats are also genuine RGBA input
            // destined for the RGBA->YCbCr filter. m_imageInFormat was matched against
            // the caller's requested input.vkFormat, which is only the packed alias when
            // the colour model said YCbCr -- so an RGBA session keeps m_imageInFormat at
            // NV12 and correctly still goes to staging.
            //
            // Without this the packed frame falls to default: -> Path B/C -> StageInputFrame
            // -> CopyLinearToOptimalImage, which fetches YcbcrVkFormatInfo() (NULL for
            // these formats) and dereferences it with no NULL check.
            case VK_FORMAT_R8G8B8A8_UNORM:                                 // AYUV
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:                       // Y410
                isDirectlyEncodable = (format == m_imageInFormat);
                break;
            // P012 is absent on purpose: the driver does not advertise it for encode.
            // Naming a format here that the runtime check below then refuses turns an
            // early, clear rejection into a late, opaque one.
            //
            // 2-plane 4:2:2 (NV16 / P210) is deliberately absent until the driver
            // advertises 4:2:2 for encode; the runtime check below against the
            // formats actually returned by GetVideoFormats is what gates it.
            default:
                break;
        }
    }

    if (isDirectlyEncodable) {
        // =============================================
        // Path A: Direct encode (zero-copy)
        // =============================================

        // THE DIRECT SUBMIT'S WAIT CAPACITY, REFUSED WHILE A STATUS CAN
        // STILL REACH THE CALLER.
        //
        // A directly encodable frame skips staging, so the waits it carries
        // are assembled into the fixed array in SubmitVideoCodingCmds. That
        // assembly refuses an over-capacity frame as well, but it runs on
        // the encoder's frame-processing path, and the direct path issues
        // its submit from the deferred-GOP flush -- which under B-frame
        // reordering is a LATER call than the one that admitted the frame.
        // A refusal reached there is a frame that is never submitted, never
        // completes and raises no completion edge, while its caller holds a
        // success it can only wait on.
        //
        // So the count is checked HERE, before the image is wrapped and
        // before any encoder resource is taken, where the refusal is the
        // value the entry point returns. VK_ERROR_TOO_MANY_OBJECTS is what
        // it is: a fixed array, named, exceeded.
        //
        // The bound is the ARRAY, not a smaller number of caller waits.
        // Eight caller waits fit exactly and must keep working.
        //
        // Scoped to the direct path deliberately. The staging lane
        // assembles its waits into a growable vector and carries no such
        // bound, so refusing a ninth wait there would invent a limit the
        // library does not have.
        //
        // Not exact in one direction: a QP-map command buffer and the
        // hardware load-balancing timeline each spend a further slot that
        // is not decided yet at this point, so a frame carrying those can
        // still be refused by the assembly rather than here. This check
        // removes the common case from the silent-failure class without
        // pretending to knowledge it does not have.
        if (waitSemaphoreCount > kDirectSubmitSemaphoreCapacity) {
            return VK_ERROR_TOO_MANY_OBJECTS;
        }

        // Wrap external image and set directly as srcEncodeImageResource.
        // No staging, no copy, no filter.
        VkResult result = WrapExternalImage(
            externalImage, externalMemory,
            format, width, height, tiling,
            encodeFrameInfo->srcEncodeImageResource);
        if (result != VK_SUCCESS) {
            return result;
        }
        // The encode will read the caller's imported image directly, so it --
        // not a staging copy -- is what must be acquired from FOREIGN.
        encodeFrameInfo->srcEncodeImageIsExternal = true;

        // Go directly to EncodeFrameCommon (skip StageInputFrame).
        // Wait/signal semaphores will be injected into SubmitVideoCodingCmds
        // by EncodeFrameCommon's pipeline.
        return EncodeFrameCommon(encodeFrameInfo);

    } else {
        // =============================================
        // Path B/C: Staging required (copy or filter)
        // =============================================
        VkResult result = WrapExternalImage(
            externalImage, externalMemory,
            format, width, height, tiling,
            encodeFrameInfo->srcStagingImageView);
        if (result != VK_SUCCESS) {
            return result;
        }

        // Which rung of the adaptation ladder this frame needs. The legacy
        // arm is handed the frame's format directly, so it can answer here:
        // a format that differs from the encode-source format the device
        // reported has to be CONVERTED, which is the compute tier; a format
        // that matches needs at most a re-tile, which is the transfer tier.
        // That is the adaptation ladder's ordering applied to one frame --
        // and it preserves today's behaviour exactly for the shape this arm
        // actually carries, a LINEAR NV12 host-staged image, which matches
        // and so still takes the copy.
        //
        // "Differs from the encode format" is necessary but NOT sufficient,
        // and the second clause is what makes this honest. The filter was
        // built for exactly ONE input format -- EncoderConfig::input.vkFormat,
        // which is what InitEncoder handed VulkanFilterYuvCompute::Create --
        // and its shader's plane count, bit depth and bindings are fixed to
        // it. A frame in some OTHER non-encode format routed here would bind
        // its planes into a shader that expects a different layout. The legacy
        // arm cannot check any of the facts the registered arm checks (the
        // wrapper it just built is view-less for LINEAR, and fabricates create
        // flags for OPTIMAL), so it must not claim more than the format
        // equality it can actually see; the ext layer refuses the classes this
        // leaves unserved (SubmitExternalFrameCommon), rather than silently
        // degrading them to the copy.
        encodeFrameInfo->externalInputViaFilter =
            (format != m_imageInFormat) &&
            (format == m_encoderConfig->input.vkFormat);

        // StageInputFrame will:
        //   - Acquire srcEncodeImageResource from pool
        //   - Record the copy/filter command buffer
        //   - Submit with wait/signal semaphores injected
        //   - Call EncodeFrameCommon() at the end
        return StageInputFrame(encodeFrameInfo);
    }
}

VkResult VkVideoEncoder::SetExternalInputFrameWithNode(
    VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
    VkSharedBaseObj<VulkanVideoImagePoolNode>& node,
    uint64_t registrationId,
    bool directlyEncodable,
    bool routeViaFilter,
    VkImageLayout srcImageCurrentLayout,
    bool srcLayoutIsExplicit,
    uint64_t frameId,
    uint64_t pts,
    bool isLastFrame,
    bool forceIdr,
    int32_t qpOverride,
    ExternalInputResidency residency,
    uint32_t waitSemaphoreCount,
    const VkSemaphore* pWaitSemaphores,
    const uint64_t* pWaitSemaphoreValues,
    const VkPipelineStageFlags2* pWaitDstStageMasks,
    uint32_t signalSemaphoreCount,
    const VkSemaphore* pSignalSemaphores,
    const uint64_t* pSignalSemaphoreValues)
{
    assert(encodeFrameInfo);
    assert(node);

    StampExternalFrameInfo(encodeFrameInfo, srcImageCurrentLayout, frameId,
                           pts, isLastFrame, forceIdr, qpOverride, residency,
                           waitSemaphoreCount, pWaitSemaphores,
                           pWaitSemaphoreValues, pWaitDstStageMasks,
                           signalSemaphoreCount, pSignalSemaphores,
                           pSignalSemaphoreValues);

    // Set AFTER the stamp, which clears the external-input block. Only this
    // entry point can carry the distinction: the LEGACY lane has no
    // registration default for the sentinel to stand in for, so every layout
    // it receives is explicit by construction -- and it also builds a fresh
    // node per frame, so it never reads a residual either way.
    encodeFrameInfo->srcExternalLayoutIsExplicit = srcLayoutIsExplicit;
    // Recorded before the Path-A return below, so a directly-encodable frame
    // still carries its registration id -- the probe's own NOT_APPLICABLE
    // latch for that case is set at ARM time, but a field that is only
    // sometimes populated is the kind of thing a later reader gets wrong.
    encodeFrameInfo->externalRegistrationId = registrationId;

    // ===== THE ONE-FRAME STAGED DETOUR FOR A DIRECTLY-ENCODABLE IMPORT =====
    //
    // Path A hands the caller's imported image straight to
    // vkCmdEncodeVideoKHR, so the producer's pixels never pass through a
    // transfer this library records and the content probe has nothing to
    // ride. That is what made the probe structurally blind to BLOCK-LINEAR
    // imports -- the class the defect appears on -- since block-linear plus
    // VIDEO_ENCODE_SRC is exactly what classifies DIRECT.
    //
    // The fix is deliberately NOT a new command buffer, a new submit, or a
    // new barrier program on the encode queue. It is to send the FIRST frame
    // of an armed registration down the staged path this library already runs
    // for every other import, and let every later frame of that registration
    // go DIRECT. NeedsCapture() is false from the moment the capture is
    // recorded -- and from the moment the probe latches NOT_APPLICABLE -- so
    // the detour is bounded at ONE frame per registration, the same budget
    // the readback already has. It is unreachable entirely unless the caller
    // chained VkVideoEncoderImportContentInfo onto the registration, which is
    // the opt-in and has no other switch.
    const bool probeStillOwesACapture =
        m_contentProbe && m_contentProbe->NeedsCapture(registrationId);

    if (directlyEncodable && !probeStillOwesACapture) {
        // Path A: the registration's node IS the encode source. The encode
        // reads the caller's imported image directly, so it -- not a
        // staging copy -- is what must be acquired from FOREIGN.
        encodeFrameInfo->srcEncodeImageResource = node;
        encodeFrameInfo->srcEncodeImageIsExternal = true;
        return EncodeFrameCommon(encodeFrameInfo);
    }

    // Path B/C: the registration's node is the staged input's source;
    // StageInputFrame acquires the pool destination and records either the
    // copy or the filter, per the routing the registration resolved.
    encodeFrameInfo->srcStagingImageView = node;
    // A DETOURED DIRECT FRAME TAKES THE COPY, NEVER THE FILTER. Its format is
    // the encode-source format by construction -- that is what made it
    // directly encodable -- so there is nothing for the filter to convert,
    // and the filter's storage read is not a site the probe rides anyway.
    encodeFrameInfo->externalInputViaFilter =
        directlyEncodable ? false : routeViaFilter;
    return StageInputFrame(encodeFrameInfo);
}

VulkanDeviceContext::QueueFamilySubmitType
VkVideoEncoder::GetStagedInputSubmitType() const
{
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    // The filter IS m_inputCommandBufferPool when it exists (InitEncoder
    // assigns it), and it was created on the compute family. Every staged
    // frame on such a session therefore holds a compute-family command
    // buffer, whichever branch recorded into it, and a command buffer may
    // only be submitted to a queue of its pool's family
    // (VUID-vkQueueSubmit2-commandBuffer-03874).
    if (m_inputComputeFilter != nullptr) {
        return VulkanDeviceContext::COMPUTE;
    }
#endif
    return ((m_vkDevCtx->GetVideoEncodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) != 0)
               ? VulkanDeviceContext::ENCODE
               : VulkanDeviceContext::TRANSFER;
}

uint32_t VkVideoEncoder::GetStagedInputQueueFamilyIdx() const
{
    switch (GetStagedInputSubmitType()) {
        case VulkanDeviceContext::COMPUTE:
            return (uint32_t)m_vkDevCtx->GetComputeQueueFamilyIdx();
        case VulkanDeviceContext::ENCODE:
            return (uint32_t)m_vkDevCtx->GetVideoEncodeQueueFamilyIdx();
        case VulkanDeviceContext::TRANSFER:
        default:
            return (uint32_t)m_vkDevCtx->GetTransferQueueFamilyIdx();
    }
}

// The staged-input queue family is what the probe's pool must be created on:
// its two vkCmdCopyImage are recorded into the STAGING command buffer, and a
// pool image created for the wrong family would be a queue-ownership
// violation on the sessions where the staged lane is not the encode queue
// (see GetStagedInputSubmitType, which has three answers, not one).
void VkVideoEncoder::ConfigureContentProbe()
{
    if (!m_contentProbe || (m_vkDevCtx == nullptr) ||
        (m_contentProbeQueueDepth == 0) || (m_encoderConfig == nullptr)) {
        return;
    }
    m_contentProbe->Configure(m_vkDevCtx, m_contentProbeQueueDepth,
                              GetStagedInputQueueFamilyIdx(),
                              m_encoderConfig->encodeWidth,
                              m_encoderConfig->encodeHeight);
}

VkResult VkVideoEncoder::StageInputFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    assert(encodeFrameInfo);

    if (encodeFrameInfo->srcEncodeImageResource == nullptr) {

        bool success = m_inputImagePool->GetAvailableImage(encodeFrameInfo->srcEncodeImageResource,
                                                           VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR);
        assert(success);
        assert(encodeFrameInfo->srcEncodeImageResource != nullptr);
        if (!success || encodeFrameInfo->srcEncodeImageResource == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    // No arm has run yet, so the library has recorded no barrier on the
    // encode-source image for this frame. Seeded here rather than relying on
    // Reset()/ClearExternalInputSync() alone, because this function is the
    // ONLY writer of the record and a writer that cannot state its own
    // starting point leaves the reader unable to tell "not staged" from
    // "staged by the previous tenant of this recycled node".
    encodeFrameInfo->srcEncodeImageStagedLayout = VK_IMAGE_LAYOUT_MAX_ENUM;

    m_inputCommandBufferPool->GetAvailablePoolNode(encodeFrameInfo->inputCmdBuffer);
    assert(encodeFrameInfo->inputCmdBuffer != nullptr);

    // Make sure command buffer is not in use anymore and reset
    encodeFrameInfo->inputCmdBuffer->ResetCommandBuffer(true, "encoderStagedInputFence");

    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cmdBuf = encodeFrameInfo->inputCmdBuffer->BeginCommandBufferRecording(beginInfo);

    // Holds for an external staging wrapper too, which has an image but no
    // view: only the raw VkImage is used below (layout-transition barriers +
    // vkCmdCopyImage).
    VkSharedBaseObj<VkImageResourceView> linearInputImageView;
    encodeFrameInfo->srcStagingImageView->GetImageView(linearInputImageView);
    if (linearInputImageView == nullptr) {
        assert(!"StageInputFrame: no staging image resource!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkSharedBaseObj<VkImageResourceView> srcEncodeImageView;
    encodeFrameInfo->srcEncodeImageResource->GetImageView(srcEncodeImageView);

    VkExtent2D copyImageExtent {
        std::min(m_encoderConfig->encodeWidth,  m_encoderConfig->input.width),
        std::min(m_encoderConfig->encodeHeight, m_encoderConfig->input.height)
    };

    VkResult result;

    // Source-side facts BOTH branches need, computed here rather than inside the
    // copy branch so the filter branch cannot silently record none of them: an
    // acquire the copy performs and the filter does not is not a stylistic
    // difference, it is the filter reading memory it does not own.
    //
    // For external input, use actual layout producer left image in (e.g. GENERAL).
    // UNDEFINED would discard contents and produce scrambled encode.
    VkImageLayout srcOldLayout;
    if (encodeFrameInfo->isExternalInput) {
        srcOldLayout = encodeFrameInfo->srcExternalImageLayout;
        if (srcOldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            srcOldLayout = VK_IMAGE_LAYOUT_GENERAL;  // Fallback for compute output
        }
    } else {
        // THE FILE-INPUT LANE, which has no caller and therefore no
        // declaration. It must not fall into the remap above, which would name
        // it GENERAL -- the "fallback for compute output" value, which
        // describes nothing about this image: on frame 1 the pool image has
        // never been in GENERAL, and on the filter arm no barrier is recorded
        // at all, so a dispatch reading it that way samples an image the spec
        // still considers UNDEFINED.
        //
        // PREINITIALIZED is the true statement, and it is true because
        // m_linearInputImagePool is now CREATED that way (see
        // VkVideoEncoder::InitEncoder): LoadNextFrame host-writes the mapped
        // image and only then calls this function, so on first use the image
        // is exactly what PREINITIALIZED asserts -- host-written, never yet
        // moved by any barrier.
        //
        // It is right ONLY on first use, which is why it is not the final
        // word: the residual-layout record below overrides it from frame 2
        // onward, exactly as it does for a reused external registration.
        // PREINITIALIZED can be true at most once in an image's life and is
        // false the instant our own acquire moves it.
        srcOldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    }
    // Local patch; not in upstream vk_video_samples. dma_buf-imported
    // external inputs are owned by VK_QUEUE_FAMILY_FOREIGN_EXT; without
    // an explicit FOREIGN -> local-queue-family acquire the read
    // returns undefined content (observed: solid zeros).
    //
    // Only dma_buf imports are FOREIGN-owned. The CPU-written staging
    // image is locally allocated: its barrier uses HOST stages, and
    // HOST + QFOT is invalid
    // (VUID-VkImageMemoryBarrier2-srcStageMask-03854).
    //
    // Prefer the caller-declared residency. The legacy AUTO
    // heuristic (FOREIGN iff layout != PREINITIALIZED) only holds for
    // FIRST-USE local staging images -- a REUSED local staging image's
    // true layout after the previous staging copy is
    // TRANSFER_SRC_OPTIMAL, which the heuristic would misclassify as a
    // foreign import (wrong QFOT + illegal HOST-stage barrier). Callers
    // that pool/reuse input images pass RESIDENCY_LOCAL explicitly.
    bool isForeignImport;
    switch (encodeFrameInfo->externalInputResidency) {
        case EXTERNAL_INPUT_RESIDENCY_LOCAL:
            isForeignImport = false;
            break;
        case EXTERNAL_INPUT_RESIDENCY_FOREIGN:
            // Even a declared-FOREIGN frame defers to a PREINITIALIZED
            // layout: PREINITIALIZED means host-written staging
            // content, and its barrier waits on HOST stages --
            // HOST + QFOT is invalid
            // (VUID-VkImageMemoryBarrier2-srcStageMask-03854). The
            // declaration routes residency; the layout still decides
            // whether a FOREIGN acquire is legal on this pass, exactly
            // as the AUTO heuristic below does.
            isForeignImport = encodeFrameInfo->isExternalInput &&
                (encodeFrameInfo->srcExternalImageLayout !=
                 VK_IMAGE_LAYOUT_PREINITIALIZED);
            break;
        case EXTERNAL_INPUT_RESIDENCY_AUTO:
        default:
            isForeignImport = encodeFrameInfo->isExternalInput &&
                (encodeFrameInfo->srcExternalImageLayout !=
                 VK_IMAGE_LAYOUT_PREINITIALIZED);
            break;
    }

    // THE LIBRARY'S OWN RECORD BEATS A REGISTRATION-TIME DECLARATION.
    //
    // Everything above computed srcOldLayout from what the CALLER said. For a
    // registration that is submitted once that is the only fact available and
    // it is the right answer. For a registration that is REUSED it is a
    // statement about frame 1 that nothing renews: the acquire below moves the
    // image, and from that instant the library -- not the caller -- knows
    // where it is. Naming the declaration again on frame 2 is
    // VUID-VkImageMemoryBarrier2-oldLayout-01197, once per plane per frame,
    // for the life of the registration.
    //
    // |m_stagedInputResidualLayout| is that knowledge, written at the bottom
    // of this function from the value the handback used as its barrier
    // newLayout, and unset (MAX_ENUM) until the library has actually moved the
    // image. So frame 1 still uses the declaration -- which is the caller's to
    // get right and which the library cannot improve on -- and every later
    // frame uses a fact.
    //
    // THREE CONDITIONS, EACH LOad-BEARING:
    //
    //  * NOT isExternalInput, deliberately. The library's own file-input lane
    //    is exactly the reused-registration shape this record exists for -- 24
    //    pool images recycled across a 60-frame file, each carrying the layout
    //    the previous frame's handback left it in. Excluding it is what made
    //    its every frame name PREINITIALIZED about an image already in GENERAL.
    //    The two surviving conditions still fence the external lane the same
    //    way, and they hold trivially for file input: nothing writes
    //    srcExternalLayoutIsExplicit off the external path
    //    (VkVideoEncoder.cpp, SubmitExternalFrameCommon), and isForeignImport
    //    is false on every arm of the switch above when isExternalInput is.
    //
    //  * !srcExternalLayoutIsExplicit. A caller that fills
    //    VkVideoEncoderFrameSubmitInfo::currentLayout for THIS frame is saying
    //    "I moved it", which is exactly the case where our record is stale.
    //    The public contract already carves this out -- UNDEFINED there means
    //    "as declared at registration" -- so honouring it is reading the
    //    documented field, not inventing a rule.
    //
    //  * !isForeignImport. When the image really did go back to a foreign
    //    owner, an agent outside this library held it between frames and may
    //    have transitioned it; our record describes only what WE did and is
    //    not authoritative. Note this is the one condition that also has to
    //    hold in the other direction, which is why the foreign arms below
    //    CLEAR the record rather than leaving a stale value for a later local
    //    frame of the same registration to read.
    //
    // WHAT THIS DELIBERATELY DOES NOT TOUCH: isForeignImport itself, computed
    // above from encodeFrameInfo->srcExternalImageLayout -- the DECLARATION --
    // and never from srcOldLayout. That separation is the whole safety
    // argument. On every OS-handle import the ext layer DERIVES residency as
    // FOREIGN, so routing falls through to the layout heuristic, and a
    // PREINITIALIZED declaration is the only thing keeping Chromium's
    // host-written staging lane out of a queue-family acquire it must not
    // take (HOST + QFOT is VUID-VkImageMemoryBarrier2-srcStageMask-03854).
    // Feeding a residual of GENERAL into that predicate would flip it
    // silently. Routing reads the declaration; only the BARRIER reads the
    // record.
    if (!isForeignImport &&
        !encodeFrameInfo->srcExternalLayoutIsExplicit &&
        encodeFrameInfo->srcStagingImageView->HasStagedInputResidualLayout()) {
        const VkImageLayout residual =
            encodeFrameInfo->srcStagingImageView->GetStagedInputResidualLayout();
        static const bool kDebugLayout =
            (getenv("VKENC_DEBUG_LAYOUT") != nullptr);
        if (kDebugLayout && (residual != srcOldLayout)) {
            VkEncPrintfErr("[LAYOUT-RESIDUAL] img=%p declared=%d -> "
                            "library-recorded=%d\n",
                    (void*)linearInputImageView->GetImageResource()->GetImage(),
                    (int)srcOldLayout, (int)residual);
        }
        srcOldLayout = residual;
    }

    // PER-FRAME routing, replacing "external input never filters".
    //
    // The predicate it replaces was `m_inputComputeFilter == nullptr ||
    // isExternalInput`, i.e. a blanket bypass: with the macro on, no external
    // frame could ever reach the filter, and with the macro off the arm was
    // literally `if (true)`, so no frame of any kind could. That is the
    // missing rung of the adaptation ladder -- an input the device cannot
    // take directly fell straight to the transfer copy even where a compute
    // pass was the only mechanism that could have converted it.
    //
    // Now: the filter runs when this session HAS one and this FRAME needs it.
    // A file-input frame needs it exactly as before (session-level
    // enablePreprocessComputeFilter, no per-frame opinion); an external frame
    // needs it when whoever admitted the frame said so -- from the
    // registration's resolved input path, or from the frame's own format on
    // the legacy arm. So one session can carry both kinds of frame, which is
    // what a single blanket predicate could not express.
    //
    // With the macro off this is a compile-time false and the copy arm is the
    // only arm, unchanged. Nothing routes a frame here that the copy cannot
    // service in that build either: the ext layer refuses a format that needs
    // converting when no filter is active, and it is that refusal -- not this
    // predicate -- that keeps a 3-plane input away from the copy that hangs
    // the GPU on it.
    bool useComputeFilter = false;
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    useComputeFilter = (m_inputComputeFilter != nullptr) &&
                       (!encodeFrameInfo->isExternalInput ||
                        encodeFrameInfo->externalInputViaFilter);
#endif

    // ROUTING-DIRECTION GUARD. A frame admitted as FILTER must never silently
    // take the copy: the two arms are not fast/slow variants of one another,
    // and the case that makes them differ -- a 3-plane source into a 2-plane
    // destination -- is CopyLinearToOptimalImage's `assert(vkPlaneFormat[2] ==
    // VK_FORMAT_UNDEFINED)`, compiled out under NDEBUG, then a 2-region
    // vkCmdCopyImage that produces VK_ERROR_DEVICE_LOST, a GPU hang and a
    // 0-byte bitstream.
    // A returned error is recoverable; a hang is not, and it presents as
    // flakiness rather than as a defect.
    //
    // Reachable whenever the admitting gate and the routing object disagree:
    // every ext gate answers from the CONFIG flag, this answers from the
    // OBJECT. InitEncoder now hard-fails when those two can diverge, so this
    // is the second lock on the same door rather than the only one.
    if (encodeFrameInfo->externalInputViaFilter && !useComputeFilter) {
        VkEncErr() << "[VkVideoEncoder] frame routed to the preprocess compute "
                      "filter on a session that has none; refusing rather than "
                      "falling back to the staging copy" << std::endl;
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    // The filter reads an EXTERNAL input through PER-PLANE storage views. A
    // wrapper built over an image whose exporter did not declare
    // MUTABLE_FORMAT carries none (VkImageResourceView::Create declines them,
    // VUID-VkImageViewCreateInfo-image-01762), and
    // VulkanFilterYuvCompute::UpdateImageDescriptorSets trims its plane
    // bindings by that count -- while ShaderGenerateImagePlaneDescriptors has
    // already cleared VK_IMAGE_ASPECT_COLOR_BIT out of m_inputImageAspects for
    // a multi-planar input, so there is no combined-view binding to fall back
    // to. The result is a push-descriptor set with ZERO input bindings and a
    // dispatch that reads unbound STORAGE_IMAGE descriptors. Refuse instead.
    if (useComputeFilter && encodeFrameInfo->isExternalInput &&
        (YcbcrVkFormatInfo(m_encoderConfig->input.vkFormat) != nullptr) &&
        (linearInputImageView->GetNumberOfPlanes() < 2)) {
        VkEncErr() << "[VkVideoEncoder] external input routed to the preprocess "
                      "compute filter carries no per-plane views (planes="
                   << linearInputImageView->GetNumberOfPlanes()
                   << "); the exporter must declare "
                      "VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT (with EXTENDED_USAGE) "
                      "and VK_IMAGE_USAGE_STORAGE_BIT" << std::endl;
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
#endif

    // THE ROUTING OBSERVABLE, recorded HERE and at no other site.
    //
    // This is the last point before the copy/filter split and the first point
    // after every refusal return above, so a frame counted here is a frame
    // whose staging barrier program was actually chosen and recorded -- on
    // either arm -- exactly once. The alternative placement, inside the two
    // arms' own release/handback pairs, needs four sites and undercounts the
    // instant a session mixes filtered and copied frames.
    //
    // isExternalInput gates it because the library's own file-input lane
    // declares no residency at all: counting it would make "every frame was
    // local" true of a session that registered nothing, which is precisely
    // the reading this channel exists to make falsifiable.
    if (encodeFrameInfo->isExternalInput) {
        if (isForeignImport) {
            m_foreignAcquireCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_localAcquireCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (!useComputeFilter) {
        // The acquire's destination family is the family of the queue this
        // batch will be submitted to -- read from the one accessor the submit
        // also reads, never re-derived. Re-deriving it here is exactly how
        // this site and SubmitStagedInputFrame came to be able to disagree.
        const uint32_t stagingQueueFamilyIdx = GetStagedInputQueueFamilyIdx();
        const uint32_t linearSrcQueueFamilyIdx = isForeignImport
            ? VK_QUEUE_FAMILY_FOREIGN_EXT
            : VK_QUEUE_FAMILY_IGNORED;
        const uint32_t linearDstQueueFamilyIdx = isForeignImport
            ? stagingQueueFamilyIdx
            : VK_QUEUE_FAMILY_IGNORED;
        VkImageLayout linearImgNewLayout = TransitionImageLayout(cmdBuf, linearInputImageView, srcOldLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                                 linearSrcQueueFamilyIdx, linearDstQueueFamilyIdx);
        // NO QUEUE-FAMILY OWNERSHIP TRANSFER IS RECORDED FOR THE STAGING
        // DESTINATION, AND THAT IS A KNOWN GAP RATHER THAN AN OVERSIGHT.
        // Stated here because "nobody wrote it down" and "we decided not to"
        // must not look the same to the next reader.
        //
        // THE SHAPE. m_inputImagePool is created VK_SHARING_MODE_EXCLUSIVE with
        // queueFamilyIndexCount = 1, pinned to GetVideoEncodeQueueFamilyIdx()
        // (see VulkanVideoImagePool::Configure and the InitEncoder call that
        // drives it). This transition and the filter arm's GENERAL transition
        // both omit the family arguments, so both default to
        // VK_QUEUE_FAMILY_IGNORED. The family-qualified arguments a few lines
        // above apply to linearInputImageView -- the SOURCE -- and only when
        // isForeignImport. The only family-qualified transition of
        // srcEncodeImageView anywhere is the Path-A FOREIGN acquire in
        // EncodeFrame, which is gated on srcEncodeImageIsExternal &&
        // externalInputResidency == FOREIGN and therefore never fires for a
        // pool-sourced staged frame.
        //
        // WHEN IT MATTERS. GetStagedInputSubmitType() returns COMPUTE iff an
        // input compute filter OBJECT exists -- keyed on the object, not on
        // which arm ran, so it covers this copy arm too. On that path the
        // staged batch is recorded and submitted on the COMPUTE family while
        // the encode reads the image on the ENCODE family, and for an EXCLUSIVE
        // resource the spec makes the contents undefined across that boundary
        // without a transfer. The staged family is a COMPUTE|TRANSFER one
        // (VulkanDeviceContext prefers a compute-ONLY family) and the encode
        // family is a different one. With NO filter the fallback returns
        // ENCODE, the same family the pool is pinned to, and nothing is
        // owed.
        //
        // WHY IT IS NOT FIXED HERE, in order of weight:
        //
        //   1. A DRIVER DEFECT SITS EXACTLY HERE. A driver can lose the
        //      device on a queue-family ownership RELEASE of an image
        //      carrying VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR when the
        //      barrier is recorded off the graphics or optical-flow
        //      families. This pool carries that usage and the release would
        //      be recorded on the staged family. The known conjunction also
        //      requires dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT,
        //      which a real-family release would NOT use, so the exact cell
        //      this would land in is unknown. Adding the transfer blind
        //      risks converting a benign spec violation into a hard device
        //      loss on the primary encode lane.
        //
        //   2. NO INSTRUMENT IN THIS TREE CAN SEE THE RULE. Core VVL does not
        //      model "EXCLUSIVE contents become undefined without an ownership
        //      transfer", and synchronization validation models memory and
        //      execution hazards, not ownership. This was confirmed rather
        //      than assumed: with validate_sync and
        //      syncval_submit_time_validation on, the bars go from 16 and 8
        //      READ_AFTER_WRITE hazards to zero once the copy's dependency is
        //      corrected (see CopyLinearToOptimalImage), while this ownership
        //      gap is untouched and reported by nothing. So a clean sync run
        //      must NOT be read as evidence that this is absent or benign.
        //
        // WHAT CLOSING IT REQUIRES, so the next attempt does not start cold:
        // establish first that a release recorded on the staged family with a
        // real-family destination, and a mirrored acquire on the encode
        // family, does not lose the device. If it survives, record the
        // release at the end
        // of StageInputFrame on srcEncodeImageView and the matching acquire in
        // EncodeFrame immediately before CmdBeginVideoCodingKHR, alongside the
        // existing Path-A acquire. Gate BOTH halves on one predicate
        // (GetStagedInputQueueFamilyIdx() != GetVideoEncodeQueueFamilyIdx())
        // read once, so a same-family session records neither and the pair can
        // never go unbalanced.
        //
        // TWO FACTS THE RELEASE ABOVE DEPENDS ON, stated so that a future
        // attempt does not have to infer either:
        //
        //   * THE OLD LAYOUT DOES NOT DIFFER PER ARM. Both arms hand the image
        //     over in VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR (the CF-02a/CF-02b
        //     handling below and on the filter arm), and encodeFrameInfo->
        //     srcEncodeImageStagedLayout states that as a fact rather than as
        //     an inference from which branch ran, so a release can name that
        //     field and be right on both arms.
        //
        //   * TransitionImageLayout's table HAS a (TRANSFER_DST_OPTIMAL ->
        //     VIDEO_ENCODE_SRC_KHR) arm, with TRANSFER/TRANSFER_WRITE ->
        //     ALL_COMMANDS/MEMORY_READ. Note the second scope: a release
        //     additionally needs the ownership arguments, and the ALL_COMMANDS
        //     destination is what keeps the arm legal on the compute family
        //     this branch can be recorded on.
        // THE RETURN VALUE IS KEPT, and that is the whole shape of this. Discarding
        // it -- `(void)srcImgNewLayout;` -- throws away the library's own statement
        // of where it just put the image, so
        // the hand-off below had nothing to name as an oldLayout and the
        // function's `FIXME - use the real old layout` had no answer for this
        // site. This variable IS the real old layout, for the one pair where
        // the library itself is the producer.
        VkImageLayout srcEncodeImgLayout = TransitionImageLayout(cmdBuf, srcEncodeImageView, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        (void)linearImgNewLayout;

        CopyLinearToOptimalImage(cmdBuf, linearInputImageView, srcEncodeImageView, copyImageExtent);

        // ===== MECHANISM-C part 2: capture what the PRODUCER handed us =====
        // Same command buffer, one command after the library's own copy read
        // this exact image in this exact layout. Splits "the compositor gave us
        // a zeroed chroma plane" from "our copy lost it".
        if (m_psnr && m_psnr->SrcCaptureEnabled()) {
            m_psnr->CaptureImported(cmdBuf, encodeFrameInfo.get(), linearInputImageView.get());
        }

        // ===== THE IMPORT CONTENT PROBE =====
        //
        // The SHIPPING sibling of the debug capture above, at the same site
        // and for the same physical reason: this is the one instruction
        // boundary in the library at which the producer's imported pixels are
        // (a) in a layout a transfer can read and (b) not yet mixed with
        // anything the library did. The difference is what happens to the
        // result -- MECHANISM-C narrates to stderr, which the shipping
        // Chromium configuration discards wholesale via silenceStdio, and
        // this reports through a chained struct that survives it.
        //
        // ONCE PER REGISTRATION, not per frame: NeedsCapture() is false from
        // the second frame of a buffer on. On the owner's 5125-frame session
        // over 5 registered buffers that is 5 readbacks, not 5125.
        if (m_contentProbe &&
            m_contentProbe->NeedsCapture(encodeFrameInfo->externalRegistrationId)) {
            const VkSharedBaseObj<VkImageResource>& probeSrcRes =
                linearInputImageView->GetImageResource();
            const VkImageCreateInfo& probeSrcCI = probeSrcRes->GetImageCreateInfo();
            VkExtent2D probeExtent = { probeSrcCI.extent.width,
                                       probeSrcCI.extent.height };
            m_contentProbe->RecordCapture(cmdBuf,
                                          encodeFrameInfo->externalRegistrationId,
                                          probeSrcRes->GetImage(),
                                          probeSrcCI.format, probeExtent,
                                          encodeFrameInfo->contentProbeCapture);
        }

        // The OTHER side of the observable. Counted here rather than derived
        // as (staged - filtered): a derived count cannot distinguish a frame
        // that took the copy from a frame that never reached this function
        // at all, and a superset counter has already made a live tier read
        // as dead once on this project.
        m_stagedCopyCount.fetch_add(1, std::memory_order_relaxed);

        // ===== CF-02a: HAND THE COPY DESTINATION TO THE ENCODER =====
        //
        // vkCmdEncodeVideoKHR requires its source picture to be in
        // VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR at the time the encode
        // executes (VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811). Without this
        // barrier the only VIDEO_ENCODE_SRC_KHR transition in the
        // whole encoder was the Path-A FOREIGN acquire in
        // RecordVideoCodingCmd, gated on srcEncodeImageIsExternal &&
        // externalInputResidency == FOREIGN -- a predicate no staged frame
        // can satisfy, because Path A returns straight to EncodeFrameCommon
        // and never calls this function. So every frame that reached the
        // staging copy was encoded out of TRANSFER_DST_OPTIMAL.
        //
        // WHY HERE. CopyLinearToOptimalImage is the last write to this image
        // in this command buffer, so this is the earliest point at which the
        // contents are final; and it is the point where the PRODUCER'S first
        // scope is still known to be the transfer that just ran, which is
        // what makes TRANSFER/TRANSFER_WRITE an honest availability operation
        // rather than a guess. The alternative site -- alongside the Path-A
        // acquire in the encode command buffer -- would have to state
        // srcStageMask = NONE, because by then the write is in another
        // submission.
        //
        // WHY THIS IS NOT A DOUBLE TRANSITION. The Path-A acquire and this
        // barrier are mutually exclusive by construction, not by luck:
        // srcEncodeImageIsExternal is set only where srcEncodeImageResource
        // IS the caller's imported image, and both of those sites return via
        // EncodeFrameCommon without entering StageInputFrame. The acquire is
        // therefore preserved untouched and is NOT made redundant by this
        // change on any path.
        //
        // WHAT THIS DOES NOT CLOSE, said plainly so it is not read as more
        // than it is: the queue-family OWNERSHIP gap documented at length
        // above is untouched. This barrier passes VK_QUEUE_FAMILY_IGNORED on
        // both sides -- it changes layout, not ownership -- which is also
        // what keeps it clear of the device-loss a VIDEO_ENCODE_SRC
        // ownership RELEASE off the graphics engine can provoke. A
        // layout-only transition is not that shape.
        srcEncodeImgLayout = TransitionImageLayout(cmdBuf, srcEncodeImageView,
                                                   srcEncodeImgLayout,
                                                   VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR);
        // The record RecordVideoCodingCmd reads -- and the exact limit of
        // what it can witness. It is
        // written from srcEncodeImgLayout, which the statement directly
        // above assigned FROM TransitionImageLayout's return, and that
        // function has exactly one return -- an unconditional
        // `return newLayout` echoing its own by-value argument, which the
        // body never reassigns. The value recorded here is therefore the
        // layout the CALL ASKED FOR, never an observation of what reached
        // cmdBuf.
        //
        // Delete this call and the running variable keeps its old value, so
        // the record says TRANSFER_DST_OPTIMAL and the reader in
        // RecordVideoCodingCmd goes red -- which is the only mutation it can
        // catch. Suppress only the CmdPipelineBarrier2KHR inside
        // TransitionImageLayout, leaving the call and its return in place,
        // and the reader stays green while no hand-off barrier is recorded
        // at all.
        //
        // So this reader gates the CALL SITE, not the barrier record. Do not
        // cite it as evidence that a barrier reached the command buffer.
        encodeFrameInfo->srcEncodeImageStagedLayout = srcEncodeImgLayout;

        // Queue-family RELEASE -- the missing half of the acquire above.
        // CopyLinearToOptimalImage is the LAST use of the imported image in
        // this command buffer, so this is the earliest correct point. Inside
        // this branch on purpose: it is the only scope where
        // linearDstQueueFamilyIdx -- the acquire's OWN destination family --
        // is still live, so the release cannot name a family the acquire did
        // not. Gated on the same isForeignImport, so a frame that never
        // acquired can never release.
        //
        // oldLayout is the LITERAL the acquire named as its newLayout, which
        // is what our copy actually left the image in
        // (VUID-VkImageMemoryBarrier2-oldLayout-01197). Naming srcOldLayout --
        // the PRE-acquire producer layout -- would break that VUID.
        //
        // newLayout is srcOldLayout -- the SAME value the next acquire of
        // this registration will name as ITS oldLayout -- so the handover
        // round-trips exactly, whatever the producer declares.
        //
        // A CONSTANT WOULD NOT DO, and GENERAL specifically would be a
        // regression for one producer: a caller that declares
        // TRANSFER_SRC_OPTIMAL round-trips, and would then find its own
        // declaration contradicted, which is VUID-...-oldLayout-01197 on the
        // next frame.
        //
        // "ROUND-TRIPS" DEPENDS ON THE (TRANSFER_SRC_OPTIMAL ->
        // TRANSFER_SRC_OPTIMAL) ARMS. The handback hands the image back in
        // TRANSFER_SRC_OPTIMAL, and the NEXT frame's acquire then presents the pair
        // (TRANSFER_SRC_OPTIMAL -> TRANSFER_SRC_OPTIMAL); with no arm for it such a
        // caller does not round-trip, it aborts (standalone build) or takes a
        // silently wrong barrier (Chromium). Covered by encoder-ext-input-residency
        // --local-tso-opaque-fd.
        // above) and cannot be PREINITIALIZED (isForeignImport excludes it),
        // so it is always a legal newLayout under VUID-...-newLayout-01198.
        //
        // For the in-tree Chromium CPU dma-buf lane this evaluates to GENERAL,
        // which is also the layout that lane needs on other grounds: it
        // declares RESIDENCY_FOREIGN and then host-writes the buffer through
        // an mmap between frames, and host access to image memory is
        // well defined only for a LINEAR image currently in GENERAL or
        // PREINITIALIZED. That second argument is real but narrower than this
        // one -- it does not hold for an OPTIMAL or DRM-modifier import
        // reaching the same release -- so the round-trip is the reason, and
        // host-writability is a property of the answer rather than its
        // justification.
        //
        // What does NOT decide it: the release/acquire layout-equality rule.
        // A release runs on a queue of the SOURCE family and its acquire on
        // the DESTINATION family, so our release (local -> FOREIGN) and our
        // next acquire (FOREIGN -> local) are opposite-direction transfers and
        // no VUID binds their layouts. Two contradictory assertions about one
        // instant are still worth not making.
        if (isForeignImport) {
            ReleaseImageToForeignQueue(cmdBuf, linearInputImageView,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       srcOldLayout,
                                       linearDstQueueFamilyIdx,
                                       VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR,
                                       VK_ACCESS_2_TRANSFER_READ_BIT_KHR);
            // The image is now owned by VK_QUEUE_FAMILY_FOREIGN_EXT and a
            // foreign agent may transition it before we see it again, so
            // this library has no record worth keeping. Cleared rather
            // than left alone: a registration can reach this arm on one
            // frame and the local arm on the next (residency AUTO/FOREIGN
            // defers to the per-frame layout), and a residual written by an
            // earlier local frame would then be read after a foreign owner
            // had the image.
            encodeFrameInfo->srcStagingImageView->SetStagedInputResidualLayout(
                VK_IMAGE_LAYOUT_MAX_ENUM);
        } else {
            // THE SAME HANDBACK, for a LOCAL registration. Everything the
            // release above argues about newLayout applies unchanged: the
            // image is handed back in the layout the next acquire of this
            // registration will declare, so the round-trip closes. The only
            // difference is that no ownership changes hands, so there is no
            // queue-family transfer and the destination scope is the caller's
            // host access rather than a foreign agent's unknown one.
            //
            // An else-if on the SAME condition, not a second if: exactly one
            // of {foreign release, local restore} can ever fire, which is the
            // structural version of the claim that a frame which never
            // acquired can never release.
            //
            // NOT GATED ON isExternalInput. The file-input lane owns its linear
            // pool image outright and declares nothing, which would argue for
            // skipping the restore as a barrier no caller can observe. It does
            // not, because the pool is created PREINITIALIZED and srcOldLayout
            // above states that fact, so the
            // library DOES have something to record: without this handback the
            // node's residual is never written, so frame 2 would name
            // PREINITIALIZED about an image its own frame-1 acquire had already
            // moved to TRANSFER_SRC_OPTIMAL -- VUID-...-oldLayout-01197.
            //
            // STATED PLAINLY: WHICH FILE-INPUT FRAMES REACH THIS HALF. The
            // copy arm requires useComputeFilter == false, which for a
            // file-input frame reduces to m_inputComputeFilter == nullptr,
            // which requires EncoderConfig::enablePreprocessComputeFilter ==
            // false. That field is constructed true (VkEncoderConfig.h) and
            // has exactly one writer in the tree --
            // vulkan_video_encoder_ext.cpp, on the ext layer, which only ever
            // produces EXTERNAL frames. There is no CLI flag and no JSON
            // schema key for it. In a build with the compute filter compiled
            // in, therefore, no file-input frame reaches this line; the arm
            // is written for consistency with the filter arm beside it.
            //
            // oldLayout is the literal TRANSFER_SRC_OPTIMAL for the same
            // reason the release names it: it is what the acquire above
            // transitioned to and what CopyLinearToOptimalImage left behind.
            //
            // AND THE RECORD. The helper returns the layout it actually
            // left the image in -- the declaration, or GENERAL where the
            // declaration was not a legal barrier destination -- so the
            // barrier and the record cannot disagree: there is no second
            // expression of the same fact to keep in step.
            const VkImageLayout handedBackLayout =
                RestoreStagedInputLayout(cmdBuf, linearInputImageView,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     srcOldLayout,
                                     VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR,
                                     VK_ACCESS_2_TRANSFER_READ_BIT_KHR);
            encodeFrameInfo->srcStagingImageView->SetStagedInputResidualLayout(
                handedBackLayout);
        }
    }
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    else {

        VkVideoPictureResourceInfoKHR srcPictureResourceInfo(*encodeFrameInfo->srcStagingImageView->GetPictureResourceInfo());
        VkVideoPictureResourceInfoKHR dstPictureResourceInfo(*encodeFrameInfo->srcEncodeImageResource->GetPictureResourceInfo());

        srcPictureResourceInfo.codedExtent = copyImageExtent;

        // The barriers the copy branch beside this one has always recorded
        // and this branch never did.
        //
        // NO LONGER GATED ON isExternalInput. That gate was written on the
        // reading that external input "is the only kind of frame that arrives
        // owned by another queue family or in a layout this encoder did not
        // choose". The FAMILY half holds, and is why the family indices below
        // are conditional. The LAYOUT half does not hold for the library's own
        // file-input frames: the linear pool image arrives in whatever the
        // previous frame left it in, or PREINITIALIZED on first use, and
        // NEITHER of those is the GENERAL that VulkanFilterYuvCompute's
        // STORAGE_IMAGE descriptors demand.
        //
        // With the gate in place the dispatch reads an image the validation
        // layer reports as UNDEFINED where GENERAL is required
        // (VUID-vkCmdDraw-None-09600), once per input plane plus twice for
        // the encode-input image, on every frame and on both the 3-plane and
        // the 2-plane shape. The dispatch is reading an image the spec
        // permits the driver to have discarded, and it survives only because
        // a LINEAR host-coherent allocation on this vendor happens not to
        // be.
        //
        // Destination family is the COMPUTE family, because the compute
        // filter is what consumes this image and a queue-family acquire must
        // execute on a queue of its DESTINATION family. That is the same
        // rule the copy branch obeys by naming the transfer/encode family --
        // and it is why this must land before the branch is reachable:
        // acquiring into the encode family and then submitting the batch on
        // the compute queue is not a slow path, it is a wedged queue.
        // This arm's running record of where srcEncodeImageView actually is,
        // for the same reason and with the same mutation property as the copy
        // arm's. Declared out here because the acquire block below closes
        // before the filter has run, and the hand-off that reads it is after
        // the dispatch.
        VkImageLayout srcEncodeImgLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        {
            // Same accessor the copy arm and the submit read. On any session
            // that reached this arm it answers the COMPUTE family, because
            // the filter IS the input command-buffer pool -- but reading it
            // rather than naming the compute family directly is what keeps
            // the three sites from ever describing different queues.
            const uint32_t filterQueueFamilyIdx = GetStagedInputQueueFamilyIdx();
            const uint32_t filterSrcQueueFamilyIdx = isForeignImport
                ? VK_QUEUE_FAMILY_FOREIGN_EXT
                : VK_QUEUE_FAMILY_IGNORED;
            const uint32_t filterDstQueueFamilyIdx = isForeignImport
                ? filterQueueFamilyIdx
                : VK_QUEUE_FAMILY_IGNORED;
            // Input -> GENERAL: the filter binds it as a STORAGE_IMAGE, and
            // a storage descriptor admits no other layout.
            TransitionImageLayout(cmdBuf, linearInputImageView,
                                  srcOldLayout, VK_IMAGE_LAYOUT_GENERAL,
                                  filterSrcQueueFamilyIdx,
                                  filterDstQueueFamilyIdx);
            // Output -> GENERAL, discarding: the destination is this
            // encoder's own pool image and the filter overwrites every
            // texel, exactly as the copy branch discards into
            // TRANSFER_DST_OPTIMAL.
            srcEncodeImgLayout =
                TransitionImageLayout(cmdBuf, srcEncodeImageView,
                                      VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_GENERAL);
        }

        if (m_encoderConfig->enablePictureRowColReplication == 1) {
            // replicate the last row and column to the padding area
            dstPictureResourceInfo.codedExtent.width = m_encoderConfig->encodeAlignedWidth;
            dstPictureResourceInfo.codedExtent.height = m_encoderConfig->encodeAlignedHeight;
        } else if (m_encoderConfig->enablePictureRowColReplication == 2) {
            // replicate only one row and one column to the padding area
            if (dstPictureResourceInfo.codedExtent.width < m_encoderConfig->encodeAlignedWidth) {
                dstPictureResourceInfo.codedExtent.width += 1;
            }
            if (dstPictureResourceInfo.codedExtent.height < m_encoderConfig->encodeAlignedHeight) {
                dstPictureResourceInfo.codedExtent.height += 1;
            }
        } else {
            // row and column replication is disabled. Don't touch the image padding area.
            dstPictureResourceInfo.codedExtent = copyImageExtent;
        }

        // Get image view for filter (nullptr if no pool)
        VkSharedBaseObj<VkImageResourceView> subsampledImageView;
#ifdef NV_AQ_GPU_LIB_SUPPORTED
        // Get subsampled Y image if pool is available (ref-counted in encodeFrameInfo)
        if (m_inputSubsampledImagePool != nullptr) {
            bool success = m_inputSubsampledImagePool->GetAvailableImage(encodeFrameInfo->subsampledImageResource,
                                                                         VK_IMAGE_LAYOUT_GENERAL);
            assert(success && encodeFrameInfo->subsampledImageResource);
            if (!success) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        if (encodeFrameInfo->subsampledImageResource) {
            encodeFrameInfo->subsampledImageResource->GetImageView(subsampledImageView);
        }
#endif // NV_AQ_GPU_LIB_SUPPORTED

        result = m_inputComputeFilter->RecordCommandBuffer(cmdBuf,
                                                           encodeFrameInfo->inputCmdBuffer->GetNodePoolIndex(),
                                                           linearInputImageView.get(),
                                                           &srcPictureResourceInfo,
                                                           srcEncodeImageView.get(),
                                                           &dstPictureResourceInfo,
                                                           subsampledImageView.get()); // nullptr if no pool

        if (result != VK_SUCCESS) {
            return result;
        }

        // What ACTUALLY ran, for SubmitStagedInputFrame. Recorded after the
        // record succeeded, so a filter that failed to record leaves the
        // frame described as unfiltered rather than as filtered-and-broken.
        encodeFrameInfo->inputFilterRecorded = true;
        // ...and the same fact made readable from OUTSIDE the library, on
        // the same line and under the same success condition, so the
        // observable can never drift from the routing flag it mirrors.
        m_inputFilterDispatchCount.fetch_add(1, std::memory_order_relaxed);

        // ===== CF-02b: HAND THE FILTER'S OUTPUT TO THE ENCODER =====
        //
        // The filter writes this image through STORAGE_IMAGE descriptors, so
        // the acquire above put it in GENERAL and it has to be there for the
        // dispatch. The only barrier VulkanFilterYuvCompute records after its
        // dispatch is a GENERAL -> GENERAL availability operation on this
        // same output image -- it makes the shader writes available and
        // deliberately does not change the layout, because the filter has no
        // opinion about what its consumer needs. This library does: the
        // consumer is vkCmdEncodeVideoKHR, and GENERAL satisfies
        // VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811 only when the
        // unifiedImageLayoutsVideo feature is enabled. It is enabled nowhere
        // -- zero occurrences across this library, media/gpu/, gpu/vulkan/
        // and the DEPS-pinned submodule -- so without this barrier the filter arm
        // encodes out of GENERAL, in violation of the spec.
        //
        // AFTER THE RECORD, NOT INSIDE THE BLOCK ABOVE: the dispatch is what
        // fills the image, so a transition placed with the acquire would
        // transition an empty image and then let the dispatch write it in a
        // layout its own descriptors reject. Placed here it is ordered after
        // RecordCommandBuffer's own trailing barrier, which is the correct
        // reading of "the filter has finished with its output".
        //
        // MASKS: see the (GENERAL -> VIDEO_ENCODE_SRC_KHR) arm. COMPUTE_SHADER
        // /SHADER_WRITE is the dispatch that produced the contents and is
        // legal here because this arm only runs on a session whose input
        // command buffer IS the filter's compute-family pool; the second
        // scope is ALL_COMMANDS/MEMORY_READ because that same family need
        // not carry VK_QUEUE_VIDEO_ENCODE_BIT_KHR, and the encode's
        // visibility arrives through the input->encode semaphore.
        //
        // Both families are VK_QUEUE_FAMILY_IGNORED: layout only, no
        // ownership transfer, so this is not the shape that can lose the
        // device.
        srcEncodeImgLayout = TransitionImageLayout(cmdBuf, srcEncodeImageView,
                                                   srcEncodeImgLayout,
                                                   VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR);
        // Same record, same limit as the copy arm above: written from
        // TransitionImageLayout's return, so it witnesses that the CALL was
        // made, not that a barrier was recorded. See the note there.
        encodeFrameInfo->srcEncodeImageStagedLayout = srcEncodeImgLayout;

        // Queue-family RELEASE -- the missing half of the filter acquire.
        // The filter's dispatch is the LAST use of the INPUT image; the only
        // barrier it records afterwards targets the OUTPUT. The input is
        // therefore still in VK_IMAGE_LAYOUT_GENERAL here, which is the
        // literal the acquire named.
        //
        // The acquire's family locals are out of scope by now, so the
        // accessor is re-read: it is pure and answers the same family the
        // acquire named on any session that reached this arm.
        //
        // srcAccessMask is SHADER_READ, deliberately not SHADER_WRITE. The
        // filter READS this image; the GENERAL->GENERAL table arm supplies
        // WRITE, which is right for the acquire direction and would be an
        // availability operation on a write that never happened here.
        if (isForeignImport) {
            // isForeignImport alone, not (isExternalInput && isForeignImport):
            // every arm of the residency switch that can set isForeignImport
            // conjoins isExternalInput already, so the second test was
            // redundant -- and dropping it is what makes the else below able
            // to mean "every frame that did not release to a foreign owner",
            // which is the set the local handback is for.
            //
            // oldLayout is GENERAL because that is what the filter acquire
            // above transitioned this image to, and the filter records no
            // further barrier on its INPUT (the one it does record after the
            // dispatch targets the output image).
            //
            // Not because "a sampled descriptor admits no other layout" -- it
            // does: a COMBINED_IMAGE_SAMPLER, which is what the Y'CbCr arm
            // binds when a conversion sampler exists, admits others; only a
            // STORAGE_IMAGE is confined to GENERAL
            // (VUID-VkDescriptorImageInfo-imageView-06711). What settles it is
            // that VulkanFilterYuvCompute declares ONE input layout for every
            // arm, and that layout is GENERAL. GENERAL is right on every lane
            // the ENCODER can reach for a different reason: a multi-planar
            // input has no aspect-0 binding so every plane descriptor is
            // forced to GENERAL, and the RGBA storage-read arm overrides back
            // to GENERAL explicitly. Stated precisely because a blanket claim
            // here reads as a licence to skip the check.
            //
            // newLayout is srcOldLayout, NOT a second GENERAL. The copy arm
            // hands back the layout the next acquire will declare, and this
            // arm must too: the helper's own rule is that newLayout should be
            // what the NEXT acquire of the same resource names, so the two
            // barriers do not assert different things about one instant, and
            // BOTH acquires read the same srcOldLayout computed once above.
            // Hardcoding GENERAL was only correct when srcOldLayout happened
            // to be GENERAL, which is not pinned: isForeignImport excludes
            // PREINITIALIZED and UNDEFINED is remapped to GENERAL, but
            // TRANSFER_SRC_OPTIMAL remains reachable from a producer's
            // declaration, and the layout table deliberately keeps an arm for
            // exactly that producer.
            //
            // VIDEO_ENCODE_SRC_KHR is reachable as a declaration too, but do
            // NOT read this as saying that lane is wired: the only
            // (VIDEO_ENCODE_SRC_KHR -> GENERAL) arm in the table is documented
            // for the filter's OUTPUT image and supplies SHADER_WRITE
            // visibility, so a producer declaring it on a filter INPUT would
            // get an acquire with no read visibility for the dispatch about to
            // sample it. That arm is owed before such a producer is
            // supported. No lane regresses: on the host-mmap lane srcOldLayout
            // IS GENERAL, so this is identical there.
            ReleaseImageToForeignQueue(cmdBuf, linearInputImageView,
                                       VK_IMAGE_LAYOUT_GENERAL,
                                       srcOldLayout,
                                       GetStagedInputQueueFamilyIdx(),
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR,
                                       VK_ACCESS_2_SHADER_READ_BIT_KHR);
            // Cleared for the same reason the copy arm's release clears it:
            // a foreign owner has the image between frames.
            encodeFrameInfo->srcStagingImageView->SetStagedInputResidualLayout(
                VK_IMAGE_LAYOUT_MAX_ENUM);
        } else {
            // The LOCAL handback on the filter arm. Same argument as the copy
            // arm beside it, with this arm's residual layout: the filter's
            // dispatch is the last use of the INPUT image and records no
            // further barrier on it, so the image is still in the GENERAL the
            // filter acquire named.
            //
            // srcStageMask COMPUTE_SHADER is legal HERE AND ONLY HERE, and not
            // by assumption: this arm is reachable only on a session that has
            // an input compute filter, and GetStagedInputSubmitType() returns
            // COMPUTE for exactly that session, so the batch is submitted on
            // the compute family. The copy arm beside it therefore must not
            // and does not name this stage.
            //
            // SHADER_READ not SHADER_WRITE, for the same reason the release
            // above states: the filter READS this image.
            //
            // REACHABILITY. A consumer that declares GENERAL -- which equals
            // this arm's residual -- makes the helper's equal-layout early
            // return fire, so no barrier is recorded and this call is a
            // no-op. A filter-routed registration declaring
            // TRANSFER_SRC_OPTIMAL instead, the ext layer's own legacy-wrap
            // default, is the shape that exercises it.
            //
            // AND IT IS LOAD-BEARING, not merely reached. Suppressing ONLY
            // the CmdPipelineBarrier2KHR inside RestoreStagedInputLayout,
            // leaving the return value alone so the registration's residual
            // record still claims TRANSFER_SRC_OPTIMAL while the image is
            // really still in GENERAL, makes frame 2's acquire name a layout
            // the image is not in.
            const VkImageLayout handedBackLayout =
                RestoreStagedInputLayout(cmdBuf, linearInputImageView,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     srcOldLayout,
                                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR,
                                     VK_ACCESS_2_SHADER_READ_BIT_KHR);
            encodeFrameInfo->srcStagingImageView->SetStagedInputResidualLayout(
                handedBackLayout);
        }
    }
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED

    // Stage QPMap if it needs staging. Reuse the same command buffer used for staging of the input image
    if (m_encoderConfig->enableQpMap && (m_qpMapTiling != VK_IMAGE_TILING_LINEAR)) {
        result = StageInputFrameQpMap(encodeFrameInfo, cmdBuf);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    result = encodeFrameInfo->inputCmdBuffer->EndCommandBufferRecording(cmdBuf);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Now submit the staged input to the queue.
    //
    // A rejected staging submit means the encode source was never written and
    // the semaphore the encode submit waits on will never be signalled.
    // Encoding anyway produces a frame from whatever the destination image
    // happened to hold and queues a wait nothing can satisfy, so the driver's
    // own VkResult is returned here rather than discarded.
    //
    // The frame's images, imported waits and registrations are deliberately
    // NOT torn down on this path. A rejected batch is not in flight, but the
    // caller's input may still be referenced by work that did land, and the
    // pending record is what accounts for it.
    result = SubmitStagedInputFrame(encodeFrameInfo);
    if (result != VK_SUCCESS) {
        return result;
    }

    // and encode the input frame with the encoder next
    return EncodeFrameCommon(encodeFrameInfo);
}

VkResult VkVideoEncoder::SubmitStagedQpMap(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    assert(encodeFrameInfo);
    assert(encodeFrameInfo->qpMapCmdBuffer != nullptr);

    const VkCommandBuffer* pCmdBuf = encodeFrameInfo->qpMapCmdBuffer->GetCommandBuffer();
    VkSemaphore frameCompleteSemaphore = encodeFrameInfo->qpMapCmdBuffer->GetSemaphore();

    VkCommandBufferSubmitInfoKHR cmdBufferInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR };
    cmdBufferInfo.commandBuffer = *pCmdBuf;
    cmdBufferInfo.deviceMask = 0;

    VkSemaphoreSubmitInfoKHR signalSemaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR };
    signalSemaphoreInfo.semaphore = frameCompleteSemaphore;
    signalSemaphoreInfo.value = 0; // Binary semaphore
    signalSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR; // Signal after transfer operations complete
    signalSemaphoreInfo.deviceIndex = 0;

    VkSubmitInfo2KHR submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR, nullptr };
    submitInfo.flags = 0;
    submitInfo.waitSemaphoreInfoCount = 0;
    submitInfo.pWaitSemaphoreInfos = nullptr;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdBufferInfo;
    submitInfo.signalSemaphoreInfoCount = (frameCompleteSemaphore != VK_NULL_HANDLE) ? 1 : 0;
    submitInfo.pSignalSemaphoreInfos = (frameCompleteSemaphore != VK_NULL_HANDLE) ? &signalSemaphoreInfo : nullptr;

    VkFence queueCompleteFence = encodeFrameInfo->qpMapCmdBuffer->GetFence();
    assert(VK_NOT_READY == m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, queueCompleteFence));

    VkResult result = m_vkDevCtx->MultiThreadedQueueSubmit(((m_vkDevCtx->GetVideoEncodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) != 0) ?
                                                                     VulkanDeviceContext::ENCODE : VulkanDeviceContext::TRANSFER,
                                                             0, // queueIndex
                                                             1, // submitCount
                                                             &submitInfo, queueCompleteFence,
                                                             "Encode Staging QpMap",
                                                             m_encodeEncodeFrameNum,
                                                             m_encodeInputFrameNum);

    // As for the input staging buffer above: a rejected batch is not in
    // flight and must not be recorded as if it were.
    if (result == VK_SUCCESS) {
        encodeFrameInfo->qpMapCmdBuffer->SetCommandBufferSubmitted();
    }
    bool syncCpuAfterStaging = false;
    if (syncCpuAfterStaging) {
        encodeFrameInfo->qpMapCmdBuffer->SyncHostOnCmdBuffComplete(false, "encoderStagedInputFence");
    }
    return result;
}

/**
 * @brief Copies YCbCr planes directly from input buffer to output buffer when formats are the same
 *
 * This function efficiently copies YCbCr data between buffers when the number of planes
 * and bit depth are identical, but potentially with different pitch values. It handles
 * 1, 2, or 3 plane formats and supports 8-bit and high bit-depth formats (10, 12, 16 bit).
 * Properly handles different chroma subsampling (4:4:4, 4:2:2, 4:2:0).
 *
 * @param pInputFrameData Source buffer containing YCbCr planes
 * @param inputPlaneLayouts Array of source buffer plane layouts (offset, pitch, etc.)
 * @param writeImagePtr Destination buffer for the YCbCr planes
 * @param dstSubresourceLayout Array of destination buffer plane layouts
 * @param width Width of the image in pixels
 * @param height Height of the image in pixels
 * @param numPlanes Number of planes in the format (1, 2, or 3)
 * @param format The VkFormat of the image for proper subsampling and bit depth detection
 */
void VkVideoEncoder::CopyYCbCrPlanesDirectCPU(
    const uint8_t* pInputFrameData,
    const VkSubresourceLayout* inputPlaneLayouts,
    uint8_t* writeImagePtr,
    const VkSubresourceLayout* dstSubresourceLayout,
    uint32_t width,
    uint32_t height,
    uint32_t numPlanes,
    VkFormat format)
{
    // Get format information
    const VkMpFormatInfo* formatInfo = YcbcrVkFormatInfo(format);

    // Packed 4:4:4 (AYUV / Y410 / Y416) has no multi-planar descriptor, so formatInfo is
    // NULL for it. Falling through to the 8-bit default below would set bytesPerPixel to
    // 1 for what is a 4-byte container and copy only width*1 of each width*4 row --
    // exactly one quarter of every scanline, leaving the rest of the staging image
    // with stale data.
    const VkPackedYcbcrFormatDesc* packedDesc = PackedYcbcrFormatDesc(format);

    // Determine bit depth and bytes per pixel from format
    const uint32_t bitDepth = (packedDesc != nullptr) ? packedDesc->bitDepth :
                              (formatInfo != nullptr) ? GetBitsPerChannel(formatInfo->planesLayout) : 8; // Default to 8-bit
    const uint32_t bytesPerPixel = (packedDesc != nullptr) ? packedDesc->bytesPerPixel :
                                   (bitDepth > 8) ? 2 : 1;

    // Determine chroma subsampling ratios
    const uint32_t chromaHorzRatio = (formatInfo != nullptr) ? (1 << formatInfo->planesLayout.secondaryPlaneSubsampledX) : 1;
    const uint32_t chromaVertRatio = (formatInfo != nullptr) ? (1 << formatInfo->planesLayout.secondaryPlaneSubsampledY) : 1;

    // Log the format subsampling for debugging
    if (m_encoderConfig->verbose) {
        const char* subsamplingDesc = "4:4:4";
        if (chromaHorzRatio == 2 && chromaVertRatio == 2) {
            subsamplingDesc = "4:2:0";
        } else if (chromaHorzRatio == 2 && chromaVertRatio == 1) {
            subsamplingDesc = "4:2:2";
        }
        VkEncPrintfOut("YCbCr copy with %s subsampling (chromaHorzRatio=%d, chromaVertRatio=%d), %d-bit\n",
               subsamplingDesc, chromaHorzRatio, chromaVertRatio, bitDepth);
    }

    // Handle all planes
    for (uint32_t plane = 0; plane < numPlanes; plane++) {
        // Source and destination plane pointers
        const uint8_t* srcPlane = pInputFrameData + inputPlaneLayouts[plane].offset;
        uint8_t* dstPlane = writeImagePtr + dstSubresourceLayout[plane].offset;

        // Get plane dimensions - adjust for chroma planes
        uint32_t planeWidth = width;
        uint32_t planeHeight = height;

        // Adjust dimensions for chroma planes based on format subsampling
        if (plane > 0) {
            if (chromaHorzRatio > 1) {
                planeWidth = (width + chromaHorzRatio - 1) / chromaHorzRatio;
            }
            if (chromaVertRatio > 1) {
                planeHeight = (height + chromaVertRatio - 1) / chromaVertRatio;
            }
        }

        // Source and destination strides
        assert(inputPlaneLayouts[plane].rowPitch <= SIZE_MAX);
        assert(dstSubresourceLayout[plane].rowPitch <= SIZE_MAX);
        const size_t srcStride = (size_t)inputPlaneLayouts[plane].rowPitch;
        const size_t dstStride = (size_t)dstSubresourceLayout[plane].rowPitch;

        // Line width in bytes
        const size_t lineBytes = planeWidth * bytesPerPixel;

        // Get the starting pointers for this plane
        const uint8_t* srcRow = srcPlane;
        uint8_t* dstRow = dstPlane;

        if (false && (bitDepth > 8)) {

            const int shiftBits = 16 - bitDepth;

            // Copy each line, incrementing pointers by stride amounts
            for (uint32_t y = 0; y < planeHeight; y++) {

                // Get the starting pointers for this row
                const uint16_t* srcRow16 = (const uint16_t*)srcRow;
                uint16_t* dstRow16 = (uint16_t*)dstRow;

                for (uint32_t i = 0; i < planeWidth; i++) {
                    *dstRow16++ = (*srcRow16++ << shiftBits);
                }

                // Advance to the next line using pointer arithmetic
                srcRow += srcStride;
                dstRow += dstStride;
            }

        } else {

            // Copy each line, incrementing pointers by stride amounts
            for (uint32_t y = 0; y < planeHeight; y++) {
                // Copy the current line
                memcpy(dstRow, srcRow, lineBytes);

                // Advance to the next line using pointer arithmetic
                srcRow += srcStride;
                dstRow += dstStride;
            }
        }
    }
}

VkResult VkVideoEncoder::SubmitStagedInputFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    assert(encodeFrameInfo);
    assert(encodeFrameInfo->inputCmdBuffer != nullptr);

    const VkCommandBuffer* pCmdBuf = encodeFrameInfo->inputCmdBuffer->GetCommandBuffer();
    VkSemaphore frameCompleteSemaphore = encodeFrameInfo->inputCmdBuffer->GetSemaphore();

    VkCommandBufferSubmitInfoKHR cmdBufferInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR };
    cmdBufferInfo.commandBuffer = *pCmdBuf;
    cmdBufferInfo.deviceMask = 0;

    // WHAT THIS BATCH ACTUALLY CONTAINS, and therefore what its semaphore
    // signals may name. A semaphore signal's FIRST synchronization scope is
    // restricted to the stages in stageMask: signalling at TRANSFER on a
    // batch whose only real work is a compute dispatch guarantees nothing
    // about that dispatch. The copy branch keeps the TRANSFER mask it has
    // always had; the filter branch -- newly reachable -- must not, because
    // the semaphores signalled here are the ones the embedder's release fence
    // is exported from (SubmitExternalFrameCommon) and the one the encode
    // submit waits on.
    //
    // ALL_COMMANDS rather than COMPUTE_SHADER: the filter branch shares this
    // command buffer with the QP-map staging copy (StageInputFrameQpMap), so
    // the batch can carry transfer work as well, and a stage mask that names
    // only the dispatch would leave that outside the signal's scope.
    const VkPipelineStageFlags2 stagedInputSignalStage =
        encodeFrameInfo->inputFilterRecorded
            ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
            : VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;

    const uint32_t MAX_SIGNAL_SEMAPHORES = 2;
    uint32_t signalSemaphoreCount = 0;
    VkSemaphoreSubmitInfoKHR signalSemaphoreInfos[MAX_SIGNAL_SEMAPHORES]{};

    if (frameCompleteSemaphore != VK_NULL_HANDLE) {
        assert(signalSemaphoreCount < MAX_SIGNAL_SEMAPHORES);
        signalSemaphoreInfos[signalSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        signalSemaphoreInfos[signalSemaphoreCount].semaphore = frameCompleteSemaphore;
        signalSemaphoreInfos[signalSemaphoreCount].value = 0; // Binary semaphore
        signalSemaphoreInfos[signalSemaphoreCount].stageMask = stagedInputSignalStage;
        signalSemaphoreInfos[signalSemaphoreCount].deviceIndex = 0;
        signalSemaphoreCount++;
    }

    if (encodeFrameInfo->subsampledImageResource) {

        // Set the semaphore for the output image
        VkSemaphoreSubmitInfoKHR subsampledImageResourceSem =
                encodeFrameInfo->subsampledImageResource->SetTimelineSemaphoreValue(
                        GetSemaphoreValue(SYNC_INPUT_PREPROCESSING_COMPLETE, encodeFrameInfo->frameInputOrderNum),
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR,
                        0);

        // Also signal binary semaphore if present (for compatibility)
        if (subsampledImageResourceSem.semaphore != VK_NULL_HANDLE) {
            assert(signalSemaphoreCount < MAX_SIGNAL_SEMAPHORES);
            signalSemaphoreInfos[signalSemaphoreCount] = subsampledImageResourceSem;
            signalSemaphoreCount++;
        }
    }

    // === External frame input: inject wait semaphores ===
    std::vector<VkSemaphoreSubmitInfoKHR> waitSemaphoreInfos;
    if (encodeFrameInfo->isExternalInput && !encodeFrameInfo->inputWaitSemaphores.empty()) {
        for (size_t i = 0; i < encodeFrameInfo->inputWaitSemaphores.size(); i++) {
            VkSemaphoreSubmitInfoKHR waitInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR};
            waitInfo.semaphore = encodeFrameInfo->inputWaitSemaphores[i];
            waitInfo.value = (i < encodeFrameInfo->inputWaitSemaphoreValues.size())
                                 ? encodeFrameInfo->inputWaitSemaphoreValues[i] : 0;
            // Same reasoning as the signal side, in the other direction: the
            // default names the stage that CONSUMES the producer's image, and
            // on the filter branch that is the compute dispatch, not a
            // transfer. A TRANSFER-only wait leaves the dispatch outside the
            // second synchronization scope, i.e. reading the producer's
            // dma-buf before the acquire semaphore is signalled.
            waitInfo.stageMask = (i < encodeFrameInfo->inputWaitDstStageMasks.size())
                                     ? encodeFrameInfo->inputWaitDstStageMasks[i]
                                     : stagedInputSignalStage;
            waitInfo.deviceIndex = 0;
            waitSemaphoreInfos.push_back(waitInfo);
        }
    }

    // === External frame input: inject signal semaphores ===
    // These are appended to the existing signal semaphores (frameCompleteSemaphore, subsampled TL)
    std::vector<VkSemaphoreSubmitInfoKHR> allSignalSemaphoreInfos(
        signalSemaphoreInfos, signalSemaphoreInfos + signalSemaphoreCount);
    if (encodeFrameInfo->isExternalInput && !encodeFrameInfo->inputSignalSemaphores.empty()) {
        for (size_t i = 0; i < encodeFrameInfo->inputSignalSemaphores.size(); i++) {
            VkSemaphoreSubmitInfoKHR signalInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR};
            signalInfo.semaphore = encodeFrameInfo->inputSignalSemaphores[i];
            signalInfo.value = (i < encodeFrameInfo->inputSignalSemaphoreValues.size())
                                   ? encodeFrameInfo->inputSignalSemaphoreValues[i] : 0;
            // The RELEASE timeline: this is what the embedder's release-fence
            // SYNC_FD is exported from, so signalling it at TRANSFER on a
            // compute batch tells the producer "input released" while the
            // filter is still sampling its dma-buf.
            signalInfo.stageMask = stagedInputSignalStage;
            signalInfo.deviceIndex = 0;
            allSignalSemaphoreInfos.push_back(signalInfo);
        }
    }

    // TODO: Convert to TL semaphore, input -> AQ -> Encode
    VkSubmitInfo2KHR submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR, nullptr };
    submitInfo.flags = 0;
    submitInfo.waitSemaphoreInfoCount = static_cast<uint32_t>(waitSemaphoreInfos.size());
    submitInfo.pWaitSemaphoreInfos = waitSemaphoreInfos.empty() ? nullptr : waitSemaphoreInfos.data();
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdBufferInfo;
    submitInfo.signalSemaphoreInfoCount = static_cast<uint32_t>(allSignalSemaphoreInfos.size());
    submitInfo.pSignalSemaphoreInfos = allSignalSemaphoreInfos.empty() ? nullptr : allSignalSemaphoreInfos.data();

    VkFence queueCompleteFence = encodeFrameInfo->inputCmdBuffer->GetFence();
    assert(VK_NOT_READY == m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, queueCompleteFence));
    // THE SUBMIT QUEUE IS KEYED OFF THE POOL'S FAMILY, NOT OFF THE BRANCH THAT
    // RAN, and that is forced rather than preferred. Both branches record into a
    // command buffer from m_inputCommandBufferPool, and InitEncoder creates that
    // pool on ONE family per session -- the compute family when the filter
    // exists, because the filter IS the pool. A command buffer may only be
    // submitted to a queue of its pool's family
    // (VUID-vkQueueSubmit2-commandBuffer-03874), so a copy-branch frame on a
    // filter-bearing session cannot legally be sent to the transfer queue
    // whatever its barriers say. Keying the submit off the branch would trade a
    // wedged queue for an invalid submit.
    //
    // So the dependency points the other way: both barrier sites read the
    // family from GetStagedInputQueueFamilyIdx(), and this submit reads the
    // queue from GetStagedInputSubmitType() -- the same fact, twice. The
    // assertion below is the invariant stated where it can fail loudly.
    const VulkanDeviceContext::QueueFamilySubmitType submitType =
            GetStagedInputSubmitType();
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    // A recorded filter dispatch implies a compute-family batch. If this ever
    // fires, a filter ran on a session whose input pool is not the filter's,
    // and the acquires recorded above name a family this submit is not on.
    assert(!encodeFrameInfo->inputFilterRecorded ||
           (submitType == VulkanDeviceContext::COMPUTE));
#endif

    VkResult result = m_vkDevCtx->MultiThreadedQueueSubmit(submitType,
                                                           0, // queueIndex
                                                           1, // submitCount
                                                           &submitInfo,
                                                           queueCompleteFence,
                                                           "Encode Staging Input",
                                                           m_encodeEncodeFrameNum,
                                                           m_encodeInputFrameNum);

    // Only a submit the driver ACCEPTED puts this node in flight. Marking a
    // rejected batch submitted tells the release-fence export that a signal
    // operation is pending execution when none was ever queued, and leaves
    // the node claiming a fence that will never be signalled. Rejected
    // commands stay Recorded, which is what they are, and reset/reuse
    // proceeds from there.
    if (result == VK_SUCCESS) {
        encodeFrameInfo->inputCmdBuffer->SetCommandBufferSubmitted();
    }
    bool syncCpuAfterStaging = false;
    if (syncCpuAfterStaging) {
        encodeFrameInfo->inputCmdBuffer->SyncHostOnCmdBuffComplete(false, "encoderStagedInputFence");
    }
#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
    if (result == VK_SUCCESS) {

        if (m_displayQueue.IsValid()) {

            // Optionally, submit the input frame for preview by the display, if enabled.
            VulkanEncoderInputFrame displayEncoderInputFrame;
            displayEncoderInputFrame.pictureIndex = (int32_t)encodeFrameInfo->frameInputOrderNum;
            displayEncoderInputFrame.displayOrder = encodeFrameInfo->gopPosition.inputOrder;
            displayEncoderInputFrame.frameCompleteSemaphore = frameCompleteSemaphore;
            // displayEncoderInputFrame.frameCompleteFence = currentEncodeFrameData->m_frameCompleteFence;
            encodeFrameInfo->srcEncodeImageResource->GetImageView(
                    displayEncoderInputFrame.imageViews[VulkanEncoderInputFrame::IMAGE_VIEW_TYPE_OPTIMAL_DISPLAY].singleLevelView );
            displayEncoderInputFrame.imageViews[VulkanEncoderInputFrame::IMAGE_VIEW_TYPE_OPTIMAL_DISPLAY].inUse = true;

            // One can also look at the linear input instead
            // displayEncoderInputFrame.imageView = currentEncodeFrameData->m_linearInputImage;
            displayEncoderInputFrame.displayWidth  = m_encoderConfig->encodeWidth;
            displayEncoderInputFrame.displayHeight = m_encoderConfig->encodeHeight;

            m_displayQueue.EnqueueFrame(&displayEncoderInputFrame);
        }
    }
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT
    return result;
}

VkResult VkVideoEncoder::AssembleBitstreamData(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                               uint32_t frameIdx, uint32_t ofTotalFrames)
{

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("assemble bitstream", 6, encodeFrameInfo, frameIdx, ofTotalFrames);
    }


    BitstreamReadback readback{};
    VkResult result = ReadbackBitstreamData(encodeFrameInfo, readback);
    if (result != VK_SUCCESS) {
        VkEncPrintfErr("\nAssembleBitstreamData Error: bitstream readback failed with result 0x%x.\n", result);
        assert(result == VK_SUCCESS);
        return result;
    }

    // On the synchronous path every frame must carry a bitstream buffer;
    // readbackDone == false here means the frame had no buffer or command
    // buffer, which would silently drop the frame's coded data.
    if (!readback.readbackDone) {
        VkEncPrintfErr("\nAssembleBitstreamData Error: no bitstream buffer to read back for frame %u.\n", frameIdx);
        assert(readback.readbackDone);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // VkVideoEncoder uses GPU mapped memory to write to file.
    //
    // The NON-PUBLISHING file-output arm, deliberately -- NOT the virtual
    // WriteBitstreamToFile() that upstream calls from this line.
    //
    // Upstream split the old inline body into ReadbackBitstreamData()
    // + WriteBitstreamToFile() and put that call here, where upstream's
    // WriteBitstreamToFile is a plain file writer. In THIS tree the same name
    // is the completion funnel: it ends in an unconditional
    // PushCapturedBitstream(), the only producer of a CapturedBitstream in the
    // tree. The two edits never overlapped textually, so the graft merged
    // cleanly and silently handed the SYNCHRONOUS path a completion publish it
    // is documented never to have -- see the guard in ProcessOrderedFrames(),
    // the WriteDataToFile() contract in the header, and
    // test/encoder-ext-drain-assembly. Publishing from here cannot work: a
    // record raised inline from SetExternalInputFrame() arrives before
    // EnqueuePendingFrame() has created the PendingFrame it must land on, so
    // DrainCapturesLocked() discards it into m_lateCaptures. Only the
    // assembly worker may publish, because only it runs after that
    // PendingFrame exists.
    //
    // WriteBitstreamToFileOutput() is byte-for-byte the body upstream's
    // WriteBitstreamToFile carries, so this call is upstream-equivalent in
    // behaviour while keeping the funnel private to the worker. It also
    // restores agreement with VkVideoEncoderAV1::AssembleBitstreamData, which
    // reaches no publish either.
    //
    // ANY future rebase that re-points this line back at WriteBitstreamToFile()
    // reintroduces the defect. test/encoder-sync-assembly is the RED for it.
    result = WriteBitstreamToFileOutput(encodeFrameInfo, readback);
    if (result != VK_SUCCESS) {
        VkEncPrintfErr("Error writing bitstream data to file\n");
        assert(result == VK_SUCCESS);
        return result;
    }

    if (m_psnr && (m_psnr->Enabled() || m_psnr->SrcCaptureEnabled())) {
        m_psnr->ComputeFramePsnr(encodeFrameInfo.get());
    }

    // POST-FENCE, and that is load-bearing: the probe's readback lands in
    // HOST_VISIBLE memory written by the staging command buffer, which this
    // frame's encode command buffer is ordered after. Scoring it before the
    // fence would read whatever the mapping happened to hold. ScoreCapture
    // is a no-op for the frames that carry no capture, which is all of them
    // but the first of each armed registration.
    if (m_contentProbe) {
        m_contentProbe->ScoreCapture(encodeFrameInfo->contentProbeCapture);
    }

    if (m_crc.Enabled()) {
        m_crc.SignalFrameEnd((uint32_t)(encodeFrameInfo->gopPosition.inputOrder));
    }

    return result;
}

VkResult VkVideoEncoder::ReadbackBitstreamData(
    VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
    BitstreamReadback& readback)
{
    if (encodeFrameInfo->outputBitstreamBuffer == nullptr ||
        encodeFrameInfo->encodeCmdBuffer == nullptr) {
        readback.readbackDone = false;
        return VK_SUCCESS;
    }

    VkResult result = encodeFrameInfo->encodeCmdBuffer->SyncHostOnCmdBuffComplete(
        false, "asyncAssemblyFence");
    if (result != VK_SUCCESS) {
        VkEncPrintfErr("\nAsync assembly: fence wait failed with result 0x%x.\n", result);
        return result;
    }

    if (m_psnr && (m_psnr->Enabled() || m_psnr->SrcCaptureEnabled())) {
        std::lock_guard<std::mutex> psnrLock(m_assemblyFileMutex);
        m_psnr->ComputeFramePsnr(encodeFrameInfo.get());
    }

    // The ASYNC assembly lane's copy of the post-fence score above. Both
    // sites are needed and neither is redundant: a session runs one lane or
    // the other, and wiring only the synchronous one is exactly how the
    // async lane silently loses an observable (this tree has shipped that
    // mistake once already, on the completion-record path). No
    // m_assemblyFileMutex here -- the probe carries its own lock and touches
    // no file output.
    if (m_contentProbe) {
        m_contentProbe->ScoreCapture(encodeFrameInfo->contentProbeCapture);
    }

    uint32_t querySlotId = (uint32_t)-1;
    VkQueryPool queryPool = encodeFrameInfo->encodeCmdBuffer->GetQueryPool(querySlotId);

    struct QueryResult {
        uint32_t bitstreamStartOffset;
        uint32_t bitstreamSize;
        VkQueryResultStatusKHR status;
    } encodeResult{};

    result = m_vkDevCtx->GetQueryPoolResults(*m_vkDevCtx, queryPool, querySlotId,
                                             1, sizeof(encodeResult), &encodeResult,
                                             sizeof(encodeResult),
                                             VK_QUERY_RESULT_WITH_STATUS_BIT_KHR |
                                             VK_QUERY_RESULT_WAIT_BIT);
    if (result != VK_SUCCESS || encodeResult.status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR) {
        VkEncPrintfErr("\nAsync assembly: query failed (0x%x, status=0x%x).\n",
                result, encodeResult.status);
        return (result != VK_SUCCESS) ? result : VK_INCOMPLETE;
    }

    readback.bitstreamStartOffset = encodeResult.bitstreamStartOffset;
    readback.bitstreamSize = encodeResult.bitstreamSize;
    readback.status = encodeResult.status;
    readback.readbackDone = true;
    return VK_SUCCESS;
}

VkResult VkVideoEncoder::WriteBitstreamToFile(
    VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
    uint32_t frameIdx, uint32_t ofTotalFrames,
    BitstreamReadback& readback)
{
    // Every frame that reaches assembly publishes exactly one completion
    // record, in both output modes; only the payload differs. In capture
    // mode (disableFileOutput) the record carries the bytes the caller
    // retrieves; in file-output mode the bytes go to the file and the
    // record carries metadata plus the file-write result, so an ext
    // consumer at the config default still gets a truthful per-frame
    // completion instead of a deadline-synthesized VK_TIMEOUT drop.
    //
    // Key the record by the CALLER's frame id when this frame
    // came through SetExternalInputFrame(). frameEncodeInputOrderNum is
    // only a fallback for the file-based path -- it coincides with the
    // caller's ids solely in the no-error, no-reorder case and drifts
    // permanently after any partially failed submission.
    CapturedBitstream cap;
    cap.frameId = (encodeFrameInfo->externalFrameId != uint64_t(-1))
                      ? encodeFrameInfo->externalFrameId
                      : encodeFrameInfo->frameEncodeInputOrderNum;
    cap.isIdr = (encodeFrameInfo->gopPosition.pictureType ==
                 VkVideoGopStructure::FRAME_TYPE_IDR);
    cap.pictureType = static_cast<uint32_t>(
        encodeFrameInfo->gopPosition.pictureType);

    VkResult result = VK_SUCCESS;
    if (m_encoderConfig && m_encoderConfig->disableFileOutput) {
        if (encodeFrameInfo->bitstreamHeaderBufferSize > 0) {
            const uint8_t* hdr =
                encodeFrameInfo->bitstreamHeaderBuffer +
                encodeFrameInfo->bitstreamHeaderOffset;
            cap.bytes.insert(
                cap.bytes.end(), hdr,
                hdr + encodeFrameInfo->bitstreamHeaderBufferSize);
        }
        if (readback.readbackDone && readback.bitstreamSize > 0) {
            const uint8_t* src;
            if (!readback.bitstreamCopy.empty()) {
                src = readback.bitstreamCopy.data();
            } else {
                VkDeviceSize maxSize;
                // bitstreamStartOffset is relative to
                // encodeInfo.dstBufferOffset (header reservation).
                src = encodeFrameInfo->outputBitstreamBuffer->
                          GetDataPtr(0, maxSize) +
                      encodeFrameInfo->encodeInfo.dstBufferOffset +
                      readback.bitstreamStartOffset;
            }
            cap.bytes.insert(cap.bytes.end(), src,
                             src + readback.bitstreamSize);
        }
    } else {
        result = WriteBitstreamToFileOutput(encodeFrameInfo, readback);
        cap.status = result;  // VK_SUCCESS, or the file-write failure code
    }
    PushCapturedBitstream(std::move(cap));
    return result;
}

// File-output arm of WriteBitstreamToFile: writes the non-VCL header, then the
// coded payload described by readback, which ReadbackBitstreamData() has
// already fetched from the feedback query pool.
// Private and non-virtual: it must never grow a second completion publish.
VkResult VkVideoEncoder::WriteBitstreamToFileOutput(
    VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
    BitstreamReadback& readback)
{
    if(encodeFrameInfo->bitstreamHeaderBufferSize > 0) {
        size_t nonVcl = WriteDataToFile(encodeFrameInfo->bitstreamHeaderBuffer + encodeFrameInfo->bitstreamHeaderOffset,
                                        encodeFrameInfo->bitstreamHeaderBufferSize);

        if (m_encoderConfig->verboseFrameStruct) {
            VkEncOut() << "       == Non-Vcl data " << (nonVcl ? "SUCCESS" : "FAIL")
                      << " File Output non-VCL data with size: " << encodeFrameInfo->bitstreamHeaderBufferSize
                      << ", Input Order: " << encodeFrameInfo->gopPosition.inputOrder
                      << ", Encode  Order: " << encodeFrameInfo->gopPosition.encodeOrder
                      << std::endl << std::flush;
        }
    }

    if (readback.readbackDone && readback.bitstreamSize > 0) {
        const uint8_t* src;
        if (!readback.bitstreamCopy.empty()) {
            src = readback.bitstreamCopy.data();
        } else {
            VkDeviceSize maxSize;
            // The feedback query's bitstreamStartOffset is relative to the
            // bound bitstream buffer range, so honor dstBufferOffset too
            // (currently always 0, but keep the pointer math spec-correct).
            src = encodeFrameInfo->outputBitstreamBuffer->GetDataPtr(0, maxSize)
                + encodeFrameInfo->encodeInfo.dstBufferOffset
                + readback.bitstreamStartOffset;
        }

        size_t totalBytesWritten = 0;
        while (totalBytesWritten < readback.bitstreamSize) {
            size_t remaining = readback.bitstreamSize - totalBytesWritten;
            size_t written = WriteDataToFile(src + totalBytesWritten, remaining);
            if (written == 0) {
                VkEncPrintfErr("Error writing VCL data\n");
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            totalBytesWritten += written;
        }

        if (m_encoderConfig->verboseFrameStruct) {
            VkEncOut() << "       == Output VCL data " << ((totalBytesWritten == readback.bitstreamSize) ? "SUCCESS" : "FAIL") << " with size: " << readback.bitstreamSize
                      << " and offset: " << readback.bitstreamStartOffset
                      << ", Input Order: " << encodeFrameInfo->gopPosition.inputOrder
                      << ", Encode  Order: " << encodeFrameInfo->gopPosition.encodeOrder << std::endl << std::flush;
        }
    }

    return VK_SUCCESS;
}

void VkVideoEncoder::AssemblyWorkerThread(int threadId)
{
    {
        char threadName[16];
        snprintf(threadName, sizeof(threadName), "VkEncAsm%d", threadId);
        vkenc::OsSetCurrentThreadName(threadName);
    }

    if (m_encoderConfig->verbose) {
        VkEncOut() << "[AsyncAssembly] Worker " << threadId << " started" << std::endl;
    }

    while (true) {
        AssemblyWorkItem item;
        bool success = m_assemblyQueue.WaitAndPop(item);
        if (!success) {
            if (m_assemblyQueue.ExitQueue()) break;
            continue;
        }

        auto& frame = item.frameInfo;
        assert(frame != nullptr);

        VkResult result = ReadbackBitstreamData(frame, item.readback);
        if (result != VK_SUCCESS) {
            VkEncPrintfErr("[AsyncAssembly] Worker %d: readback failed (0x%x) "
                    "seq=%lu\n", threadId, result,
                    (unsigned long)item.sequenceNumber);
            m_assemblyErrorCount++;
            // Deliver the per-frame failure through the completion funnel
            // in BOTH output modes -- an empty record with the failure
            // VkResult (e.g. VK_INCOMPLETE for a non-COMPLETE query status
            // such as INSUFFICIENT_BITSTREAM_BUFFER_RANGE). A frame dropped
            // here without a record never surfaces at the Ext caller's
            // retrieval, so the stream stalls with no diagnosis instead of
            // an actionable per-frame error.
            {
                std::unique_lock<std::mutex> lock(m_assemblyFileMutex);
                // Wait for this frame's turn before advancing the hand-off.
                // The success path below waits on exactly this predicate, so
                // advancing out of turn -- as this path used to -- steps past a
                // lower-numbered worker's slot and strands it forever: the
                // condition variable has no timeout, so the whole assembly
                // pipeline wedges with nothing logged. Taking the turn also
                // keeps failed captures in submission order with successful
                // ones, which the consumer's FIFO assumes.
                m_assemblyOrderCV.wait(lock, [&] {
                    return item.sequenceNumber == m_nextWriteSequence.load();
                });
                CapturedBitstream cap;
                cap.frameId = (frame->externalFrameId != uint64_t(-1))
                                  ? frame->externalFrameId
                                  : frame->frameEncodeInputOrderNum;
                cap.isIdr = false;
                cap.pictureType = 0;
                cap.status = result;
                PushCapturedBitstream(std::move(cap));
                m_nextWriteSequence++;
            }
            m_assemblyOrderCV.notify_all();
            ReleaseAssemblyItem(item);
            continue;
        }

        // PSNR / recon capture for the threaded path happens inside
        // ReadbackBitstreamData (locked, right after the fence wait). A
        // second call here read the same frame's recon state without holding
        // that lock, concurrently with the worker that does. In steady state
        // it was a no-op -- ComputeFramePsnr nulls its staging image, so the
        // second call found nothing to measure -- which is why nothing
        // visibly broke; the unsynchronised read is the reason it is gone,
        // not a miscount.

        {
            std::unique_lock<std::mutex> lock(m_assemblyFileMutex);
            m_assemblyOrderCV.wait(lock, [&] {
                return item.sequenceNumber == m_nextWriteSequence.load();
            });

            result = WriteBitstreamToFile(frame,
                                          (uint32_t)item.sequenceNumber,
                                          (uint32_t)item.sequenceNumber + 1,
                                          item.readback);
            if (result != VK_SUCCESS) {
                VkEncPrintfErr("[AsyncAssembly] Worker %d: write failed (0x%x) "
                        "seq=%lu\n", threadId, result,
                        (unsigned long)item.sequenceNumber);
                m_assemblyErrorCount++;
            }

            m_nextWriteSequence++;
        }
        m_assemblyOrderCV.notify_all();

        ReleaseAssemblyItem(item);
    }

    if (m_encoderConfig->verbose) {
        VkEncOut() << "[AsyncAssembly] Worker " << threadId << " exiting" << std::endl;
    }
}

// Hands a deferred-frame chain to the assembly workers, and shortens |frames|
// by exactly the frames it hands over: on return |frames| is the part of the
// chain the encoder still owns -- empty when every frame was queued, and the
// unqueued remainder when a push was refused. A queued frame is released by
// the worker that finishes it, so this is what lets the caller release what is
// left without reaching a frame a worker is already assembling.
VkResult VkVideoEncoder::QueueFramesForAssembly(
    VkSharedBaseObj<VkVideoEncodeFrameInfo>& frames, uint32_t numFrames)
{
    while (frames != nullptr) {
        // Read the link while this thread is still the frame's only owner.
        // From the moment the item is on the queue a worker may take it and
        // clear the frame's links, so the chain cannot be walked across a push.
        VkSharedBaseObj<VkVideoEncodeFrameInfo> next = frames->dependantFrames;

        AssemblyWorkItem item;
        item.frameInfo = frames;
        // Assign the number, consume it only on a successful push. An
        // incremented-then-abandoned number (Push fails only when the queue
        // is flushing) would never take its turn, and every later item would
        // wait forever on the timeout-less ordering condition variable.
        // Safe unlocked: this method is session-serial (submit thread only).
        item.sequenceNumber = m_assemblySequenceCounter;

        bool pushed = m_assemblyQueue.Push(item);
        if (!pushed) {
            VkEncPrintfErr("[AsyncAssembly] Failed to push to assembly queue\n");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        m_assemblySequenceCounter++;

        // Ownership of this frame has moved to the queued item.
        frames = next;
    }
    return VK_SUCCESS;
}

void VkVideoEncoder::ReleaseAssemblyItem(AssemblyWorkItem& item)
{
    if (item.frameInfo) {
        // Detach children before Reset — children are queued as independent
        // work items and may already be freed by another worker thread.
        // Without this, Reset() calls ReleaseChildrenFrames() which drops
        // shared_ptr references to already-freed children → UAF.
        item.frameInfo->dependantFrames = nullptr;
        item.frameInfo->Reset(true);
        item.frameInfo = nullptr;
    }
}

#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
// Which conversion the preprocess compute filter has to perform, derived from
// the two formats the filter sits between: the format frames ARRIVE in and
// the format the device accepts as an encode source.
//
// CONTEXT_DESIGN:594-598 makes the mechanism choice the LIBRARY's -- "query
// the device first, use hardware if it exists, compute if it does not" -- and
// this is the second half of that: once the answer is "compute", something
// still has to say WHICH compute. Until now nothing did.
// EncoderConfig::filterType was initialised to YCBCRCOPY and assigned nowhere
// outside TestCases.cpp (DEVICE_INDEPENDENCE_PLAN:253-256), which is why
// enabling the filter without plumbing it yields a copy rather than a
// conversion.
//
// NOTE, honestly: no document in the corpus states this mapping. The values
// are enumerated (GLSLANG:384) and the obligation to assign one is stated,
// but which value belongs to which format pair is a choice made here. The
// reasoning is the filter's own contract, from VulkanFilterYuvCompute.h:
// YCBCRCOPY is the compute-based copy that performs format, plane-count and
// bit-depth conversion between two YCbCr formats -- explicitly contrasted
// there with the XFER_* transfer modes, which "must have matching plane
// counts". A 3-plane I420 source and a 2-plane NV12 destination is exactly
// that contrast, so it is YCBCRCOPY and not a transfer.
static VulkanFilterYuvCompute::FilterType VkEncDeriveFilterType(
    VkFormat filterInputFormat, VkFormat encodeSourceFormat)
{
    const bool inputIsYcbcr  = (YcbcrVkFormatInfo(filterInputFormat)  != nullptr);
    const bool outputIsYcbcr = (YcbcrVkFormatInfo(encodeSourceFormat) != nullptr);
    if (!inputIsYcbcr && outputIsYcbcr) {
        return VulkanFilterYuvCompute::RGBA2YCBCR;
    }
    if (inputIsYcbcr && !outputIsYcbcr) {
        return VulkanFilterYuvCompute::YCBCR2RGBA;
    }
    // YCbCr -> YCbCr, including the identity. YCBCRCOPY is a compute pass
    // either way; the plane-count and bit-depth handling it carries is what
    // the 3-plane -> 2-plane case needs, and the identity case is what the
    // file-input path has always used.
    return VulkanFilterYuvCompute::YCBCRCOPY;
}
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED

VkResult VkVideoEncoder::InitEncoder(VkSharedBaseObj<EncoderConfig>& encoderConfig)
{

    if (!VulkanVideoCapabilities::IsCodecTypeSupported(m_vkDevCtx,
                                                       m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                                       encoderConfig->codec)) {
        VkEncErr() << "ERROR [" << __FILE__ << ":" << __LINE__ << "]: "
                  << "The video codec " << VkVideoCoreProfile::CodecToName(encoderConfig->codec)
                  << " is not supported!" << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    m_encoderConfig = encoderConfig;

    m_crc.BeginCrcCalculation(encoderConfig->crcInitValue,
                              encoderConfig->outputCrcPerFrame,
                              encoderConfig->crcOutputFileName);

    // Update the video profile
    encoderConfig->InitVideoProfile();

    VkResult result = encoderConfig->InitDeviceCapabilities(m_vkDevCtx);
    if (result != VK_SUCCESS) {
        VkEncErr() << "ERROR [" << __FILE__ << ":" << __LINE__ << "]: "
                  << "InitDeviceCapabilities() failed. VkResult: " << result
                  << " (0x" << std::hex << result << std::dec << ")"
                  << " - The video profile/format may not be supported by the driver." << std::endl;
        return result;
    }

    if (encoderConfig->qualityLevel >= encoderConfig->videoEncodeCapabilities.maxQualityLevels) {
        VkEncErr() << "ERROR [" << __FILE__ << ":" << __LINE__ << "]: "
                  << "Quality level " << encoderConfig->qualityLevel
                  << " is greater than the maximum supported quality level "
                  << (encoderConfig->videoEncodeCapabilities.maxQualityLevels - 1) << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (encoderConfig->useDpbArray == false &&
        (encoderConfig->videoCapabilities.flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR) == 0) {
        VkEncOut() << "Separate DPB was requested, but the implementation does not support it!" << std::endl;
        VkEncOut() << "Fallback to layered DPB!" << std::endl;
        encoderConfig->useDpbArray = true;
    }

    if (m_encoderConfig->enableQpMap) {
        if ((m_encoderConfig->qpMapMode == EncoderConfig::DELTA_QP_MAP) &&
            ((m_encoderConfig->videoEncodeCapabilities.flags & VK_VIDEO_ENCODE_CAPABILITY_QUANTIZATION_DELTA_MAP_BIT_KHR) == 0)) {
                VkEncErr() << "ERROR [" << __FILE__ << ":" << __LINE__ << "]: "
                          << "Delta QP Map was requested, but the implementation does not support it!" << std::endl;
                return VK_ERROR_INITIALIZATION_FAILED;
        }
        if ((m_encoderConfig->qpMapMode == EncoderConfig::EMPHASIS_MAP) &&
            ((m_encoderConfig->videoEncodeCapabilities.flags & VK_VIDEO_ENCODE_CAPABILITY_EMPHASIS_MAP_BIT_KHR) == 0)) {
                VkEncErr() << "ERROR [" << __FILE__ << ":" << __LINE__ << "]: "
                          << "Emphasis Map was requested, but the implementation does not support it!" << std::endl;
                return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    if (m_encoderConfig->enableIntraRefresh) {
        VkVideoEncodeIntraRefreshModeFlagBitsKHR mode = VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_NONE_KHR;
        const char* modeString = nullptr;

        if (!VulkanVideoCapabilities::IsVideoEncodeIntraRefreshSupported(m_vkDevCtx)) {
            VkEncOut() << "Intra-refresh has been requested, but the implementation does not support it." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        switch (m_encoderConfig->intraRefreshMode) {
        case EncoderConfig::REFRESH_PER_PARTITION:
            mode = VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_PER_PICTURE_PARTITION_BIT_KHR;
            modeString = "Per-picture partition";
            break;
        case EncoderConfig::REFRESH_BLOCK_ROWS:
            mode = VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_BLOCK_ROW_BASED_BIT_KHR;
            modeString = "Block row-based";
            break;
        case EncoderConfig::REFRESH_BLOCK_COLUMNS:
            mode = VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_BLOCK_COLUMN_BASED_BIT_KHR;
            modeString = "Block column-based";
            break;
        case EncoderConfig::REFRESH_BLOCKS:
            mode = VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_BLOCK_BASED_BIT_KHR;
            modeString = "Block-based";
            break;
        default:
            break;
        }

        if ((mode & m_encoderConfig->intraRefreshCapabilities.intraRefreshModes) == 0) {
            VkEncOut() << modeString << " intra-refresh was requested, but the implementation does not support it." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (m_encoderConfig->intraRefreshCycleDuration >
            m_encoderConfig->intraRefreshCapabilities.maxIntraRefreshCycleDuration) {
            VkEncOut() << "The requested intra-refresh cycle duration is greater than the maximum ("
                      << m_encoderConfig->intraRefreshCapabilities.maxIntraRefreshCycleDuration
                      << ") supported by the implementation" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    // Reconfigure the gopStructure structure because the device may not support
    // specific GOP structure. For example it may not support B-frames.
    if (m_encoderConfig->asyncAssembly) {
        m_encoderConfig->numInputImages += m_encoderConfig->numBitstreamBuffersToPreallocate;
    }

    // gopStructure.Init() should be called after  encoderConfig->InitDeviceCapabilities().
    m_encoderConfig->gopStructure.Init(m_encoderConfig->numFrames);
    if (encoderConfig->GetMaxBFrameCount() < m_encoderConfig->gopStructure.GetConsecutiveBFrameCount()) {
        if (m_encoderConfig->verbose) {
            VkEncOut() << "Max consecutive B frames: " << (uint32_t)encoderConfig->GetMaxBFrameCount() << " lower than the configured one: " << (uint32_t)m_encoderConfig->gopStructure.GetConsecutiveBFrameCount() << std::endl;
            VkEncOut() << "Fallback to the max value: " << (uint32_t)m_encoderConfig->gopStructure.GetConsecutiveBFrameCount() << std::endl;
        }
        m_encoderConfig->gopStructure.SetConsecutiveBFrameCount(encoderConfig->GetMaxBFrameCount());
    }

    if (m_encoderConfig->enableIntraRefresh) {
        if (!m_encoderConfig->IntraRefreshWithBFramesAllowed() &&
            (m_encoderConfig->gopStructure.GetConsecutiveBFrameCount() != 0)) {

            if (m_encoderConfig->verbose) {
                VkEncOut() << "Use of B-frames / compound prediction is not supported when intra-refresh is enabled" << std::endl;
                VkEncOut() << "Setting the count of Consecutive B-frames to 0" << std::endl;
            }
            m_encoderConfig->gopStructure.SetConsecutiveBFrameCount(0);
        }
    }

    // AV1 CAPTURE CANNOT REORDER IN THIS RELEASE.
    //
    // This is the definitive check, placed after gopStructure.Init(), after
    // the device-maximum clamp and after the intra-refresh adjustment, so it
    // reads the count that will actually be encoded rather than the one that
    // was requested -- the driver-preferred sentinel in particular only
    // resolves in InitDeviceCapabilities. It runs before pools and workers
    // start, so a refused session leaves nothing running.
    //
    // Reordering AV1 emits show-existing-frame headers, and the temporal unit
    // is assembled by the file writer rather than by the capture path. A
    // captured reordered stream is therefore missing those headers and its
    // frame identity cannot be reconstructed. File output keeps B-frames;
    // capture keeps B=0, which is what Chromium uses.
    if ((m_encoderConfig->codec == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) &&
        (m_encoderConfig->disableFileOutput != 0) &&
        (m_encoderConfig->gopStructure.GetConsecutiveBFrameCount() > 0)) {
        VkEncErr() << "[VkVideoEncoder] AV1 in-memory capture does not support "
                      "B-frames in this release (effective consecutive B "
                      "frames: "
                   << (uint32_t)m_encoderConfig->gopStructure.GetConsecutiveBFrameCount()
                   << "); use file output, or request 0" << std::endl;
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    if (m_encoderConfig->verbose) {
        VkEncOut() << std::endl << "GOP frame count: " << (uint32_t)m_encoderConfig->gopStructure.GetGopFrameCount();
        VkEncOut() << ", IDR period: " << (uint32_t)m_encoderConfig->gopStructure.GetIdrPeriod();
        VkEncOut() << ", Consecutive B frames: " << (uint32_t)m_encoderConfig->gopStructure.GetConsecutiveBFrameCount();
        m_encoderConfig->gopStructure.IsClosedGop() ? VkEncOut() << ", Closed GOP" : VkEncOut() << ", Open GOP";
        VkEncOut() << std::endl;

        const uint64_t maxFramesToDump = std::min<uint32_t>(m_encoderConfig->numFrames, m_encoderConfig->gopStructure.GetGopFrameCount() + 19);
        m_encoderConfig->gopStructure.PrintGopStructure(maxFramesToDump);

        if (m_encoderConfig->verboseFrameStruct) {
            m_encoderConfig->gopStructure.DumpFramesGopStructure(0, maxFramesToDump);
        }
    }

    if (m_encoderConfig->enableOutOfOrderRecording) {

        // Testing only - don't use for production!
        if (m_encoderConfig->gopStructure.GetConsecutiveBFrameCount() == 0) {
            // Queue at least 4 IDR, I, P frames to be able to test the out-of-order
            // recording sequence.
            m_holdRefFramesInQueue = 4;
        } else {
            // Queue atleast 2 reference frames along with non-ref frames
            m_holdRefFramesInQueue = 2;
        }

        if (m_holdRefFramesInQueue > 4) {
            // We don't want to make the queue too deep. This would require a lot of reference images
            m_holdRefFramesInQueue = 4;
        }

    }

    // The required num of DPB images.
    // Defense-in-depth cap. At H.264 Level >= 5.0 the legacy level-max
    // sizing in InitDpbCount() returned 17 Vulkan slots (16 refs + 1 setup;
    // the driver's maxDpbSlots=17 / maxActiveReferencePictures=16
    // advertisement is spec-correct). A 17 here breaks the H.264
    // DPB manager's eviction accounting (VkEncDpbH264::IsDpbFull counts 16
    // entries against a threshold of 17 -> eviction never runs -> the
    // reference set freezes -> progressive drift). The root fixes are the
    // DpbSequenceStart() clamp and need-based InitDpbCount() sizing; this
    // cap remains as defense-in-depth for any config path that still yields
    // >16, and additionally keeps clear of a driver slot-index-16
    // limitation: the driver hangs the encode engine (fence waits time out
    // with VK_ERROR_DEVICE_LOST) when a reference is bound at DPB slot
    // index 16. The cap is defense-in-depth on top of the DpbSequenceStart()
    // clamp, so that binding is never exercised by this encoder.
    m_maxDpbPicturesCount = std::min<uint32_t>(encoderConfig->InitDpbCount(), 16u);

    encoderConfig->InitRateControl();

    VkFormat supportedDpbFormats[8];
    VkFormat supportedInFormats[8];
    uint32_t formatCount = sizeof(supportedDpbFormats) / sizeof(supportedDpbFormats[0]);
    result = VulkanVideoCapabilities::GetVideoFormats(m_vkDevCtx, encoderConfig->videoCoreProfile,
                                                      VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
                                                      formatCount, supportedDpbFormats);

    if(result != VK_SUCCESS) {
        VkEncPrintfErr("\nInitEncoder Error: Failed to get desired video format for the DPB.\n");
        return result;
    }

    // Re-arm the capacity before the next query. GetVideoFormats takes formatCount by
    // REFERENCE and overwrites it with min(supportedFormatCount, formatCount)
    // (VulkanVideoCapabilities.h:377), so the DPB query above shrinks it to the DPB list
    // length and every later query silently inherits that as its cap. With the DPB list
    // shorter than the SRC list -- which is the normal case now that SRC advertises both
    // the semi-planar and the packed 4:4:4 form while the DPB is currently only
    // semi-planar -- the trailing SRC entries become invisible and the packed format can
    // never be selected.
    formatCount = sizeof(supportedInFormats) / sizeof(supportedInFormats[0]);
    result = VulkanVideoCapabilities::GetVideoFormats(m_vkDevCtx, encoderConfig->videoCoreProfile,
                                                      VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
                                                      formatCount, supportedInFormats);

    if(result != VK_SUCCESS) {
        VkEncPrintfErr("\nInitEncoder Error: Failed to get desired video format for input images.\n");
        return result;
    }

    m_imageDpbFormat = supportedDpbFormats[0];

    // Select the encode-source format that MATCHES the request, rather than taking
    // whatever the driver happened to list first.
    //
    // The driver returns every format compatible with the profile, and both the set and
    // the listing order are its choice. A 4:4:4 profile can advertise the semi-planar
    // and the packed form together (packed 4:4:4 rides an RGBA alias and is never a DPB
    // format), and nothing says which of them comes first, so element [0] can be either.
    // Even among the semi-planar formats [0] is only correct by luck -- the profile
    // filter narrows by chroma and bit depth, but nothing guarantees a unique survivor.
    m_imageInFormat = VK_FORMAT_UNDEFINED;
    const VkFormat requestedInFormat = encoderConfig->input.vkFormat;

    // --preferPackedYcbcr asks for the packed 4:4:4 encode source (AYUV / Y410) whenever
    // the driver offers one. It deliberately outranks the input-format match below: the
    // input format describes the FILE, and the compute filter converts from any source
    // layout to the encode source, so honouring the preference only when the file already
    // happened to be packed would make the option almost useless. Matching against the
    // advertised list (rather than naming a format) keeps this correct for profiles that
    // have no packed form -- 4:2:0 and 4:2:2 simply find nothing and fall through.
    if (encoderConfig->preferPackedYcbcr) {
        for (uint32_t fmtIdx = 0; fmtIdx < formatCount; fmtIdx++) {
            if (PackedYcbcrFormatDesc(supportedInFormats[fmtIdx]) != nullptr) {
                m_imageInFormat = supportedInFormats[fmtIdx];
                break;
            }
        }
        if ((m_imageInFormat == VK_FORMAT_UNDEFINED) && encoderConfig->verbose) {
            VkEncPrintfOut("--preferPackedYcbcr: no packed format advertised for this profile; "
                   "using the normal selection.\n");
        }
    }

    if (m_imageInFormat == VK_FORMAT_UNDEFINED) {
        for (uint32_t fmtIdx = 0; fmtIdx < formatCount; fmtIdx++) {
            if (supportedInFormats[fmtIdx] == requestedInFormat) {
                m_imageInFormat = supportedInFormats[fmtIdx];
                break;
            }
        }
    }
    if (m_imageInFormat == VK_FORMAT_UNDEFINED) {
        // Fall back to the driver's first choice so existing callers that never set
        // input.vkFormat keep working, but say so -- a silent substitution here is
        // how a 4:4:4 request ends up encoded as 4:2:0.
        m_imageInFormat = supportedInFormats[0];
        if (requestedInFormat != VK_FORMAT_UNDEFINED) {
            fprintf(stderr,
                    "\nInitEncoder Warning: requested encode-source format %d is not "
                    "advertised by the driver for this profile; falling back to %d. "
                    "The encoded chroma format will NOT match the request.\n",
                    (int)requestedInFormat, (int)m_imageInFormat);
            // Dump what the driver DOES offer. Without this the fallback tells you only
            // that your request was refused, not what to ask for instead -- and the set
            // is profile-dependent, so it cannot be inferred from a static table.
            VkEncPrintfErr("InitEncoder: driver advertises %u encode-source format(s) "
                            "for this profile:", formatCount);
            for (uint32_t fmtIdx = 0; fmtIdx < formatCount; fmtIdx++) {
                VkEncPrintfErr(" %d", (int)supportedInFormats[fmtIdx]);
            }
            VkEncPrintfErr("\n");
        }
    }

    // Without the preprocess filter the input image IS the encode source, so ANY
    // requested format the driver does not advertise for this profile cannot be
    // encoded -- there is nothing left to convert it. Which formats those are is a
    // property of the driver and the profile, not of a particular layout or
    // subsampling; the filter is what makes the rest reachable. Refuse here, where
    // the caller can still be told why and what to ask for instead.
    //
    // Without this check the fallback above substitutes the driver's first
    // advertised format while frames keep arriving in the requested one, and the
    // mismatch costs the device rather than the call: VK_ERROR_DEVICE_LOST and a
    // 0-byte bitstream, several hundred lines from its cause.
    //
    // Reachable by default rather than only on an unusual request, which is why it
    // is checked here at all: EncoderConfig::input.vkFormat defaults to a format no
    // driver advertises as an encode source.
    //
    // Not supporting a format is a legitimate configuration; losing the device
    // over it is not.
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    const bool preprocessFilterAvailable =
        (encoderConfig->enablePreprocessComputeFilter != 0);
#else
    // Filter compiled out: EncoderConfig has no enablePreprocessComputeFilter
    // field to read and there is no filter to turn on, so the refusal below
    // is unconditional.
    const bool preprocessFilterAvailable = false;
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED

    if (!preprocessFilterAvailable &&
        (requestedInFormat != VK_FORMAT_UNDEFINED) &&
        (requestedInFormat != m_imageInFormat)) {
        VkEncPrintfErr("\nInitEncoder Error: encode-source format %d was requested with the "
                "preprocess compute filter disabled, but the driver does not advertise "
                "it for this profile. Without the filter the input image is the encode "
                "source, so no conversion is possible. Either enable the filter "
                "(EncoderConfig::enablePreprocessComputeFilter) or supply one of the "
                "%u advertised format(s):",
                (int)requestedInFormat, formatCount);
        for (uint32_t fmtIdx = 0; fmtIdx < formatCount; fmtIdx++) {
            VkEncPrintfErr(" %d", (int)supportedInFormats[fmtIdx]);
        }
        VkEncPrintfErr("\n");
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    // State the encode-source format that was actually chosen. Without this the choice is
    // unobservable from outside: a packed and a 2-plane 4:4:4 source both produce a
    // yuv444p bitstream, so a --preferPackedYcbcr that silently did nothing would look
    // exactly like one that worked.
    if (encoderConfig->verbose) {
        const VkPackedYcbcrFormatDesc* pPacked = PackedYcbcrFormatDesc(m_imageInFormat);
        VkEncPrintfOut("InitEncoder: encode-source format %d (%s)%s\n",
               (int)m_imageInFormat,
               (pPacked != nullptr) ? pPacked->debugName : "planar/semi-planar",
               encoderConfig->preferPackedYcbcr ? " [--preferPackedYcbcr]" : "");
    }

    if (encoderConfig->enableQpMap) {
        VkFormat supportedQpMapFormats[8];
        VkExtent2D supportedQpMapTexelSize[8];
        VkImageTiling supportedQpMapTiling[8];
        VkImageUsageFlagBits imageUsageFlag = (encoderConfig->qpMapMode == EncoderConfig::DELTA_QP_MAP) ? VK_IMAGE_USAGE_VIDEO_ENCODE_QUANTIZATION_DELTA_MAP_BIT_KHR
                                                                                                        : VK_IMAGE_USAGE_VIDEO_ENCODE_EMPHASIS_MAP_BIT_KHR;

        result = VulkanVideoCapabilities::GetVideoFormats(m_vkDevCtx, encoderConfig->videoCoreProfile,
                                                          imageUsageFlag,
                                                          formatCount, supportedQpMapFormats, supportedQpMapTiling,
                                                          true, supportedQpMapTexelSize);

        if(result != VK_SUCCESS) {
            VkEncPrintfErr("\nInitEncoder Error: Failed to get desired video format for qpMap images.\n");
            return result;
        }

        m_imageQpMapFormat = supportedQpMapFormats[0];
        m_qpMapTexelSize = supportedQpMapTexelSize[0];
        m_qpMapTiling = supportedQpMapTiling[0];

        if (encoderConfig->enableAQ == VK_FALSE) {
            uint32_t qpMapFrameCount = encoderConfig->qpMapFileHandler.GetFrameCount(encoderConfig->input.width,
                                                                                     encoderConfig->input.height,
                                                                                     m_qpMapTexelSize);
            if (qpMapFrameCount < encoderConfig->numFrames) {
                VkEncErr() << "Number of QP maps (" << qpMapFrameCount << ") in the input QP map file "
                          << "is less than the number of frames (" << encoderConfig->numFrames
                          << ") to be encoded." << std::endl;
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
    }

    uint32_t requestedW = encoderConfig->encodeWidth;
    uint32_t requestedH = encoderConfig->encodeHeight;

    encoderConfig->encodeWidth  = std::max(encoderConfig->encodeWidth,  encoderConfig->videoCapabilities.minCodedExtent.width);
    encoderConfig->encodeHeight = std::max(encoderConfig->encodeHeight, encoderConfig->videoCapabilities.minCodedExtent.height);

    encoderConfig->encodeWidth  = std::min(encoderConfig->encodeWidth,  encoderConfig->videoCapabilities.maxCodedExtent.width);
    encoderConfig->encodeHeight = std::min(encoderConfig->encodeHeight, encoderConfig->videoCapabilities.maxCodedExtent.height);

    // Refuse an extent the device cannot encode, rather than quietly encoding a
    // cropped picture. The min-clamp above is benign padding, but clamping down
    // to maxCodedExtent changes what the caller asked for: it gets a smaller
    // picture than it requested with nothing to say so, and comparing that
    // against a reference at the requested size reads as a quality failure with
    // no cause. maxCodedExtent varies by device and by profile, so the request
    // is checked against the reported capability rather than a fixed limit.
    if ((requestedW > encoderConfig->videoCapabilities.maxCodedExtent.width) ||
        (requestedH > encoderConfig->videoCapabilities.maxCodedExtent.height)) {
        VkEncPrintfErr("[CAPS] ERROR: requested %ux%u exceeds this profile's maximum "
                        "coded extent %ux%u; refusing to encode a cropped picture\n",
                requestedW, requestedH,
                encoderConfig->videoCapabilities.maxCodedExtent.width,
                encoderConfig->videoCapabilities.maxCodedExtent.height);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    // Keep the session's maximum in step with the clamped extent. These are what
    // VideoSession/DPB creation is sized from, and a session asked for more than
    // the device's maxCodedExtent is not rejected cleanly -- the encoder faults
    // with an access violation instead of failing validation -- so they must
    // never exceed the capability either.
    encoderConfig->encodeMaxWidth  = std::min(encoderConfig->encodeMaxWidth,
                                              encoderConfig->videoCapabilities.maxCodedExtent.width);
    encoderConfig->encodeMaxHeight = std::min(encoderConfig->encodeMaxHeight,
                                              encoderConfig->videoCapabilities.maxCodedExtent.height);

    m_maxCodedExtent = { encoderConfig->encodeMaxWidth, encoderConfig->encodeMaxHeight }; // max coded size
    m_streamBufferSize = std::max(m_minStreamBufferSize, (size_t)encoderConfig->input.fullImageSize); // use worst case size

    encoderConfig->encodeAlignedWidth  = vk::alignedSize (encoderConfig->encodeWidth, encoderConfig->videoCapabilities.pictureAccessGranularity.width);
    encoderConfig->encodeAlignedHeight = vk::alignedSize (encoderConfig->encodeHeight, encoderConfig->videoCapabilities.pictureAccessGranularity.height);

    VkEncPrintfErr("[CAPS] encode=%ux%u range=[%ux%u..%ux%u] granularity=%ux%u",
            encoderConfig->encodeWidth, encoderConfig->encodeHeight,
            encoderConfig->videoCapabilities.minCodedExtent.width,
            encoderConfig->videoCapabilities.minCodedExtent.height,
            encoderConfig->videoCapabilities.maxCodedExtent.width,
            encoderConfig->videoCapabilities.maxCodedExtent.height,
            encoderConfig->videoCapabilities.pictureAccessGranularity.width,
            encoderConfig->videoCapabilities.pictureAccessGranularity.height);
    if (encoderConfig->encodeWidth != requestedW || encoderConfig->encodeHeight != requestedH)
        VkEncPrintfErr(" (clamped from %ux%u)", requestedW, requestedH);
    if (encoderConfig->encodeAlignedWidth != encoderConfig->encodeWidth ||
        encoderConfig->encodeAlignedHeight != encoderConfig->encodeHeight)
        VkEncPrintfErr(" (aligned to %ux%u)", encoderConfig->encodeAlignedWidth, encoderConfig->encodeAlignedHeight);
    VkEncPrintfErr("\n");

    const uint32_t maxActiveReferencePicturesCount = encoderConfig->videoCapabilities.maxActiveReferencePictures;
    const uint32_t maxDpbPicturesCount = std::min<uint32_t>(m_maxDpbPicturesCount, encoderConfig->videoCapabilities.maxDpbSlots);

    VkVideoSessionCreateFlagsKHR sessionCreateFlags{};
    void* sessionCreateInfoChain = nullptr;

    if (!m_encoderConfig->disableEncodeParameterOptimizations) {
        sessionCreateFlags |= VK_VIDEO_SESSION_CREATE_ALLOW_ENCODE_PARAMETER_OPTIMIZATIONS_BIT_KHR;
    }
#ifdef VK_KHR_video_maintenance1
    m_videoMaintenance1FeaturesSupported = VulkanVideoCapabilities::GetVideoMaintenance1FeatureSupported(m_vkDevCtx);
    if (m_videoMaintenance1FeaturesSupported) {
        sessionCreateFlags |= VK_VIDEO_SESSION_CREATE_INLINE_QUERIES_BIT_KHR;
    }
#endif // VK_KHR_video_maintenance1
    if (m_encoderConfig->enableQpMap) {
        if (m_encoderConfig->qpMapMode == EncoderConfig::DELTA_QP_MAP) {
            sessionCreateFlags |= VK_VIDEO_SESSION_CREATE_ALLOW_ENCODE_QUANTIZATION_DELTA_MAP_BIT_KHR;
        } else {
            sessionCreateFlags |= VK_VIDEO_SESSION_CREATE_ALLOW_ENCODE_EMPHASIS_MAP_BIT_KHR;
        }
    }

    VkVideoEncodeSessionIntraRefreshCreateInfoKHR intraRefreshCreateInfo{};
    if (m_encoderConfig->enableIntraRefresh) {
        VkVideoEncodeIntraRefreshModeFlagBitsKHR mode = VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_NONE_KHR;

        switch (m_encoderConfig->intraRefreshMode) {
        case EncoderConfig::REFRESH_PER_PARTITION:
            mode = VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_PER_PICTURE_PARTITION_BIT_KHR;
            break;
        case EncoderConfig::REFRESH_BLOCK_ROWS:
            mode = VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_BLOCK_ROW_BASED_BIT_KHR;
            break;
        case EncoderConfig::REFRESH_BLOCK_COLUMNS:
            mode = VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_BLOCK_COLUMN_BASED_BIT_KHR;
            break;
        case EncoderConfig::REFRESH_BLOCKS:
            mode = VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_BLOCK_BASED_BIT_KHR;
            break;
        default:
            break;
        }

        intraRefreshCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_INTRA_REFRESH_CREATE_INFO_KHR;
        intraRefreshCreateInfo.pNext = sessionCreateInfoChain;
        intraRefreshCreateInfo.intraRefreshMode = mode;

        sessionCreateInfoChain = &intraRefreshCreateInfo;
    }

    if (!m_videoSession ||
            !m_videoSession->IsCompatible( m_vkDevCtx,
                                           sessionCreateFlags,
                                           m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                           &encoderConfig->videoCoreProfile,
                                           m_imageInFormat,
                                           m_maxCodedExtent,
                                           m_imageDpbFormat,
                                           maxDpbPicturesCount,
                                           maxActiveReferencePicturesCount) ) {

        result = VulkanVideoSession::Create( m_vkDevCtx,
                                             sessionCreateFlags,
                                             m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                             &encoderConfig->videoCoreProfile,
                                             m_imageInFormat,
                                             m_maxCodedExtent,
                                             m_imageDpbFormat,
                                             maxDpbPicturesCount,
                                             maxActiveReferencePicturesCount,
                                             sessionCreateInfoChain,
                                             m_videoSession);

        // RELEASE-VISIBLE, and it has to be. Chromium builds this library
        // with NDEBUG, where the assert below compiles to nothing -- so without
        // this check a failed vkCreateVideoSessionKHR is not merely
        // unhandled, it was never examined, and InitEncoder ran on to report
        // VK_SUCCESS. VulkanVideoSession::Create leaves its out-parameter
        // untouched on every one of its early error returns, so m_videoSession
        // keeps its PREVIOUS value: null on a first init, which
        // InitEncoderCodec then dereferences as *m_videoSession while building
        // the session parameters; or, when this branch was entered because
        // IsCompatible() said no, the stale incompatible session, which is
        // worse, because it encodes against a profile the caller never
        // negotiated.
        //
        // The assert is kept alongside the check on purpose: in a debug build
        // a failure here should still be fatal at the point of failure.
        if (result != VK_SUCCESS) {
            VkEncPrintfErr("\nInitEncoder Error: Failed to create the video session (0x%x).\n", result);
            assert(result == VK_SUCCESS);
            return result;
        }

        // after creating a new video session, we need a codec reset.
        // Success path only: this records that a NEW session needs a codec
        // reset, and after a failed Create there is no new session. The flag
        // is write-only in this tree today -- nothing reads it -- so the
        // placement is currently inert and is correct for when a reader lands.
        m_resetEncoder = true;
    }



    // THE ENCODE-SOURCE POOL ASKS ONLY FOR THE USAGE IT PERFORMS, and
    // VK_IMAGE_USAGE_STORAGE_BIT is the one bit that has to be earned.
    //
    // The preprocess compute filter is its only consumer here: the filter
    // writes its output through per-plane views of the encode-source image
    // bound as VK_DESCRIPTOR_TYPE_STORAGE_IMAGE. Every other producer reaches
    // the same image through vkCmdCopyImage, and a directly encodable
    // registration is not staged into it at all.
    //
    // Declaring it on a session that never filters costs profile
    // compatibility, which is not a diagnostic detail but the encode itself:
    // vkCmdEncodeVideoKHR requires its source image to be compatible with the
    // bound session's video profile
    // (VUID-vkCmdEncodeVideoKHR-pEncodeInfo-08206). These images carry no
    // VkVideoProfileListInfoKHR -- the pool creates them
    // VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR instead, so that one
    // pool can serve whatever profile the session negotiates -- and a
    // profile-independent image is compatible with a profile only while every
    // usage it declares is one vkGetPhysicalDeviceVideoFormatPropertiesKHR
    // reports for that profile. STORAGE is not among the usages reported for
    // an encode-source format, so an unconditional request presents every
    // frame of a non-filtering session to the encoder through an image the
    // profile does not admit.
    //
    // Two session shapes can carry a filtered frame, and both are known here:
    //   * a file input, which is converted on every frame whatever its
    //     format; and
    //   * an external registration in the session's FILTER-INPUT format,
    //     which is by construction a different format from the encode source
    //     -- a registration that already matches the encode source is either
    //     encoded from the caller's own image or staged into this pool with
    //     a copy, and neither path binds a storage view.
    const bool preprocessFilterWritesEncodeSource =
        encoderConfig->IsPreprocessComputeFilterEnabled() &&
        (encoderConfig->inputFileHandler.HasFileName() ||
         (encoderConfig->input.vkFormat != m_imageInFormat));

    const VkImageUsageFlags inImageUsage = ( VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                                             VK_IMAGE_USAGE_SAMPLED_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             (preprocessFilterWritesEncodeSource
                                                  ? VK_IMAGE_USAGE_STORAGE_BIT
                                                  : 0) );
    const VkImageUsageFlags dpbImageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;

    // Linear staging pool — only needed for file-based input (CPU upload).
    // External input (IPC) provides OPTIMAL images directly.
    if (!encoderConfig->repeatInputFrames) {
        result = VulkanVideoImagePool::Create(m_vkDevCtx, m_linearInputImagePool);
        if (result != VK_SUCCESS) {
            VkEncPrintfErr("\nInitEncoder Error: Failed to create linearInputImagePool.\n");
            return result;
        }

        VkExtent2D linearInputImageExtent{
            std::max(m_maxCodedExtent.width, encoderConfig->input.width),
            std::max(m_maxCodedExtent.height, encoderConfig->input.height)};

        result = m_linearInputImagePool->Configure(
            m_vkDevCtx, encoderConfig->numInputImages, encoderConfig->input.vkFormat,
            linearInputImageExtent,
            (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
            m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
             VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
            nullptr, VK_IMAGE_ASPECT_COLOR_BIT, false, false, true,
            0 /* drmFormatModifier */,
            // PREINITIALIZED, not the UNDEFINED default. VkVideoEncoder::
            // LoadNextFrame host-writes this image through a persistent
            // mapping and only THEN calls StageInputFrame, so the host write
            // precedes every barrier this library records on it. UNDEFINED
            // says the opposite -- that the contents may be discarded -- and
            // left the image with no layout at all for the filter's
            // STORAGE_IMAGE descriptors to match, which is
            // VUID-vkCmdDraw-None-09600 ("expects GENERAL -- instead, current
            // layout is UNDEFINED") once per input plane per frame.
            //
            // PREINITIALIZED is legal here on both counts the spec attaches to
            // it: the tiling is LINEAR and the memory is HOST_VISIBLE.
            VK_IMAGE_LAYOUT_PREINITIALIZED);
        if (result != VK_SUCCESS) {
            VkEncPrintfErr("\nInitEncoder Error: Failed to Configure linearInputImagePool.\n");
            return result;
        }
    }

    result =  VulkanVideoImagePool::Create(m_vkDevCtx, m_inputImagePool);
    if(result != VK_SUCCESS) {
        VkEncPrintfErr("\nInitEncoder Error: Failed to create inputImagePool.\n");
        return result;
    }

    VkExtent2D imageExtent {
        std::max(m_maxCodedExtent.width, encoderConfig->videoCapabilities.minCodedExtent.width),
        std::max(m_maxCodedExtent.height, encoderConfig->videoCapabilities.minCodedExtent.height)
    };

    // Query and select DRM format modifier if requested
    if (encoderConfig->drmFormatModifierIndex >= 0) {
        result = SelectDrmFormatModifier(encoderConfig, m_imageInFormat, inImageUsage, imageExtent);
        if (result != VK_SUCCESS) {
            VkEncPrintfErr("\nInitEncoder Error: Failed to select DRM format modifier.\n");
            return result;
        }
    }

    // WHICH FAMILY WILL WRITE THIS POOL.
    //
    // Deliberately not GetStagedInputQueueFamilyIdx(): that answers from
    // m_inputComputeFilter, which InitEncoder does not create until several
    // hundred lines below here. It would say ENCODE or TRANSFER for every
    // session and be wrong for precisely the sessions this matters to. The
    // routing decision is made here from the config predicate the filter's
    // creation is gated on -- the same one the ext admission gates read.
    uint32_t stagedInputQueueFamilyIdx =
        ((m_vkDevCtx->GetVideoEncodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) != 0)
            ? (uint32_t)m_vkDevCtx->GetVideoEncodeQueueFamilyIdx()
            : (uint32_t)m_vkDevCtx->GetTransferQueueFamilyIdx();
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    if (encoderConfig->IsPreprocessComputeFilterEnabled()) {
        stagedInputQueueFamilyIdx =
            (uint32_t)m_vkDevCtx->GetComputeQueueFamilyIdx();
    }
#endif
    const std::vector<uint32_t> inputPoolQueueFamilies = {
        (uint32_t)m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
        stagedInputQueueFamilyIdx };

    result = m_inputImagePool->Configure( m_vkDevCtx,
                                          encoderConfig->numInputImages,
                                          m_imageInFormat,
                                          imageExtent,
                                          inImageUsage,
                                          inputPoolQueueFamilies,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          nullptr,
                                          VK_IMAGE_ASPECT_COLOR_BIT,
                                          false,   // useImageArray
                                          false,   // useImageViewArray
                                          false,   // useLinear
                                          encoderConfig->selectedDrmFormatModifier
                                          );
    if(result != VK_SUCCESS) {
        VkEncPrintfErr("\nInitEncoder Error: Failed to Configure inputImagePool.\n");
        return result;
    }

    assert(m_vkDevCtx->GetVideoEncodeQueueFamilyIdx() != -1);
    assert(m_vkDevCtx->GetVideoEncodeNumQueues() > 0);
    assert(m_vkDevCtx->GetVideoEncodeDefaultQueueIndex() < m_vkDevCtx->GetVideoEncodeNumQueues());

    if (m_currentVideoQueueIndx < 0) {
        m_currentVideoQueueIndx = m_vkDevCtx->GetVideoEncodeDefaultQueueIndex();
    } else if (m_vkDevCtx->GetVideoEncodeNumQueues() > 1) {
        m_currentVideoQueueIndx %= m_vkDevCtx->GetVideoEncodeNumQueues();
        assert(m_currentVideoQueueIndx < m_vkDevCtx->GetVideoEncodeNumQueues());
        assert(m_currentVideoQueueIndx >= 0);
    } else {
        m_currentVideoQueueIndx = 0;
    }

    if (encoderConfig->enableHwLoadBalancing) {

        if (m_vkDevCtx->GetVideoEncodeNumQueues() < 2) {
            VkEncOut() << "\t WARNING: Enabling HW Load Balancing for a device with only " <<
                    m_vkDevCtx->GetVideoEncodeNumQueues() << " queue!!!" << std::endl;
        }

        // Create the timeline semaphore object for the HW LoadBalancing Timeline Semaphore
        VkSemaphoreTypeCreateInfo timelineCreateInfo;
        timelineCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        timelineCreateInfo.pNext = NULL;
        timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineCreateInfo.initialValue = 0LLU; // assuming m_EncodePicCount starts at 0.

        VkSemaphoreCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        createInfo.pNext = &timelineCreateInfo;
        createInfo.flags = 0;

        VkResult result = m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &createInfo, NULL, &m_hwLoadBalancingTimelineSemaphore);
        if (result == VK_SUCCESS) {
            m_currentVideoQueueIndx = 0; // start with index zero
        }
        VkEncOut() << "\t Enabling HW Load Balancing for device with "
                  << m_vkDevCtx->GetVideoEncodeNumQueues() << " queues" << std::endl;
    }

    if (encoderConfig->enableQpMap) {

        if (m_qpMapTiling != VK_IMAGE_TILING_LINEAR) {

            // If the linear tiling is not supported, we need to stage the image
            result =  VulkanVideoImagePool::Create(m_vkDevCtx, m_linearQpMapImagePool);
            if(result != VK_SUCCESS) {
                VkEncPrintfErr("\nInitEncoder Error: Failed to create linearQpMapImagePool.\n");
                return result;
            }

            VkExtent2D linearQpMapImageExtent {
                (std::max(m_maxCodedExtent.width,  encoderConfig->input.width) + m_qpMapTexelSize.width - 1) / m_qpMapTexelSize.width,
                (std::max(m_maxCodedExtent.height, encoderConfig->input.height) + m_qpMapTexelSize.height - 1) / m_qpMapTexelSize.height
            };

            result = m_linearQpMapImagePool->Configure( m_vkDevCtx,
                                                        encoderConfig->numInputImages,
                                                        m_imageQpMapFormat,
                                                        linearQpMapImageExtent,
                                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                        m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                                        ( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  |
                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
                                                        nullptr, // pVideoProfile
                                                        VK_IMAGE_ASPECT_COLOR_BIT, // a whole YCbCr or RGBA image
                                                        false,   // useImageArray
                                                        false,   // useImageViewArray
                                                        true     // useLinear
                                                      );
            if(result != VK_SUCCESS) {
                VkEncPrintfErr("\nInitEncoder Error: Failed to Configure linearQpMapImagePool.\n");
                return result;
            }
        }
        result =  VulkanVideoImagePool::Create(m_vkDevCtx, m_qpMapImagePool);
        if(result != VK_SUCCESS) {
            VkEncPrintfErr("\nInitEncoder Error: Failed to create inputImagePool.\n");
            return result;
        }

        VkExtent2D qpMapExtent {
            (std::max(m_maxCodedExtent.width, encoderConfig->videoCapabilities.minCodedExtent.width) + m_qpMapTexelSize.width - 1) / m_qpMapTexelSize.width,
            (std::max(m_maxCodedExtent.height, encoderConfig->videoCapabilities.minCodedExtent.height) + m_qpMapTexelSize.height - 1) / m_qpMapTexelSize.height
        };

        // Add STORAGE_BIT only when AQ is enabled (required for compute shader writes)
        VkImageUsageFlags qpMapImageUsage = (((encoderConfig->qpMapMode == EncoderConfig::DELTA_QP_MAP) ?
                                              VK_IMAGE_USAGE_VIDEO_ENCODE_QUANTIZATION_DELTA_MAP_BIT_KHR :
                                              VK_IMAGE_USAGE_VIDEO_ENCODE_EMPHASIS_MAP_BIT_KHR) |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (encoderConfig->enableAQ) {
            qpMapImageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;  // Required for compute shader writes (AQ library)
        }

        // When AQ is enabled, use DEVICE_LOCAL only (no HOST access flags)
        // When AQ is disabled, use existing logic (DEVICE_LOCAL for optimal, HOST flags for linear)
        VkMemoryPropertyFlags qpMapMemoryProperties;
        bool qpMapMemoryUseLinear;
        if (encoderConfig->enableAQ) {
            qpMapMemoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            qpMapMemoryUseLinear = false;
        } else {
            qpMapMemoryProperties = (m_qpMapTiling != VK_IMAGE_TILING_LINEAR) ?
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT :
                                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            qpMapMemoryUseLinear = (m_qpMapTiling == VK_IMAGE_TILING_LINEAR);
        }

        result = m_qpMapImagePool->Configure( m_vkDevCtx,
                                              encoderConfig->numInputImages,
                                              m_imageQpMapFormat,
                                              qpMapExtent,
                                              qpMapImageUsage,
                                              m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                              qpMapMemoryProperties,
                                              encoderConfig->videoCoreProfile.GetProfile(), // pVideoProfile
                                              VK_IMAGE_ASPECT_COLOR_BIT, // a whole YCbCr or RGBA image
                                              false,   // useImageArray
                                              false,   // useImageViewArray
                                              qpMapMemoryUseLinear    // useLinear
                                            );
        if(result != VK_SUCCESS) {
            VkEncPrintfErr("\nInitEncoder Error: Failed to Configure qpMapImagePool.\n");
            return result;
        }
    }

    result =  VulkanVideoImagePool::Create(m_vkDevCtx, m_dpbImagePool);
    if(result != VK_SUCCESS) {
        VkEncPrintfErr("\nInitEncoder Error: Failed to create dpbImagePool.\n");
        return result;
    }

    const uint32_t numEncodeImagesInFlight = std::max<uint32_t>(m_holdRefFramesInQueue + m_holdRefFramesInQueue * m_encoderConfig->gopStructure.GetConsecutiveBFrameCount(), 4);
    const uint32_t asyncAssemblySlack = m_encoderConfig->asyncAssembly ? m_encoderConfig->numBitstreamBuffersToPreallocate : 0;
    const uint32_t maxEncodeQueueDepth = std::max<uint32_t>(maxDpbPicturesCount, maxActiveReferencePicturesCount) + numEncodeImagesInFlight + asyncAssemblySlack;
    result = m_dpbImagePool->Configure(m_vkDevCtx,
                                       maxEncodeQueueDepth,
                                       m_imageDpbFormat,
                                       imageExtent,
                                       dpbImageUsage,
                                       m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                       encoderConfig->videoCoreProfile.GetProfile(), // pVideoProfile
                                       VK_IMAGE_ASPECT_COLOR_BIT, // a whole YCbCr or RGBA image
                                       encoderConfig->useDpbArray,                   // useImageArray
                                       false,   // useImageViewArrays
                                       false    // useLinear
                                      );
    if(result != VK_SUCCESS) {
        VkEncPrintfErr("\nInitEncoder Error: Failed to Configure inputImagePool.\n");
        return result;
    }

    int32_t availableBuffers = (int32_t)m_bitstreamBuffersQueue.GetAvailableNodesNumber();
    if (availableBuffers < encoderConfig->numBitstreamBuffersToPreallocate) {

        uint32_t allocateNumBuffers = std::min<uint32_t>(
                m_bitstreamBuffersQueue.GetMaxNodes(),
                (encoderConfig->numBitstreamBuffersToPreallocate - availableBuffers));

        allocateNumBuffers = std::min<uint32_t>(allocateNumBuffers,
                m_bitstreamBuffersQueue.GetFreeNodesNumber());

        for (uint32_t i = 0; i < allocateNumBuffers; i++) {

            VkSharedBaseObj<VulkanBitstreamBufferImpl> bitstreamBuffer;
            VkDeviceSize allocSize = std::max<VkDeviceSize>(m_streamBufferSize, m_minStreamBufferSize);

            result = VulkanBitstreamBufferImpl::Create(m_vkDevCtx,
                    m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                    VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
                    allocSize,
                    encoderConfig->videoCapabilities.minBitstreamBufferOffsetAlignment,
                    encoderConfig->videoCapabilities.minBitstreamBufferSizeAlignment,
                    nullptr, 0, bitstreamBuffer);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                VkEncPrintfErr("\nERROR: VulkanBitstreamBufferImpl::Create() result: 0x%x\n", result);
                break;
            }

            int32_t nodeAddedWithIndex = m_bitstreamBuffersQueue.AddNodeToPool(bitstreamBuffer, false);
            if (nodeAddedWithIndex < 0) {
                assert("Could not add the new node to the pool");
                break;
            }
        }
    }

#ifdef NV_AQ_GPU_LIB_SUPPORTED
    // AQ strength semantics: [-1.0, 1.0] valid, < -1.0 means disabled
    const bool enableSpatialAQ = (m_encoderConfig->spatialAQStrength >= -1.0f);
    const bool enableTemporalAQ = (m_encoderConfig->temporalAQStrength >= -1.0f);
    if (enableSpatialAQ || enableTemporalAQ)
    {
        // Create AQ processor using the interface
        m_aqAnalyzes = CreateVulkanAqAnalyzer( -1,                              // deviceId
                                               vk::DeviceUuidUtils(),           // deviceUuid
                                               m_vkDevCtx->getInstance(),       // vkInstance,
                                               m_vkDevCtx->getPhysicalDevice(), // VkPhysicalDevice vkPhysicalDevice,
                                               m_vkDevCtx->getDevice(),         // VkDevice vkDevice,
                                               m_vkDevCtx->GetComputeQueueFamilyIdx(), // queueFamilyIndex
                                               0 // queueInstanceIdx - always 0 for compute in vkDevCtx
                                               );


        if (!m_aqAnalyzes) {
            VkEncErr() << "Failed to create AQ processor (API may not be available in this library)" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        nvenc_aq::EncodeAqAnalyzes::AQConfig config;
        // Basic parameters
        config.width = m_encoderConfig->encodeWidth;
        config.height = m_encoderConfig->encodeHeight;
        config.bitDepth = m_encoderConfig->encodeBitDepthLuma;

        config.chromaFormat = 1;  // 4:2:0 (chromaFormatIDC = 1)
        switch (m_encoderConfig->input.chromaSubsampling) {
             case VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR:
                 config.chromaFormat = 0;  // 4:0:0 (chromaFormatIDC = 0)
                 break;
             case VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR:
                 config.chromaFormat = 1;  // 4:2:0 (chromaFormatIDC = 1)
                 break;
             case VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR:
                 config.chromaFormat = 2;  // 4:2:2 (chromaFormatIDC = 2)
                 break;
             case VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR:
                 config.chromaFormat = 3;  // 4:4:4 (chromaFormatIDC = 3)
                 break;
             default:
                 break;
        }

        // for debugging - allocates linear images that are used to compare the sub-sampling.
        config.resourceFlags = nvenc_aq::EncodeAqAnalyzes::AQConfig::AQ_RESOURCE_CPU_UPLOAD;

        config.maxQueueSlots = encoderConfig->numInputImages;
        VkEncPrintfOut("DEBUG: chromaFormat=%u\n", config.chromaFormat);

        switch (m_encoderConfig->codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            config.codecType = nvenc_aq::EncodeAqAnalyzes::AQConfig::AQ_CODEC_H264;
            break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            config.codecType = nvenc_aq::EncodeAqAnalyzes::AQConfig::AQ_CODEC_HEVC;
            break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            config.codecType = nvenc_aq::EncodeAqAnalyzes::AQConfig::AQ_CODEC_AV1;
            break;
        default:
            VkEncErr() << "Unknown codec: " << m_encoderConfig->codec << ", defaulting to H.264" << std::endl;
            config.codecType = nvenc_aq::EncodeAqAnalyzes::AQConfig::AQ_CODEC_H264;
            break;
        }

        // Determine AQ modes - use the enable flags computed from strength values
        config.enableSpatialAQ = enableSpatialAQ;
        config.enableTemporalAQ = enableTemporalAQ;
        // Map normalized strength [-1.0, 1.0] to integer strength [1, 15]
        // Formula: strength = 8 + 7 * normalized (where 0.0 -> 8, -1.0 -> 1, 1.0 -> 15)
        if (enableSpatialAQ) {
            float normStrength = std::max(-1.0f, std::min(1.0f, m_encoderConfig->spatialAQStrength));
            config.spatialAQStrength = static_cast<uint32_t>(8.0f + 7.0f * normStrength);
            // Clamp to valid range [1, 15] (0 would mean disabled, which is handled by enableSpatialAQ)
            config.spatialAQStrength = std::max(1u, std::min(15u, config.spatialAQStrength));
        } else {
            config.spatialAQStrength = 0;
        }
        config.spatialAQStrengthNorm = m_encoderConfig->spatialAQStrength;
        config.temporalAQStrengthNorm = m_encoderConfig->temporalAQStrength;

        // Input image format: nvpro encoder's VulkanFilterYuvCompute may left-shift N-bit values
        // to fill the 16-bit R16_UNORM format when encoderConfig->input.msbShift is set.
        // The AQ shader needs to know this shift to correctly denormalize the values.
        // Only set inputBitShift if msbShift is enabled in the input configuration.
        config.inputBitShift = m_encoderConfig->input.msbShift;

        // GOP parameters
        config.gopFrameCount = m_encoderConfig->gopStructure.GetGopFrameCount();
        config.consecutiveBFrameCount = m_encoderConfig->gopStructure.GetConsecutiveBFrameCount();
        config.idrPeriod = m_encoderConfig->gopStructure.GetIdrPeriod();
        config.closedGOP = m_encoderConfig->gopStructure.IsClosedGop();

        // for debugging - allocates linear images that are used to compare the sub-sampling.
        config.resourceFlags = nvenc_aq::EncodeAqAnalyzes::AQConfig::AQ_RESOURCE_CPU_UPLOAD;

        // Dumping
        config.enableBufferDumping = true;
        config.enableRawFilesDumping = true;  // Enable raw file dumps when dumping is enabled
        config.outputDumpDir = m_encoderConfig->aqDumpDir.c_str();
        // Use input order for dump filenames (nvpro encoder expects input order, not encode order)
        config.dumpFilenameOrdering = nvenc_aq::EncodeAqAnalyzes::AQConfig::AQ_DUMP_FILENAME_INPUT_ORDER;

        int result = m_aqAnalyzes->Configure(config);
        if (result != 0) {
            assert(!"Failed to configure AQ processor!!!");
            VkEncErr() << "Failed to configure AQ processor: " << result << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
#endif // NV_AQ_GPU_LIB_SUPPORTED

#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    if (encoderConfig->enablePreprocessComputeFilter) {

        // Colour conversion parameters for the RGBA->YCbCr preprocess filter,
        // derived from the VUI the caller asked for.
        //
        // These were hardcoded to BT.2020 / full range behind three FIXMEs, and
        // they are not cosmetic. VulkanFilterYuvCompute::InitRGBA2YCBCR reads
        // exactly these two fields back out of the sampler-conversion info to
        // choose the shader's matrix (from ycbcrModel) and its range mapping
        // (from ycbcrRange). Hardcoding them converted every RGBA frame at
        // BT.2020 full range while the bitstream's VUI advertised whatever the
        // caller set -- so a conforming decoder was required to mis-colour the
        // result. A Chromium session is the concrete case: its config builder
        // defaults matrixCoefficients to 1 (BT.709) and videoFullRange to
        // VK_FALSE, i.e. the two values furthest from what was being applied.
        //
        // The VUI fields are the right source precisely because they are what
        // the bitstream will advertise: deriving from them makes the conversion
        // and the advertisement agree by construction rather than by luck.
        //
        // FALLBACK, and why it is not "just a default": matrix_coefficients
        // values that name no matrix this filter can express -- 0 (Identity/
        // GBR), 2 (Unspecified), 7 (SMPTE 240M), and anything outside the three
        // VkSamplerYcbcrModelConversion values below -- must not be passed
        // through. VkSamplerYcbcrModelConversion has no encoding for them, so
        // the filter would resolve them to YcbcrBtStandardUnknown, whose
        // {kb,kr} = {0,0} is not a refusal but a matrix whose luma is a copy of
        // the green channel. Falling back to BT.709 keeps the output sane and
        // matches both the ext layer's documented default and what the Chromium
        // builder sets when no colour space is supplied. It is announced,
        // because silently substituting a matrix is how this class of bug is
        // born.
        VkSamplerYcbcrModelConversion ycbcrModelConversion;
        YcbcrBtStandard ycbcrBtStandard;
        switch (encoderConfig->matrix_coefficients) {
            case 5:  // BT.601-7 625 (PAL/SECAM)
            case 6:  // BT.601-7 525 (NTSC) -- same matrix
                ycbcrModelConversion = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
                ycbcrBtStandard      = YcbcrBtStandardBt601Ebu;
                break;
            case 9:  // BT.2020 non-constant luminance
            case 10: // BT.2020 constant luminance
                ycbcrModelConversion = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
                ycbcrBtStandard      = YcbcrBtStandardBt2020;
                break;
            case 1:  // BT.709
                ycbcrModelConversion = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
                ycbcrBtStandard      = YcbcrBtStandardBt709;
                break;
            default:
                fprintf(stderr,
                        "\nInitEncoder: preprocess filter: VUI matrix_coefficients %u "
                        "names no matrix the RGBA->YCbCr filter can express; "
                        "converting as BT.709.\n",
                        (unsigned)encoderConfig->matrix_coefficients);
                ycbcrModelConversion = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
                ycbcrBtStandard      = YcbcrBtStandardBt709;
                break;
        }

        const VkSamplerYcbcrRange ycbcrRange = encoderConfig->video_full_range_flag ?
                                                   VK_SAMPLER_YCBCR_RANGE_ITU_FULL :
                                                   VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        // From the RESOLVED standard, not from matrix_coefficients again, so
        // the constants and the model can never name different matrices.
        const YcbcrPrimariesConstants ycbcrPrimariesConstants =
            GetYcbcrPrimariesConstants(ycbcrBtStandard);

        const VkSamplerYcbcrConversionCreateInfo ycbcrConversionCreateInfo {
                   VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
                   nullptr,
                   m_imageInFormat,
                   ycbcrModelConversion,
                   ycbcrRange,
                   { VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY
                   },
                   VK_CHROMA_LOCATION_MIDPOINT, // FIXME
                   VK_CHROMA_LOCATION_MIDPOINT, // FIXME
                   VK_FILTER_LINEAR,
                   false
                   };

        static const VkSamplerCreateInfo samplerInfo = {
                   VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                   nullptr,
                   0,
                   VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                   // mipLodBias  anisotropyEnable  maxAnisotropy  compareEnable      compareOp         minLod  maxLod          borderColor
                   // unnormalizedCoordinates
                   0.0, false, 0.00, false, VK_COMPARE_OP_NEVER, 0.0, 16.0, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, false
        };

        // VulkanFilterYuvCompute now supports subsampling
        uint32_t filterFlags = VulkanFilterYuvCompute::FLAG_NONE;
        if (encoderConfig->input.msbShift > 0) {
            // msbShift > 0 says the source samples are LSB-aligned: a 10-bit
            // value occupies 0..1023 of a 16-bit container, so the
            // VK_FORMAT_R16_UNORM view the filter reads normalizes it to
            // pixel / 65535 -- 1/64 of the intended magnitude at 10-bit.
            // GenApplyBlockOutputShift multiplies the written sample by
            // 2^msbShift, which is exactly what restores it.
            //
            // The input MSB-to-LSB shift is that transform's inverse and
            // belongs to an already-MSB-aligned source (P010-style, which
            // DetectInputMsbShift reports as msbShift == 0). Such a source
            // normalizes to (pixel << 6) / 65535 ~= pixel / 1023 on its own and
            // needs no shift in either direction. Setting both flags from this
            // one condition would cancel them and leave every sample at 1/64
            // scale: a near-black frame that encodes to a structurally valid
            // bitstream a fraction of the expected size.
            filterFlags |= VulkanFilterYuvCompute::FLAG_OUTPUT_LSB_TO_MSB_SHIFT;
        }
#ifdef NV_AQ_GPU_LIB_SUPPORTED
        if (m_aqAnalyzes) {
            // Enable Y subsampling for AQ if enabled in the encoder
            filterFlags |= VulkanFilterYuvCompute::FLAG_ENABLE_Y_SUBSAMPLING;
        }
#endif // NV_AQ_GPU_LIB_SUPPORTED
        // Enable row/column replication
        filterFlags |= VulkanFilterYuvCompute::FLAG_ENABLE_ROW_COLUMN_REPLICATION_ALL;

        // The mechanism choice belongs inside the library, where the device
        // capabilities are known, rather than in the embedder
        // (CONTEXT_DESIGN:594-598). m_imageInFormat is not assumed here: it
        // was read out of vkGetPhysicalDeviceVideoFormatPropertiesKHR above,
        // so a future device that accepts something other than NV12 as an
        // encode source changes this derivation without changing a line.
        encoderConfig->filterType =
            VkEncDeriveFilterType(encoderConfig->input.vkFormat,
                                  m_imageInFormat);

        result = VulkanFilterYuvCompute::Create(m_vkDevCtx,
                                                m_vkDevCtx->GetComputeQueueFamilyIdx(),
                                                0, // queueIndex
                                                encoderConfig->filterType,
                                                encoderConfig->numInputImages,
                                                encoderConfig->input.vkFormat,  // in filter format (can be RGB)
                                                m_imageInFormat,  // out filter - same as input for now.
                                                filterFlags,
                                                &ycbcrConversionCreateInfo,
                                                &ycbcrPrimariesConstants,
                                                &samplerInfo,
                                                m_inputComputeFilter);

        // FATAL, and it has to be. Every ext-path gate that admits a frame
        // for conversion -- SupportsFormat, ValidateImageDescriptor,
        // RegisterImageResource's VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER --
        // answers from the CONFIG flag (IsPreprocessComputeFilterEnabled),
        // while StageInputFrame routes on the OBJECT
        // (m_inputComputeFilter != nullptr). The `else` arm below overwrites
        // |result| with the command-buffer pool's own VK_SUCCESS, so without
        // this return a failed VulkanFilterYuvCompute::Create (push-descriptor
        // layout unsupported on a borrowed device, runtime GLSL compile,
        // pipeline creation -- Create leaves its out-parameter null and
        // returns the error) would leave InitEncoder reporting SUCCESS on a
        // session the ext layer believes can convert. A 3-plane frame then
        // reaches CopyLinearToOptimalImage, whose 2-region copy from a
        // 3-plane source is a VK_ERROR_DEVICE_LOST / GPU hang / 0-byte
        // bitstream. An init failure
        // with a reason is recoverable; that is not.
        if ((result != VK_SUCCESS) || (m_inputComputeFilter == nullptr)) {
            VkEncPrintfErr("\nInitEncoder Error: enablePreprocessComputeFilter is set "
                    "but the preprocess compute filter could not be created "
                    "(%d).\n", result);
            return (result != VK_SUCCESS) ? result : VK_ERROR_INITIALIZATION_FAILED;
        }

        // Filter-dispatch observable: record WHICH conversion was built.
        // After the fatal check above, so the kind is only ever published
        // for a filter that actually exists. Translated to the public
        // taxonomy here rather than exposing VulkanFilterYuvCompute's enum,
        // which is an internal header the ext consumer does not include.
        switch (encoderConfig->filterType) {
        case VulkanFilterYuvCompute::RGBA2YCBCR:
            m_inputFilterKind.store(INPUT_FILTER_RGBA_TO_YCBCR,
                                    std::memory_order_relaxed);
            break;
        case VulkanFilterYuvCompute::YCBCR2RGBA:
            m_inputFilterKind.store(INPUT_FILTER_YCBCR_TO_RGBA,
                                    std::memory_order_relaxed);
            break;
        default:
            // VkEncDeriveFilterType collapses every YCbCr->YCbCr pair,
            // including 3-plane I420 -> 2-plane NV12 and the identity,
            // onto YCBCRCOPY.
            m_inputFilterKind.store(INPUT_FILTER_YCBCR_COPY,
                                    std::memory_order_relaxed);
            break;
        }
    }

    if ((result == VK_SUCCESS) && (m_inputComputeFilter != nullptr) ) {

        m_inputCommandBufferPool = m_inputComputeFilter;

#ifdef NV_AQ_GPU_LIB_SUPPORTED
        if (m_aqAnalyzes) {
            // Allocate subsampled image pool for the new filter capability
            result = VulkanVideoImagePool::Create(m_vkDevCtx, m_inputSubsampledImagePool);
            if (result != VK_SUCCESS) {
                VkEncPrintfErr("\nInitEncoder Error: Failed to create inputSubsampledImagePool.\n");
                return result;
            }

            // Subsampled dimensions: half of input dimensions
            VkExtent2D subsampledExtent {
                (std::max(m_maxCodedExtent.width, encoderConfig->input.width) + 1) / 2,
                (std::max(m_maxCodedExtent.height, encoderConfig->input.height) + 1) / 2
            };

            // Align images, worse cases for AV1
            const uint32_t subsampledExtentAlign = 32;
            subsampledExtent.width = vk::alignedSize(subsampledExtent.width, subsampledExtentAlign);
            subsampledExtent.height = vk::alignedSize(subsampledExtent.height, subsampledExtentAlign);

            // Determine format based on input bit depth
            const VkMpFormatInfo* inputMpInfo = YcbcrVkFormatInfo(encoderConfig->input.vkFormat);
            const uint32_t inputBitDepth = inputMpInfo ? GetBitsPerChannel(inputMpInfo->planesLayout) : 8;
            VkFormat subsampledYFormat = (inputBitDepth > 8) ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;

            result = m_inputSubsampledImagePool->Configure(
                m_vkDevCtx,
                encoderConfig->numInputImages + 4,
                subsampledYFormat, // R8_UNORM or R16_UNORM (single-channel Y)
                subsampledExtent,
                VK_IMAGE_USAGE_STORAGE_BIT |           // Write from this filter, read from AQ
                VK_IMAGE_USAGE_SAMPLED_BIT |           // Texture sampling for AQ
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |      // CPU readback for debug
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,       // Clear/init if needed
                m_vkDevCtx->GetComputeQueueFamilyIdx(),
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,  // Device-local for max GPU performance
                nullptr,  // pVideoProfile
                VK_IMAGE_ASPECT_PLANE_0_BIT, // Y-plane only image
                false,    // useImageArray
                false,    // useImageViewArray
                false     // useLinear - OPTIMAL tiling for GPU performance
            );

            if (result != VK_SUCCESS) {
                VkEncPrintfErr("\nInitEncoder Error: Failed to Configure inputSubsampledImagePool.\n");
                return result;
            }
        }
#endif // NV_AQ_GPU_LIB_SUPPORTED
    } else
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    {

        result = VulkanCommandBufferPool::Create(m_vkDevCtx, m_inputCommandBufferPool);
        if(result != VK_SUCCESS) {
            VkEncPrintfErr("\nInitEncoder Error: Failed to create m_inputCommandBufferPool.\n");
            return result;
        }

        result = m_inputCommandBufferPool->Configure( m_vkDevCtx,
                                                      encoderConfig->numInputImages, // numPoolNodes
                                                      ((m_vkDevCtx->GetVideoEncodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) != 0) ?
                                                          m_vkDevCtx->GetVideoEncodeQueueFamilyIdx() :
                                                          m_vkDevCtx->GetTransferQueueFamilyIdx(), // queueFamilyIndex
                                                      false,    // createQueryPool - not needed for the input transfer
                                                      nullptr,  // pVideoProfile   - not needed for the input transfer
                                                      true,     // createSemaphores
                                                      true      // createFences
                                                     );
    }

    if (result != VK_SUCCESS) {
        VkEncPrintfErr("\nInitEncoder Error: Failed to Configure m_inputCommandBufferPool.\n");
        return result;
    }

    result = VulkanCommandBufferPool::Create(m_vkDevCtx, m_encodeCommandBufferPool);
    if(result != VK_SUCCESS) {
        VkEncPrintfErr("\nInitEncoder Error: Failed to create m_encodeCommandBufferPool.\n");
        return result;
    }

    VkQueryPoolVideoEncodeFeedbackCreateInfoKHR encodeFeedbackCreateInfo =
        {VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR};

    encodeFeedbackCreateInfo.pNext = encoderConfig->videoCoreProfile.GetProfile();
    encodeFeedbackCreateInfo.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
                                                   VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;

    result = m_encodeCommandBufferPool->Configure( m_vkDevCtx,
                                                   encoderConfig->numInputImages, // numPoolNodes
                                                   m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(), // queueFamilyIndex
                                                   true,      // createQueryPool - not needed for the input transfer
                                                   &encodeFeedbackCreateInfo, // VideoEncodeFeedback + VideoProfile
                                                   true,     // createSemaphores
                                                   true      // createFences
                                                  );
    if(result != VK_SUCCESS) {
        VkEncPrintfErr("\nInitEncoder Error: Failed to Configure m_encodeCommandBufferPool.\n");
        return result;
    }

    result = CreateFrameInfoBuffersQueue(encoderConfig->numInputImages);
    if(result != VK_SUCCESS) {
        VkEncPrintfErr("\nInitEncoder Error: Failed to create FrameInfoBuffersQueue.\n");
        return result;
    }

    // Start the queue consumer thread
    if (m_enableEncoderThreadQueue) {

        const uint32_t maxPendingQueueNodes = 2;
        m_encoderThreadQueue.SetMaxPendingQueueNodes(std::min<uint32_t>(m_encoderConfig->gopStructure.GetGopFrameCount() + 1, maxPendingQueueNodes));
        m_encoderQueueConsumerThread = std::thread(&VkVideoEncoder::ConsumerThread, this);
    }

    // The dma-buf import content probe, if the ext layer has already injected
    // one. There is no env var and no build flag gating it, because the
    // OPT-IN IS THE CALLER CHAINING VkVideoEncoderImportContentInfo onto a
    // registration -- and no allocation happens here either way: the probe's
    // device-memory pool is configured lazily on the first capture actually
    // recorded, so a session that arms nothing allocates nothing.
    m_contentProbeQueueDepth = maxEncodeQueueDepth;
    ConfigureContentProbe();

    // MECHANISM-C shares the PSNR helper's pool machinery but is gated on its own
    // env var, so the encoder-input capture can run WITHOUT the PSNR path's
    // per-frame host sync.
    static const bool kDebugDumpSrc =
        (getenv("VKENC_DEBUG_DUMP_SRC") != nullptr);
    if (encoderConfig->IsPsnrMetricsEnabled() || kDebugDumpSrc) {
        if (!m_psnr) {
            result = VkVideoEncoderPsnr::Create(m_psnr);
            if (result != VK_SUCCESS) {
                VkEncPrintfErr("\nInitEncoder Error: Failed to create PSNR helper.\n");
                return result;
            }
        }
        result = m_psnr->Configure(m_vkDevCtx, encoderConfig, maxEncodeQueueDepth,
                                  m_imageDpbFormat, imageExtent,
                                  m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                  m_imageInFormat);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    // Zeroed here and ONLY here: they stay monotonic for the life of the
    // session, across any number of non-terminal drains (see
    // StartAssemblyThreads).
    m_assemblySequenceCounter = 0;
    m_nextWriteSequence = 0;
    m_assemblyErrorCount = 0;
    m_frameProcessingErrorCount = 0;
    // A false return here means asyncAssembly is off (--syncAssembly), which
    // is a configuration and not a failure. The synchronous fallback's own
    // guard in ProcessOrderedFrames covers the case where that configuration
    // cannot serve the caller's completion needs.
    (void)StartAssemblyThreads();

    return VK_SUCCESS;
}

// THE single site that brings the assembly workers up: called from
// InitEncoder and, for a non-terminal drain, from DrainAndRestartThreads().
// Idempotent; returns false and does nothing when the session is configured
// for synchronous assembly.
//
// The sequence counters are deliberately NOT reset here. InitEncoder zeroes
// them once, before the first call, and they stay monotonic for the session.
// Resetting them on a restart would happen to be safe -- a joined pipeline
// leaves m_nextWriteSequence == m_assemblySequenceCounter -- but leaving them
// alone is correct WITHOUT depending on that, and a restart that somehow ran
// with work still in flight then wedges on the ordering condition variable
// instead of silently putting two frames on the same turn.
bool VkVideoEncoder::StartAssemblyThreads()
{
    if (!m_encoderConfig || !m_encoderConfig->asyncAssembly) {
        return false;
    }
    if (m_asyncAssemblyEnabled) {
        return true;  // already up
    }
    if (!m_assemblyThreads.empty()) {
        VkEncPrintfErr("[AsyncAssembly] refusing to start: %u worker(s) "
                "still present; the previous pipeline was not joined\n",
                (uint32_t)m_assemblyThreads.size());
        return false;
    }
    // Clears the sticky flush latch a previous SetFlushAndExit() raised. On
    // the InitEncoder call the latch was never raised and this is a no-op; on
    // a restart it is the step without which every Push() below would be
    // refused and QueueFramesForAssembly would fail on the first frame.
    if (!m_assemblyQueue.ClearFlushAndReuse()) {
        VkEncPrintfErr("[AsyncAssembly] refusing to start: the assembly "
                "queue is not drained\n");
        return false;
    }
    // THE CAPACITY SIDE OF THE INEQUALITY, WHICH MUST NOT BE IGNORED.
    // CanAcceptNewInputFrame() refuses while (Size() + burst) > capacity. With
    // capacity pinned to numBitstreamBuffersToPreallocate (default 8) any burst
    // above 8 makes that test false FOREVER -- Size() 0 does not help -- so the
    // ext session answers VK_NOT_READY on every frame and never makes
    // progress. That is a hang, not backpressure, and the header permits
    // 1..254 consecutive B frames. Take the max so the queue can always hold
    // one full burst.
    //
    // Only the QUEUE capacity moves. numBitstreamBuffersToPreallocate is left
    // alone deliberately: it also feeds numInputImages, the async-assembly
    // slack and the preallocation check, and growing those would cost real
    // image memory for a problem that lives in this queue.
    const uint32_t assemblyCapacity =
        std::max<uint32_t>(m_encoderConfig->numBitstreamBuffersToPreallocate,
                           (uint32_t)GetMaxAssemblyBurst());
    m_assemblyQueue.SetMaxPendingQueueNodes(assemblyCapacity);
    m_assemblyQueueCapacity = assemblyCapacity;
    // THE INVARIANT, stated where it can be checked: at the instant
    // CanAcceptNewInputFrame() returns true, one EnqueueFrame() must not be
    // able to push more than (capacity - Size()) items. Checkable at init as
    // capacity >= burst. It holds by construction after the max() above --
    // m_assemblyQueueCapacity is uint32_t and the burst tops out at 257 -- so
    // this asserts the construction, not the configuration.
    assert(m_assemblyQueueCapacity >= GetMaxAssemblyBurst());
    m_asyncAssemblyEnabled = true;
    for (uint32_t i = 0; i < m_encoderConfig->assemblyThreadCount; i++) {
        m_assemblyThreads.emplace_back(
            &VkVideoEncoder::AssemblyWorkerThread, this, (int)i);
    }
    VkEncOut() << "[AsyncAssembly] Started " << m_encoderConfig->assemblyThreadCount
              << " assembly worker threads (queue capacity="
              << (int)assemblyCapacity << ", burst="
              << (int)GetMaxAssemblyBurst() << ")"
              << std::endl;
    return true;
}

VkDeviceSize VkVideoEncoder::GetBitstreamBuffer(VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer)
{
    VkDeviceSize newSize = m_streamBufferSize;
    assert(m_vkDevCtx);

    VkSharedBaseObj<VulkanBitstreamBufferImpl> newBitstreamBuffer;

    const bool enablePool = true;
    const bool debugBitstreamBufferDumpAlloc = false;
    int32_t availablePoolNode = -1;
    if (enablePool) {
        availablePoolNode = m_bitstreamBuffersQueue.GetAvailableNodeFromPool(newBitstreamBuffer);
    }
    if (!(availablePoolNode >= 0)) {
        VkResult result = VulkanBitstreamBufferImpl::Create(m_vkDevCtx,
                m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
                newSize,
                m_encoderConfig->videoCapabilities.minBitstreamBufferOffsetAlignment,
                m_encoderConfig->videoCapabilities.minBitstreamBufferSizeAlignment,
                nullptr, 0, newBitstreamBuffer);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            VkEncPrintfErr("\nERROR: VulkanBitstreamBufferImpl::Create() result: 0x%x\n", result);
            return 0;
        }
        if (debugBitstreamBufferDumpAlloc) {
            VkEncOut() << "\tAllocated bitstream buffer with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;
        }
        if (enablePool) {
            int32_t nodeAddedWithIndex = m_bitstreamBuffersQueue.AddNodeToPool(newBitstreamBuffer, true);
            if (nodeAddedWithIndex < 0) {
                assert("Could not add the new node to the pool");
            }
        }

    } else {

        assert(newBitstreamBuffer);
        newSize = newBitstreamBuffer->GetMaxSize();

#ifdef CLEAR_BITSTREAM_BUFFERS_ON_CREATE
        newBitstreamBuffer->MemsetData(0x0, copySize, newSize - copySize);
#endif
        if (debugBitstreamBufferDumpAlloc) {
            VkEncOut() << "\t\tFrom bitstream buffer pool with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;

            VkEncOut() << "\t\t\t FreeNodes " << m_bitstreamBuffersQueue.GetFreeNodesNumber();
            VkEncOut() << " of MaxNodes " << m_bitstreamBuffersQueue.GetMaxNodes();
            VkEncOut() << ", AvailableNodes " << m_bitstreamBuffersQueue.GetAvailableNodesNumber();
            VkEncOut() << std::endl;
        }
    }
    bitstreamBuffer = newBitstreamBuffer;
    if (newSize > m_streamBufferSize) {
        VkEncOut() << "\tAllocated bitstream buffer with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;
        m_streamBufferSize = (size_t)newSize;
    }
    return bitstreamBuffer->GetMaxSize();
}

VkImageLayout VkVideoEncoder::TransitionImageLayout(VkCommandBuffer cmdBuf,
                                                    VkSharedBaseObj<VkImageResourceView>& imageView,
                                                    VkImageLayout oldLayout, VkImageLayout newLayout,
                                                    uint32_t srcQueueFamilyIndex,
                                                    uint32_t dstQueueFamilyIndex)
{
    uint32_t baseArrayLayer = 0;
    VkImageMemoryBarrier2KHR imageBarrier = {

            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR, // VkStructureType sType
            nullptr, // const void*     pNext
            VK_PIPELINE_STAGE_2_NONE_KHR, // VkPipelineStageFlags2KHR srcStageMask
            0, // VkAccessFlags2KHR        srcAccessMask
            VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, // VkPipelineStageFlags2KHR dstStageMask;
            VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR, // VkAccessFlags   dstAccessMask
            oldLayout, // VkImageLayout   oldLayout // FIXME - use the real old layout
            newLayout, // VkImageLayout   newLayout
            srcQueueFamilyIndex, // uint32_t        srcQueueFamilyIndex
            dstQueueFamilyIndex, // uint32_t   dstQueueFamilyIndex
            imageView->GetImageResource()->GetImage(), // VkImage         image;
            {
                // VkImageSubresourceRange   subresourceRange
                VK_IMAGE_ASPECT_COLOR_BIT, // VkImageAspectFlags aspectMask
                0, // uint32_t           baseMipLevel
                1, // uint32_t           levelCount
                baseArrayLayer, // uint32_t           baseArrayLayer
                1, // uint32_t           layerCount;
            },
    };

    if ((oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) && (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) {
        imageBarrier.srcAccessMask = 0;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) && (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)) {
        imageBarrier.srcAccessMask = 0;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_GENERAL) &&
               (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) &&
               (srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT)) {
        // Local patch; not in upstream vk_video_samples.
        //
        // The staging copy's FOREIGN acquire, split out from the arm below.
        // An acquire's FIRST synchronisation and access scopes are IGNORED --
        // the matching release supplies them -- so naming a stage here is
        // meaningless, and naming VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT (which
        // this pair carried while it served both callers) is worse than
        // meaningless: it is the stage that is illegal on this queue family
        // the moment the same pair is reached WITHOUT an acquire.
        //
        // Empty first scope, stated deliberately -- the identical idiom, for
        // the identical reason, as the (VIDEO_ENCODE_SRC_KHR ->
        // VIDEO_ENCODE_SRC_KHR) FOREIGN acquire further down.
        imageBarrier.srcAccessMask = 0;
        imageBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_GENERAL) && (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) {
        // Local patch; not in upstream vk_video_samples.
        //
        // THE SAME PAIR, NOT AN ACQUIRE: the staging copy's source-side
        // transition for an image this device already owns. Every producer
        // that reaches it writes the LINEAR staging image from the HOST or
        // with a TRANSFER, never with a compute dispatch:
        //
        //   * the library's own file-input lane, which memcpy's the frame
        //     into a persistently mapped linear image (LoadNextFrame ->
        //     CopyYCbCrPlanesDirectCPU) and reaches this pair because a
        //     non-external frame's srcOldLayout is remapped UNDEFINED ->
        //     GENERAL before the acquire;
        //   * an external LOCAL registration that declares GENERAL, which is
        //     what a caller reusing a host-written staging image must declare.
        //
        // NOT VK_ACCESS_SHADER_WRITE_BIT / VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        // which would describe compute-filter output. That is wrong in two
        // independent ways on this lane, both of them reachable on the file-input
        // path: the host's stores
        // got NO availability operation before the transfer read (no
        // validation error, potentially wrong pixels), and COMPUTE_SHADER is
        // not a stage the staging queue family necessarily supports
        // (VUID-vkCmdPipelineBarrier2-srcStageMask-09675 -- which exempts
        // acquires by its own wording, which is exactly why splitting the
        // acquire out above is what makes this arm safe to state correctly).
        //
        // HOST and TRANSFER together, rather than HOST alone: a first
        // synchronisation scope wider than the producer needs is never
        // incorrect, and covering both spares the next producer the
        // rediscovery. Both are legal on every family this batch can be
        // submitted to -- HOST requires no queue capability at all, and
        // TRANSFER is implied by COMPUTE and by VIDEO_ENCODE. COMPUTE_SHADER
        // is deliberately NOT in the union: it is the one stage that could be
        // rejected here, and the only compute producer that can reach this
        // pair is a foreign one, which takes the acquire arm above.
        imageBarrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT_KHR |
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) && (newLayout == VK_IMAGE_LAYOUT_GENERAL)) {
        imageBarrier.srcAccessMask = 0;
        imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_GENERAL) &&
               (newLayout == VK_IMAGE_LAYOUT_GENERAL) &&
               (srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT)) {
        // Local patch; not in upstream vk_video_samples.
        //
        // The preprocess compute filter's input acquire, FOREIGN half. An
        // external producer that hands over an image leaves it in GENERAL,
        // and GENERAL is also what the filter reads it in (a STORAGE_IMAGE
        // descriptor admits no other layout), so the transition is a no-op in
        // layout terms and entirely real in ownership and visibility terms.
        //
        // Split from the non-acquire arm below by the same rule, and with the
        // same idiom, as (GENERAL -> TRANSFER_SRC_OPTIMAL) and
        // (TRANSFER_SRC_OPTIMAL -> TRANSFER_SRC_OPTIMAL) above: an acquire's
        // FIRST synchronisation and access scopes are IGNORED, the matching
        // release supplies them, so the first scope is stated empty rather
        // than invented. The VK_ACCESS_SHADER_WRITE_BIT /
        // VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT on a pair serving BOTH callers would
        // describe a compute producer that only the foreign side could have -- and
        // on the foreign side it is ignored.
        imageBarrier.srcAccessMask = 0;
        imageBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_GENERAL) && (newLayout == VK_IMAGE_LAYOUT_GENERAL)) {
        // Local patch; not in upstream vk_video_samples.
        //
        // THE SAME PAIR, NOT AN ACQUIRE: the filter's input transition for an
        // image this device already owns. This is the arm the shipping
        // Chromium filter lane actually takes, and the scopes it used to
        // carry were wrong for every producer that can reach it.
        //
        // WHO REACHES IT. The gate at the filter call site is
        // isExternalInput, NOT isForeignImport, so a LOCAL registration lands
        // here with both families IGNORED. Two producers reach it and
        // NEITHER is a compute dispatch:
        //
        //   * the HOST, through a persistent mapping. This is Chromium's
        //     shipping CPU/shmem staging tier on an I420 or RGBA session: it
        //     host-writes the image every frame through a coherent mmap,
        //     declares RESIDENCY_LOCAL, and leaves currentLayout UNDEFINED so
        //     srcOldLayout is remapped to GENERAL. Its stores got NO
        //     availability operation before the filter's SHADER_READ.
        //   * a TRANSFER, which is what the in-tree suite above does with
        //     vkCmdCopyBufferToImage.
        //
        // WHY IT IS SILENT, and why it is the copy arm's defect class rather
        // than a new one: the layers track
        // LAYOUTS, and this arm passes GENERAL -> GENERAL through verbatim,
        // so a completely wrong first scope is spec-clean. Worse here than on
        // the copy arm in one respect -- there the defect began at frame 2,
        // here the UNDEFINED -> GENERAL remap puts frame 1 on it too.
        //
        // COMPUTE_SHADER IS SAFE IN THIS UNION, and only here. Naming it on
        // the copy arm would risk
        // VUID-vkCmdPipelineBarrier2-srcStageMask-09675, which is why that
        // arm deliberately omits it. This arm is reachable ONLY on a session
        // that has an input compute filter, and GetStagedInputSubmitType()
        // returns COMPUTE for exactly that session, so the batch is submitted
        // on a family that supports it. That is the same argument the local
        // filter handback beside it already makes for its own COMPUTE_SHADER.
        // It is retained rather than dropped because a genuinely
        // compute-written LOCAL producer is a shape a caller may still have.
        imageBarrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT_KHR |
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR |
                                     VK_ACCESS_2_SHADER_WRITE_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) && (newLayout == VK_IMAGE_LAYOUT_GENERAL)) {
        // Same acquire, for a producer that hands the image over after a
        // transfer read. It does not describe the library's own staged input:
        // the staging release hands the image back in the layout the next
        // acquire declares, so a reused registration is left in srcOldLayout
        // rather than TRANSFER_SRC_OPTIMAL. Kept because an external producer
        // may still hand over in TRANSFER_SRC_OPTIMAL.
        imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) && (newLayout == VK_IMAGE_LAYOUT_GENERAL)) {
        // Host-written staging content read by the filter instead of by a
        // copy. HOST stages, and therefore never combined with a queue-family
        // transfer (VUID-VkImageMemoryBarrier2-srcStageMask-03854) -- the
        // callers apply that rule, the same way they already do for the
        // PREINITIALIZED -> TRANSFER_SRC_OPTIMAL arm.
        imageBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_HOST_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR) && (newLayout == VK_IMAGE_LAYOUT_GENERAL)) {
        // The filter's OUTPUT image, taken from the encoder's own input pool,
        // which hands its nodes out declared VIDEO_ENCODE_SRC_KHR.
        imageBarrier.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_GENERAL) &&
               (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR) &&
               (srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT)) {
        // Local patch; not in upstream vk_video_samples.
        //
        // THE FIFTH ARM OF THE SPLIT, and the one the previous four missed.
        // Path A's acquire in VkVideoEncoder::RecordVideoCodingCmd names
        // pathAProducerLayout as its oldLayout, and that value is whatever the
        // producer declared -- which for a caller that leaves
        // VkVideoEncoderExternalImageDescriptor::defaultLayout alone is
        // GENERAL. So the Path-A acquire lands on THIS pair, not on the
        // FOREIGN-guarded (VIDEO_ENCODE_SRC_KHR -> VIDEO_ENCODE_SRC_KHR) arm
        // below. Without this arm it falls into the non-acquire arm that follows
        // and goes out carrying that arm's compute-producer first scope.
        //
        // Shared with the non-acquire arm, the Path-A acquire went out
        // naming VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT and
        // VK_ACCESS_SHADER_WRITE_BIT on a queue family that reports
        // TRANSFER|SPARSE|VIDEO_ENCODE and no COMPUTE at all.
        //
        // Empty first scope, for the same reason and with the same idiom as
        // the four sibling arms: an acquire's FIRST synchronisation and access
        // scopes are IGNORED -- the matching release supplies them -- so
        // naming a stage here is meaningless, and naming a stage the recording
        // family does not support is what makes it a latent defect rather than
        // merely noise. The exemption that keeps it quiet today is explicit in
        // VUID-vkCmdPipelineBarrier2-srcStageMask-09675, which constrains
        // srcStageMask to the recording family's stages only when the barrier
        // does NOT specify an acquire operation. Share the arm with a
        // non-acquire caller -- which is exactly what was happening -- and the
        // exemption is gone.
        //
        // WHAT THIS DOES NOT FIX: the Path-A device loss. The acquire is
        // correct in isolation; the RELEASE is the trigger, and the trigger
        // is a driver defect.
        imageBarrier.srcAccessMask = 0;
        imageBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_GENERAL) && (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR)) {
        // THE SAME PAIR, NOT AN ACQUIRE: the compute filter's OUTPUT image on
        // its way into the encode. That caller passes no queue families (both
        // default to VK_QUEUE_FAMILY_IGNORED), so it keeps the compute-producer
        // first scope below, which is correct FOR IT and only for it.
        //
        // CF-02b. This arm was written for that caller and then had no
        // caller: until StageInputFrame's filter arm was taught to hand its
        // output over (below), NOTHING in the tree produced this pair with
        // IGNORED families, and the encode read the filter's output in
        // GENERAL. That is legal only with the unifiedImageLayoutsVideo
        // feature, which has zero occurrences anywhere in this library, in
        // media/gpu/ or in gpu/vulkan/ -- so it was a real violation of
        // VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811, invisible for the
        // reason VkVideoEncodeFrameInfo::srcEncodeImageStagedLayout
        // documents.
        //
        // FIRST SCOPE, unchanged and correct: the filter's dispatch WROTE
        // this image, and this arm is only ever recorded into the input
        // command buffer of a session that HAS a filter -- which is the
        // filter's own pool, created on the COMPUTE family -- so
        // COMPUTE_SHADER is a stage that family supports.
        //
        // SECOND SCOPE, CORRECTED from (VIDEO_ENCODE, VIDEO_ENCODE_READ) to
        // (ALL_COMMANDS, MEMORY_READ), and this is the whole reason the arm
        // could not simply be called as it stood.
        // VUID-vkCmdPipelineBarrier2-dstStageMask-09676 requires every stage
        // in dstStageMask to be valid for the queue family the command pool
        // was created on. A device may well expose a compute family with
        // NO VK_QUEUE_VIDEO_ENCODE_BIT_KHR at all and an encode family with
        // no COMPUTE, and on such a device no single barrier can name
        // COMPUTE_SHADER as its source and VIDEO_ENCODE as its destination
        // and be legal anywhere: the two stages have no queue family in
        // common.
        // Naming VIDEO_ENCODE here would have traded a silent layout
        // violation for a loud, and equally real, barrier violation.
        //
        // ALL_COMMANDS is not a cop-out and it is not the fallback's
        // resignation. It carries no queue-capability requirement, so it is
        // legal on every family this batch can be recorded on -- and the
        // actual reader, vkCmdEncodeVideoKHR, is in a DIFFERENT SUBMISSION
        // ordered by the input->encode binary semaphore. A semaphore signal
        // makes all prior writes available and its wait makes them visible,
        // so the encode's visibility does not come from this barrier's second
        // scope in any case; what this barrier owes is the LAYOUT TRANSITION
        // and an execution dependency on the dispatch that produced the
        // contents. Both are supplied here.
        imageBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR) && (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR)) {
        imageBarrier.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR) && (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) {
        imageBarrier.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) && (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR)) {
        imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR) && (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) {
        imageBarrier.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) && (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR)) {
        imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) && (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR)) {
        // Local patch; not in upstream vk_video_samples.
        //
        // CF-02a, AND THE ARM THE COPY BRANCH'S OWN COMMENT ASKED FOR BY
        // NAME. StageInputFrame's copy branch discarded the pool image into
        // TRANSFER_DST_OPTIMAL, ran vkCmdCopyImage into it, and then recorded
        // NOTHING -- so the encode read its source in TRANSFER_DST_OPTIMAL,
        // which VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811 forbids. The
        // branch is now taught to hand the image over, and this is the arm
        // that hand-off lands on. Without it the call would have fallen into
        // the total ALL_COMMANDS fallback below: still functionally safe,
        // still an actual transition, but a full pipeline stall plus an
        // unconditional per-frame "MISSING ARM" line on stderr.
        //
        // NOTE THE ASYMMETRY WITH THE ARM DIRECTLY ABOVE, because it is the
        // one thing a reader is most likely to get wrong when adding the next
        // arm: that one is TRANSFER_SRC (a producer that READ the image, so
        // TRANSFER_READ) and this one is TRANSFER_DST (vkCmdCopyImage WROTE
        // it, so TRANSFER_WRITE). Only a WRITE needs an availability
        // operation. Copying the sibling's masks would have produced a
        // barrier that makes nothing available and is silent about it --
        // every arm passes oldLayout/newLayout through verbatim, so the
        // layers stay happy and a wrong first scope raises no VUID at all.
        //
        // SECOND SCOPE IS ALL_COMMANDS/MEMORY_READ RATHER THAN
        // VIDEO_ENCODE/VIDEO_ENCODE_READ, for the reason spelled out on the
        // GENERAL -> VIDEO_ENCODE_SRC_KHR arm above and one more that is
        // specific to this pair. The copy branch is NOT the no-filter branch:
        // useComputeFilter is (m_inputComputeFilter != nullptr) && (this
        // FRAME needs it), so a filter-equipped session routing a frame that
        // needs no conversion takes THIS branch while
        // m_inputCommandBufferPool is still the filter's compute-family pool.
        // A compute family without VK_QUEUE_VIDEO_ENCODE_BIT_KHR makes
        // VIDEO_ENCODE in dstStageMask trip
        // VUID-vkCmdPipelineBarrier2-dstStageMask-09676 on exactly that
        // configuration -- and on no other, which is the shape of a defect
        // that ships. ALL_COMMANDS has no queue-capability requirement and is
        // legal on all three families this batch can be recorded on
        // (COMPUTE, TRANSFER, ENCODE); the encode's visibility comes from the
        // input->encode binary semaphore, not from here.
        imageBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) &&
               (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) &&
               (srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT)) {
        // Local patch; not in upstream vk_video_samples.
        //
        // THE STAGING COPY'S ACQUIRE FOR A PRODUCER THAT DECLARES
        // TRANSFER_SRC_OPTIMAL, foreign half. The copy arm's acquire
        // transitions TO TRANSFER_SRC_OPTIMAL, so a caller whose declared
        // input layout IS TRANSFER_SRC_OPTIMAL produces an equal-layout pair
        // -- and equal layouts are not a no-op: this is still an ownership
        // transfer and still the only memory dependency between the
        // producer's writes and vkCmdCopyImage's read.
        //
        // Split from the non-acquire arm below for exactly the reason, and
        // with exactly the idiom, that (GENERAL -> TRANSFER_SRC_OPTIMAL) is
        // split above: an acquire's FIRST synchronisation and access scopes
        // are IGNORED -- the matching release supplies them -- so the first
        // scope is stated empty rather than invented.
        imageBarrier.srcAccessMask = 0;
        imageBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) &&
               (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) {
        // Local patch; not in upstream vk_video_samples.
        //
        // THE SAME PAIR, NOT AN ACQUIRE. This is the arm the library had been
        // documenting for two releases without having: VkVideoEncoder.cpp's
        // staging release (:1290-1294) and its filter release (:1513-1517)
        // both promise, in as many words, that "a caller that declares
        // TRANSFER_SRC_OPTIMAL round-trips today". It did not round-trip. It
        // reached the terminal else, which under __cpp_exceptions -- the
        // standalone CMake build -- is an uncaught throw from a function with
        // no handler anywhere on its call stack, i.e. std::terminate, and
        // under Chromium's -fno-exceptions build is a SILENT fall-through
        // that leaves the struct defaults: a VIDEO_ENCODE second scope in
        // front of a vkCmdCopyImage TRANSFER read, with no diagnostic.
        //
        // The declaration is not exotic. It is what a caller that pools a
        // staging image and last used it as a copy source must state to be
        // truthful, it is what the ext layer's own legacy wrap uses as its
        // default, and the library's residual-layout record hands this exact
        // value back to such a caller on every frame.
        //
        // SCOPES: mirror the non-acquire (GENERAL -> TRANSFER_SRC_OPTIMAL)
        // arm above verbatim, because the producer is the same producer --
        // the host through a persistent mapping, or a transfer -- and only
        // the layout it chose to name differs. In particular COMPUTE_SHADER
        // is deliberately absent: it is the one stage the staging queue
        // family may not support
        // (VUID-vkCmdPipelineBarrier2-srcStageMask-09675), and the only
        // compute producer that can reach this pair is a foreign one, which
        // takes the acquire arm above.
        imageBarrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT_KHR |
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) && (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) {
        imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) && (newLayout == VK_IMAGE_LAYOUT_UNDEFINED)) {
        imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.dstAccessMask = 0;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) && (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) {
        // Local patch; not in upstream vk_video_samples.
        // A host-written LINEAR external input. Chromium's VulkanVideoEncode-
        // Accelerator shmem-staging path host-fills the NV12 planes through a
        // persistent mapping and then SUBMITS the frame with
        // currentLayout=PREINITIALIZED -- forwarded as desc.defaultLayout, and
        // reaching this table as srcExternalImageLayout. That SUBMIT-time
        // layout is what this arm keys on. It is NOT the staging image's
        // create-time layout: that is initialLayout=UNDEFINED, as
        // VUID-VkImageCreateInfo-pNext-01443 requires of an external-memory
        // image (vulkan_video_encode_accelerator.cc:1604). This comment used to
        // say Chromium created the image PREINITIALIZED; that stopped being
        // true when Chromium moved to UNDEFINED, and behaviour never depended
        // on it. Chromium hands the frame to SetExternalInputFrame; the library
        // stages it to an OPTIMAL encode image via CopyLinearToOptimalImage. The
        // upstream transition table only covers producer-left GENERAL images, so
        // PREINITIALIZED fell through to the (exceptions-off) empty else and kept
        // the default VIDEO_ENCODE-stage barrier -> wrong sync for the following
        // transfer read. Make the host writes available to the copy.
        imageBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_HOST_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR) &&
               (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR) &&
               (srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT)) {
        // Local patch; not in upstream vk_video_samples.
        //
        // THE PATH-A ACQUIRE ALREADY TAKES THIS PAIR AND HAD NO ARM. A
        // producer that hands over an encode-source image declares
        // VIDEO_ENCODE_SRC_KHR, and the acquire transitions it to
        // VIDEO_ENCODE_SRC_KHR -- an ownership transfer with no layout
        // change. With no arm it fell to the else below, which in Chromium's
        // -fno-exceptions build silently kept the struct defaults and in the
        // standalone CMake build THROWS. It has been surviving in Chromium
        // only because those defaults happen to suit an acquire feeding a
        // video-encode read; stating it makes that luck a contract.
        imageBarrier.srcAccessMask = 0;
        imageBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR) &&
               (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR)) {
        // Same pair, NOT an acquire. The arm above deliberately leaves the
        // first synchronisation scope empty because a release operation
        // supplies it and an acquire's is ignored; an intra-queue barrier with
        // those masks would be ordered against nothing. This table dispatches
        // on the layout pair and would otherwise hand the acquire's scopes to
        // any future caller. No such caller exists today -- the only site that
        // produces this pair is the Path-A FOREIGN acquire -- so this arm
        // exists to keep the next one from inheriting the wrong scope.
        imageBarrier.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    } else {
        // Local patch; not in upstream vk_video_samples.
        //
        // THE TOTAL FALLBACK, replacing a construct that was the worst of
        // both worlds: `throw std::invalid_argument` under __cpp_exceptions
        // and an EMPTY BODY without it. That is precisely inverted with
        // respect to which build ships. The standalone CMake build -- which
        // defines __cpp_exceptions, has no handler anywhere on
        // StageInputFrame's call stack, and is a test harness -- got
        // std::terminate. Chromium, which compiles this file -fno-exceptions
        // and is the build that actually ships, fell THROUGH the empty else
        // and recorded the barrier with the struct defaults set at the top of
        // this function: srcStageMask NONE, srcAccessMask 0, dstStageMask
        // VIDEO_ENCODE, dstAccessMask VIDEO_ENCODE_READ. In front of a
        // vkCmdCopyImage that is the wrong second scope, and it is silent --
        // the layers cannot object, because oldLayout/newLayout are passed
        // through verbatim so the layout tracker stays consistent and no VUID
        // is violated. A wrong barrier with no diagnostic is the single
        // hardest defect class in this file to find; two of the arms above
        // exist because it was found the hard way.
        //
        // WHY NOT KEEP THE THROW: a library that terminates the embedder's
        // process because a caller named a legal VkImageLayout this table has
        // not been taught yet is not a library. And it terminated only in the
        // build that cannot ship, so it bought no shipping safety at all.
        //
        // WHY NOT RETURN AN ERROR: three of this function's ten call sites
        // discard the return value entirely and two more cast it to (void).
        // Threading a status out would be a far wider change than the defect
        // warrants and would give the discarding sites nothing.
        //
        // WHY THESE MASKS ARE SAFE ON EVERY QUEUE THIS CAN RECORD ON:
        // VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT carries no queue-capability
        // requirement, so unlike COMPUTE_SHADER or VIDEO_ENCODE it can
        // trip neither VUID-vkCmdPipelineBarrier2-srcStageMask-09675 nor
        // -dstStageMask-09676 on the transfer, compute or encode family this
        // batch may be submitted to. MEMORY_READ|MEMORY_WRITE expand to every
        // access type the stages support.
        //
        // THE COST, STATED: on an unhandled pair this degenerates to a full
        // pipeline stall for this image. That is the correct price for "the
        // library does not know what your producer did", and it is paid only
        // on a pair no arm claims.
        imageBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
        imageBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT_KHR;
        imageBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT_KHR |
                                     VK_ACCESS_2_MEMORY_WRITE_BIT_KHR;
        // ALL_COMMANDS does NOT include VK_PIPELINE_STAGE_2_HOST_BIT, and a
        // host-written producer is the most likely unhandled caller to arrive
        // here, so its stores would otherwise get no availability operation.
        // Added ONLY when there is no ownership transfer: HOST stages
        // combined with a queue-family transfer is
        // VUID-VkImageMemoryBarrier2-srcStageMask-03854. An acquire or a
        // release names a real family on one side, so either one reaching
        // this fallback takes it without HOST, whatever layout pair carried
        // it in.
        if ((srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) &&
            (dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED)) {
            imageBarrier.srcStageMask  |= VK_PIPELINE_STAGE_2_HOST_BIT_KHR;
            imageBarrier.srcAccessMask |= VK_ACCESS_2_HOST_WRITE_BIT_KHR;
        }
        // UNCONDITIONAL, not behind VKENC_DEBUG_LAYOUT. Reaching this branch
        // means the table is incomplete for a caller that exists, which is a
        // fact the next reader of a log needs whether or not they knew to ask
        // for it. It names the pair and both families, because "an unhandled
        // pair occurred" is not actionable and a COUNT of them cannot even
        // distinguish one recurring pair from several different ones.
        VkEncErr() << "[VkVideoEncoder] TransitionImageLayout: no arm for ("
                   << (int)oldLayout << " -> " << (int)newLayout
                   << ") srcQueueFamily=" << (int)srcQueueFamilyIndex
                   << " dstQueueFamily=" << (int)dstQueueFamilyIndex
                   << "; recording the conservative ALL_COMMANDS fallback. "
                      "This is a MISSING ARM, not a supported shape."
                   << std::endl;
    }

    const VkDependencyInfoKHR dependencyInfo = {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
        nullptr,
        VK_DEPENDENCY_BY_REGION_BIT,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &imageBarrier,
    };
    m_vkDevCtx->CmdPipelineBarrier2KHR(cmdBuf, &dependencyInfo);

    // THE BARRIER PROGRAM THIS TABLE ACTUALLY SELECTED, under the same env
    // var as the residual/restore probes beside it.
    //
    // WHY THIS EXISTS: most of what this function decides is invisible to the
    // validation layers. The layers track LAYOUTS, and every arm here passes
    // oldLayout/newLayout through verbatim, so a completely wrong pair of
    // synchronisation scopes is spec-clean and silent -- which is how a
    // wrong pair of scopes survives review. A test that wants to assert the
    // library made the right
    // memory dependency has no other way to see it: no public observable
    // reports a barrier's masks, and a COUNT of barriers cannot distinguish a
    // right one from a wrong one.
    static const bool kDebugLayout =
        (getenv("VKENC_DEBUG_LAYOUT") != nullptr);
    if (kDebugLayout) {
        VkEncPrintfErr("[LAYOUT-BARRIER] img=%p old=%d new=%d srcQF=%d "
                        "dstQF=%d srcStage=0x%llx srcAccess=0x%llx "
                        "dstStage=0x%llx dstAccess=0x%llx\n",
                (void*)imageView->GetImageResource()->GetImage(),
                (int)oldLayout, (int)newLayout,
                (int)srcQueueFamilyIndex, (int)dstQueueFamilyIndex,
                (unsigned long long)imageBarrier.srcStageMask,
                (unsigned long long)imageBarrier.srcAccessMask,
                (unsigned long long)imageBarrier.dstStageMask,
                (unsigned long long)imageBarrier.dstAccessMask);
    }

    return newLayout;
}

void VkVideoEncoder::ReleaseImageToForeignQueue(VkCommandBuffer cmdBuf,
                                                VkSharedBaseObj<VkImageResourceView>& imageView,
                                                VkImageLayout oldLayout,
                                                VkImageLayout newLayout,
                                                uint32_t srcQueueFamilyIndex,
                                                VkPipelineStageFlags2KHR srcStageMask,
                                                VkAccessFlags2KHR srcAccessMask)
{
    assert(imageView);
    // A release is defined only on a queue of its SOURCE family, and an
    // ownership transfer needs a real family on at least one side. If the
    // caller could not name one there was no acquire either, so record
    // nothing rather than a one-sided transfer.
    if ((srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) ||
        (srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT)) {
        assert(!"ReleaseImageToForeignQueue: no local source queue family");
        return;
    }
    // HOST stage + a queue-family transfer is invalid
    // (VUID-VkImageMemoryBarrier2-srcStageMask-03854). Unreachable by
    // construction -- every isForeignImport arm already excludes
    // PREINITIALIZED -- but asserted so a later caller cannot reintroduce it.
    assert((srcStageMask & VK_PIPELINE_STAGE_2_HOST_BIT_KHR) == 0);

    uint32_t baseArrayLayer = 0;
    const VkImageMemoryBarrier2KHR imageBarrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR, // VkStructureType sType
            nullptr,       // const void*              pNext
            srcStageMask,  // VkPipelineStageFlags2KHR srcStageMask  -- USED by a release
            srcAccessMask, // VkAccessFlags2KHR        srcAccessMask -- USED by a release
            // IGNORED for a release; the spec says to set these to 0.
            VK_PIPELINE_STAGE_2_NONE_KHR, // VkPipelineStageFlags2KHR dstStageMask
            0,                            // VkAccessFlags2KHR        dstAccessMask
            //
            // WHY THE CALLER CHOOSES BOTH LAYOUTS, and what does NOT decide it.
            //
            // An earlier version of this comment argued that a transition here
            // is "unsatisfiable" because the spec requires a release and its
            // acquire to repeat identical layouts and a
            // VK_QUEUE_FAMILY_FOREIGN_EXT consumer records no Vulkan acquire.
            // THAT ARGUMENT WAS WRONG on both halves. A transition is defined
            // by any barrier whose layouts differ, independently of pairing;
            // the equality rule exists only so a transition submitted twice
            // executes once. And the rule never reaches this barrier anyway: a
            // release runs on a queue of the SOURCE family and its acquire on
            // the DESTINATION family, so our release (local -> FOREIGN) and
            // our own next acquire of the same image (FOREIGN -> local) are
            // two OPPOSITE-DIRECTION transfers, not two halves of one. Nothing
            // in the spec binds their layouts together.
            //
            // What DOES decide it is the caller: |newLayout| should be the
            // layout the NEXT acquire of the same resource will declare, so
            // the two barriers do not assert different things about one
            // instant. Lanes whose producer re-declares the layout it handed
            // over pass oldLayout == newLayout, which defines no transition
            // and is the cheapest correct thing to say.
            //
            // NOTE WHAT THIS DOES NOT CLAIM. The spec sequences a queue-family
            // layout transition to happen-after the release and happen-before
            // the acquire, and no acquire is ever recorded by a FOREIGN
            // consumer -- so the library can NAME a layout here but cannot
            // promise the foreign agent observes it, and contents are
            // undefined after a release regardless. This is internal
            // self-consistency, not a guarantee to the consumer.
            oldLayout, // VkImageLayout   oldLayout
            newLayout, // VkImageLayout   newLayout
            srcQueueFamilyIndex,         // uint32_t   srcQueueFamilyIndex
            VK_QUEUE_FAMILY_FOREIGN_EXT, // uint32_t   dstQueueFamilyIndex
            imageView->GetImageResource()->GetImage(), // VkImage image
            {
                // Must match the acquire's subresource range exactly.
                VK_IMAGE_ASPECT_COLOR_BIT, // VkImageAspectFlags aspectMask
                0,              // uint32_t baseMipLevel
                1,              // uint32_t levelCount
                baseArrayLayer, // uint32_t baseArrayLayer
                1,              // uint32_t layerCount
            },
    };

    const VkDependencyInfoKHR dependencyInfo = {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
        nullptr,
        // 0, not BY_REGION: by-region has meaning only inside a render pass
        // instance, and an ownership transfer is not a per-region operation.
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &imageBarrier,
    };
    m_vkDevCtx->CmdPipelineBarrier2KHR(cmdBuf, &dependencyInfo);

    // Record-time observable. A missing release is invisible to the validation
    // layers -- that is exactly why it survived -- so counting is the only way
    // to prove it ran, and to prove it does NOT run on the lanes that never
    // acquired.
    static const bool kDebugQfot =
        (getenv("VKENC_DEBUG_QFOT") != nullptr);
    if (kDebugQfot) {
        // Both layouts: oldLayout is the one VUID-...-oldLayout-01197
        // constrains, and printing only one makes the copy and filter lanes
        // indistinguishable in a log when they agree on it.
        VkEncPrintfErr("[QFOT-REL] img=%p old=%d new=%d srcFamily=%u -> "
                        "FOREIGN srcStage=0x%llx srcAccess=0x%llx\n",
                (void*)imageView->GetImageResource()->GetImage(),
                (int)oldLayout, (int)newLayout, srcQueueFamilyIndex,
                (unsigned long long)srcStageMask,
                (unsigned long long)srcAccessMask);
    }
}

VkImageLayout VkVideoEncoder::RestoreStagedInputLayout(VkCommandBuffer cmdBuf,
                                              VkSharedBaseObj<VkImageResourceView>& imageView,
                                              VkImageLayout residualLayout,
                                              VkImageLayout declaredLayout,
                                              VkPipelineStageFlags2KHR srcStageMask,
                                              VkAccessFlags2KHR srcAccessMask)
{
    assert(imageView);

    // SUBSTITUTION FIRST, so that everything below -- including the
    // equal-layout early return -- reasons about the layout the image will
    // ACTUALLY be left in rather than about the one that was asked for.
    //
    // Neither UNDEFINED nor PREINITIALIZED is a legal barrier destination
    // (VUID-VkImageMemoryBarrier2-newLayout-01198), and PREINITIALIZED is
    // unrestorable by construction: it asserts "never yet in any other
    // layout since creation", which can be true at most once in an image's
    // life and is false the moment our own acquire moves it. GENERAL is the
    // only other layout in which host access to a LINEAR image is defined,
    // which is what such a caller does between frames, and it is a legal
    // destination.
    //
    // This used to record nothing here. See the header for why that was
    // right then and wrong now: the return value is stored on the
    // registration's node and named as the NEXT acquire's oldLayout, so the
    // substituted layout is not a second false declaration -- it is the one
    // true statement in the sequence.
    VkImageLayout targetLayout = declaredLayout;
    if ((declaredLayout == VK_IMAGE_LAYOUT_UNDEFINED) ||
        (declaredLayout == VK_IMAGE_LAYOUT_PREINITIALIZED)) {
        targetLayout = VK_IMAGE_LAYOUT_GENERAL;
        static const bool kDebugLayout =
            (getenv("VKENC_DEBUG_LAYOUT") != nullptr);
        if (kDebugLayout) {
            VkEncPrintfErr("[LAYOUT-RESTORE] SUBSTITUTED img=%p residual=%d "
                            "declared=%d -> GENERAL (declared layout is not a "
                            "legal barrier destination; the library records "
                            "GENERAL and names it on the next acquire)\n",
                    (void*)imageView->GetImageResource()->GetImage(),
                    (int)residualLayout, (int)declaredLayout);
        }
    }

    // Our arm already leaves the image where the next acquire will name it.
    // Record nothing -- a barrier here would be a no-op transition whose only
    // effect is to construct a pair the layout table has no arm for. The
    // RETURN VALUE is still |targetLayout|, so the caller's record is right
    // on this branch too.
    if (residualLayout == targetLayout) {
        return targetLayout;
    }

    // HOST is the destination scope because the only consumer between this
    // handback and the caller's next acquire is the caller writing the LINEAR
    // staging image through a persistent mapping -- which is also why the
    // layout being restored to is one in which host access is defined.
    //
    // HOST is additionally the one destination stage that CANNOT be rejected
    // for the recording queue: it requires no queue capability, so it cannot
    // trip VUID-vkCmdPipelineBarrier2-dstStageMask-09676. That matters
    // concretely -- an earlier attempt at this fix reached for the layout
    // table's compute-filter scopes here and emitted
    // VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT on a queue family that reports
    // TRANSFER|SPARSE_BINDING|VIDEO_ENCODE, producing 24 new validation
    // errors. The shipping PREINITIALIZED -> TRANSFER_SRC_OPTIMAL arm already
    // names HOST stages on that exact family every frame with none.
    //
    // No queue-family transfer (both families IGNORED), so
    // VUID-VkImageMemoryBarrier2-srcStageMask-03854 -- which forbids HOST
    // stages combined with an ownership transfer, and which
    // ReleaseImageToForeignQueue asserts against for that reason -- does not
    // apply here.
    uint32_t baseArrayLayer = 0;
    const VkImageMemoryBarrier2KHR imageBarrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR, // VkStructureType sType
            nullptr,       // const void*              pNext
            srcStageMask,  // VkPipelineStageFlags2KHR srcStageMask
            srcAccessMask, // VkAccessFlags2KHR        srcAccessMask
            VK_PIPELINE_STAGE_2_HOST_BIT_KHR, // VkPipelineStageFlags2KHR dstStageMask
            VK_ACCESS_2_HOST_WRITE_BIT_KHR |
                VK_ACCESS_2_HOST_READ_BIT_KHR, // VkAccessFlags2KHR dstAccessMask
            // oldLayout: the literal our arm named as its acquire newLayout,
            // which is what VUID-VkImageMemoryBarrier2-oldLayout-01197
            // constrains.
            residualLayout, // VkImageLayout oldLayout
            // newLayout: what the NEXT acquire of this registration will
            // name as its oldLayout -- which is this function's return
            // value, recorded by the caller on the registration's node.
            // Equal to the caller's declaration whenever that declaration
            // is a legal barrier destination, and GENERAL when it is not.
            targetLayout, // VkImageLayout newLayout
            VK_QUEUE_FAMILY_IGNORED, // uint32_t srcQueueFamilyIndex
            VK_QUEUE_FAMILY_IGNORED, // uint32_t dstQueueFamilyIndex
            imageView->GetImageResource()->GetImage(), // VkImage image
            {
                // Must match the acquire's subresource range exactly.
                VK_IMAGE_ASPECT_COLOR_BIT, // VkImageAspectFlags aspectMask
                0,              // uint32_t baseMipLevel
                1,              // uint32_t levelCount
                baseArrayLayer, // uint32_t baseArrayLayer
                1,              // uint32_t layerCount
            },
    };

    const VkDependencyInfoKHR dependencyInfo = {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
        nullptr,
        // 0, not BY_REGION: by-region has meaning only inside a render pass
        // instance. Same reasoning as ReleaseImageToForeignQueue.
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &imageBarrier,
    };
    m_vkDevCtx->CmdPipelineBarrier2KHR(cmdBuf, &dependencyInfo);

    static const bool kDebugLayout =
        (getenv("VKENC_DEBUG_LAYOUT") != nullptr);
    if (kDebugLayout) {
        VkEncPrintfErr("[LAYOUT-RESTORE] img=%p old=%d new=%d "
                        "srcStage=0x%llx srcAccess=0x%llx\n",
                (void*)imageView->GetImageResource()->GetImage(),
                (int)residualLayout, (int)targetLayout,
                (unsigned long long)srcStageMask,
                (unsigned long long)srcAccessMask);
    }

    return targetLayout;
}

VkResult VkVideoEncoder::CopyLinearToOptimalImage(VkCommandBuffer& commandBuffer,
                                                  VkSharedBaseObj<VkImageResourceView>& srcImageView,
                                                  VkSharedBaseObj<VkImageResourceView>& dstImageView,
                                                  const VkExtent2D& copyImageExtent,
                                                  uint32_t srcCopyArrayLayer,
                                                  uint32_t dstCopyArrayLayer,
                                                  VkImageLayout srcImageLayout,
                                                  VkImageLayout dstImageLayout)

{

    const VkSharedBaseObj<VkImageResource>& srcImageResource = srcImageView->GetImageResource();
    const VkSharedBaseObj<VkImageResource>& dstImageResource = dstImageView->GetImageResource();

    assert(srcImageResource->GetImageCreateInfo().extent.width  >= copyImageExtent.width);
    assert(srcImageResource->GetImageCreateInfo().extent.height >= copyImageExtent.height);

    assert(dstImageResource->GetImageCreateInfo().extent.width  >= copyImageExtent.width);
    assert(dstImageResource->GetImageCreateInfo().extent.height >= copyImageExtent.height);

    const VkFormat format = srcImageResource->GetImageCreateInfo().format;

    // Bind memory for the image.
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);

    // Currently formats that have more than 2 output planes are not supported. 444 formats have a shared CbCr planes in all current tests
    assert((mpInfo->vkPlaneFormat[2] == VK_FORMAT_UNDEFINED) && (mpInfo->vkPlaneFormat[3] == VK_FORMAT_UNDEFINED));

    // Copy src buffer to image.
    VkImageCopy copyRegion[3]{};
    copyRegion[0].extent.width  = copyImageExtent.width;
    copyRegion[0].extent.height = copyImageExtent.height;
    copyRegion[0].extent.depth  = 1;
    copyRegion[0].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    copyRegion[0].srcSubresource.mipLevel = 0;
    copyRegion[0].srcSubresource.baseArrayLayer = srcCopyArrayLayer;
    copyRegion[0].srcSubresource.layerCount = 1;
    copyRegion[0].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    copyRegion[0].dstSubresource.mipLevel = 0;
    copyRegion[0].dstSubresource.baseArrayLayer = dstCopyArrayLayer;
    copyRegion[0].dstSubresource.layerCount = 1;
    copyRegion[1].extent.width = copyRegion[0].extent.width;
    if (mpInfo->planesLayout.secondaryPlaneSubsampledX != 0) {
        copyRegion[1].extent.width = (copyRegion[1].extent.width + 1) / 2;
    }

    copyRegion[1].extent.height = copyRegion[0].extent.height;
    if (mpInfo->planesLayout.secondaryPlaneSubsampledY != 0) {
        copyRegion[1].extent.height = (copyRegion[1].extent.height + 1) / 2;
    }

    copyRegion[1].extent.depth = 1;
    copyRegion[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    copyRegion[1].srcSubresource.mipLevel = 0;
    copyRegion[1].srcSubresource.baseArrayLayer = srcCopyArrayLayer;
    copyRegion[1].srcSubresource.layerCount = 1;
    copyRegion[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    copyRegion[1].dstSubresource.mipLevel = 0;
    copyRegion[1].dstSubresource.baseArrayLayer = dstCopyArrayLayer;
    copyRegion[1].dstSubresource.layerCount = 1;

    m_vkDevCtx->CmdCopyImage(commandBuffer, srcImageResource->GetImage(), srcImageLayout,
                             dstImageResource->GetImage(), dstImageLayout,
                             (uint32_t)2, copyRegion);

    {
        // MAKE THE COPY'S WRITE VISIBLE TO THE ENCODE'S READ.
        //
        // It names the copy's WRITE, not its read, and a real destination stage.
        // VK_ACCESS_TRANSFER_READ_BIT as the source and
        // VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT as the destination stage are both
        // wrong for what this has to accomplish:
        //
        //   * the interesting access this copy performed on the image the
        //     ENCODE will read is the WRITE to the destination, not the read
        //     of the source, so TRANSFER_READ made none of the written data
        //     available;
        //   * BOTTOM_OF_PIPE as a DESTINATION stage makes nothing visible to
        //     anything -- it is the end of the pipeline, so there is no
        //     subsequent stage for the dstAccessMask to apply to.
        //
        // The consequence is a real read-after-write hazard on the staged
        // input lane, not a theoretical one: vkCmdEncodeVideoKHR reads a
        // resource vkCmdCopyImage wrote, while the dependency as written
        // allows all accesses at VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT rather
        // than VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR at
        // VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR. The hazard spans two
        // submits, so only submit-time synchronization validation can see
        // it.
        //
        // Naming TRANSFER_WRITE as the
        // source access and ALL_COMMANDS as the destination stage covers the
        // video-encode stage and its read access without this file having to
        // depend on the synchronization2 / video pipeline-stage enums, which
        // the surrounding code does not use.
        //
        // THIS IS A WIDENING, which is why it is safe to make on the shipping
        // staged lane: it adds synchronization rather than removing it, so it
        // cannot introduce a race that was not already there. It costs a
        // stricter dependency at the end of a staging copy that is already
        // fenced against the encode submit.
        //
        // NOT THE QUEUE-FAMILY OWNERSHIP QUESTION, which is separate and is
        // NOT addressed here: this pool image is VK_SHARING_MODE_EXCLUSIVE and
        // pinned to the encode family, and when a compute filter exists the
        // staged batch is recorded and submitted on the COMPUTE family with no
        // ownership transfer either way. Synchronization validation does not
        // model ownership, so a clean sync run says nothing about it. See the
        // note at the staged transitions in StageInputFrame.
        VkMemoryBarrier memoryBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        m_vkDevCtx->CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                               1, &memoryBarrier, 0,
                                0, 0, 0);
    }

    return VK_SUCCESS;
}


VkResult VkVideoEncoder::CopyLinearToLinearImage(VkCommandBuffer& commandBuffer,
                                                 VkSharedBaseObj<VkImageResourceView>& srcImageView,
                                                 VkSharedBaseObj<VkImageResourceView>& dstImageView,
                                                 const VkExtent2D& copyImageExtent,
                                                 uint32_t srcCopyArrayLayer,
                                                 uint32_t dstCopyArrayLayer,
                                                 VkImageLayout srcImageLayout,
                                                 VkImageLayout dstImageLayout)

{

    const VkSharedBaseObj<VkImageResource>& srcImageResource = srcImageView->GetImageResource();
    const VkSharedBaseObj<VkImageResource>& dstImageResource = dstImageView->GetImageResource();

    assert(srcImageResource->GetImageCreateInfo().extent.width  >= copyImageExtent.width);
    assert(srcImageResource->GetImageCreateInfo().extent.height >= copyImageExtent.height);

    assert(dstImageResource->GetImageCreateInfo().extent.width  >= copyImageExtent.width);
    assert(dstImageResource->GetImageCreateInfo().extent.height >= copyImageExtent.height);

    // Copy src buffer to image.
    VkImageCopy copyRegion{};
    copyRegion.extent.width  = copyImageExtent.width;
    copyRegion.extent.height = copyImageExtent.height;
    copyRegion.extent.depth  = 1;
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.mipLevel = 0;
    copyRegion.srcSubresource.baseArrayLayer = srcCopyArrayLayer;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.mipLevel = 0;
    copyRegion.dstSubresource.baseArrayLayer = dstCopyArrayLayer;
    copyRegion.dstSubresource.layerCount = 1;

    m_vkDevCtx->CmdCopyImage(commandBuffer, srcImageResource->GetImage(), srcImageLayout,
                             dstImageResource->GetImage(), dstImageLayout,
                             (uint32_t)1, &copyRegion);

    {
        VkMemoryBarrier memoryBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        // Same defect, same fix, as the one corrected in
        // CopyLinearToOptimalImage: this copy WRITES the destination, so
        // naming its READ made none of the written data available, and
        // BOTTOM_OF_PIPE as a DESTINATION stage makes nothing visible to
        // anything. The destination here is the QP map, which ProcessQpMap
        // chains onto encodeInfo as quantizationMapInfo.quantizationMap and
        // vkCmdEncodeVideoKHR then reads -- and with useDedicatedCommandBuf it
        // is a SEPARATE submit, i.e. the cross-submit case.
        //
        // NOT PROVEN. Unlike its sibling this carries no RED: no harness under
        // vk_video_encoder/test/ enables qpMap at all, so every "0 hazards"
        // result in this tree is silent on this lane by construction rather
        // than by cleanliness. Landed anyway because it is the identical
        // two-value strict widening of both scopes and therefore cannot
        // introduce a race. To prove it, add a qpMap arm (the CLI reaches it
        // via --qpMapFileName) and run it with the validation layer FORCED.
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        m_vkDevCtx->CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                               1, &memoryBarrier, 0,
                                0, 0, 0);
    }

    return VK_SUCCESS;
}

void VkVideoEncoder::ProcessQpMap(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    if ((m_encoderConfig->enableQpMap == VK_FALSE) ||
            ((encodeFrameInfo->srcQpMapImageResource == nullptr) &&
                    encodeFrameInfo->srcQpMapStagingResource == nullptr )) {
        return;
    }

    VkVideoPictureResourceInfoKHR* pSrcQpMapPictureResource = encodeFrameInfo->srcQpMapImageResource->GetPictureResourceInfo();
    encodeFrameInfo->quantizationMapInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUANTIZATION_MAP_INFO_KHR;
    encodeFrameInfo->quantizationMapInfo.pNext = nullptr;
    encodeFrameInfo->quantizationMapInfo.quantizationMap = pSrcQpMapPictureResource->imageViewBinding;
    encodeFrameInfo->quantizationMapInfo.quantizationMapExtent = { (m_encoderConfig->encodeWidth + m_qpMapTexelSize.width - 1) / m_qpMapTexelSize.width,
                                                                   (m_encoderConfig->encodeHeight + m_qpMapTexelSize.height - 1) / m_qpMapTexelSize.height };

    encodeFrameInfo->encodeInfo.flags |= ((m_encoderConfig->qpMapMode == EncoderConfig::DELTA_QP_MAP) ?
                                            VK_VIDEO_ENCODE_WITH_QUANTIZATION_DELTA_MAP_BIT_KHR :
                                            VK_VIDEO_ENCODE_WITH_EMPHASIS_MAP_BIT_KHR);

    vk::ChainNextVkStruct(encodeFrameInfo->encodeInfo, encodeFrameInfo->quantizationMapInfo);
}

void VkVideoEncoder::FillIntraRefreshInfo(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    encodeFrameInfo->intraRefreshInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INTRA_REFRESH_INFO_KHR;
    encodeFrameInfo->intraRefreshInfo.pNext = nullptr;
    encodeFrameInfo->intraRefreshInfo.intraRefreshCycleDuration = m_encoderConfig->intraRefreshCycleDuration;
    encodeFrameInfo->intraRefreshInfo.intraRefreshIndex = encodeFrameInfo->gopPosition.intraRefreshIndex;

    encodeFrameInfo->encodeInfo.flags |= VK_VIDEO_ENCODE_INTRA_REFRESH_BIT_KHR;

    vk::ChainNextVkStruct(encodeFrameInfo->encodeInfo, encodeFrameInfo->intraRefreshInfo);
}

bool VkVideoEncoder::HandleCtrlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    // Fold any queued mid-stream rate-control update BEFORE the master-gate
    // check. The gate is raised at session init only (frame 0 carries
    // RESET + RATE_CONTROL + QUALITY_LEVEL); a mid-stream update must raise
    // it itself, or the armed update is never consumed and the session runs
    // at its initial rate forever.
    ApplyPendingRateControlUpdate();
    if (m_sendRateControlCmd) {
        m_sendControlCmd = true;
    }
    if (m_sendControlCmd == 0) {
        return false;
    }
    m_sendControlCmd = false;
    encodeFrameInfo->sendControlCmd = true;

    VkBaseInStructure* pNext = nullptr;

    if (m_sendResetControlCmd == true) {

        m_sendResetControlCmd = false;
        encodeFrameInfo->sendResetControlCmd = true;
        encodeFrameInfo->controlCmd |= VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    }

    if (m_sendQualityLevelCmd == true) {

        m_sendQualityLevelCmd = false;
        encodeFrameInfo->sendQualityLevelCmd = true;
        encodeFrameInfo->controlCmd |= VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR;

        encodeFrameInfo->qualityLevel = m_encoderConfig->qualityLevel;
        encodeFrameInfo->qualityLevelInfo.sType  = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
        encodeFrameInfo->qualityLevelInfo.qualityLevel = encodeFrameInfo->qualityLevel;
        if (pNext != nullptr) {
            vk::ChainNextVkStruct(encodeFrameInfo->rateControlInfo, *pNext);
        }

        pNext = (VkBaseInStructure*)&encodeFrameInfo->qualityLevelInfo;
    }

    const bool sendRateControlCmd = m_sendRateControlCmd;
    if (m_sendRateControlCmd == true) {

        m_sendRateControlCmd = false;
        encodeFrameInfo->sendRateControlCmd = true;
        encodeFrameInfo->controlCmd |= VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;

        encodeFrameInfo->rateControlInfo = m_rateControlInfo;
        encodeFrameInfo->rateControlInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR;

        for (uint32_t layerIndx = 0; layerIndx < ARRAYSIZE(m_rateControlLayersInfo); layerIndx++) {
            encodeFrameInfo->rateControlLayersInfo[layerIndx] = m_rateControlLayersInfo[layerIndx];
            encodeFrameInfo->rateControlLayersInfo[layerIndx].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR;
        }

        // layerCount must be 0 when rateControlMode is DEFAULT or DISABLED
        VkVideoEncodeRateControlModeFlagBitsKHR rcMode = encodeFrameInfo->rateControlInfo.rateControlMode;
        if (rcMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR ||
            rcMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
            encodeFrameInfo->rateControlInfo.pLayers = nullptr;
            encodeFrameInfo->rateControlInfo.layerCount = 0;
        } else {
            encodeFrameInfo->rateControlInfo.pLayers = encodeFrameInfo->rateControlLayersInfo;
            encodeFrameInfo->rateControlInfo.layerCount = 1;
        }
        // NOTE: the begin-coding cache is deliberately NOT written here.
        // Upstream this assignment was dead -- the unconditional wipe in
        // RecordVideoCodingCmd clobbered it -- and gating that wipe on RESET
        // (as the spec requires) brought it back to life, where it declares
        // the newly commanded values with only the BASE chain. Begin-coding
        // must describe the session's rate-control state in full, codec
        // struct included, or VUID-vkCmdBeginVideoCodingKHR-pBeginInfo-08254
        // fires on the update frame. The post-control refresh below installs
        // the complete chain; leave the cache to it.

        if (pNext != nullptr) {
            vk::ChainNextVkStruct(encodeFrameInfo->rateControlInfo, *pNext);
        }

        pNext = (VkBaseInStructure*)&encodeFrameInfo->rateControlInfo;
    }

    encodeFrameInfo->pControlCmdChain = pNext;

    return sendRateControlCmd;
}

VkResult VkVideoEncoder::RecordVideoCodingCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                              uint32_t frameIdx, uint32_t ofTotalFrames)
{

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("cmdBuf recording", 4, encodeFrameInfo, frameIdx, ofTotalFrames);
    }

    // Get a encodeCmdBuffer pool to record the video commands
    bool success = m_encodeCommandBufferPool->GetAvailablePoolNode(encodeFrameInfo->encodeCmdBuffer);
    assert(success);
    if (!success) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // Reset the command buffer and sync
    encodeFrameInfo->encodeCmdBuffer->ResetCommandBuffer(true, "encoderEncodeFence");

    VkSharedBaseObj<VulkanCommandBufferPool::PoolNode>& encodeCmdBuffer = encodeFrameInfo->encodeCmdBuffer;

    assert(encodeFrameInfo != nullptr);
    assert(encodeCmdBuffer != nullptr);

    // ******* Start command buffer recording *************
    const VkCommandBufferBeginInfo beginInfo { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                               VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

    VkCommandBuffer cmdBuf = encodeCmdBuffer->BeginCommandBufferRecording(beginInfo);

    // ******* Record the video commands *************
    VkVideoBeginCodingInfoKHR encodeBeginInfo { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
    encodeBeginInfo.videoSession = *encodeFrameInfo->videoSession;
    encodeBeginInfo.videoSessionParameters = *encodeFrameInfo->videoSessionParameters;

    assert((encodeFrameInfo->encodeInfo.referenceSlotCount) <= ARRAYSIZE(encodeFrameInfo->dpbImageResources));
    // TODO: Calculate the number of DPB slots for begin against the multiple frames.
    encodeBeginInfo.referenceSlotCount = encodeFrameInfo->encodeInfo.referenceSlotCount + 1;

    encodeBeginInfo.pReferenceSlots = encodeFrameInfo->referenceSlotsInfo;

    const VulkanDeviceContext* vkDevCtx = encodeCmdBuffer->GetDeviceContext();

    // Handle the query indexes — querySlotId comes from the encode command
    // buffer pool node, set by GetQueryPool(). Unique per in-flight encode.
    uint32_t querySlotId = (uint32_t)-1;
    VkQueryPool queryPool = encodeCmdBuffer->GetQueryPool(querySlotId);
    assert(queryPool != VK_NULL_HANDLE);
    assert(querySlotId != (uint32_t)-1);

    // Clear the query results
    const uint32_t numQuerySamples = 1;
    vkDevCtx->CmdResetQueryPool(cmdBuf, queryPool, querySlotId, numQuerySamples);

    if (encodeFrameInfo->controlCmd & VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR)
    {
        // A session RESET returns rate control to the initial (default)
        // state, and the begin-coding info must describe exactly that. For
        // a mid-stream control command (rate-control update WITHOUT reset)
        // the begin info must keep describing the CURRENT state -- the
        // control command inside this coding scope then changes it, and the
        // cache refresh below picks the new state up for later frames.
        m_beginRateControlInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR, NULL};
    }

    encodeBeginInfo.pNext = &m_beginRateControlInfo;

    static const bool kDebugPsnr =
        (getenv("VKENC_DEBUG_PSNR") != nullptr);
    if (kDebugPsnr) {
        VkEncPrintfErr("[BEGINRC] picType=%d controlCmd=0x%x beginRCmode=%d\n",
                (int)encodeFrameInfo->gopPosition.pictureType,
                (unsigned)encodeFrameInfo->controlCmd,
                (int)m_beginRateControlInfo.rateControlMode);
    }

    // Path-A queue-family acquire. A dma_buf-imported image is owned by
    // VK_QUEUE_FAMILY_FOREIGN_EXT; without acquiring it into the encode family
    // the encode reads memory it does not own, which is undefined by the spec
    // (the staging path's equivalent acquire records having observed solid
    // zeros without it). Recorded on the acquiring queue and outside the video
    // coding scope. The layout does not change -- the producer already hands
    // the image over in VIDEO_ENCODE_SRC_KHR -- so this is purely the ownership
    // half of the transfer. Honours the CALLER-DECLARED residency: a caller
    // that pools its own images passes RESIDENCY_LOCAL and gets no barrier.
    //
    // THE PRODUCER LAYOUT, COMPUTED ONCE FOR BOTH HALVES, and at this scope
    // deliberately: as a local inside the acquire block below it is out of scope
    // for the release at the bottom of this function, which would then have to
    // hardcode a literal. Computing it here is what lets the two
    // barriers agree.
    //
    // NEITHER SENTINEL SURVIVES INTO A BARRIER. This value is named as the
    // release's newLayout at the bottom of this function, and
    // VUID-VkImageMemoryBarrier2-newLayout-01198 forbids BOTH
    // VK_IMAGE_LAYOUT_UNDEFINED and VK_IMAGE_LAYOUT_PREINITIALIZED there.
    // They arrive for different reasons -- UNDEFINED is the absent
    // declaration, PREINITIALIZED is a real one, a producer stating that the
    // image is exactly as created and has never been transitioned -- and
    // neither is legal in that position. VIDEO_ENCODE_SRC_KHR stands in for
    // both. It is the layout the acquire below targets in any case, so the
    // substituted pair is an ownership transfer with no layout change, and
    // the two halves still name one value.
    //
    // A caller that needs the image left in a particular layout between
    // frames states a legal one per frame; the two the spec reserves for
    // image creation cannot be honoured as a handback.
    const VkImageLayout declaredInputLayout =
        encodeFrameInfo->srcExternalImageLayout;
    const VkImageLayout pathAProducerLayout =
        ((declaredInputLayout != VK_IMAGE_LAYOUT_UNDEFINED) &&
         (declaredInputLayout != VK_IMAGE_LAYOUT_PREINITIALIZED))
            ? declaredInputLayout
            : VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;

    if (encodeFrameInfo->srcEncodeImageIsExternal &&
        (encodeFrameInfo->externalInputResidency ==
         EXTERNAL_INPUT_RESIDENCY_FOREIGN)) {
        VkSharedBaseObj<VkImageResourceView> srcEncodeImageView;
        if (encodeFrameInfo->srcEncodeImageResource->GetImageView(
                srcEncodeImageView)) {
            TransitionImageLayout(cmdBuf, srcEncodeImageView,
                                  pathAProducerLayout,
                                  VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
                                  VK_QUEUE_FAMILY_FOREIGN_EXT,
                                  (uint32_t)m_vkDevCtx->GetVideoEncodeQueueFamilyIdx());
        }
    }

    // ===== THE READER OF THE STAGED-INPUT LAYOUT RECORD =====
    //
    // The last point before the video coding scope opens, and therefore the
    // last point at which the encoder can still say something about the image
    // vkCmdEncodeVideoKHR is about to read.
    //
    // THIS IS NOT DECORATION, AND IT IS NOT A MIRROR OF THE BARRIER. The
    // layer check that should catch this -- VUID-vkCmdEncodeVideoKHR-
    // pEncodeInfo-10811 -- reads the image-layout map of THIS command buffer,
    // and the staging barriers are recorded into a different one, so the map
    // has no entry for this image and ValidateVideoImageLayout returns true
    // without comparing anything. The submit-time sweep keys off the same
    // registry and is equally blind. So on the staged paths there is no
    // instrument in the tree that can see the rule at all, and this record --
    // written by the staging arm, read here by the consumer, in a different
    // function and a different command buffer -- is the only thing that can.
    //
    // MAX_ENUM means the frame was never staged: Path A / RESIDENCY_LOCAL,
    // where Chromium declares defaultLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_
    // SRC_KHR and the frame is conformant by declaration, and the AV1
    // show-existing pseudo-frames, which have no source picture at all.
    // Judging those would be judging a fact this library did not record.
    //
    // A DIAGNOSTIC, NOT A REFUSAL: by the time this is reachable the frame is
    // recorded, submitted and in flight, and there is no correct way to
    // unwind it here. VkEncErr rather than assert alone, so it survives the
    // Release build that ships -- an NDEBUG-only check on a defect class
    // whose entire history is "silent in the build that ships" would be the
    // same mistake again.
    if ((encodeFrameInfo->srcEncodeImageStagedLayout != VK_IMAGE_LAYOUT_MAX_ENUM) &&
        (encodeFrameInfo->srcEncodeImageStagedLayout != VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR)) {
        VkEncErr() << "[VkVideoEncoder] the staged encode-source image is in "
                      "layout " << (int)encodeFrameInfo->srcEncodeImageStagedLayout
                   << ", not VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR ("
                   << (int)VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR
                   << "); vkCmdEncodeVideoKHR requires VIDEO_ENCODE_SRC_KHR "
                      "(VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811). A "
                      "StageInputFrame arm is missing its hand-off barrier."
                   << std::endl;
        assert(!"staged encode-source image is not in VIDEO_ENCODE_SRC_KHR");
    }

    // ===== MECHANISM-C: capture the ENCODER INPUT, one command before the
    // encode reads it, from the encode command buffer itself. =====
    if (m_psnr && m_psnr->SrcCaptureEnabled()) {
        m_psnr->CaptureSource(cmdBuf, encodeFrameInfo.get());
    }

    vkDevCtx->CmdBeginVideoCodingKHR(cmdBuf, &encodeBeginInfo);

    if (encodeFrameInfo->controlCmd != VkVideoCodingControlFlagsKHR()) {

        VkVideoCodingControlInfoKHR renderControlInfo = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
                                                          encodeFrameInfo->pControlCmdChain,
                                                          encodeFrameInfo->controlCmd};
        vkDevCtx->CmdControlVideoCodingKHR(cmdBuf, &renderControlInfo);

        // Cache the new session rate-control state for subsequent frames'
        // BeginCoding. The state the control command establishes includes
        // the codec-specific RC struct and the per-layer codec structs, and
        // VUID-vkCmdBeginVideoCodingKHR-pBeginInfo-08254 requires the
        // begin-info chain to match that state in FULL -- a base-only cache
        // trips validation on every subsequent frame. Everything is
        // snapshotted by VALUE: the frame's copies are pool-recycled and
        // m_rateControlLayersInfo is mutated by ApplyPendingRateControl-
        // Update before the next control command records, so neither may
        // back the cache.
        const VkBaseInStructure* baseRc  = nullptr;
        const VkBaseInStructure* codecRc = nullptr;
        for (const VkBaseInStructure* p =
                 reinterpret_cast<const VkBaseInStructure*>(encodeFrameInfo->pControlCmdChain);
             p != nullptr; p = p->pNext) {
            switch ((uint32_t)p->sType) {
                case VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR:
                    baseRc = p;
                    break;
                case VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR:
                case VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR:
                case VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_INFO_KHR:
                    codecRc = p;
                    break;
                default:
                    break;
            }
        }
        if (baseRc != nullptr) {
            m_beginRateControlInfo =
                *reinterpret_cast<const VkVideoEncodeRateControlInfoKHR*>(baseRc);
            m_beginRateControlInfo.pNext = nullptr;
            if (codecRc != nullptr) {
                switch ((uint32_t)codecRc->sType) {
                    case VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR:
                        m_beginRateControlInfoH264 =
                            *reinterpret_cast<const VkVideoEncodeH264RateControlInfoKHR*>(codecRc);
                        m_beginRateControlInfoH264.pNext = nullptr;
                        m_beginRateControlInfo.pNext = &m_beginRateControlInfoH264;
                        break;
                    case VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR:
                        m_beginRateControlInfoH265 =
                            *reinterpret_cast<const VkVideoEncodeH265RateControlInfoKHR*>(codecRc);
                        m_beginRateControlInfoH265.pNext = nullptr;
                        m_beginRateControlInfo.pNext = &m_beginRateControlInfoH265;
                        break;
                    case VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_INFO_KHR:
                        m_beginRateControlInfoAV1 =
                            *reinterpret_cast<const VkVideoEncodeAV1RateControlInfoKHR*>(codecRc);
                        m_beginRateControlInfoAV1.pNext = nullptr;
                        m_beginRateControlInfo.pNext = &m_beginRateControlInfoAV1;
                        break;
                    default:
                        break;
                }
            }
            if (m_beginRateControlInfo.layerCount > 0) {
                for (uint32_t li = 0; li < ARRAYSIZE(m_beginRateControlLayersInfo); li++) {
                    m_beginRateControlLayersInfo[li] =
                        encodeFrameInfo->rateControlLayersInfo[li];
                    m_beginRateControlLayersInfo[li].pNext = nullptr;
                    const VkBaseInStructure* layerExt =
                        reinterpret_cast<const VkBaseInStructure*>(
                            encodeFrameInfo->rateControlLayersInfo[li].pNext);
                    if (layerExt != nullptr) {
                        switch ((uint32_t)layerExt->sType) {
                            case VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR:
                                m_beginRateControlLayersInfoH264[li] =
                                    *reinterpret_cast<const VkVideoEncodeH264RateControlLayerInfoKHR*>(layerExt);
                                m_beginRateControlLayersInfoH264[li].pNext = nullptr;
                                m_beginRateControlLayersInfo[li].pNext =
                                    &m_beginRateControlLayersInfoH264[li];
                                break;
                            case VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR:
                                m_beginRateControlLayersInfoH265[li] =
                                    *reinterpret_cast<const VkVideoEncodeH265RateControlLayerInfoKHR*>(layerExt);
                                m_beginRateControlLayersInfoH265[li].pNext = nullptr;
                                m_beginRateControlLayersInfo[li].pNext =
                                    &m_beginRateControlLayersInfoH265[li];
                                break;
                            case VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_LAYER_INFO_KHR:
                                m_beginRateControlLayersInfoAV1[li] =
                                    *reinterpret_cast<const VkVideoEncodeAV1RateControlLayerInfoKHR*>(layerExt);
                                m_beginRateControlLayersInfoAV1[li].pNext = nullptr;
                                m_beginRateControlLayersInfo[li].pNext =
                                    &m_beginRateControlLayersInfoAV1[li];
                                break;
                            default:
                                break;
                        }
                    }
                }
                m_beginRateControlInfo.pLayers = m_beginRateControlLayersInfo;
            }
        }
    }

    if (m_videoMaintenance1FeaturesSupported)
    {
        VkVideoInlineQueryInfoKHR videoInlineQueryInfoKHR;
        videoInlineQueryInfoKHR.pNext = NULL;
        videoInlineQueryInfoKHR.sType = VK_STRUCTURE_TYPE_VIDEO_INLINE_QUERY_INFO_KHR;
        videoInlineQueryInfoKHR.queryPool = queryPool;
        videoInlineQueryInfoKHR.firstQuery = querySlotId;
        videoInlineQueryInfoKHR.queryCount = numQuerySamples;

        vk::ChainNextVkStruct(encodeFrameInfo->encodeInfo, videoInlineQueryInfoKHR);

        vkDevCtx->CmdEncodeVideoKHR(cmdBuf, &encodeFrameInfo->encodeInfo);
    }
    else
    {
        vkDevCtx->CmdBeginQuery(cmdBuf, queryPool, querySlotId, VkQueryControlFlags());

        vkDevCtx->CmdEncodeVideoKHR(cmdBuf, &encodeFrameInfo->encodeInfo);

        vkDevCtx->CmdEndQuery(cmdBuf, queryPool, querySlotId);
    }

    if (encodeFrameInfo->setupImageResource) {
        VkSharedBaseObj<VkImageResourceView> setupEncodeImageView;
        encodeFrameInfo->setupImageResource->GetImageView(setupEncodeImageView);

        TransitionImageLayout(cmdBuf, setupEncodeImageView, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR);
    }

    VkVideoEndCodingInfoKHR encodeEndInfo { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
    vkDevCtx->CmdEndVideoCodingKHR(cmdBuf, &encodeEndInfo);

    // Queue-family RELEASE -- the missing half of the Path-A acquire made
    // just before CmdBeginVideoCodingKHR. Placed immediately after the coding
    // scope closes, mirroring the acquire's placement immediately before it
    // opened. In-scope would also be legal, but out-of-scope keeps the two
    // halves symmetric and cannot interleave with the PSNR readback below,
    // which touches only the reconstructed picture.
    //
    // The gate is a copy of the acquire's: a frame that did not acquire must
    // never release.
    if (encodeFrameInfo->srcEncodeImageIsExternal &&
        (encodeFrameInfo->externalInputResidency ==
         EXTERNAL_INPUT_RESIDENCY_FOREIGN)) {
        VkSharedBaseObj<VkImageResourceView> srcEncodeImageViewRel;
        if (encodeFrameInfo->srcEncodeImageResource->GetImageView(
                srcEncodeImageViewRel)) {
            // oldLayout is the LITERAL the acquire above named as its
            // newLayout, and nothing transitions this image between the encode
            // and here, so the literal IS the current layout -- which is what
            // VUID-VkImageMemoryBarrier2-oldLayout-01197 constrains.
            //
            // newLayout is pathAProducerLayout -- the SAME value the NEXT
            // frame's acquire will name as ITS oldLayout -- so the handover
            // round-trips the producer's declaration wherever that
            // declaration is one a barrier may name as a destination, and the
            // substitute computed for it at the top of this function where it
            // is not. A hardcoded
            // VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR on both sides is correct
            // only for a producer that declares VIDEO_ENCODE_SRC_KHR: a
            // registration that leaves
            // VkVideoEncoderExternalImageDescriptor::defaultLayout alone
            // declares GENERAL, so frame 1 would acquire
            // (GENERAL -> VIDEO_ENCODE_SRC_KHR) and release
            // (VIDEO_ENCODE_SRC_KHR -> VIDEO_ENCODE_SRC_KHR), leaving the
            // image in VIDEO_ENCODE_SRC_KHR while frame 2's acquire declares
            // GENERAL about it.
            //
            // THIS DOES NOT MAKE PATH A WORK, and the reason is not in this
            // function. The release below is a spec-legal queue-family
            // ownership release to VK_QUEUE_FAMILY_FOREIGN_EXT, and a driver
            // can lose the device executing one whenever the image carries
            // VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR (or the _DPB_ bit) and
            // the barrier is recorded on any queue family except graphics or
            // optical flow. For a RELEASE that is forced, not a choice: a
            // release must run on a queue of its SOURCE family, and the only
            // family that owns this image is the encode family. It presents
            // as a device loss with an EMPTY VK_EXT_device_fault record, and
            // the validation layer reports nothing either way.
            //
            // No re-expression of this barrier avoids it. The layout pair,
            // the stage masks, the external-memory-ness, the video coding
            // scope and even the barrier API generation are all irrelevant
            // to the outcome.
            //
            // NOTHING IS SUPPRESSED HERE, DELIBERATELY. Dropping this release
            // makes the row encode, but it would leave the acquire above
            // permanently unmatched and would silently change what the
            // library promises a real dma_buf consumer. That is a design
            // decision, not a workaround to slip in under a driver bug.
            ReleaseImageToForeignQueue(
                cmdBuf, srcEncodeImageViewRel,
                VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
                pathAProducerLayout,
                (uint32_t)m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
                VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR);
        }
    }

    if (m_psnr && m_psnr->Enabled() && (encodeFrameInfo->setupImageResource != nullptr)) {
        VkSharedBaseObj<VkImageResourceView> setupEncodeImageView;
        encodeFrameInfo->setupImageResource->GetImageView(setupEncodeImageView);
        TransitionImageLayout(cmdBuf, setupEncodeImageView, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        m_psnr->CaptureOutput(cmdBuf, encodeFrameInfo.get());
        TransitionImageLayout(cmdBuf, setupEncodeImageView, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR);
    }

    // ******* End recording of the video commands *************

    VkResult result = encodeCmdBuffer->EndCommandBufferRecording(cmdBuf);

    return result;
}

VkResult VkVideoEncoder::SubmitVideoCodingCmds(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                               uint32_t frameIdx, uint32_t ofTotalFrames)
{

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("queue submit", 5, encodeFrameInfo, frameIdx, ofTotalFrames);
    }

    assert(encodeFrameInfo);

    assert(encodeFrameInfo->encodeCmdBuffer != nullptr);

    const VkCommandBuffer* pCmdBuf = encodeFrameInfo->encodeCmdBuffer->GetCommandBuffer();
    // The encode command buffer pool's binary semaphore is unused — the
    // external input caller uses timeline semaphores (releaseSem) for sync,
    // and ProcessOutputBitstream uses fences. Signaling this binary sem
    // without a consumer triggers VUID-vkQueueSubmit2-semaphore-03868 on
    // pool node reuse.
    VkSemaphore frameCompleteSemaphore = VK_NULL_HANDLE;

    // Create command buffer submit info
    VkCommandBufferSubmitInfoKHR cmdBufferInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR };
    cmdBufferInfo.commandBuffer = *pCmdBuf;
    cmdBufferInfo.deviceMask = 0;

    // Create wait semaphore submit infos
    // If we are processing the input staging, wait for it's semaphore
    // to be done before processing the input frame with the encoder.
    // For external direct input (no staging), inject external wait semaphores here.
    const uint32_t waitSemaphoreMaxCount = kDirectSubmitSemaphoreCapacity;
    VkSemaphoreSubmitInfoKHR waitSemaphoreInfos[waitSemaphoreMaxCount]{};

    const uint32_t signalSemaphoreMaxCount = kDirectSubmitSemaphoreCapacity;
    VkSemaphoreSubmitInfoKHR signalSemaphoreInfos[signalSemaphoreMaxCount]{};

    uint32_t waitSemaphoreCount = 0;
    uint32_t signalSemaphoreCount = 0;

    // -----------------------------------------------------------------------
    // CAPACITY IS A CONTRACT HERE, NOT A BUDGET.
    //
    // Both arrays above are fixed 8-slot stack arrays, and every loop that
    // fills them below stops at the boundary. That is not a degradation, it
    // is a correctness failure with a SUCCESS status on it:
    //
    //   WAIT side -- each entry names a producer the encode must not run
    //   ahead of. Dropping one makes vkCmdEncodeVideoKHR read the input image
    //   while a producer is still writing it: an intermittent
    //   read-before-write race, and the caller is handed a valid release
    //   fence and told nothing. The ext layer appends the imported ACQUIRE
    //   fence semaphore LAST (see the waitWithAcquire block in
    //   vulkan_video_encoder_ext.cpp), so a caller that supplied 8 waits and
    //   an armed acquireFenceFd had exactly the producer fence the handle API
    //   exists to honour dropped -- while the public header promises that
    //   supplying both "is legal and loses neither".
    //
    //   SIGNAL side -- a dropped signal is a semaphore nobody ever signals,
    //   i.e. a consumer that waits forever.
    //
    // The library already reasoned this way about the signal direction:
    // kMaxCallerSignalsForReleaseFence in vulkan_video_encoder_ext.cpp exists
    // precisely because "the direct-encode submit ... silently stops
    // appending when it fills". This is the same argument, applied to both
    // directions and enforced where the arrays actually are.
    //
    // Refusing is the honest answer, and it is checked HERE -- before a
    // single entry is placed -- so the refusal is total rather than a
    // half-filled array handed to the queue. The per-site bounds below are
    // left in place as belt-and-braces; with this check they cannot
    // truncate anything.
    //
    // Note this deliberately does NOT mirror kMaxCallerSignalsForReleaseFence
    // as a numeric cap on the wait side. That constant is 4, and a 7-wait
    // frame with an armed acquire fence is 8 entries -- it FITS, it works
    // today, and refusing it would trade a silent drop for a refusal of legal
    // input. The boundary that matters is the array, so the array is what is
    // checked.
    // -----------------------------------------------------------------------
    {
        uint32_t requiredWaits = 0;
#ifdef NV_AQ_GPU_LIB_SUPPORTED
        if (encodeFrameInfo->aqProcessorSlot) {
            requiredWaits++;
        } else
#else
        if (encodeFrameInfo->inputCmdBuffer) {
            requiredWaits++;
        }
#endif // NV_AQ_GPU_LIB_SUPPORTED
        if (encodeFrameInfo->qpMapCmdBuffer) {
            requiredWaits++;
        }
        if (encodeFrameInfo->isExternalInput && !encodeFrameInfo->inputCmdBuffer) {
            requiredWaits += (uint32_t)encodeFrameInfo->inputWaitSemaphores.size();
        }
        // Reserved, not optional: the HW load-balancing pair below appends
        // unconditionally on count.
        if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {
            requiredWaits++;
        }
        if (requiredWaits > waitSemaphoreMaxCount) {
            VkEncPrintfErr("\nEncoder Error: this frame needs %u wait semaphores but the "
                    "direct submit array holds %u. Refusing the submit rather than "
                    "dropping a wait: a dropped wait lets vkCmdEncodeVideoKHR read "
                    "the input image before the producer that wait names has "
                    "finished writing it.\n",
                    requiredWaits, waitSemaphoreMaxCount);
            assert(!"direct submit wait array would overflow");
            return VK_ERROR_TOO_MANY_OBJECTS;
        }

        // Upper bound on the signal side. The external block's release-timeline
        // entry only fires at a queue flush point and the pass-through loop then
        // starts at index 1, so that block contributes at most
        // inputSignalSemaphores.size(); counting the full size is conservative
        // and never refuses a shape that would have been assembled correctly.
        uint32_t requiredSignals = 0;
        if (frameCompleteSemaphore != VK_NULL_HANDLE) {
            requiredSignals++;
        }
        if (encodeFrameInfo->isExternalInput && !encodeFrameInfo->inputCmdBuffer &&
            !encodeFrameInfo->inputSignalSemaphores.empty()) {
            requiredSignals += (uint32_t)encodeFrameInfo->inputSignalSemaphores.size();
        }
        if ((m_completionTimelineSemaphore != VK_NULL_HANDLE) &&
            (encodeFrameInfo->externalFrameId != uint64_t(-1))) {
            requiredSignals++;
        }
        if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {
            requiredSignals++;
        }
        if (requiredSignals > signalSemaphoreMaxCount) {
            VkEncPrintfErr("\nEncoder Error: this frame needs %u signal semaphores but the "
                    "direct submit array holds %u. Refusing the submit rather than "
                    "dropping a signal: a dropped signal is a semaphore nobody ever "
                    "signals, and whoever waits on it waits forever.\n",
                    requiredSignals, signalSemaphoreMaxCount);
            assert(!"direct submit signal array would overflow");
            return VK_ERROR_TOO_MANY_OBJECTS;
        }
    }

#ifdef NV_AQ_GPU_LIB_SUPPORTED
    if (encodeFrameInfo->aqProcessorSlot) {
        uint64_t inputSeqNumber = encodeFrameInfo->aqProcessorSlot->GetInputSeqNumber();
        assert(encodeFrameInfo->frameEncodeInputOrderNum == inputSeqNumber);
        AqProcessor::SlotState slotState = encodeFrameInfo->aqProcessorSlot->GetState();
        VkVideoGopStructure::GopPosition gopPosition = encodeFrameInfo->aqProcessorSlot->GetGopPosition();
        VkEncPrintfOut("Submitting AQ qpMap inputSeqNumber %" PRIu64 ", type: %s, state: %s\n", inputSeqNumber,
                VkVideoGopStructure::GetFrameTypeName(gopPosition.pictureType),
                AqProcessor::GetSlotStateDisplayName(slotState));
        assert((slotState == AqProcessor::SlotState::GRAPH_COMPLETED) ||
                (slotState == AqProcessor::SlotState::GRAPH_COMPLETED_SYNCED));
        if (encodeFrameInfo->srcQpMapImageResource) {
            // Set the semaphore for the output image
            VkSemaphoreSubmitInfoKHR qpDeltaImageSem =
                    encodeFrameInfo->srcQpMapImageResource->GetSemaphoreSubmitInfo();
            if (qpDeltaImageSem.semaphore != VK_NULL_HANDLE) {
                assert(waitSemaphoreCount < waitSemaphoreMaxCount);
                waitSemaphoreInfos[waitSemaphoreCount] = qpDeltaImageSem;
                assert(waitSemaphoreInfos[waitSemaphoreCount].value != 0);
                assert(waitSemaphoreInfos[waitSemaphoreCount].semaphore != VK_NULL_HANDLE);
                waitSemaphoreCount++;
            } else {
                assert(!"qpDeltaImageSem must have a valid semaphore");
            }
        } else {
            assert(!"srcQpMapImageResource must have a valid when aqProcessorSlot is valid");
        }
    } else
#else // NV_AQ_GPU_LIB_SUPPORTED
    if (encodeFrameInfo->inputCmdBuffer) {
        waitSemaphoreInfos[waitSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].semaphore = encodeFrameInfo->inputCmdBuffer->GetSemaphore();
        waitSemaphoreInfos[waitSemaphoreCount].value = 0; // Binary semaphore
        // Use transfer bit since these semaphores come from transfer operations
        // -- except when the staged input was produced by the preprocess
        // COMPUTE filter, which SubmitStagedInputFrame then signals with an
        // ALL_COMMANDS scope. Waiting at TRANSFER on that signal would let
        // vkCmdEncodeVideoKHR read a pool image the filter has only partially
        // written. (The TRANSFER default on the copy branch is left as-is:
        // that is pre-existing behaviour on the shipped path, not something
        // this change makes reachable.)
        waitSemaphoreInfos[waitSemaphoreCount].stageMask =
            encodeFrameInfo->inputFilterRecorded
                ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                : VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].deviceIndex = 0;
        waitSemaphoreCount++;
    }
#endif // NV_AQ_GPU_LIB_SUPPORTED
    if (encodeFrameInfo->qpMapCmdBuffer) {
        waitSemaphoreInfos[waitSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].semaphore = encodeFrameInfo->qpMapCmdBuffer->GetSemaphore();
        waitSemaphoreInfos[waitSemaphoreCount].value = 0; // Binary semaphore
        // Use transfer bit since these semaphores come from transfer operations
        waitSemaphoreInfos[waitSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].deviceIndex = 0;
        waitSemaphoreCount++;
    }

    // For external direct input (Path A: no staging, image goes directly to encode):
    // Inject external wait semaphores here. For the staging path (Paths B/C),
    // these are injected in SubmitStagedInputFrame() instead.
    if (encodeFrameInfo->isExternalInput && !encodeFrameInfo->inputCmdBuffer) {
        for (size_t i = 0; i < encodeFrameInfo->inputWaitSemaphores.size() &&
                           waitSemaphoreCount < waitSemaphoreMaxCount; i++) {
            waitSemaphoreInfos[waitSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
            waitSemaphoreInfos[waitSemaphoreCount].semaphore = encodeFrameInfo->inputWaitSemaphores[i];
            waitSemaphoreInfos[waitSemaphoreCount].value =
                (i < encodeFrameInfo->inputWaitSemaphoreValues.size())
                    ? encodeFrameInfo->inputWaitSemaphoreValues[i] : 0;
            waitSemaphoreInfos[waitSemaphoreCount].stageMask =
                (i < encodeFrameInfo->inputWaitDstStageMasks.size())
                    ? encodeFrameInfo->inputWaitDstStageMasks[i]
                    : VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
            waitSemaphoreInfos[waitSemaphoreCount].deviceIndex = 0;
            waitSemaphoreCount++;
        }
    }

    // Create signal semaphore submit info if needed
    if (frameCompleteSemaphore != VK_NULL_HANDLE) {
        assert(signalSemaphoreCount < signalSemaphoreMaxCount);
        signalSemaphoreInfos[signalSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        signalSemaphoreInfos[signalSemaphoreCount].semaphore = frameCompleteSemaphore;
        signalSemaphoreInfos[signalSemaphoreCount].value = 0; // Binary semaphore
        signalSemaphoreInfos[signalSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        signalSemaphoreInfos[signalSemaphoreCount].deviceIndex = 0;
        signalSemaphoreCount++;
    }

    // Path A (direct encode, no staging): signal the producer's input-release
    // timeline (signal semaphore index 0) at queue flush points. Submits
    // arrive here in ENCODE order — under B-frame GOPs each ordered batch is
    // a reference frame followed by the deferred B-frames that precede it in
    // input order — so signaling each frame's own release value would free
    // the B inputs while the encode engine still has to read them, with
    // non-monotonic timeline values. The batch chain tail
    // (dependantFrames == nullptr) is the flush point: every input received
    // so far has been encode-submitted, and queue completion is FIFO, so
    // when this submit retires all of them are consumed. Signal the max
    // release value submitted so far — one monotonic signal per batch,
    // correct for any intra-batch order. Single-frame batches degenerate to
    // per-frame signaling. Values of 0 (binary semantics) and extra signal
    // semaphores (index 1+) pass through per-frame as before. For Paths B/C
    // the staging submit consumes the input in input order instead.
    if (encodeFrameInfo->isExternalInput && !encodeFrameInfo->inputCmdBuffer &&
        !encodeFrameInfo->inputSignalSemaphores.empty()) {
        size_t firstPassThroughIdx = 0;
        const uint64_t releaseValue = !encodeFrameInfo->inputSignalSemaphoreValues.empty()
                                          ? encodeFrameInfo->inputSignalSemaphoreValues[0] : 0;
        if (releaseValue != 0) {
            firstPassThroughIdx = 1;
            if (releaseValue > m_maxSubmittedInputReleaseId) {
                m_maxSubmittedInputReleaseId = releaseValue;
            }
            // Flush point = last EXTERNAL frame of the ordered batch. The
            // chain may end with codec pseudo-frames that are not external
            // inputs and read no input image (e.g. the AV1 show-existing
            // overlay node InsertOrdered appends after the B-frames), so
            // "dependantFrames == nullptr" alone would miss the tail.
            bool queueFlushPoint = true;
            for (const VkVideoEncodeFrameInfo* next = encodeFrameInfo->dependantFrames.get();
                 next != nullptr; next = next->dependantFrames.get()) {
                if (next->isExternalInput) {
                    queueFlushPoint = false;
                    break;
                }
            }
            static const bool releaseDebug = (getenv("VKENC_RELEASE_DEBUG") != nullptr);
            if (releaseDebug) {
                VkEncPrintfErr("[RELDBG] submit relVal=%llu max=%llu last=%llu tail=%d inOrd=%llu encOrd=%llu\n",
                        (unsigned long long)releaseValue,
                        (unsigned long long)m_maxSubmittedInputReleaseId,
                        (unsigned long long)m_lastSignaledInputReleaseId,
                        (int)queueFlushPoint,
                        (unsigned long long)encodeFrameInfo->gopPosition.inputOrder,
                        (unsigned long long)encodeFrameInfo->gopPosition.encodeOrder);
            }
            if (queueFlushPoint &&
                (m_maxSubmittedInputReleaseId > m_lastSignaledInputReleaseId) &&
                (signalSemaphoreCount < signalSemaphoreMaxCount)) {
                signalSemaphoreInfos[signalSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
                signalSemaphoreInfos[signalSemaphoreCount].semaphore = encodeFrameInfo->inputSignalSemaphores[0];
                signalSemaphoreInfos[signalSemaphoreCount].value = m_maxSubmittedInputReleaseId;
                signalSemaphoreInfos[signalSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
                signalSemaphoreInfos[signalSemaphoreCount].deviceIndex = 0;
                signalSemaphoreCount++;
                m_lastSignaledInputReleaseId = m_maxSubmittedInputReleaseId;
            }
        }
        for (size_t i = firstPassThroughIdx;
             i < encodeFrameInfo->inputSignalSemaphores.size() &&
             signalSemaphoreCount < signalSemaphoreMaxCount; i++) {
            signalSemaphoreInfos[signalSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
            signalSemaphoreInfos[signalSemaphoreCount].semaphore = encodeFrameInfo->inputSignalSemaphores[i];
            signalSemaphoreInfos[signalSemaphoreCount].value =
                (i < encodeFrameInfo->inputSignalSemaphoreValues.size())
                    ? encodeFrameInfo->inputSignalSemaphoreValues[i] : 0;
            signalSemaphoreInfos[signalSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
            signalSemaphoreInfos[signalSemaphoreCount].deviceIndex = 0;
            signalSemaphoreCount++;
        }
    }

    // Completion timeline (ext currency 3): one monotonic GPU signal per
    // ordered batch, at the same flush points as the input-release timeline
    // above and for the same reason -- encode order is not input order
    // under B-frames, and a timeline may not signal non-monotonically.
    // Unlike the release signal this covers the staged paths as well: the
    // currency is encode completion, not input release, so the gate is
    // "external frame", not "Path A". The flush-point scan is repeated
    // rather than hoisted so the shipping release block is not touched.
    // Value is max(externalFrameId)+1 over external frames submitted so
    // far; +1 because ids may legally start at 0 and a timeline cannot
    // signal its initial value.
    if ((m_completionTimelineSemaphore != VK_NULL_HANDLE) &&
        (encodeFrameInfo->externalFrameId != uint64_t(-1))) {
        const uint64_t completionValue = encodeFrameInfo->externalFrameId + 1;
        if (completionValue > m_maxSubmittedCompletionValue) {
            m_maxSubmittedCompletionValue = completionValue;
        }
        // Flush point = last EXTERNAL frame of the ordered batch; the chain
        // may end with codec pseudo-frames -- the same scan as the release
        // block above, for the same reason.
        bool completionFlushPoint = true;
        for (const VkVideoEncodeFrameInfo* next = encodeFrameInfo->dependantFrames.get();
             next != nullptr; next = next->dependantFrames.get()) {
            if (next->isExternalInput) {
                completionFlushPoint = false;
                break;
            }
        }
        static const bool completionDebug = (getenv("VKENC_COMPLETION_DEBUG") != nullptr);
        if (completionDebug) {
            VkEncPrintfErr("[CMPDBG] submit id=%llu val=%llu max=%llu last=%llu tail=%d\n",
                    (unsigned long long)encodeFrameInfo->externalFrameId,
                    (unsigned long long)completionValue,
                    (unsigned long long)m_maxSubmittedCompletionValue,
                    (unsigned long long)m_lastSignaledCompletionValue,
                    (int)completionFlushPoint);
        }
        // The capacity guard degrades by SKIPPING the signal -- a later
        // flush point catches up via the running max -- rather than
        // overflowing the array.
        if (completionFlushPoint &&
            (m_maxSubmittedCompletionValue > m_lastSignaledCompletionValue) &&
            (signalSemaphoreCount < signalSemaphoreMaxCount)) {
            signalSemaphoreInfos[signalSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
            signalSemaphoreInfos[signalSemaphoreCount].semaphore = m_completionTimelineSemaphore;
            signalSemaphoreInfos[signalSemaphoreCount].value = m_maxSubmittedCompletionValue;
            signalSemaphoreInfos[signalSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
            signalSemaphoreInfos[signalSemaphoreCount].deviceIndex = 0;
            signalSemaphoreCount++;
            m_lastSignaledCompletionValue = m_maxSubmittedCompletionValue;
        }
    }

    if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {

        if (m_verbose) {
            uint64_t  currSemValue = 0;
            VkResult semResult = m_vkDevCtx->GetSemaphoreCounterValue(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, &currSemValue);
            VkEncOut() << "\t TL semaphore value: " << currSemValue << ", status: " << semResult << std::endl;
        }

        // CAPACITY GUARD -- this pair had none.
        //
        // Every other appender in this function tests before it writes (the
        // frameCompleteSemaphore assert, the release-timeline guard, the
        // signal pass-through bound, the completion-timeline guard). These two
        // did not. With the arrays already full at 8 this wrote a 48-byte
        // VkSemaphoreSubmitInfoKHR one past the end of an 8-element STACK
        // array -- memory corruption, not a bad frame -- and then set
        // waitSemaphoreInfoCount to 9 so the driver read the overrun entry
        // too.
        //
        // The capacity check at the top of this function now reserves a slot
        // for this pair, so this cannot fire. It is kept anyway: "cannot fire"
        // is a property of code fifty lines above that a later edit can remove
        // without ever touching this block, and the cost of being wrong here
        // is a stack smash.
        assert(waitSemaphoreCount < waitSemaphoreMaxCount);
        assert(signalSemaphoreCount < signalSemaphoreMaxCount);
        if ((waitSemaphoreCount >= waitSemaphoreMaxCount) ||
            (signalSemaphoreCount >= signalSemaphoreMaxCount)) {
            VkEncPrintfErr("\nEncoder Error: no room for the HW load-balancing timeline "
                    "semaphore (waits %u/%u, signals %u/%u). Refusing the submit "
                    "rather than writing past the end of the submit arrays.\n",
                    waitSemaphoreCount, waitSemaphoreMaxCount,
                    signalSemaphoreCount, signalSemaphoreMaxCount);
            return VK_ERROR_TOO_MANY_OBJECTS;
        }
        waitSemaphoreInfos[waitSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].pNext = nullptr;
        waitSemaphoreInfos[waitSemaphoreCount].semaphore = m_hwLoadBalancingTimelineSemaphore;
        waitSemaphoreInfos[waitSemaphoreCount].value = encodeFrameInfo->frameEncodeEncodeOrderNum; // wait for the current value to be signaled
        waitSemaphoreInfos[waitSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].deviceIndex = 0;
        waitSemaphoreCount++;

        signalSemaphoreInfos[signalSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        signalSemaphoreInfos[signalSemaphoreCount].pNext = nullptr;
        signalSemaphoreInfos[signalSemaphoreCount].semaphore = m_hwLoadBalancingTimelineSemaphore;
        signalSemaphoreInfos[signalSemaphoreCount].value = encodeFrameInfo->frameEncodeEncodeOrderNum + 1; // signal the future m_decodePicCount value
        signalSemaphoreInfos[signalSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        signalSemaphoreInfos[signalSemaphoreCount].deviceIndex = 0;
        signalSemaphoreCount++;
    }

    // Create submit info
    VkSubmitInfo2KHR submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR, nullptr };
    submitInfo.flags = 0;
    submitInfo.waitSemaphoreInfoCount = waitSemaphoreCount;
    submitInfo.pWaitSemaphoreInfos = (waitSemaphoreCount > 0) ? waitSemaphoreInfos : nullptr;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdBufferInfo;
    submitInfo.signalSemaphoreInfoCount = signalSemaphoreCount;
    submitInfo.pSignalSemaphoreInfos = (signalSemaphoreCount > 0) ? signalSemaphoreInfos : nullptr;

    VkFence queueCompleteFence = encodeFrameInfo->encodeCmdBuffer->GetFence();
    assert(VK_NOT_READY == m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, queueCompleteFence));

    VkResult result = m_vkDevCtx->MultiThreadedQueueSubmit(VulkanDeviceContext::ENCODE,
                                                           m_currentVideoQueueIndx, // queueIndex
                                                           1, // submitCount
                                                           &submitInfo,
                                                           queueCompleteFence,
                                                           "Video Encode",
                                                           m_encodeEncodeFrameNum,
                                                           m_encodeInputFrameNum);

    encodeFrameInfo->encodeCmdBuffer->SetCommandBufferSubmitted();
    bool syncCpuAfterEncoding = false;
    if (syncCpuAfterEncoding) {
        encodeFrameInfo->encodeCmdBuffer->SyncHostOnCmdBuffComplete(false, "encoderEncodeFence");
    }

    if (m_verbose && (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE)) { // For TL semaphore debug
       uint64_t  currSemValue = 0;
       VkResult semResult = m_vkDevCtx->GetSemaphoreCounterValue(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, &currSemValue);
       VkEncOut() << "\t TL semaphore value ater submit: " << currSemValue << ", status: " << semResult << std::endl;

       const bool waitOnTlSemaphore = false;
       if (waitOnTlSemaphore) {
           uint64_t value = encodeFrameInfo->frameEncodeEncodeOrderNum + 1; // wait on the future frameEncodeEncodeOrderNum
           VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, VK_SEMAPHORE_WAIT_ANY_BIT, 1,
                                        &m_hwLoadBalancingTimelineSemaphore, &value };
           VkEncOut() << "\t TL semaphore wait for value: " << value << std::endl;
           semResult = m_vkDevCtx->WaitSemaphores(*m_vkDevCtx, &waitInfo, 1000 * 1000 * 1000 /* 1000 mSec */);

           semResult = m_vkDevCtx->GetSemaphoreCounterValue(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, &currSemValue);
           VkEncOut() << "\t TL semaphore value: " << currSemValue << ", status: " << semResult << std::endl;
       }
    }

    if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {
        m_currentVideoQueueIndx++;
        m_currentVideoQueueIndx %= m_vkDevCtx->GetVideoEncodeNumQueues();
    }

    return result;
}

VkResult VkVideoEncoder::PushOrderedFrames()
{
    VkResult result = VK_SUCCESS;
    if (m_lastDeferredFrame) {

        if (m_enableEncoderThreadQueue) {

            bool success = m_encoderThreadQueue.Push(m_lastDeferredFrame);
            if (success) {
                m_lastDeferredFrame = nullptr;
            } else {
                assert(!"Queue returned not ready");
                result = VK_NOT_READY;
            }

        } else {

            if (!m_encoderConfig->enableOutOfOrderRecording) {
                result = ProcessOrderedFrames(m_lastDeferredFrame, m_numDeferredFrames);
            } else {
                // Testing only - don't use for production!
                result = ProcessOutOfOrderFrames(m_lastDeferredFrame, m_numDeferredFrames);
            }
            if (result != VK_SUCCESS) {
                // The frames are given away immediately below whether the
                // push succeeded or not, so this counter is the trace the
                // drain reads afterwards. Counted here because every push --
                // the per-frame ones the enqueue makes and the final one the
                // drain makes -- arrives through this single path.
                m_frameProcessingErrorCount++;
            }
            if (m_asyncAssemblyEnabled) {
                m_lastDeferredFrame = nullptr;
            } else {
                VkVideoEncodeFrameInfo::ResetAndReleaseFrames(m_lastDeferredFrame);
                assert(m_lastDeferredFrame == nullptr);
            }
        }
        m_numDeferredFrames = 0;
        m_numDeferredRefFrames = 0;
    }
    return result;
}

VkResult VkVideoEncoder::ProcessOrderedFrames(VkSharedBaseObj<VkVideoEncodeFrameInfo>& frames, uint32_t numFrames) {

    std::vector<std::pair<std::string, std::function<VkResult(VkSharedBaseObj<VkVideoEncodeFrameInfo>&, uint32_t, uint32_t)>>> callbacks = {
        {"StartOfVideoCodingEncodeOrder",  [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return StartOfVideoCodingEncodeOrder(frame, frameIdx, ofTotalFrames); }},
        {"ProcessDpb",                     [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return ProcessDpb(frame, frameIdx, ofTotalFrames); }},
        {"RecordVideoCodingCmd",           [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return RecordVideoCodingCmd(frame, frameIdx, ofTotalFrames); }},
        {"SubmitVideoCodingCmds",          [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return SubmitVideoCodingCmds(frame, frameIdx, ofTotalFrames); }},
    };

    if (!m_asyncAssemblyEnabled) {
        // The synchronous fallback assembles in line and publishes NO
        // CapturedBitstream, so a session that HAS a completion subscriber
        // would encode every frame correctly and report none of them. That is
        // exactly the defect this branch used to cause after a
        // DrainPendingFrames(); DrainAndRestartThreads() now brings the
        // workers back, so reaching here with a subscriber attached means
        // the restart did not happen. Fail loudly rather than drop frames
        // quietly -- the same treatment ProcessOutOfOrderFrames already gives
        // its own unpublishable branch, a few lines below.
        //
        // Deliberately scoped to "a subscriber exists". Without one there is
        // nothing to drop: the file-based CLI apps (--syncAssembly) retrieve
        // nothing and this branch remains their normal, correct path.
        if (HasCompletionSubscriber()) {
            VkEncPrintfErr("\nProcessOrderedFrames fell back to synchronous assembly "
                    "while a completion subscriber is registered: that path "
                    "publishes no completion record, so %u frame(s) would be "
                    "encoded and never reported. The assembly workers were "
                    "not restarted after a drain.\n",
                    numFrames);
            return VK_ERROR_UNKNOWN;
        }
        callbacks.push_back(
            {"AssembleBitstreamData", [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return AssembleBitstreamData(frame, frameIdx, ofTotalFrames); }}
        );
    }

    VkResult result = VK_SUCCESS;
    for (const auto& pair : callbacks) {
        const auto& callback = pair.second;

        uint32_t processedFramesCount = 0;
        result = VkVideoEncodeFrameInfo::ProcessFrames(this, frames, processedFramesCount, numFrames, callback);
        if (m_encoderConfig->verbose) {
            const std::string& description = pair.first;
            VkEncOut() << "====== Total number of frames processed by " << description << ": " << processedFramesCount << " : " << result << std::endl;
        }

        if (result != VK_SUCCESS) {
            break;
        }
    }

    if (result == VK_SUCCESS && m_asyncAssemblyEnabled) {
        result = QueueFramesForAssembly(frames, numFrames);
    }

    return result;
}

VkResult VkVideoEncoder::ProcessOutOfOrderFrames(VkSharedBaseObj<VkVideoEncodeFrameInfo>& frames, uint32_t numFrames) {

    // This path assembles synchronously and never queues for the assembly
    // workers, unlike ProcessOrderedFrames which chooses between the two on
    // m_asyncAssemblyEnabled. With async assembly ON, every frame reaching
    // here would be assembled off-thread of the capture queue and never
    // published -- lost to the consumer, silently.
    //
    // REACHABILITY. The dispatch gate is enableOutOfOrderRecording, NOT the
    // B-frame count: both call sites choose this function on
    // `m_encoderConfig->enableOutOfOrderRecording`, which only the CLI flag
    // --testOutOfOrderRecording sets (VkEncoderConfig.cpp) and which nothing
    // in the ext layer ever writes. So the file-based CLI reaches this today,
    // while the ext path -- the only one that registers a completion
    // subscriber -- cannot. consecutiveBFrames does not appear in this
    // decision at any point.
    //
    // Because it only ever assembles synchronously, this function must refuse
    // BOTH shapes in which a frame would be encoded and never reported: async
    // assembly on (immediately below) and a completion subscriber with async
    // off (after it). Fail loudly rather than drop frames quietly -- the same
    // treatment ProcessOrderedFrames gives its own unpublishable branch.
    if (m_asyncAssemblyEnabled) {
        VkEncPrintfErr("\nProcessOutOfOrderFrames reached with async assembly "
                "enabled: this path has no queue-for-assembly branch and "
                "would drop %u frame(s) without publishing them. Give it the "
                "ProcessOrderedFrames async branch before enabling B-frames "
                "on the ext path.\n",
                numFrames);
        return VK_ERROR_UNKNOWN;
    }

    // The mirror of ProcessOrderedFrames' subscriber guard, and the reason it
    // is needed HERE only became true when AssembleBitstreamData stopped
    // publishing: before that, a synchronous out-of-order assembly with a
    // subscriber attached would publish a record too EARLY -- ahead of the
    // EnqueuePendingFrame() that creates its PendingFrame -- so
    // DrainCapturesLocked() would discard it and count it in
    // m_lateCaptures: wrong, but LOUD. With the publish correctly absent
    // from the synchronous path, that same configuration encodes every
    // frame and reports none of them SILENTLY, which is strictly harder to
    // diagnose and is exactly the class ProcessOrderedFrames refuses.
    //
    // Unreachable from any CLI or ext configuration today, because the
    // subscriber and enableOutOfOrderRecording come from surfaces that never
    // overlap (see REACHABILITY above). It is a guard against a future
    // enablement -- wiring out-of-order recording or B-frames onto the ext
    // path.
    //
    // It is NOT untestable, and an earlier version of this comment claimed it
    // was. m_asyncAssemblyEnabled defaults false, SetOnBitstreamCaptured is
    // public, and this guard returns before anything touches a device -- so
    // test/encoder-sync-assembly pins it directly, device-free, in
    // CaseOutOfOrderSubscriberGuard. Neutralise the branch and the call falls
    // through into the callback sequence and answers
    // VK_ERROR_FEATURE_NOT_PRESENT instead of VK_ERROR_UNKNOWN, so the
    // coverage discriminates rather than merely executing the line.
    if (HasCompletionSubscriber()) {
        VkEncPrintfErr("\nProcessOutOfOrderFrames reached with a completion "
                "subscriber registered and async assembly off: this path "
                "assembles synchronously and publishes no completion record, "
                "so %u frame(s) would be encoded and never reported.\n",
                numFrames);
        return VK_ERROR_UNKNOWN;
    }

    const std::vector<std::pair<bool, std::function<VkResult(VkSharedBaseObj<VkVideoEncodeFrameInfo>&, uint32_t, uint32_t)>>> callbacksSeq = {
        {true,  [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return StartOfVideoCodingEncodeOrder(frame, frameIdx, ofTotalFrames); }},
        {true,  [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return ProcessDpb(frame, frameIdx, ofTotalFrames); }},
        {false, [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return RecordVideoCodingCmd(frame, frameIdx, ofTotalFrames); }},
        {true,  [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return SubmitVideoCodingCmds(frame, frameIdx, ofTotalFrames); }},
        {true,  [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return AssembleBitstreamData(frame, frameIdx, ofTotalFrames); }}
    };

    VkResult result = VK_SUCCESS;
    for (const auto& pair : callbacksSeq) {
        const auto& callback = pair.second;
        const bool inOrder = pair.first;

        if (inOrder) {
            uint32_t processedFramesCount = 0;
            result = VkVideoEncodeFrameInfo::ProcessFrames(this, frames, processedFramesCount, numFrames, callback);
            assert(processedFramesCount == numFrames);
        } else {
            uint32_t lastFramesIndex = numFrames;
            result = VkVideoEncodeFrameInfo::ProcessFramesReverse(this, frames, lastFramesIndex, numFrames, callback);
            assert(lastFramesIndex == 0);
        }

        if (result != VK_SUCCESS) {
            break;
        }
    }

    return result;
}

void VkVideoEncoder::DumpStateInfo(const char* stageName, uint32_t ident,
                                   VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                   int32_t frameIdx, uint32_t ofTotalFrames) const
{
    VkEncOut() << std::string(ident, ' ') << "===> "
              << VkVideoCoreProfile::CodecToName(m_encoderConfig->codec) << ": "
              << stageName << " [" <<  frameIdx << " of " << ofTotalFrames << "]"
              << " type " << VkVideoGopStructure::GetFrameTypeName(encodeFrameInfo->gopPosition.pictureType)
              << ", frameInputOrderNum: " << (uint32_t)encodeFrameInfo->frameEncodeInputOrderNum
              << ", frameEncodeOrderNum: " << (uint32_t)encodeFrameInfo->frameEncodeEncodeOrderNum
              << ", GOP input order: " << encodeFrameInfo->gopPosition.inputOrder
              << ", GOP encode  order: " << encodeFrameInfo->gopPosition.encodeOrder
              << " picOrderCntVal: " << encodeFrameInfo->picOrderCntVal
              << std::endl << std::flush;
}

bool VkVideoEncoder::WaitForThreadsToComplete()
{
    // The last deferred frame is encoded here, so its result belongs to this
    // drain as much as the workers do -- on a synchronous session it is the
    // only place a frame is processed at all.
    const VkResult pushResult = PushOrderedFrames();

    if (m_enableEncoderThreadQueue) {
        m_encoderThreadQueue.SetFlushAndExit();
        if (m_encoderQueueConsumerThread.joinable()) {
            m_encoderQueueConsumerThread.join();
        }
    }

    if (m_asyncAssemblyEnabled) {
        m_assemblyQueue.SetFlushAndExit();
        for (auto& t : m_assemblyThreads) {
            if (t.joinable()) t.join();
        }
        m_assemblyThreads.clear();
        m_asyncAssemblyEnabled = false;
        if (m_assemblyErrorCount > 0) {
            VkEncPrintfErr("[AsyncAssembly] Completed with %u errors\n",
                    m_assemblyErrorCount.load());
        }
    }

    // The verdict on the whole session, and the only one a caller that
    // watched none of the run can reach. Every arm is a frame that did not
    // become bitstream.
    return (pushResult == VK_SUCCESS) &&
           (m_assemblyErrorCount.load() == 0) &&
           (m_frameProcessingErrorCount.load() == 0);
}

// The NON-TERMINAL drain. See the header for why this exists separately from
// WaitForThreadsToComplete().
//
// The join above is exactly what makes the restart safe: on return from
// WaitForThreadsToComplete() every work item has been popped and has taken
// its ordering turn, no worker thread is alive, and the assembly queue is
// empty -- which is the precondition ClearFlushAndReuse() checks for.
//
// The ENCODER-QUEUE consumer thread is deliberately not restarted. It is
// joined by the same call and its queue carries the same sticky latch, but
// m_enableEncoderThreadQueue is false for every session in this tree, so
// there is no restart path here that could be tested. If it is ever enabled,
// this is where the second half belongs -- so this says so, loudly, rather
// than bringing half the pipeline back and leaving the caller to find out
// which half.
bool VkVideoEncoder::DrainAndRestartThreads()
{
    WaitForThreadsToComplete();

    if (m_enableEncoderThreadQueue) {
        VkEncPrintfErr("\nDrainAndRestartThreads with the encoder thread queue "
                "enabled: its consumer thread was joined and is NOT restarted "
                "by this path. Give it a restart before enabling "
                "m_enableEncoderThreadQueue.\n");
        return false;
    }

    return StartAssemblyThreads();
}

// Ext currency 3 (see the ext header): one library-owned timeline per
// session, GPU-signaled at queue flush points in SubmitVideoCodingCmds.
// Idempotent; runs on the session-serial thread before any submit.
VkResult VkVideoEncoder::CreateCompletionTimelineSemaphore()
{
    if (m_completionTimelineSemaphore != VK_NULL_HANDLE) {
        return VK_SUCCESS;
    }

    VkSemaphoreTypeCreateInfo timelineCreateInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCreateInfo.initialValue  = 0;

    // Exportability is a physical-device property: ask, never assume. The
    // query chain uses a SEPARATE type-info instance so the create chain's
    // pNext is not aliased. When the answer is no -- or the dispatch entry
    // is absent -- the semaphore is created plain and stays fully usable
    // in-process; only the export arm is refused, by type, at the ext
    // layer. OPAQUE_FD is the only export type this library ships (the
    // Win32 semaphore-export arm is reserved), and querying a handle-type
    // bit is portable Vulkan, so this file stays free of OS-specific code.
    VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    if (m_vkDevCtx->GetPhysicalDeviceExternalSemaphoreProperties != nullptr) {
        VkSemaphoreTypeCreateInfo queryTypeInfo = timelineCreateInfo;
        VkPhysicalDeviceExternalSemaphoreInfo extInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        extInfo.pNext      = &queryTypeInfo;
        extInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkExternalSemaphoreProperties extProps{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        m_vkDevCtx->GetPhysicalDeviceExternalSemaphoreProperties(
            m_vkDevCtx->getPhysicalDevice(), &extInfo, &extProps);
        if ((extProps.externalSemaphoreFeatures &
             VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0) {
            exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            timelineCreateInfo.pNext = &exportInfo;
            m_completionSemaphoreExportable = true;
        }
    }

    VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &timelineCreateInfo;
    VkResult result = m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &createInfo,
                                                  NULL,
                                                  &m_completionTimelineSemaphore);
    if (result != VK_SUCCESS) {
        m_completionTimelineSemaphore   = VK_NULL_HANDLE;
        m_completionSemaphoreExportable = false;
    }
    return result;
}

int32_t VkVideoEncoder::DeinitEncoder()
{
    // Join all worker threads before destroying resources.
    // Without this, the destructor destroys std::vector<std::thread>
    // with joinable threads → std::terminate.
    WaitForThreadsToComplete();

#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
    m_displayQueue.Flush();
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT
    m_lastDeferredFrame = nullptr;

    if (m_vkDevCtx)
        m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::ENCODE, 0);

    if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {
         m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, NULL);
         m_hwLoadBalancingTimelineSemaphore = VK_NULL_HANDLE;
    }

    // Completion timeline (ext currency 3): the same lifecycle discipline
    // as the load-balancing timeline above -- WaitForThreadsToComplete()
    // and the ENCODE queue wait-idle have already run, so no submitted
    // batch can still reference it
    // (VUID-vkDestroySemaphore-semaphore-01137).
    if (m_completionTimelineSemaphore != VK_NULL_HANDLE) {
         m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_completionTimelineSemaphore, NULL);
         m_completionTimelineSemaphore = VK_NULL_HANDLE;
    }
    m_completionSemaphoreExportable = false;
    m_maxSubmittedCompletionValue   = 0;
    m_lastSignaledCompletionValue   = 0;

    m_linearInputImagePool    = nullptr;
    m_inputImagePool          = nullptr;
    m_dpbImagePool            = nullptr;

#ifdef NV_AQ_GPU_LIB_SUPPORTED
    m_aqAnalyzes =  nullptr;
    m_inputSubsampledImagePool = nullptr;
#endif // NV_AQ_GPU_LIB_SUPPORTED
    m_qpMapImagePool          = nullptr;
    if (m_encoderConfig && m_encoderConfig->IsPsnrMetricsEnabled() && m_psnr) {
        if (m_psnr->Enabled()) {
            const double psnrY = m_psnr->GetAveragePsnrY();
            const double psnrU = m_psnr->GetAveragePsnrU();
            const double psnrV = m_psnr->GetAveragePsnrV();
            VkEncPrintfOut("Average PSNR (dB): Y=%.2f", psnrY);
            if (psnrU >= 0.0) {
                VkEncPrintfOut(" U=%.2f", psnrU);
            }
            if (psnrV >= 0.0) {
                VkEncPrintfOut(" V=%.2f", psnrV);
            }
            VkEncPrintfOut("\n");
            fflush(stdout);
        } else {
            VkEncPrintfErr("PSNR was requested (--psnr) but metrics are unavailable (initialization may have failed).\n");
            fflush(stderr);
        }
    }
    if (m_contentProbe) {
        m_contentProbe->Deinit();
    }

    if (m_psnr) {
        m_psnr->Deinit();
    }
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    m_inputComputeFilter      = nullptr;
#endif
    m_inputCommandBufferPool  = nullptr;
    m_encodeCommandBufferPool = nullptr;

    m_videoSessionParameters =  nullptr;
    m_videoSession = nullptr;

    m_crc.Deinit();

    m_encoderConfig = nullptr;

    return 0;
}

void VkVideoEncoder::ConsumerThread()
{
    vkenc::OsSetCurrentThreadName("VkEncConsumer");

   VkEncOut() << "ConsumerThread is stating now.\n" << std::endl;
   do {
       VkSharedBaseObj<VkVideoEncodeFrameInfo> encodeFrameInfo;
       bool success = m_encoderThreadQueue.WaitAndPop(encodeFrameInfo);
       if (success) { // 5 seconds in nanoseconds
           VkEncOut() << "==>>>> Consumed: " << (uint32_t)encodeFrameInfo->gopPosition.inputOrder
                      << ", Order: " << (uint32_t)encodeFrameInfo->gopPosition.encodeOrder << std::endl << std::flush;

           VkResult result;
           if (!m_encoderConfig->enableOutOfOrderRecording) {
               result = ProcessOrderedFrames(encodeFrameInfo, 0);
           } else {
               // Testing only - don't use for production!
               result = ProcessOutOfOrderFrames(encodeFrameInfo, 0);
           }
           // Only the frames the assembly workers did not take are still
           // here, and those are the encoder's to release.
           VkVideoEncodeFrameInfo::ResetAndReleaseFrames(encodeFrameInfo);
           assert(encodeFrameInfo == nullptr);
           if (result != VK_SUCCESS) {
               VkEncOut() << "Error processing frames from the frame thread!" << std::endl;
               m_frameProcessingErrorCount++;
               m_encoderThreadQueue.SetFlushAndExit();
           }

       } else {
           bool shouldExit = m_encoderThreadQueue.ExitQueue();
           VkEncOut() << "Thread should exit: " << (shouldExit ? "Yes" : "No") << std::endl;
       }
   } while (!m_encoderThreadQueue.ExitQueue());

   VkEncOut() << "ConsumerThread is exiting now.\n" << std::endl;
}

size_t VkVideoEncoder::WriteDataToFile(const uint8_t* data, size_t size)
{
    if (!data || size == 0) {
        return 0;
    }

    if (m_crc.Enabled()) {
        m_crc.UpdateCrc(data, size);
    }
    // Skip the fwrite when disableFileOutput is set.
    // WriteBitstreamToFile captures the bytes into
    // m_capturedBitstreams separately. Returning size signals
    // success to the existing callers.
    if (m_encoderConfig && m_encoderConfig->disableFileOutput) {
        return size;
    }
    size_t bytesWritten = fwrite(data, 1, size, m_encoderConfig->outputFileHandler.GetFileHandle());
    return bytesWritten;
}

// Pop the oldest completion record from the in-memory FIFO populated by
// PushCapturedBitstream whenever a drain-capable consumer exists (the
// encoded bytes are carried only in capture mode). Returns true if a
// record was popped, false if the queue is empty. Used by
// VulkanVideoEncoderExtImpl to route the records into the matching
// PendingFrame entries.
bool VkVideoEncoder::TryPopCapturedBitstream(
    uint64_t* out_frame_id, std::vector<uint8_t>* out_bytes,
    bool* out_is_idr, uint32_t* out_picture_type,
    VkResult* out_status)
{
    std::lock_guard<std::mutex> lock(m_capturedBitstreamsMutex);
    if (m_capturedBitstreams.empty()) {
        return false;
    }
    CapturedBitstream& front = m_capturedBitstreams.front();
    if (out_frame_id)     *out_frame_id     = front.frameId;
    if (out_bytes)        *out_bytes        = std::move(front.bytes);
    if (out_is_idr)       *out_is_idr       = front.isIdr;
    if (out_picture_type) *out_picture_type = front.pictureType;
    if (out_status)       *out_status       = front.status;
    m_capturedBitstreams.pop_front();
    return true;
}

size_t VkVideoEncoder::GetCrcValues(uint32_t* pCrcValues, size_t buffSize) const
{
    return m_crc.GetCrcValues(pCrcValues, buffSize);
}

double VkVideoEncoder::GetAveragePsnr() const
{
    return (m_psnr && m_psnr->Enabled()) ? m_psnr->GetAveragePsnrY() : -1.0;
}

double VkVideoEncoder::GetAveragePsnrU() const
{
    return (m_psnr && m_psnr->Enabled()) ? m_psnr->GetAveragePsnrU() : -1.0;
}

double VkVideoEncoder::GetAveragePsnrV() const
{
    return (m_psnr && m_psnr->Enabled()) ? m_psnr->GetAveragePsnrV() : -1.0;
}
