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
#include <vector>
#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkVideoCore/VulkanVideoCapabilities.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"
#include "VkVideoEncoder/VkEncoderConfigH264.h"
#include "VkVideoEncoder/VkEncoderConfigH265.h"
#include "VkVideoEncoder/VkEncoderConfigAV1.h"
#include "VkCodecUtils/YCbCrConvUtilsCpu.h"

static size_t getFormatTexelSize(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_UNORM:
        return 1;
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
        return 2;
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
        return 4;
    default:
        assert(!"unknown format");
        return 0;
    }
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

        size_t formatSize = getFormatTexelSize(m_imageQpMapFormat);
        uint32_t inputQpMapWidth = (m_encoderConfig->input.width + m_qpMapTexelSize.width - 1) / m_qpMapTexelSize.width;
        uint32_t qpMapWidth = (m_encoderConfig->encodeWidth + m_qpMapTexelSize.width - 1) / m_qpMapTexelSize.width;
        uint32_t qpMapHeight = (m_encoderConfig->encodeHeight + m_qpMapTexelSize.height - 1) / m_qpMapTexelSize.height;
        uint64_t qpMapFileOffset = qpMapWidth * qpMapHeight * encodeFrameInfo->frameInputOrderNum * formatSize;
        const uint8_t* pQpMapData = m_encoderConfig->qpMapFileHandler.GetMappedPtr(qpMapFileOffset);

        const VkSubresourceLayout* dstQpMapSubresourceLayout = dstQpMapImageResource->GetSubresourceLayout();

        for (uint32_t j = 0; j < qpMapHeight; j++) {
            memcpy(writeQpMapImagePtr + (dstQpMapSubresourceLayout[0].offset + j * dstQpMapSubresourceLayout[0].rowPitch),
                   pQpMapData + j * inputQpMapWidth * formatSize, qpMapWidth * formatSize);
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

    const uint8_t* pInputFrameData = m_encoderConfig->inputFileHandler.GetMappedPtr(m_encoderConfig->input.fullImageSize, encodeFrameInfo->frameInputOrderNum);

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

    // and encode the input frame with the encoder next
    return EncodeFrame(encodeFrameInfo);
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

    m_inputCommandBufferPool->GetAvailablePoolNode(encodeFrameInfo->inputCmdBuffer);
    assert(encodeFrameInfo->inputCmdBuffer != nullptr);

    // Make sure command buffer is not in use anymore and reset
    encodeFrameInfo->inputCmdBuffer->ResetCommandBuffer(true, "encoderStagedInputFence");

    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cmdBuf = encodeFrameInfo->inputCmdBuffer->BeginCommandBufferRecording(beginInfo);

    VkSharedBaseObj<VkImageResourceView> linearInputImageView;
    encodeFrameInfo->srcStagingImageView->GetImageView(linearInputImageView);

    VkSharedBaseObj<VkImageResourceView> srcEncodeImageView;
    encodeFrameInfo->srcEncodeImageResource->GetImageView(srcEncodeImageView);

    VkExtent2D copyImageExtent {
        std::min(m_encoderConfig->encodeWidth,  m_encoderConfig->input.width),
        std::min(m_encoderConfig->encodeHeight, m_encoderConfig->input.height)
    };

    VkResult result;
    if (m_inputComputeFilter == nullptr) {
        VkImageLayout linearImgNewLayout = TransitionImageLayout(cmdBuf, linearInputImageView, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageLayout srcImgNewLayout = TransitionImageLayout(cmdBuf, srcEncodeImageView, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        (void)linearImgNewLayout;
        (void)srcImgNewLayout;

        CopyLinearToOptimalImage(cmdBuf, linearInputImageView, srcEncodeImageView, copyImageExtent);

    } else {

        VkVideoPictureResourceInfoKHR srcPictureResourceInfo(*encodeFrameInfo->srcStagingImageView->GetPictureResourceInfo());
        VkVideoPictureResourceInfoKHR dstPictureResourceInfo(*encodeFrameInfo->srcEncodeImageResource->GetPictureResourceInfo());

        srcPictureResourceInfo.codedExtent = copyImageExtent;

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
        result = m_inputComputeFilter->RecordCommandBuffer(cmdBuf,
                                                           linearInputImageView,
                                                           &srcPictureResourceInfo,
                                                           srcEncodeImageView,
                                                           &dstPictureResourceInfo,
                                                           encodeFrameInfo->inputCmdBuffer->GetNodePoolIndex());
        if (result != VK_SUCCESS) {
            return result;
        }
    }

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

    // Now submit the staged input to the queue
    SubmitStagedInputFrame(encodeFrameInfo);

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

    encodeFrameInfo->qpMapCmdBuffer->SetCommandBufferSubmitted();
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

    // Determine bit depth and bytes per pixel from format
    const uint32_t bitDepth = (formatInfo != nullptr) ? GetBitsPerChannel(formatInfo->planesLayout) : 8; // Default to 8-bit
    const uint32_t bytesPerPixel = (bitDepth > 8) ? 2 : 1;

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
        printf("YCbCr copy with %s subsampling (chromaHorzRatio=%d, chromaVertRatio=%d), %d-bit\n",
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

    VkFence queueCompleteFence = encodeFrameInfo->inputCmdBuffer->GetFence();
    assert(VK_NOT_READY == m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, queueCompleteFence));
    const VulkanDeviceContext::QueueFamilySubmitType submitType =
            (m_inputComputeFilter != nullptr) ? VulkanDeviceContext::COMPUTE :
                    (((m_vkDevCtx->GetVideoEncodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) != 0) ?
                            VulkanDeviceContext::ENCODE : VulkanDeviceContext::TRANSFER);

    VkResult result = m_vkDevCtx->MultiThreadedQueueSubmit(submitType,
                                                           0, // queueIndex
                                                           1, // submitCount
                                                           &submitInfo,
                                                           queueCompleteFence,
                                                           "Encode Staging Input",
                                                           m_encodeEncodeFrameNum,
                                                           m_encodeInputFrameNum);

    encodeFrameInfo->inputCmdBuffer->SetCommandBufferSubmitted();
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

    assert(encodeFrameInfo->outputBitstreamBuffer != nullptr);
    assert(encodeFrameInfo->encodeCmdBuffer != nullptr);

    if(encodeFrameInfo->bitstreamHeaderBufferSize > 0) {
        size_t nonVcl = fwrite(encodeFrameInfo->bitstreamHeaderBuffer + encodeFrameInfo->bitstreamHeaderOffset,
               1, encodeFrameInfo->bitstreamHeaderBufferSize,
               m_encoderConfig->outputFileHandler.GetFileHandle());

        if (m_encoderConfig->verboseFrameStruct) {
            std::cout << "       == Non-Vcl data " << (nonVcl ? "SUCCESS" : "FAIL")
                      << " File Output non-VCL data with size: " << encodeFrameInfo->bitstreamHeaderBufferSize
                      << ", Input Order: " << encodeFrameInfo->gopPosition.inputOrder
                      << ", Encode  Order: " << encodeFrameInfo->gopPosition.encodeOrder
                      << std::endl << std::flush;
        }
    }

    VkResult result = encodeFrameInfo->encodeCmdBuffer->SyncHostOnCmdBuffComplete(false, "encoderEncodeFence");
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nWait on encoder complete fence has failed with result 0x%x.\n", result);
        return result;
    }

    uint32_t querySlotId = (uint32_t)-1;
    VkQueryPool queryPool = encodeFrameInfo->encodeCmdBuffer->GetQueryPool(querySlotId);

    // Since we can use a single command buffer from multiple frames,
    // we can't just use the querySlotId from the command buffer.
    // Instead we use the input image index that should be unique for each frame.
    querySlotId = (uint32_t)encodeFrameInfo->srcEncodeImageResource->GetImageIndex();

    // get output results
    struct VulkanVideoEncodeStatus {
        uint32_t bitstreamStartOffset;
        uint32_t bitstreamSize;
        VkQueryResultStatusKHR status;
    } encodeResult{};

    // Fetch the coded VCL data and its information
    result = m_vkDevCtx->GetQueryPoolResults(*m_vkDevCtx, queryPool, querySlotId,
                                             1, sizeof(encodeResult), &encodeResult, sizeof(encodeResult),
                                             VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);


    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nRetrieveData Error: Failed to get vcl query pool results.\n");
        assert(result == VK_SUCCESS);
        return result;
    }

    if (encodeResult.status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR) {
        fprintf(stderr, "\nencodeResult.status is (0x%x) NOT STATUS_COMPLETE! bitstreamStartOffset %u, bitstreamSize %u\n",
                encodeResult.status, encodeResult.bitstreamStartOffset, encodeResult.bitstreamSize);
        assert(encodeResult.status == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);
        return VK_INCOMPLETE;
    }

    VkDeviceSize maxSize;
    uint8_t* data = encodeFrameInfo->outputBitstreamBuffer->GetDataPtr(0, maxSize);

    size_t totalBytesWritten = 0;
    while (totalBytesWritten < encodeResult.bitstreamSize) { // handle partial writes
        size_t remainingBytes = encodeResult.bitstreamSize - totalBytesWritten;
        size_t bytesWritten = fwrite(data + encodeResult.bitstreamStartOffset + totalBytesWritten, 1, remainingBytes,
                                    m_encoderConfig->outputFileHandler.GetFileHandle());
        if (bytesWritten == 0) {
            std::cerr << "Error writing VCL data" << std::endl;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        totalBytesWritten += bytesWritten;
    }

    if (m_encoderConfig->verboseFrameStruct) {
        std::cout << "       == Output VCL data " << ((totalBytesWritten == encodeResult.bitstreamSize) ? "SUCCESS" : "FAIL") << " with size: " << encodeResult.bitstreamSize
                  << " and offset: " << encodeResult.bitstreamStartOffset
                  << ", Input Order: " << encodeFrameInfo->gopPosition.inputOrder
                  << ", Encode  Order: " << encodeFrameInfo->gopPosition.encodeOrder << std::endl << std::flush;
    }
    return result;
}

VkResult VkVideoEncoder::InitEncoder(VkSharedBaseObj<EncoderConfig>& encoderConfig)
{

    if (!VulkanVideoCapabilities::IsCodecTypeSupported(m_vkDevCtx,
                                                       m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                                       encoderConfig->codec)) {
        std::cout << "*** The video codec " << VkVideoCoreProfile::CodecToName(encoderConfig->codec) << " is not supported! ***" << std::endl;
        assert(!"The video codec is not supported");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    m_encoderConfig = encoderConfig;

    // Update the video profile
    encoderConfig->InitVideoProfile();

    encoderConfig->InitDeviceCapabilities(m_vkDevCtx);

    if (encoderConfig->qualityLevel >= encoderConfig->videoEncodeCapabilities.maxQualityLevels) {
        std::cerr << "Quality level " << encoderConfig->qualityLevel
                  << " is greater than the maximum supported quality level "
                  << (encoderConfig->videoEncodeCapabilities.maxQualityLevels - 1) << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (encoderConfig->useDpbArray == false &&
        (encoderConfig->videoCapabilities.flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR) == 0) {
        std::cout << "Separate DPB was requested, but the implementation does not support it!" << std::endl;
        std::cout << "Fallback to layered DPB!" << std::endl;
        encoderConfig->useDpbArray = true;
    }

    if (m_encoderConfig->enableQpMap) {
        if ((m_encoderConfig->qpMapMode == EncoderConfig::DELTA_QP_MAP) &&
            ((m_encoderConfig->videoEncodeCapabilities.flags & VK_VIDEO_ENCODE_CAPABILITY_QUANTIZATION_DELTA_MAP_BIT_KHR) == 0)) {
                std::cout << "Delta QP Map was requested, but the implementation does not support it!" << std::endl;
                assert(!"Delta QP Map is not supported");
                return VK_ERROR_INITIALIZATION_FAILED;
        }
        if ((m_encoderConfig->qpMapMode == EncoderConfig::EMPHASIS_MAP) &&
            ((m_encoderConfig->videoEncodeCapabilities.flags & VK_VIDEO_ENCODE_CAPABILITY_EMPHASIS_MAP_BIT_KHR) == 0)) {
                std::cout << "Emphasis Map was requested, but the implementation does not support it!" << std::endl;
                assert(!"Emphasis QP Map is not supported");
                return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    // Reconfigure the gopStructure structure because the device may not support
    // specific GOP structure. For example it may not support B-frames.
    // gopStructure.Init() should be called after  encoderConfig->InitDeviceCapabilities().
    m_encoderConfig->gopStructure.Init(m_encoderConfig->numFrames);
    if (encoderConfig->GetMaxBFrameCount() < m_encoderConfig->gopStructure.GetConsecutiveBFrameCount()) {
        if (m_encoderConfig->verbose) {
            std::cout << "Max consecutive B frames: " << (uint32_t)encoderConfig->GetMaxBFrameCount() << " lower than the configured one: " << (uint32_t)m_encoderConfig->gopStructure.GetConsecutiveBFrameCount() << std::endl;
            std::cout << "Fallback to the max value: " << (uint32_t)m_encoderConfig->gopStructure.GetConsecutiveBFrameCount() << std::endl;
        }
        m_encoderConfig->gopStructure.SetConsecutiveBFrameCount(encoderConfig->GetMaxBFrameCount());
    }
    if (m_encoderConfig->verbose) {
        std::cout << std::endl << "GOP frame count: " << (uint32_t)m_encoderConfig->gopStructure.GetGopFrameCount();
        std::cout << ", IDR period: " << (uint32_t)m_encoderConfig->gopStructure.GetIdrPeriod();
        std::cout << ", Consecutive B frames: " << (uint32_t)m_encoderConfig->gopStructure.GetConsecutiveBFrameCount();
        std::cout << std::endl;

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

    // The required num of DPB images
    m_maxDpbPicturesCount = encoderConfig->InitDpbCount();

    encoderConfig->InitRateControl();

    VkFormat supportedDpbFormats[8];
    VkFormat supportedInFormats[8];
    uint32_t formatCount = sizeof(supportedDpbFormats) / sizeof(supportedDpbFormats[0]);
    VkResult result = VulkanVideoCapabilities::GetVideoFormats(m_vkDevCtx, encoderConfig->videoCoreProfile,
                                                               VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
                                                               formatCount, supportedDpbFormats);

    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to get desired video format for the DPB.\n");
        return result;
    }

    result = VulkanVideoCapabilities::GetVideoFormats(m_vkDevCtx, encoderConfig->videoCoreProfile,
                                                      VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
                                                      formatCount, supportedInFormats);

    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to get desired video format for input images.\n");
        return result;
    }

    m_imageDpbFormat = supportedDpbFormats[0];
    m_imageInFormat = supportedInFormats[0];

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
            fprintf(stderr, "\nInitEncoder Error: Failed to get desired video format for qpMap images.\n");
            return result;
        }

        m_imageQpMapFormat = supportedQpMapFormats[0];
        m_qpMapTexelSize = supportedQpMapTexelSize[0];
        m_qpMapTiling = supportedQpMapTiling[0];
    }

    encoderConfig->encodeWidth  = std::max(encoderConfig->encodeWidth,  encoderConfig->videoCapabilities.minCodedExtent.width);
    encoderConfig->encodeHeight = std::max(encoderConfig->encodeHeight, encoderConfig->videoCapabilities.minCodedExtent.height);

    encoderConfig->encodeWidth  = std::min(encoderConfig->encodeWidth,  encoderConfig->videoCapabilities.maxCodedExtent.width);
    encoderConfig->encodeHeight = std::min(encoderConfig->encodeHeight, encoderConfig->videoCapabilities.maxCodedExtent.height);

    m_maxCodedExtent = { encoderConfig->encodeMaxWidth, encoderConfig->encodeMaxHeight }; // max coded size
    m_streamBufferSize = std::max(m_minStreamBufferSize, (size_t)encoderConfig->input.fullImageSize); // use worst case size

    encoderConfig->encodeAlignedWidth  = vk::alignedSize (encoderConfig->encodeWidth, encoderConfig->videoCapabilities.pictureAccessGranularity.width);
    encoderConfig->encodeAlignedHeight = vk::alignedSize (encoderConfig->encodeHeight, encoderConfig->videoCapabilities.pictureAccessGranularity.height);

    const uint32_t maxActiveReferencePicturesCount = encoderConfig->videoCapabilities.maxActiveReferencePictures;
    const uint32_t maxDpbPicturesCount = std::min<uint32_t>(m_maxDpbPicturesCount, encoderConfig->videoCapabilities.maxDpbSlots);

    VkVideoSessionCreateFlagsKHR sessionCreateFlags{};
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
                                             m_videoSession);

        // after creating a new video session, we need a codec reset.
        m_resetEncoder = true;
        assert(result == VK_SUCCESS);
    }



    const VkImageUsageFlags inImageUsage = ( VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const VkImageUsageFlags dpbImageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;

    // NOTE: Create linearInputImage
    result =  VulkanVideoImagePool::Create(m_vkDevCtx, m_linearInputImagePool);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to create linearInputImagePool.\n");
        return result;
    }

    VkExtent2D linearInputImageExtent {
        std::max(m_maxCodedExtent.width,  encoderConfig->input.width),
        std::max(m_maxCodedExtent.height, encoderConfig->input.height)
    };

    result = m_linearInputImagePool->Configure( m_vkDevCtx,
                                                encoderConfig->numInputImages,
                                                encoderConfig->input.vkFormat,
                                                linearInputImageExtent,
                                                  ( VK_IMAGE_USAGE_SAMPLED_BIT |
                                                    VK_IMAGE_USAGE_STORAGE_BIT |
                                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
                                                m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                                  ( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
                                                nullptr, // pVideoProfile
                                                false,   // useImageArray
                                                false,   // useImageViewArray
                                                true     // useLinear
                                              );
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to Configure linearInputImagePool.\n");
        return result;
    }

    result =  VulkanVideoImagePool::Create(m_vkDevCtx, m_inputImagePool);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to create inputImagePool.\n");
        return result;
    }

    VkExtent2D imageExtent {
        std::max(m_maxCodedExtent.width, encoderConfig->videoCapabilities.minCodedExtent.width),
        std::max(m_maxCodedExtent.height, encoderConfig->videoCapabilities.minCodedExtent.height)
    };

    result = m_inputImagePool->Configure( m_vkDevCtx,
                                          encoderConfig->numInputImages,
                                          m_imageInFormat,
                                          imageExtent,
                                          inImageUsage,
                                          m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          encoderConfig->videoCoreProfile.GetProfile(), // pVideoProfile
                                          false,   // useImageArray
                                          false,   // useImageViewArray
                                          false    // useLinear
                                          );
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to Configure inputImagePool.\n");
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
            std::cout << "\t WARNING: Enabling HW Load Balancing for a device with only " <<
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
        std::cout << "\t Enabling HW Load Balancing for device with "
                  << m_vkDevCtx->GetVideoEncodeNumQueues() << " queues" << std::endl;
    }

    if (encoderConfig->enableQpMap) {

        if (m_qpMapTiling != VK_IMAGE_TILING_LINEAR) {

            // If the linear tiling is not supported, we need to stage the image
            result =  VulkanVideoImagePool::Create(m_vkDevCtx, m_linearQpMapImagePool);
            if(result != VK_SUCCESS) {
                fprintf(stderr, "\nInitEncoder Error: Failed to create linearQpMapImagePool.\n");
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
                                                        false,   // useImageArray
                                                        false,   // useImageViewArray
                                                        true     // useLinear
                                                      );
            if(result != VK_SUCCESS) {
                fprintf(stderr, "\nInitEncoder Error: Failed to Configure linearQpMapImagePool.\n");
                return result;
            }
        }
        result =  VulkanVideoImagePool::Create(m_vkDevCtx, m_qpMapImagePool);
        if(result != VK_SUCCESS) {
            fprintf(stderr, "\nInitEncoder Error: Failed to create inputImagePool.\n");
            return result;
        }

        VkExtent2D qpMapExtent {
            (std::max(m_maxCodedExtent.width, encoderConfig->videoCapabilities.minCodedExtent.width) + m_qpMapTexelSize.width - 1) / m_qpMapTexelSize.width,
            (std::max(m_maxCodedExtent.height, encoderConfig->videoCapabilities.minCodedExtent.height) + m_qpMapTexelSize.height - 1) / m_qpMapTexelSize.height
        };

        const VkImageUsageFlags qpMapImageUsage = (((encoderConfig->qpMapMode == EncoderConfig::DELTA_QP_MAP) ?
                                                    VK_IMAGE_USAGE_VIDEO_ENCODE_QUANTIZATION_DELTA_MAP_BIT_KHR :
                                                    VK_IMAGE_USAGE_VIDEO_ENCODE_EMPHASIS_MAP_BIT_KHR) |
                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT);

        result = m_qpMapImagePool->Configure( m_vkDevCtx,
                                              encoderConfig->numInputImages,
                                              m_imageQpMapFormat,
                                              qpMapExtent,
                                              qpMapImageUsage,
                                              m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                              (m_qpMapTiling != VK_IMAGE_TILING_LINEAR) ?
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT :
                                                    ( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
                                              encoderConfig->videoCoreProfile.GetProfile(), // pVideoProfile
                                              false,   // useImageArray
                                              false,   // useImageViewArray
                                              m_qpMapTiling == VK_IMAGE_TILING_LINEAR   // useLinear
                                            );
        if(result != VK_SUCCESS) {
            fprintf(stderr, "\nInitEncoder Error: Failed to Configure qpMapImagePool.\n");
            return result;
        }
    }

    result =  VulkanVideoImagePool::Create(m_vkDevCtx, m_dpbImagePool);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to create dpbImagePool.\n");
        return result;
    }

    uint32_t numEncodeImagesInFlight = std::max<uint32_t>(m_holdRefFramesInQueue + m_holdRefFramesInQueue * m_encoderConfig->gopStructure.GetConsecutiveBFrameCount(), 4);
    result = m_dpbImagePool->Configure(m_vkDevCtx,
                                       std::max<uint32_t>(maxDpbPicturesCount, maxActiveReferencePicturesCount) + numEncodeImagesInFlight,
                                       m_imageDpbFormat,
                                       imageExtent,
                                       dpbImageUsage,
                                       m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                       encoderConfig->videoCoreProfile.GetProfile(), // pVideoProfile
                                       encoderConfig->useDpbArray,                   // useImageArray
                                       false,   // useImageViewArrays
                                       false    // useLinear
                                      );
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to Configure inputImagePool.\n");
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
                fprintf(stderr, "\nERROR: VulkanBitstreamBufferImpl::Create() result: 0x%x\n", result);
                break;
            }

            int32_t nodeAddedWithIndex = m_bitstreamBuffersQueue.AddNodeToPool(bitstreamBuffer, false);
            if (nodeAddedWithIndex < 0) {
                assert("Could not add the new node to the pool");
                break;
            }
        }
    }

    if (encoderConfig->enablePreprocessComputeFilter) {

        const VkSamplerYcbcrRange ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL; // FIXME
        const VkSamplerYcbcrModelConversion ycbcrModelConversion = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;   // FIXME
        const YcbcrPrimariesConstants ycbcrPrimariesConstants = GetYcbcrPrimariesConstants(YcbcrBtStandardBt2020); // FIXME

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

        result = VulkanFilterYuvCompute::Create(m_vkDevCtx,
                                                m_vkDevCtx->GetComputeQueueFamilyIdx(),
                                                0, // queueIndex
                                                encoderConfig->filterType,
                                                encoderConfig->numInputImages,
                                                encoderConfig->input.vkFormat,  // in filter format (can be RGB)
                                                m_imageInFormat,  // out filter - same as input for now.
                                                false, // inputEnableMsbToLsbShift
                                                (encoderConfig->input.msbShift > 0),
                                                &ycbcrConversionCreateInfo,
                                                &ycbcrPrimariesConstants,
                                                &samplerInfo,
                                                m_inputComputeFilter);
    }

    if ((result == VK_SUCCESS) && (m_inputComputeFilter != nullptr) ) {

        m_inputCommandBufferPool = m_inputComputeFilter;

    } else {

        result = VulkanCommandBufferPool::Create(m_vkDevCtx, m_inputCommandBufferPool);
        if(result != VK_SUCCESS) {
            fprintf(stderr, "\nInitEncoder Error: Failed to create m_inputCommandBufferPool.\n");
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
        fprintf(stderr, "\nInitEncoder Error: Failed to Configure m_inputCommandBufferPool.\n");
        return result;
    }

    result = VulkanCommandBufferPool::Create(m_vkDevCtx, m_encodeCommandBufferPool);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to create m_encodeCommandBufferPool.\n");
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
        fprintf(stderr, "\nInitEncoder Error: Failed to Configure m_encodeCommandBufferPool.\n");
        return result;
    }

    result = CreateFrameInfoBuffersQueue(encoderConfig->numInputImages);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to create FrameInfoBuffersQueue.\n");
        return result;
    }

    // Start the queue consumer thread
    if (m_enableEncoderThreadQueue) {

        const uint32_t maxPendingQueueNodes = 2;
        m_encoderThreadQueue.SetMaxPendingQueueNodes(std::min<uint32_t>(m_encoderConfig->gopStructure.GetGopFrameCount() + 1, maxPendingQueueNodes));
        m_encoderQueueConsumerThread = std::thread(&VkVideoEncoder::ConsumerThread, this);
    }

    return VK_SUCCESS;
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
            fprintf(stderr, "\nERROR: VulkanBitstreamBufferImpl::Create() result: 0x%x\n", result);
            return 0;
        }
        if (debugBitstreamBufferDumpAlloc) {
            std::cout << "\tAllocated bitstream buffer with size " << newSize << " B, " <<
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
            std::cout << "\t\tFrom bitstream buffer pool with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;

            std::cout << "\t\t\t FreeNodes " << m_bitstreamBuffersQueue.GetFreeNodesNumber();
            std::cout << " of MaxNodes " << m_bitstreamBuffersQueue.GetMaxNodes();
            std::cout << ", AvailableNodes " << m_bitstreamBuffersQueue.GetAvailableNodesNumber();
            std::cout << std::endl;
        }
    }
    bitstreamBuffer = newBitstreamBuffer;
    if (newSize > m_streamBufferSize) {
        std::cout << "\tAllocated bitstream buffer with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;
        m_streamBufferSize = (size_t)newSize;
    }
    return bitstreamBuffer->GetMaxSize();
}

