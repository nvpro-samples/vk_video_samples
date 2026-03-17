/*
 * Copyright 2026 NVIDIA Corporation.
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkVideoEncoder/VkVideoEncoderPsnr.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VkImageResource.h"

VkResult VkVideoEncoderPsnr::Create(VkSharedBaseObj<VkVideoEncoderPsnr>& psnr)
{
    psnr = new VkVideoEncoderPsnr();
    return (psnr != nullptr) ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

int32_t VkVideoEncoderPsnr::AddRef()
{
    return ++m_refCount;
}

int32_t VkVideoEncoderPsnr::Release()
{
    int32_t ret = --m_refCount;
    if (ret == 0) {
        delete this;
    }
    return ret;
}

VkVideoEncoderPsnr::~VkVideoEncoderPsnr()
{
    Deinit();
}

VkResult VkVideoEncoderPsnr::Configure(const VulkanDeviceContext* vkDevCtx,
                                       VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                       uint32_t maxEncodeQueueDepth,
                                       VkFormat imageDpbFormat,
                                       const VkExtent2D& imageExtent,
                                       uint32_t encodeQueueFamilyIndex)
{
    if (m_vkDevCtx == nullptr) {
        m_vkDevCtx = vkDevCtx;
        m_encoderConfig = encoderConfig.Get();
        m_maxEncodeQueueDepth = maxEncodeQueueDepth;
        m_imageDpbFormat = imageDpbFormat;
        m_imageExtent = imageExtent;
        m_encodeQueueFamilyIndex = encodeQueueFamilyIndex;

        if ((m_encoderConfig == nullptr) || !m_encoderConfig->IsPsnrMetricsEnabled()) {
            m_initResult = VK_SUCCESS;
        } else {
            VkResult result = VulkanVideoImagePool::Create(m_vkDevCtx, m_psnrReconImagePool);
            if (result != VK_SUCCESS) {
                m_initResult = result;
            } else {
                result = m_psnrReconImagePool->Configure(m_vkDevCtx,
                                                       m_maxEncodeQueueDepth,
                                                       m_imageDpbFormat,
                                                       m_imageExtent,
                                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                       m_encodeQueueFamilyIndex,
                                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                       nullptr,
                                                       VK_IMAGE_ASPECT_COLOR_BIT,
                                                       false,
                                                       false,
                                                       true);
                if (result != VK_SUCCESS) {
                    fprintf(stderr, "\nVkVideoEncoderPsnr: Failed to Configure psnrReconImagePool.\n");
                    m_psnrReconImagePool = nullptr;
                    m_initResult = result;
                } else {
                    m_initResult = VK_SUCCESS;
                }
            }
        }
    }

    if ((m_encoderConfig != nullptr) && m_encoderConfig->IsPsnrMetricsEnabled() && (m_psnrReconImagePool == nullptr)) {
        return m_initResult;
    }
    m_psnrSum = 0.0;
    m_psnrSumU = 0.0;
    m_psnrSumV = 0.0;
    m_psnrFrameCount = 0;
    return VK_SUCCESS;
}

void VkVideoEncoderPsnr::CaptureInput(void* encodeFrameInfoVoid, const uint8_t* pInputFrameData)
{
    if (!m_encoderConfig->IsPsnrMetricsEnabled() || (encodeFrameInfoVoid == nullptr) || (pInputFrameData == nullptr)) {
        return;
    }
    VkVideoEncoder::VkVideoEncodeFrameInfo& encodeFrameInfo = *static_cast<VkVideoEncoder::VkVideoEncodeFrameInfo*>(encodeFrameInfoVoid);
    const uint32_t width = std::min(m_encoderConfig->encodeWidth, m_encoderConfig->input.width);
    const uint32_t height = std::min(m_encoderConfig->encodeHeight, m_encoderConfig->input.height);
    const bool chroma420 = (m_encoderConfig->input.chromaSubsampling == VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
    const uint32_t chromaW = chroma420 ? ((width + 1) / 2) : width;
    const uint32_t chromaH = chroma420 ? ((height + 1) / 2) : height;
    const size_t yPlaneSize = (size_t)(width * height);
    const size_t uPlaneSize = (size_t)(chromaW * chromaH);
    const uint32_t numPlanes = m_encoderConfig->input.numPlanes;

    if (numPlanes >= 1) {
        m_psnrInputY.resize(yPlaneSize);
        const VkSubresourceLayout& ly = m_encoderConfig->input.planeLayouts[0];
        for (uint32_t row = 0; row < height; row++) {
            memcpy(m_psnrInputY.data() + (row * width),
                   pInputFrameData + ly.offset + ((size_t)(row) * ly.rowPitch),
                   width);
        }
        encodeFrameInfo.psnrFrameData.psnrInputY = m_psnrInputY;
    }
    if (numPlanes >= 2) {
        m_psnrInputU.resize(uPlaneSize);
        const VkSubresourceLayout& lu = m_encoderConfig->input.planeLayouts[1];
        for (uint32_t row = 0; row < chromaH; row++) {
            memcpy(m_psnrInputU.data() + (row * chromaW),
                   pInputFrameData + lu.offset + ((size_t)(row) * lu.rowPitch),
                   chromaW);
        }
        encodeFrameInfo.psnrFrameData.psnrInputU = m_psnrInputU;
    }
    if (numPlanes >= 3) {
        m_psnrInputV.resize(uPlaneSize);
        const VkSubresourceLayout& lv = m_encoderConfig->input.planeLayouts[2];
        for (uint32_t row = 0; row < chromaH; row++) {
            memcpy(m_psnrInputV.data() + (row * chromaW),
                   pInputFrameData + lv.offset + ((size_t)(row) * lv.rowPitch),
                   chromaW);
        }
        encodeFrameInfo.psnrFrameData.psnrInputV = m_psnrInputV;
    }
}

bool VkVideoEncoderPsnr::CaptureOutput(VkCommandBuffer cmdBuf, void* encodeFrameInfoVoid)
{
    if ((encodeFrameInfoVoid == nullptr) || (m_psnrReconImagePool == nullptr) || (m_encoderConfig == nullptr)) {
        return false;
    }
    VkVideoEncoder::VkVideoEncodeFrameInfo& encodeFrameInfo = *static_cast<VkVideoEncoder::VkVideoEncodeFrameInfo*>(encodeFrameInfoVoid);
    if (encodeFrameInfo.setupImageResource == nullptr) {
        return false;
    }
    if (!m_psnrReconImagePool->GetAvailableImage(encodeFrameInfo.psnrFrameData.psnrStagingImage, VK_IMAGE_LAYOUT_UNDEFINED)) {
        return false;
    }
    VkSharedBaseObj<VkImageResourceView> setupEncodeImageView;
    encodeFrameInfo.setupImageResource->GetImageView(setupEncodeImageView);
    VkSharedBaseObj<VkImageResourceView> stagingImageView;
    encodeFrameInfo.psnrFrameData.psnrStagingImage->GetImageView(stagingImageView);

    const uint32_t w = m_encoderConfig->encodeWidth;
    const uint32_t h = m_encoderConfig->encodeHeight;
    const bool chroma420 = (m_encoderConfig->input.chromaSubsampling == VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
    const uint32_t chromaW = chroma420 ? ((w + 1) / 2) : w;
    const uint32_t chromaH = chroma420 ? ((h + 1) / 2) : h;
    const uint32_t numInputPlanes = std::min(3u, m_encoderConfig->input.numPlanes);
    const bool dpbIs2Plane = (m_imageDpbFormat == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
    VkImage dpbImage = setupEncodeImageView->GetImageResource()->GetImage();
    VkImage stagingImage = stagingImageView->GetImageResource()->GetImage();

    if (dpbIs2Plane) {
        VkImageCopy r0 = { { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 }, { 0, 0, 0 }, { w, h, 1 } };
        m_vkDevCtx->CmdCopyImage(cmdBuf, dpbImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r0);
        VkImageCopy r1 = { { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 }, { 0, 0, 0 }, { chromaW, chromaH, 1 } };
        m_vkDevCtx->CmdCopyImage(cmdBuf, dpbImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r1);
    } else {
        for (uint32_t p = 0; p < numInputPlanes; p++) {
            const uint32_t pw = (p == 0) ? w : chromaW;
            const uint32_t ph = (p == 0) ? h : chromaH;
            VkImageCopy region = {
                { (VkImageAspectFlags)(VK_IMAGE_ASPECT_PLANE_0_BIT << p), 0, 0, 1 }, { 0, 0, 0 },
                { (VkImageAspectFlags)(VK_IMAGE_ASPECT_PLANE_0_BIT << p), 0, 0, 1 }, { 0, 0, 0 },
                { pw, ph, 1 }
            };
            m_vkDevCtx->CmdCopyImage(cmdBuf, dpbImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
    }
    return true;
}

void VkVideoEncoderPsnr::ComputeFramePsnr(void* encodeFrameInfoVoid)
{
    if (encodeFrameInfoVoid == nullptr) {
        return;
    }
    VkVideoEncoder::VkVideoEncodeFrameInfo& encodeFrameInfo = *static_cast<VkVideoEncoder::VkVideoEncodeFrameInfo*>(encodeFrameInfoVoid);
    if (encodeFrameInfo.psnrFrameData.psnrStagingImage == nullptr) {
        return;
    }
    if (encodeFrameInfo.setupImageResource == nullptr) {
        return;
    }
    const uint32_t width = std::min(m_encoderConfig->encodeWidth, m_encoderConfig->input.width);
    const uint32_t height = std::min(m_encoderConfig->encodeHeight, m_encoderConfig->input.height);
    const uint32_t encodeW = m_encoderConfig->encodeWidth;
    const uint32_t encodeH = m_encoderConfig->encodeHeight;
    const bool chroma420 = (m_encoderConfig->input.chromaSubsampling == VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
    const uint32_t chromaW = chroma420 ? ((width + 1) / 2) : width;
    const uint32_t chromaH = chroma420 ? ((height + 1) / 2) : height;
    const uint32_t encodeChromaW = chroma420 ? ((encodeW + 1) / 2) : encodeW;
    const uint32_t encodeChromaH = chroma420 ? ((encodeH + 1) / 2) : encodeH;
    const size_t yPlaneSizeInput = (size_t)(width * height);
    const size_t uPlaneSizeInput = (size_t)(chromaW * chromaH);
    const size_t yPlaneSizeRecon = (size_t)(encodeW * encodeH);
    const size_t uPlaneSizeRecon = (size_t)(encodeChromaW * encodeChromaH);
    const uint32_t numPlanes = std::min(3u, m_encoderConfig->input.numPlanes);

    const uint8_t* inputY = (!encodeFrameInfo.psnrFrameData.psnrInputY.empty()) ? encodeFrameInfo.psnrFrameData.psnrInputY.data() : m_psnrInputY.data();
    const uint8_t* inputU = (!encodeFrameInfo.psnrFrameData.psnrInputU.empty()) ? encodeFrameInfo.psnrFrameData.psnrInputU.data() : m_psnrInputU.data();
    const uint8_t* inputV = (!encodeFrameInfo.psnrFrameData.psnrInputV.empty()) ? encodeFrameInfo.psnrFrameData.psnrInputV.data() : m_psnrInputV.data();
    const size_t inputYSize = (!encodeFrameInfo.psnrFrameData.psnrInputY.empty()) ? encodeFrameInfo.psnrFrameData.psnrInputY.size() : m_psnrInputY.size();
    const size_t inputUSize = (!encodeFrameInfo.psnrFrameData.psnrInputU.empty()) ? encodeFrameInfo.psnrFrameData.psnrInputU.size() : m_psnrInputU.size();
    const size_t inputVSize = (!encodeFrameInfo.psnrFrameData.psnrInputV.empty()) ? encodeFrameInfo.psnrFrameData.psnrInputV.size() : m_psnrInputV.size();
    if (inputYSize != yPlaneSizeInput) {
        return;
    }

    if (encodeFrameInfo.encodeCmdBuffer == nullptr) {
        return;
    }
    VkResult syncResult = encodeFrameInfo.encodeCmdBuffer->SyncHostOnCmdBuffComplete(false, "encoderEncodeFence");
    if (syncResult != VK_SUCCESS) {
        fprintf(stderr, "\nPSNR: wait on encoder fence failed (0x%x), skipping frame PSNR.\n", syncResult);
        return;
    }

    const bool dpbIs2Plane = (m_imageDpbFormat == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);

    VkSharedBaseObj<VkImageResourceView> stagingImageView;
    encodeFrameInfo.psnrFrameData.psnrStagingImage->GetImageView(stagingImageView);
    if (!stagingImageView) {
        return;
    }
    const VkSharedBaseObj<VkImageResource>& imageResourceRef = stagingImageView->GetImageResource();
    VkDeviceMemory mem = imageResourceRef->GetDeviceMemory();
    VkDeviceSize memOffset = imageResourceRef->GetImageDeviceMemoryOffset();
    VkDeviceSize memSize = imageResourceRef->GetImageDeviceMemorySize();
    VkImage vkImage = imageResourceRef->GetImage();
    VkDevice device = m_vkDevCtx->getDevice();

    void* mapped = nullptr;
    VkResult mapResult = m_vkDevCtx->MapMemory(device, mem, memOffset, memSize, 0, &mapped);
    if ((mapResult != VK_SUCCESS) || (mapped == nullptr)) {
        return;
    }
    const uint8_t* basePtr = static_cast<const uint8_t*>(mapped);

    VkImageSubresource subres = {};
    subres.mipLevel = 0;
    subres.arrayLayer = 0;
    VkSubresourceLayout layout = {};
    subres.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    m_vkDevCtx->GetImageSubresourceLayout(device, vkImage, &subres, &layout);
    m_psnrReconY.resize(yPlaneSizeRecon);
    for (uint32_t y = 0; y < encodeH; y++) {
        memcpy(m_psnrReconY.data() + (y * encodeW), basePtr + layout.offset + (y * layout.rowPitch), encodeW);
    }

    if (numPlanes >= 2) {
        m_psnrReconU.resize(uPlaneSizeRecon);
        if (dpbIs2Plane) {
            m_psnrReconV.resize(uPlaneSizeRecon);
            subres.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            m_vkDevCtx->GetImageSubresourceLayout(device, vkImage, &subres, &layout);
            const uint8_t* uvPtr = basePtr + layout.offset;
            for (uint32_t y = 0; y < encodeChromaH; y++) {
                for (uint32_t x = 0; x < encodeChromaW; x++) {
                    m_psnrReconU[(y * encodeChromaW) + x] = uvPtr[(y * layout.rowPitch) + (2 * x)];
                    m_psnrReconV[(y * encodeChromaW) + x] = uvPtr[(y * layout.rowPitch) + (2 * x) + 1];
                }
            }
        } else {
            subres.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            m_vkDevCtx->GetImageSubresourceLayout(device, vkImage, &subres, &layout);
            for (uint32_t y = 0; y < encodeChromaH; y++) {
                memcpy(m_psnrReconU.data() + (y * encodeChromaW), basePtr + layout.offset + (y * layout.rowPitch), encodeChromaW);
            }
            if (numPlanes >= 3) {
                m_psnrReconV.resize(uPlaneSizeRecon);
                subres.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(device, vkImage, &subres, &layout);
                for (uint32_t y = 0; y < encodeChromaH; y++) {
                    memcpy(m_psnrReconV.data() + (y * encodeChromaW), basePtr + layout.offset + (y * layout.rowPitch), encodeChromaW);
                }
            }
        }
    }

    m_vkDevCtx->UnmapMemory(device, mem);

#define VKVENC_DEBUG_DUMP_PSNR_FRAMES 1
#if VKVENC_DEBUG_DUMP_PSNR_FRAMES
    {
        const uint32_t inputOrder = (uint32_t)encodeFrameInfo.gopPosition.inputOrder;
        char filename[256];
        snprintf(filename, sizeof(filename), "input_frame_%05u_%ux%u.yuv", inputOrder, width, height);
        std::ofstream outInput(filename, std::ios::binary);
        if (outInput) {
            outInput.write(reinterpret_cast<const char*>(inputY), yPlaneSizeInput);
            if ((numPlanes >= 2) && (inputUSize >= uPlaneSizeInput)) {
                outInput.write(reinterpret_cast<const char*>(inputU), uPlaneSizeInput);
            }
            if ((numPlanes >= 3) && (inputVSize >= uPlaneSizeInput)) {
                outInput.write(reinterpret_cast<const char*>(inputV), uPlaneSizeInput);
            }
        }
        snprintf(filename, sizeof(filename), "recon_frame_%05u_%ux%u.yuv", inputOrder, encodeW, encodeH);
        std::ofstream outRecon(filename, std::ios::binary);
        if (outRecon) {
            outRecon.write(reinterpret_cast<const char*>(m_psnrReconY.data()), yPlaneSizeRecon);
            if ((numPlanes >= 2) && (m_psnrReconU.size() >= uPlaneSizeRecon)) {
                outRecon.write(reinterpret_cast<const char*>(m_psnrReconU.data()), uPlaneSizeRecon);
            }
            if ((numPlanes >= 3) && (m_psnrReconV.size() >= uPlaneSizeRecon)) {
                outRecon.write(reinterpret_cast<const char*>(m_psnrReconV.data()), uPlaneSizeRecon);
            }
        }
    }
#endif

    auto computePlanePsnr = [](const uint8_t* srcPtr, size_t srcStride,
                              const uint8_t* reconPtr, size_t reconStride,
                              uint32_t compareWidth, uint32_t compareHeight) -> double {
        const size_t n = (size_t)(compareWidth * compareHeight);
        if ((n == 0) || (srcPtr == nullptr) || (reconPtr == nullptr)) {
            return -1.0;
        }
        uint64_t sumSqDiff = 0;
        for (uint32_t y = 0; y < compareHeight; y++) {
            for (uint32_t x = 0; x < compareWidth; x++) {
                int d = (int)(srcPtr[(y * srcStride) + x]) - (int)(reconPtr[(y * reconStride) + x]);
                sumSqDiff += (uint64_t)(d * d);
            }
        }
        double mse = (double)(sumSqDiff) / (double)(n);
        return (mse <= 1e-10) ? 100.0 : (10.0 * log10((255.0 * 255.0) / mse));
    };

    double framePsnrY = computePlanePsnr(inputY, width, m_psnrReconY.data(), encodeW, width, height);
    if (framePsnrY >= 0.0) {
        m_psnrSum += framePsnrY;
    }
    if ((numPlanes >= 2) && (inputUSize == uPlaneSizeInput) && (m_psnrReconU.size() >= uPlaneSizeInput)) {
        double framePsnrU = computePlanePsnr(inputU, chromaW, m_psnrReconU.data(), encodeChromaW, chromaW, chromaH);
        if (framePsnrU >= 0.0) {
            m_psnrSumU += framePsnrU;
        }
    }
    if ((numPlanes >= 3) && (inputVSize == uPlaneSizeInput) && (m_psnrReconV.size() >= uPlaneSizeInput)) {
        double framePsnrV = computePlanePsnr(inputV, chromaW, m_psnrReconV.data(), encodeChromaW, chromaW, chromaH);
        if (framePsnrV >= 0.0) {
            m_psnrSumV += framePsnrV;
        }
    }
    m_psnrFrameCount++;
    if (encodeFrameInfo.psnrFrameData.psnrStagingImage) {
        encodeFrameInfo.psnrFrameData.psnrStagingImage = nullptr;
    }
}

double VkVideoEncoderPsnr::GetAveragePsnrY() const
{
    if (m_psnrFrameCount > 0) {
        return m_psnrSum / (double)(m_psnrFrameCount);
    }
    return -1.0;
}

double VkVideoEncoderPsnr::GetAveragePsnrU() const
{
    if ((m_encoderConfig != nullptr) && (m_encoderConfig->input.numPlanes >= 2) && (m_psnrFrameCount > 0)) {
        return m_psnrSumU / (double)(m_psnrFrameCount);
    }
    return -1.0;
}

double VkVideoEncoderPsnr::GetAveragePsnrV() const
{
    if ((m_encoderConfig != nullptr) && (m_encoderConfig->input.numPlanes >= 3) && (m_psnrFrameCount > 0)) {
        return m_psnrSumV / (double)(m_psnrFrameCount);
    }
    return -1.0;
}

double VkVideoEncoderPsnr::GetAveragePsnr() const
{
    return GetAveragePsnrY();
}

void VkVideoEncoderPsnr::Deinit()
{
    m_psnrReconImagePool = nullptr;
    m_encoderConfig = nullptr;
    m_psnrInputY.clear();
    m_psnrInputU.clear();
    m_psnrInputV.clear();
    m_psnrReconY.clear();
    m_psnrReconU.clear();
    m_psnrReconV.clear();
}
