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

#include <atomic>
#include "VkCodecUtils/VkEncoderStdioLatch.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkVideoEncoder/VkVideoEncoderPsnr.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VkImageResource.h"

VkResult VkVideoEncoderPsnr::Create(VkSharedBaseObj<VkVideoEncoderPsnr>& psnr)
{
    psnr.reset(new VkVideoEncoderPsnr());
    return (psnr != nullptr) ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
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
                                       uint32_t encodeQueueFamilyIndex,
                                       VkFormat imageInFormat)
{
    if (m_vkDevCtx == nullptr) {
        m_imageInFormat = (imageInFormat != VK_FORMAT_UNDEFINED) ? imageInFormat : imageDpbFormat;
        m_vkDevCtx = vkDevCtx;
        m_encoderConfig = encoderConfig;
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
                    VkEncPrintfErr("\nVkVideoEncoderPsnr: Failed to Configure psnrReconImagePool.\n");
                    m_psnrReconImagePool = nullptr;
                    m_initResult = result;
                } else {
                    m_initResult = VK_SUCCESS;
                }
            }
        }
    }

    // ===== MECHANISM-C: encoder-INPUT capture pool =====
    // Independent of IsPsnrMetricsEnabled() on purpose: the PSNR path forces a
    // per-frame host wait on the encode fence, which is exactly the kind of
    // serialisation that can hide a submission-ordering race.
    if ((m_capSrcImagePool == nullptr) && (getenv("VKENC_DEBUG_DUMP_SRC") != nullptr)) {
        const char* maxFilesEnv = getenv("VKENC_DEBUG_DUMP_SRC_FILES");
        m_capSrcMaxFiles = (maxFilesEnv != nullptr) ? (uint32_t)atoi(maxFilesEnv) : 0;
        const char* strideEnv = getenv("VKENC_DEBUG_DUMP_SRC_STRIDE");
        m_capStride = (strideEnv != nullptr) ? (uint32_t)std::max(1, atoi(strideEnv)) : 8u;
        m_capSrcEnabled = true;
        VkResult capRes = VulkanVideoImagePool::Create(m_vkDevCtx, m_capSrcImagePool);
        if (capRes == VK_SUCCESS) {
            capRes = m_capSrcImagePool->Configure(m_vkDevCtx,
                                                 m_maxEncodeQueueDepth + 2,
                                                 m_imageInFormat,
                                                 m_imageExtent,
                                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                 m_encodeQueueFamilyIndex,
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                 nullptr,
                                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                                 false,
                                                 false,
                                                 true);
        }
        if (capRes != VK_SUCCESS) {
            VkEncPrintfErr("[CAPSRC] pool configure FAILED (0x%x); capture disabled\n", capRes);
            m_capSrcImagePool = nullptr;
        } else {
            VkEncPrintfErr("[CAPSRC] enabled: fmt=%d %ux%u nodes=%u maxFiles=%u\n",
                    (int)m_imageInFormat, m_imageExtent.width, m_imageExtent.height,
                    m_maxEncodeQueueDepth + 2, m_capSrcMaxFiles);
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
        encodeFrameInfo.psnrFrameData.psnrInputY.resize(yPlaneSize);
        const VkSubresourceLayout& ly = m_encoderConfig->input.planeLayouts[0];
        for (uint32_t row = 0; row < height; row++) {
            memcpy(encodeFrameInfo.psnrFrameData.psnrInputY.data() + (row * width),
                   pInputFrameData + ly.offset + ((size_t)(row) * ly.rowPitch),
                   width);
        }
    }
    if (numPlanes >= 2) {
        encodeFrameInfo.psnrFrameData.psnrInputU.resize(uPlaneSize);
        const VkSubresourceLayout& lu = m_encoderConfig->input.planeLayouts[1];
        for (uint32_t row = 0; row < chromaH; row++) {
            memcpy(encodeFrameInfo.psnrFrameData.psnrInputU.data() + (row * chromaW),
                   pInputFrameData + lu.offset + ((size_t)(row) * lu.rowPitch),
                   chromaW);
        }
    }
    if (numPlanes >= 3) {
        encodeFrameInfo.psnrFrameData.psnrInputV.resize(uPlaneSize);
        const VkSubresourceLayout& lv = m_encoderConfig->input.planeLayouts[2];
        for (uint32_t row = 0; row < chromaH; row++) {
            memcpy(encodeFrameInfo.psnrFrameData.psnrInputV.data() + (row * chromaW),
                   pInputFrameData + lv.offset + ((size_t)(row) * lv.rowPitch),
                   chromaW);
        }
    }
}