VkImageLayout VkVideoEncoder::TransitionImageLayout(VkCommandBuffer cmdBuf,
                                                    VkSharedBaseObj<VkImageResourceView>& imageView,
                                                    VkImageLayout oldLayout, VkImageLayout newLayout)
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
            VK_QUEUE_FAMILY_IGNORED, // uint32_t        srcQueueFamilyIndex
            VK_QUEUE_FAMILY_IGNORED, // uint32_t   dstQueueFamilyIndex
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
    } else if ((oldLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR) && (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR)) {
        imageBarrier.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
        imageBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    } else {
#ifdef __cpp_exceptions
        throw std::invalid_argument("unsupported layout transition!");
#endif
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

    return newLayout;
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
        VkMemoryBarrier memoryBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        m_vkDevCtx->CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
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
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        m_vkDevCtx->CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
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

VkResult VkVideoEncoder::HandleCtrlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
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

        encodeFrameInfo->rateControlInfo.pLayers = encodeFrameInfo->rateControlLayersInfo;
        encodeFrameInfo->rateControlInfo.layerCount = 1;
        m_beginRateControlInfo = encodeFrameInfo->rateControlInfo;

        if (pNext != nullptr) {
            vk::ChainNextVkStruct(encodeFrameInfo->rateControlInfo, *pNext);
        }

        pNext = (VkBaseInStructure*)&encodeFrameInfo->rateControlInfo;
    }

    encodeFrameInfo->pControlCmdChain = pNext;

    return VK_SUCCESS;
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

    // Handle the query indexes
    uint32_t querySlotId = (uint32_t)-1;
    VkQueryPool queryPool = encodeCmdBuffer->GetQueryPool(querySlotId);

    // Since we can use a single command buffer from multiple frames,
    // we can't just use the querySlotId from the command buffer.
    // Instead we use the input image index that should be unique for each frame.
    querySlotId = (uint32_t)encodeFrameInfo->srcEncodeImageResource->GetImageIndex();

    // Clear the query results
    const uint32_t numQuerySamples = 1;
    vkDevCtx->CmdResetQueryPool(cmdBuf, queryPool, querySlotId, numQuerySamples);

    if (encodeFrameInfo->controlCmd != VkVideoCodingControlFlagsKHR())
    {
        m_beginRateControlInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR, NULL};
    }

    encodeBeginInfo.pNext = &m_beginRateControlInfo;

    vkDevCtx->CmdBeginVideoCodingKHR(cmdBuf, &encodeBeginInfo);

    if (encodeFrameInfo->controlCmd != VkVideoCodingControlFlagsKHR()) {

        VkVideoCodingControlInfoKHR renderControlInfo = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
                                                          encodeFrameInfo->pControlCmdChain,
                                                          encodeFrameInfo->controlCmd};
        vkDevCtx->CmdControlVideoCodingKHR(cmdBuf, &renderControlInfo);

        m_beginRateControlInfo = *(VkVideoEncodeRateControlInfoKHR*)encodeFrameInfo->pControlCmdChain;
        // Do not walk the chain, otherwise we end up creating a loop here.
        m_beginRateControlInfo.pNext = (VkBaseInStructure*)(&encodeFrameInfo->pControlCmdChain);
    }

    if (m_videoMaintenance1FeaturesSupported)
    {
        VkVideoInlineQueryInfoKHR videoInlineQueryInfoKHR;
        videoInlineQueryInfoKHR.pNext = NULL;
        videoInlineQueryInfoKHR.sType = VK_STRUCTURE_TYPE_VIDEO_INLINE_QUERY_INFO_KHR;
        videoInlineQueryInfoKHR.queryPool = queryPool;
        videoInlineQueryInfoKHR.firstQuery = querySlotId;
        videoInlineQueryInfoKHR.queryCount = numQuerySamples;
        VkBaseInStructure* pStruct = (VkBaseInStructure*)&encodeFrameInfo->encodeInfo;
        vk::ChainNextVkStruct(*pStruct, videoInlineQueryInfoKHR);

        vkDevCtx->CmdEncodeVideoKHR(cmdBuf, &encodeFrameInfo->encodeInfo);

        // Remove the stack pointer from the chain, causes a use after free otherwise in GetEncodeFrameInfoH264
        encodeFrameInfo->encodeInfo.pNext = videoInlineQueryInfoKHR.pNext;
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
    // The encode operation complete semaphore is not needed at this point.
    VkSemaphore frameCompleteSemaphore = VK_NULL_HANDLE; // encodeFrameInfo->encodeCmdBuffer->GetSemaphore();

    // Create command buffer submit info
    VkCommandBufferSubmitInfoKHR cmdBufferInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR };
    cmdBufferInfo.commandBuffer = *pCmdBuf;
    cmdBufferInfo.deviceMask = 0;

    // Create wait semaphore submit infos
    // If we are processing the input staging, wait for it's semaphore
    // to be done before processing the input frame with the encoder.
    const uint32_t waitSemaphoreMaxCount = 3;
    VkSemaphoreSubmitInfoKHR waitSemaphoreInfos[waitSemaphoreMaxCount]{};

    const uint32_t signalSemaphoreMaxCount = 1;
    VkSemaphoreSubmitInfoKHR signalSemaphoreInfos[signalSemaphoreMaxCount]{};

    uint32_t waitSemaphoreCount = 0;
    uint32_t signalSemaphoreCount = 0;

    if (encodeFrameInfo->inputCmdBuffer) {
        waitSemaphoreInfos[waitSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].semaphore = encodeFrameInfo->inputCmdBuffer->GetSemaphore();
        waitSemaphoreInfos[waitSemaphoreCount].value = 0; // Binary semaphore
        // Use transfer bit since these semaphores come from transfer operations
        waitSemaphoreInfos[waitSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].deviceIndex = 0;
        waitSemaphoreCount++;
    }
    if (encodeFrameInfo->qpMapCmdBuffer) {
        waitSemaphoreInfos[waitSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].semaphore = encodeFrameInfo->qpMapCmdBuffer->GetSemaphore();
        waitSemaphoreInfos[waitSemaphoreCount].value = 0; // Binary semaphore
        // Use transfer bit since these semaphores come from transfer operations
        waitSemaphoreInfos[waitSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].deviceIndex = 0;
        waitSemaphoreCount++;
    }

    // Create signal semaphore submit info if needed
    VkSemaphoreSubmitInfoKHR signalSemaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR };
    if (frameCompleteSemaphore != VK_NULL_HANDLE) {
        signalSemaphoreInfo.semaphore = frameCompleteSemaphore;
        signalSemaphoreInfo.value = 0; // Binary semaphore
        signalSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        signalSemaphoreInfo.deviceIndex = 0;
    }

    if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {

        if (m_verbose) {
            uint64_t  currSemValue = 0;
            VkResult semResult = m_vkDevCtx->GetSemaphoreCounterValue(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, &currSemValue);
            std::cout << "\t TL semaphore value: " << currSemValue << ", status: " << semResult << std::endl;
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
    submitInfo.signalSemaphoreInfoCount = (frameCompleteSemaphore != VK_NULL_HANDLE) ? 1 : 0;
    submitInfo.pSignalSemaphoreInfos = (frameCompleteSemaphore != VK_NULL_HANDLE) ? &signalSemaphoreInfo : nullptr;
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
       std::cout << "\t TL semaphore value ater submit: " << currSemValue << ", status: " << semResult << std::endl;

       const bool waitOnTlSemaphore = false;
       if (waitOnTlSemaphore) {
           uint64_t value = encodeFrameInfo->frameEncodeEncodeOrderNum + 1; // wait on the future frameEncodeEncodeOrderNum
           VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, VK_SEMAPHORE_WAIT_ANY_BIT, 1,
                                        &m_hwLoadBalancingTimelineSemaphore, &value };
           std::cout << "\t TL semaphore wait for value: " << value << std::endl;
           semResult = m_vkDevCtx->WaitSemaphores(*m_vkDevCtx, &waitInfo, 1000 * 1000 * 1000 /* 1000 mSec */);

           semResult = m_vkDevCtx->GetSemaphoreCounterValue(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, &currSemValue);
           std::cout << "\t TL semaphore value: " << currSemValue << ", status: " << semResult << std::endl;
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
            VkVideoEncodeFrameInfo::ReleaseChildrenFrames(m_lastDeferredFrame);
            assert(m_lastDeferredFrame == nullptr);
        }
        m_numDeferredFrames = 0;
        m_numDeferredRefFrames = 0;
    }
    return result;
}

VkResult VkVideoEncoder::ProcessOrderedFrames(VkSharedBaseObj<VkVideoEncodeFrameInfo>& frames, uint32_t numFrames) {

    const std::vector<std::pair<std::string, std::function<VkResult(VkSharedBaseObj<VkVideoEncodeFrameInfo>&, uint32_t, uint32_t)>>> callbacks = {
        {"StartOfVideoCodingEncodeOrder",  [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return StartOfVideoCodingEncodeOrder(frame, frameIdx, ofTotalFrames); }},
        {"ProcessDpb",                     [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return ProcessDpb(frame, frameIdx, ofTotalFrames); }},
        {"RecordVideoCodingCmd",           [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return RecordVideoCodingCmd(frame, frameIdx, ofTotalFrames); }},
        {"SubmitVideoCodingCmds",          [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return SubmitVideoCodingCmds(frame, frameIdx, ofTotalFrames); }},
        {"AssembleBitstreamData",          [this](VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame, uint32_t frameIdx, uint32_t ofTotalFrames) { return AssembleBitstreamData(frame, frameIdx, ofTotalFrames); }}
    };

    VkResult result = VK_SUCCESS;
    for (const auto& pair : callbacks) {
        const auto& callback = pair.second;

        uint32_t processedFramesCount = 0;
        result = VkVideoEncodeFrameInfo::ProcessFrames(this, frames, processedFramesCount, numFrames, callback);
        if (m_encoderConfig->verbose) {
            const std::string& description = pair.first;
            std::cout << "====== Total number of frames processed by " << description << ": " << processedFramesCount << " : " << result << std::endl;
        }

        if (result != VK_SUCCESS) {
            break;
        }
    }

    return result;
}