// ===================== MECHANISM-C: ENCODER-INPUT CAPTURE =====================
//
// WHAT IT MEASURES. The pixels of encodeFrameInfo->srcEncodeImageResource -- the
// image named as pSrcPictureResource of vkCmdEncodeVideoKHR -- read out of the
// ENCODE command buffer, one command before vkCmdBeginVideoCodingKHR. That places
// the copy in the same submission, on the same queue, behind the same (or absent)
// cross-queue dependency as the encode's own read. A capture that shows zero
// chroma therefore says the encode read zero chroma too; the corruption is at or
// before the encode's input, not inside the encode.
//
// WHY THE BARRIERS CANNOT LAUNDER THE ANSWER. Both barriers below name
// VK_QUEUE_FAMILY_IGNORED and scope their source at ALL_COMMANDS *within this
// command buffer's submission*. A pipeline barrier cannot create a dependency on
// work submitted to a different queue, and cannot substitute for a semaphore
// between submissions. So if the staged copy has not landed when the encode runs,
// it has not landed when this capture runs either.
bool VkVideoEncoderPsnr::CaptureSource(VkCommandBuffer cmdBuf, void* encodeFrameInfoVoid)
{
    if ((encodeFrameInfoVoid == nullptr) || (m_capSrcImagePool == nullptr) || (m_vkDevCtx == nullptr)) {
        return false;
    }
    VkVideoEncoder::VkVideoEncodeFrameInfo& encodeFrameInfo =
        *static_cast<VkVideoEncoder::VkVideoEncodeFrameInfo*>(encodeFrameInfoVoid);
    if (encodeFrameInfo.srcEncodeImageResource == nullptr) {
        return false;
    }
    const bool srcIs2Plane = (m_imageInFormat == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
    if (!srcIs2Plane) {
        // Once per process, and once has to be true rather than likely --
        // see the equivalent claims in the encoder core. Debug capture is
        // opt-in, but two sessions that opt in reach this together.
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            VkEncPrintfErr("[CAPSRC] input format %d is not 2-plane NV12; capture skipped\n",
                    (int)m_imageInFormat);
        }
        return false;
    }
    if (!m_capSrcImagePool->GetAvailableImage(encodeFrameInfo.psnrFrameData.capSrcImage,
                                              VK_IMAGE_LAYOUT_UNDEFINED)) {
        m_capSrcMissCount++;
        return false;
    }
    VkSharedBaseObj<VkImageResourceView> srcView;
    encodeFrameInfo.srcEncodeImageResource->GetImageView(srcView);
    VkSharedBaseObj<VkImageResourceView> dstView;
    encodeFrameInfo.psnrFrameData.capSrcImage->GetImageView(dstView);
    if (!srcView || !dstView) {
        encodeFrameInfo.psnrFrameData.capSrcImage = nullptr;
        return false;
    }
    VkImage srcImage = srcView->GetImageResource()->GetImage();
    VkImage dstImage = dstView->GetImageResource()->GetImage();

    // The layout the staging arm recorded, or the spec-required encode layout when
    // the frame was never staged (Path A / RESIDENCY_LOCAL).
    const VkImageLayout srcLayout =
        (encodeFrameInfo.srcEncodeImageStagedLayout != VK_IMAGE_LAYOUT_MAX_ENUM)
            ? encodeFrameInfo.srcEncodeImageStagedLayout
            : VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;

    VkImageMemoryBarrier2KHR bars[2] = {};
    bars[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR;
    bars[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    bars[0].srcAccessMask = VK_ACCESS_2_NONE;
    bars[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    bars[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    bars[0].oldLayout = srcLayout;
    bars[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bars[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bars[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bars[0].image = srcImage;
    bars[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bars[1] = bars[0];
    bars[1].srcAccessMask = VK_ACCESS_2_NONE;
    bars[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bars[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bars[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bars[1].image = dstImage;

    VkDependencyInfoKHR depInfo = {};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
    depInfo.imageMemoryBarrierCount = 2;
    depInfo.pImageMemoryBarriers = bars;
    m_vkDevCtx->CmdPipelineBarrier2KHR(cmdBuf, &depInfo);

    const uint32_t w = m_encoderConfig->encodeWidth;
    const uint32_t h = m_encoderConfig->encodeHeight;
    VkImageCopy r0 = { { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 }, { 0, 0, 0 },
                       { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 }, { 0, 0, 0 }, { w, h, 1 } };
    m_vkDevCtx->CmdCopyImage(cmdBuf, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r0);
    VkImageCopy r1 = { { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 }, { 0, 0, 0 },
                       { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 }, { 0, 0, 0 },
                       { (w + 1) / 2, (h + 1) / 2, 1 } };
    m_vkDevCtx->CmdCopyImage(cmdBuf, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r1);

    // Hand the encode source back in exactly the layout it was handed to us in,
    // so the encode's own VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811 still holds.
    VkImageMemoryBarrier2KHR back = bars[0];
    back.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    back.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    back.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    back.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = srcLayout;
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &back;
    m_vkDevCtx->CmdPipelineBarrier2KHR(cmdBuf, &depInfo);
    return true;
}

// Records (imported linear image -> host-visible LINEAR image) into the STAGING
// command buffer, immediately after CopyLinearToOptimalImage has read the same
// image. This is the pixels the PRODUCER handed us, before the library's own
// copy can be blamed for them. Pool is configured lazily because the imported
// image's format and extent are not known until the first frame arrives.
bool VkVideoEncoderPsnr::CaptureImported(VkCommandBuffer cmdBuf, void* encodeFrameInfoVoid,
                                        void* linearImageViewVoid)
{
    if (!m_capSrcEnabled || (encodeFrameInfoVoid == nullptr) ||
        (linearImageViewVoid == nullptr) || (m_vkDevCtx == nullptr)) {
        return false;
    }
    VkVideoEncoder::VkVideoEncodeFrameInfo& encodeFrameInfo =
        *static_cast<VkVideoEncoder::VkVideoEncodeFrameInfo*>(encodeFrameInfoVoid);
    VkImageResourceView* linearView = static_cast<VkImageResourceView*>(linearImageViewVoid);
    const VkSharedBaseObj<VkImageResource>& srcRes = linearView->GetImageResource();
    const VkImageCreateInfo& ci = srcRes->GetImageCreateInfo();

    if (m_capImpImagePool == nullptr) {
        if (ci.format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
            // As for the capture-source claim above.
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true, std::memory_order_relaxed)) {
                VkEncPrintfErr("[CAPIMP] imported format %d is not 2-plane NV12; capture disabled\n",
                        (int)ci.format);
            }
            return false;
        }
        m_capImpFormat = ci.format;
        m_capImpExtent.width = ci.extent.width;
        m_capImpExtent.height = ci.extent.height;
        VkResult r = VulkanVideoImagePool::Create(m_vkDevCtx, m_capImpImagePool);
        if (r == VK_SUCCESS) {
            r = m_capImpImagePool->Configure(m_vkDevCtx, m_maxEncodeQueueDepth + 2,
                                             m_capImpFormat, m_capImpExtent,
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                             m_encodeQueueFamilyIndex,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                             nullptr, VK_IMAGE_ASPECT_COLOR_BIT,
                                             false, false, true);
        }
        if (r != VK_SUCCESS) {
            VkEncPrintfErr("[CAPIMP] pool configure FAILED (0x%x); capture disabled\n", r);
            m_capImpImagePool = nullptr;
            return false;
        }
        VkEncPrintfErr("[CAPIMP] enabled: fmt=%d %ux%u tiling=%d\n",
                (int)m_capImpFormat, m_capImpExtent.width, m_capImpExtent.height, (int)ci.tiling);
    }
    if (!m_capImpImagePool->GetAvailableImage(encodeFrameInfo.psnrFrameData.capImpImage,
                                              VK_IMAGE_LAYOUT_UNDEFINED)) {
        m_capImpMissCount++;
        return false;
    }
    VkSharedBaseObj<VkImageResourceView> dstView;
    encodeFrameInfo.psnrFrameData.capImpImage->GetImageView(dstView);
    if (!dstView) {
        encodeFrameInfo.psnrFrameData.capImpImage = nullptr;
        return false;
    }
    VkImage srcImage = srcRes->GetImage();
    VkImage dstImage = dstView->GetImageResource()->GetImage();
    encodeFrameInfo.psnrFrameData.capSeq = m_capSeq.fetch_add(1) + 1;
    encodeFrameInfo.psnrFrameData.capImpImageId = (uint64_t)srcImage;
    encodeFrameInfo.psnrFrameData.capImpMemId = (uint64_t)srcRes->GetDeviceMemory();

    // The imported image is already in TRANSFER_SRC_OPTIMAL here -- the staging
    // arm put it there and CopyLinearToOptimalImage just read it -- so only the
    // destination needs a barrier, plus a TRANSFER->TRANSFER execution dependency
    // so this copy is ordered after the library's own.
    VkImageMemoryBarrier2KHR bar = {};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR;
    bar.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    bar.srcAccessMask = VK_ACCESS_2_NONE;
    bar.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    bar.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = dstImage;
    bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfoKHR dep = {};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &bar;
    m_vkDevCtx->CmdPipelineBarrier2KHR(cmdBuf, &dep);

    const uint32_t w = std::min(m_capImpExtent.width, m_encoderConfig->encodeWidth);
    const uint32_t h = std::min(m_capImpExtent.height, m_encoderConfig->encodeHeight);
    VkImageCopy r0 = { { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 }, { 0, 0, 0 },
                       { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 }, { 0, 0, 0 }, { w, h, 1 } };
    m_vkDevCtx->CmdCopyImage(cmdBuf, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r0);
    VkImageCopy r1 = { { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 }, { 0, 0, 0 },
                       { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 }, { 0, 0, 0 },
                       { (w + 1) / 2, (h + 1) / 2, 1 } };
    m_vkDevCtx->CmdCopyImage(cmdBuf, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r1);
    return true;
}

// Host-side reader, shared by both captures. Must be called only after the
// ENCODE command buffer's fence has been waited on.
//
// COST DISCIPLINE: v1 of this walked the whole 3.1 MB of HOST_VISIBLE image
// memory byte-wise and dropped the pipeline from ~200 fps to 3.6 fps. Rows are
// now bulk-memcpy'd into cached scratch and only every m_capStride-th row is
// touched, which is ample for "is this plane all zeros".
void VkVideoEncoderPsnr::DumpCapturedNode(VkSharedBaseObj<VulkanVideoImagePoolNode>& node,
                                          const char* tag, uint32_t frameIdx,
                                          uint32_t w, uint32_t h, bool writeFile,
                                          uint64_t seq, uint64_t imgId)
{
    if (node == nullptr) {
        return;
    }
    VkSharedBaseObj<VkImageResourceView> view;
    node->GetImageView(view);
    if (!view) { node = nullptr; return; }
    const VkSharedBaseObj<VkImageResource>& res = view->GetImageResource();
    VkDevice device = m_vkDevCtx->getDevice();
    void* mapped = nullptr;
    if ((m_vkDevCtx->MapMemory(device, res->GetDeviceMemory(),
                               res->GetImageDeviceMemoryOffset(),
                               res->GetImageDeviceMemorySize(), 0, &mapped) != VK_SUCCESS) ||
        (mapped == nullptr)) {
        node = nullptr;
        return;
    }
    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    const uint32_t cw = (w + 1) / 2;
    const uint32_t ch = (h + 1) / 2;
    const uint32_t stride = writeFile ? 1u : m_capStride;
    VkImage img = res->GetImage();
    VkImageSubresource sub = {};
    VkSubresourceLayout ly = {};
    VkSubresourceLayout lc = {};
    sub.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    m_vkDevCtx->GetImageSubresourceLayout(device, img, &sub, &ly);
    sub.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    m_vkDevCtx->GetImageSubresourceLayout(device, img, &sub, &lc);

    m_capScratch.resize((size_t)std::max(w, 2u * cw) + 64);
    uint8_t* row = m_capScratch.data();

    uint64_t sumY = 0; uint64_t nY = 0;
    for (uint32_t y = 0; y < h; y += stride) {
        memcpy(row, base + ly.offset + ((size_t)y * ly.rowPitch), w);
        for (uint32_t x = 0; x < w; x++) { sumY += row[x]; }
        nY += w;
    }
    uint64_t sumU = 0, sumV = 0, nC = 0, zeroUV = 0;
    for (uint32_t y = 0; y < ch; y += stride) {
        memcpy(row, base + lc.offset + ((size_t)y * lc.rowPitch), (size_t)2 * cw);
        for (uint32_t x = 0; x < cw; x++) {
            const uint8_t u = row[(2 * x) + 0];
            const uint8_t v = row[(2 * x) + 1];
            sumU += u; sumV += v;
            if ((u == 0) && (v == 0)) { zeroUV++; }
        }
        nC += cw;
    }

    if (writeFile) {
        char filename[256];
        snprintf(filename, sizeof(filename), "%s_frame_%05u_%ux%u.yuv", tag, frameIdx, w, h);
        std::ofstream out(filename, std::ios::binary);
        if (out) {
            std::vector<uint8_t> line(w);
            for (uint32_t y = 0; y < h; y++) {
                memcpy(line.data(), base + ly.offset + ((size_t)y * ly.rowPitch), w);
                out.write(reinterpret_cast<const char*>(line.data()), w);
            }
            std::vector<uint8_t> up(cw), vp(cw), pair((size_t)2 * cw);
            for (uint32_t pl = 0; pl < 2; pl++) {
                for (uint32_t y = 0; y < ch; y++) {
                    memcpy(pair.data(), base + lc.offset + ((size_t)y * lc.rowPitch), (size_t)2 * cw);
                    for (uint32_t x = 0; x < cw; x++) { up[x] = pair[(2 * x) + pl]; }
                    out.write(reinterpret_cast<const char*>(up.data()), cw);
                }
            }
            (void)vp;
        }
    }
    m_vkDevCtx->UnmapMemory(device, res->GetDeviceMemory());

    VkEncPrintfErr("[%s] seq=%llu gopf=%u img=0x%llx Ymean=%.3f Umean=%.3f Vmean=%.3f zeroUVpct=%.2f\n",
            tag, (unsigned long long)seq, frameIdx, (unsigned long long)imgId,
            nY ? (double)sumY / (double)nY : -1.0,
            nC ? (double)sumU / (double)nC : -1.0,
            nC ? (double)sumV / (double)nC : -1.0,
            nC ? 100.0 * (double)zeroUV / (double)nC : -1.0);
    node = nullptr;
}

void VkVideoEncoderPsnr::DumpCapturedSource(void* encodeFrameInfoVoid)
{
    VkVideoEncoder::VkVideoEncodeFrameInfo& encodeFrameInfo =
        *static_cast<VkVideoEncoder::VkVideoEncodeFrameInfo*>(encodeFrameInfoVoid);
    const uint32_t inputOrder = (uint32_t)encodeFrameInfo.gopPosition.inputOrder;
    const bool writeFile = (m_capSrcFilesWritten < m_capSrcMaxFiles);
    if (encodeFrameInfo.psnrFrameData.capImpImage != nullptr) {
        DumpCapturedNode(encodeFrameInfo.psnrFrameData.capImpImage, "CAPIMP", inputOrder,
                         std::min(m_capImpExtent.width, m_encoderConfig->encodeWidth),
                         std::min(m_capImpExtent.height, m_encoderConfig->encodeHeight),
                         writeFile,
                         encodeFrameInfo.psnrFrameData.capSeq,
                         encodeFrameInfo.psnrFrameData.capImpImageId);
    }
    if (encodeFrameInfo.psnrFrameData.capSrcImage != nullptr) {
        DumpCapturedNode(encodeFrameInfo.psnrFrameData.capSrcImage, "CAPSRC", inputOrder,
                         m_encoderConfig->encodeWidth, m_encoderConfig->encodeHeight,
                         writeFile,
                         encodeFrameInfo.psnrFrameData.capSeq,
                         encodeFrameInfo.psnrFrameData.capImpMemId);
        if (writeFile) { m_capSrcFilesWritten++; }
    }
    fflush(stderr);
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

    // Transition staging image from UNDEFINED to TRANSFER_DST before copy
    VkImageMemoryBarrier2KHR stagingBarrier = {};
    stagingBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR;
    stagingBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    stagingBarrier.srcAccessMask = VK_ACCESS_2_NONE;
    stagingBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    stagingBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    stagingBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    stagingBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    stagingBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    stagingBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    stagingBarrier.image = stagingImage;
    stagingBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfoKHR depInfo = {};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &stagingBarrier;
    m_vkDevCtx->CmdPipelineBarrier2KHR(cmdBuf, &depInfo);

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
    // MECHANISM-C runs without the PSNR pool, so a frame carrying only a source
    // capture must not be turned away by the PSNR preconditions.
    const bool haveSrcCapture = (encodeFrameInfo.psnrFrameData.capSrcImage != nullptr) ||
                                (encodeFrameInfo.psnrFrameData.capImpImage != nullptr);
    const bool havePsnrCapture = (encodeFrameInfo.psnrFrameData.psnrStagingImage != nullptr) &&
                                 (encodeFrameInfo.setupImageResource != nullptr);
    if (!haveSrcCapture && !havePsnrCapture) {
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

    const uint8_t* inputY = encodeFrameInfo.psnrFrameData.psnrInputY.data();
    const uint8_t* inputU = encodeFrameInfo.psnrFrameData.psnrInputU.data();
    const uint8_t* inputV = encodeFrameInfo.psnrFrameData.psnrInputV.data();
    const size_t inputYSize = encodeFrameInfo.psnrFrameData.psnrInputY.size();
    const size_t inputUSize = encodeFrameInfo.psnrFrameData.psnrInputU.size();
    const size_t inputVSize = encodeFrameInfo.psnrFrameData.psnrInputV.size();
    // Ext (SubmitExternalFrame) path never runs CaptureInput (no CPU input buffer),
    // so psnrInput* is empty. Still read back + dump the reconstructed frame so we can
    // compare the recon against the reference offline; only skip the input-vs-recon math.
    const bool haveInput = (!encodeFrameInfo.psnrFrameData.psnrInputY.empty()) &&
                           (inputYSize == yPlaneSizeInput);

    if (encodeFrameInfo.encodeCmdBuffer == nullptr) {
        return;
    }
    VkResult syncResult = encodeFrameInfo.encodeCmdBuffer->SyncHostOnCmdBuffComplete(false, "encoderEncodeFence");
    if (syncResult != VK_SUCCESS) {
        VkEncPrintfErr("\nPSNR: wait on encoder fence failed (0x%x), skipping frame PSNR.\n", syncResult);
        return;
    }

    // MECHANISM-C readback. The encode fence is signalled, so the capture copy
    // recorded ahead of the coding scope in this same command buffer is done.
    if (haveSrcCapture) {
        DumpCapturedSource(encodeFrameInfoVoid);
    }
    if (!havePsnrCapture) {
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
    if (getenv("VKENC_DEBUG_DUMP_FRAMES")) {
        const uint32_t inputOrder = (uint32_t)encodeFrameInfo.gopPosition.inputOrder;
        char filename[256];
        snprintf(filename, sizeof(filename), "input_frame_%05u_%ux%u.yuv", inputOrder, width, height);
        std::ofstream outInput(haveInput ? filename : "/dev/null", std::ios::binary);
        if (haveInput && outInput) {
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

    if (haveInput) {
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
    }
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
    m_psnrReconY.clear();
    m_psnrReconU.clear();
    m_psnrReconV.clear();
}