VkResult VkVideoEncoder::ProcessOutOfOrderFrames(VkSharedBaseObj<VkVideoEncodeFrameInfo>& frames, uint32_t numFrames) {

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
    std::cout << std::string(ident, ' ') << "===> "
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
    PushOrderedFrames();

    if (m_enableEncoderThreadQueue) {
        m_encoderThreadQueue.SetFlushAndExit();
        if (m_encoderQueueConsumerThread.joinable()) {
            m_encoderQueueConsumerThread.join();
        }
    }

    return true;
}

int32_t VkVideoEncoder::DeinitEncoder()
{
#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
    m_displayQueue.Flush();
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT
    m_lastDeferredFrame = nullptr;

    m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::ENCODE, 0);

    if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {
         m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, NULL);
         m_hwLoadBalancingTimelineSemaphore = VK_NULL_HANDLE;
    }

    m_linearInputImagePool = nullptr;
    m_inputImagePool       = nullptr;
    m_dpbImagePool         = nullptr;

    m_inputComputeFilter      = nullptr;
    m_inputCommandBufferPool  = nullptr;
    m_encodeCommandBufferPool = nullptr;

    m_videoSessionParameters =  nullptr;
    m_videoSession = nullptr;

    m_encoderConfig = nullptr;

    return 0;
}

void VkVideoEncoder::ConsumerThread()
{
   std::cout << "ConsumerThread is stating now.\n" << std::endl;
   do {
       VkSharedBaseObj<VkVideoEncodeFrameInfo> encodeFrameInfo;
       bool success = m_encoderThreadQueue.WaitAndPop(encodeFrameInfo);
       if (success) { // 5 seconds in nanoseconds
           std::cout << "==>>>> Consumed: " << (uint32_t)encodeFrameInfo->gopPosition.inputOrder
                      << ", Order: " << (uint32_t)encodeFrameInfo->gopPosition.encodeOrder << std::endl << std::flush;

           VkResult result;
           if (!m_encoderConfig->enableOutOfOrderRecording) {
               result = ProcessOrderedFrames(encodeFrameInfo, 0);
           } else {
               // Testing only - don't use for production!
               result = ProcessOutOfOrderFrames(encodeFrameInfo, 0);
           }
           VkVideoEncodeFrameInfo::ReleaseChildrenFrames(encodeFrameInfo);
           assert(encodeFrameInfo == nullptr);
           if (result != VK_SUCCESS) {
               std::cout << "Error processing frames from the frame thread!" << std::endl;
               m_encoderThreadQueue.SetFlushAndExit();
           }

       } else {
           bool shouldExit = m_encoderThreadQueue.ExitQueue();
           std::cout << "Thread should exit: " << (shouldExit ? "Yes" : "No") << std::endl;
       }
   } while (!m_encoderThreadQueue.ExitQueue());

   std::cout << "ConsumerThread is exiting now.\n" << std::endl;
}
