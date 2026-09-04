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

#include "VkVideoEncoder/VkVideoEncoderContentProbe.h"

#include <algorithm>
#include <cstring>

#include "VkCodecUtils/VkImageResource.h"
#include "VkCodecUtils/VulkanDeviceContext.h"

VkResult VkVideoEncoderContentProbe::Create(
    VkSharedBaseObj<VkVideoEncoderContentProbe>& probe)
{
    probe.reset(new VkVideoEncoderContentProbe());
    return (probe != nullptr) ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkVideoEncoderContentProbe::~VkVideoEncoderContentProbe()
{
    Deinit();
}

void VkVideoEncoderContentProbe::Configure(const VulkanDeviceContext* vkDevCtx,
                                           uint32_t poolDepth,
                                           uint32_t queueFamilyIndex,
                                           uint32_t encodeWidth,
                                           uint32_t encodeHeight)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vkDevCtx = vkDevCtx;
    // +2 over the queue depth for the same reason the PSNR capture pool takes
    // it: a node is held from the record site until the post-fence score, so
    // the in-flight set can briefly exceed the encode queue depth.
    m_poolDepth = poolDepth + 2u;
    m_queueFamilyIndex = queueFamilyIndex;
    m_encodeExtent.width = encodeWidth;
    m_encodeExtent.height = encodeHeight;
}

bool VkVideoEncoderContentProbe::IsProbeableFormat(VkFormat format)
{
    return IsProbeableFormatLocked(format);
}

bool VkVideoEncoderContentProbe::IsProbeableFormatLocked(VkFormat format)
{
    // 8-bit 2-plane 420 only. The predicate is stated over Y, U and V plane
    // means, so a format with no chroma planes to score (RGBA) has no verdict
    // to give, and a 10/12-bit packed format stores its samples in the high
    // bits of 16-bit words -- the byte-wise mean below would be reading the
    // wrong half. Widening this means teaching the scorer the word layout,
    // not just adding a case here, and NOT_APPLICABLE is the honest answer
    // until someone does.
    return (format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
}

void VkVideoEncoderContentProbe::ArmRegistration(uint64_t registrationId,
                                                 CaptureSite captureSite)
{
    if (registrationId == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    Registration& reg = m_registrations[registrationId];
    // A re-arm of an already-scored registration must not erase its verdict.
    //
    // A registration id is reused only after the generation counter has
    // invalidated the old one, and ForgetRegistration has already run by that
    // point. So a re-arm arriving for an id that still holds a verdict is a
    // REPEAT of a live registration, not a new one, and the verdict it
    // reached must survive it.
    if ((reg.state == STATE_CLEAN) || (reg.state == STATE_DAMAGED_CHROMA) ||
        (reg.state == STATE_DAMAGED_ALL)) {
        return;
    }
    // See the header: this is "can the probe's readback ride this
    // registration", NOT "is this buffer directly encodable". Latched here
    // rather than discovered per frame so the caller gets the answer in its
    // registration echo.
    reg.state = (captureSite == CaptureSite::kReachable) ? STATE_ARMED
                                                        : STATE_NOT_APPLICABLE;
    reg.capturePending = false;
}

void VkVideoEncoderContentProbe::ForgetRegistration(uint64_t registrationId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_registrations.erase(registrationId);
    m_verdicts.erase(registrationId);
    m_damagedOrder.erase(
        std::remove(m_damagedOrder.begin(), m_damagedOrder.end(), registrationId),
        m_damagedOrder.end());
    // m_probedCount / m_damagedCount are SESSION TOTALS and are deliberately
    // not decremented: "three of the buffers this session imported were
    // damaged" stays true after those three are retired, and a consumer
    // reading a falling total would conclude the damage went away.
}

VkVideoEncoderContentProbe::State
VkVideoEncoderContentProbe::GetRegistrationState(uint64_t registrationId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_registrations.find(registrationId);
    return (it == m_registrations.end()) ? STATE_NOT_EVALUATED : it->second.state;
}

bool VkVideoEncoderContentProbe::IsArmed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_registrations.empty();
}

bool VkVideoEncoderContentProbe::NeedsCapture(uint64_t registrationId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_registrations.find(registrationId);
    return (it != m_registrations.end()) && (it->second.state == STATE_ARMED) &&
           !it->second.capturePending;
}

bool VkVideoEncoderContentProbe::RecordCapture(VkCommandBuffer cmdBuf,
                                               uint64_t registrationId,
                                               VkImage srcImage,
                                               VkFormat srcFormat,
                                               const VkExtent2D& srcExtent,
                                               FrameCapture& outCapture)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if ((m_vkDevCtx == nullptr) || (cmdBuf == VK_NULL_HANDLE) ||
        (srcImage == VK_NULL_HANDLE)) {
        return false;
    }
    auto it = m_registrations.find(registrationId);
    if ((it == m_registrations.end()) || (it->second.state != STATE_ARMED) ||
        it->second.capturePending) {
        return false;
    }
    if (!IsProbeableFormatLocked(srcFormat)) {
        // Latched, not returned bare: without this the caller would ask again
        // on every frame of a session whose format can never be scored.
        it->second.state = STATE_NOT_APPLICABLE;
        return false;
    }

    const uint32_t w = std::min(srcExtent.width, m_encodeExtent.width);
    const uint32_t h = std::min(srcExtent.height, m_encodeExtent.height);
    if ((w == 0) || (h == 0)) {
        it->second.state = STATE_NOT_APPLICABLE;
        return false;
    }

    // Lazily configured, so a session that arms nothing allocates nothing.
    // The extent is the first probed registration's; a later registration
    // larger than the pool is refused rather than truncated, because a
    // truncated read scores a region the producer may legitimately not have
    // written and the whole point is that the score is trustworthy.
    if (m_pool == nullptr) {
        m_poolFormat = srcFormat;
        m_poolExtent.width = srcExtent.width;
        m_poolExtent.height = srcExtent.height;
        VkResult r = VulkanVideoImagePool::Create(m_vkDevCtx, m_pool);
        if (r == VK_SUCCESS) {
            r = m_pool->Configure(
                m_vkDevCtx, m_poolDepth, m_poolFormat, m_poolExtent,
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                m_queueFamilyIndex,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                nullptr, VK_IMAGE_ASPECT_COLOR_BIT, false, false, true);
        }
        if (r != VK_SUCCESS) {
            m_pool = nullptr;
            it->second.state = STATE_NOT_APPLICABLE;
            return false;
        }
    }
    if ((srcFormat != m_poolFormat) ||
        (srcExtent.width > m_poolExtent.width) ||
        (srcExtent.height > m_poolExtent.height)) {
        it->second.state = STATE_NOT_APPLICABLE;
        return false;
    }

    VkSharedBaseObj<VulkanVideoImagePoolNode> node;
    if (!m_pool->GetAvailableImage(node, VK_IMAGE_LAYOUT_UNDEFINED)) {
        // Transient: every node is still held by an unscored capture. Leave
        // the registration ARMED so the next frame retries -- this is the one
        // refusal that is not a latch.
        return false;
    }
    VkSharedBaseObj<VkImageResourceView> dstView;
    node->GetImageView(dstView);
    if (!dstView) {
        return false;
    }
    const VkImage dstImage = dstView->GetImageResource()->GetImage();

    // ORDERING AND LAYOUT, and why only the destination needs a barrier.
    // This is recorded into the staging command buffer immediately after
    // CopyLinearToOptimalImage read |srcImage|, so the source is already in
    // TRANSFER_SRC_OPTIMAL and already owned by this queue family. What is
    // needed is (a) the destination brought to TRANSFER_DST_OPTIMAL from
    // UNDEFINED, and (b) a TRANSFER->TRANSFER execution dependency so this
    // copy is ordered after the library's own read of the same image.
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

    VkImageCopy luma = { { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 }, { 0, 0, 0 },
                         { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 }, { 0, 0, 0 },
                         { w, h, 1 } };
    m_vkDevCtx->CmdCopyImage(cmdBuf, srcImage,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &luma);
    VkImageCopy chroma = { { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 }, { 0, 0, 0 },
                           { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 }, { 0, 0, 0 },
                           { (w + 1) / 2, (h + 1) / 2, 1 } };
    m_vkDevCtx->CmdCopyImage(cmdBuf, srcImage,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &chroma);

    it->second.capturePending = true;
    outCapture.image = node;
    outCapture.registrationId = registrationId;
    outCapture.width = w;
    outCapture.height = h;
    return true;
}

void VkVideoEncoderContentProbe::ScoreCapture(FrameCapture& capture)
{
    if (capture.image == nullptr) {
        return;
    }
    // Released on EVERY exit below, including the failure ones: a node that
    // is never released is a pool slot gone for the life of the session, and
    // the pool is sized for the number of concurrent probes and nothing more.
    struct Release {
        FrameCapture* c;
        ~Release() { c->image = nullptr; }
    } release{ &capture };

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_vkDevCtx == nullptr) {
        return;
    }
    auto it = m_registrations.find(capture.registrationId);
    if (it == m_registrations.end()) {
        // Unregistered between the record and the fence. Nothing to report to.
        return;
    }
    it->second.capturePending = false;
    if (it->second.state != STATE_ARMED) {
        return;
    }

    VkSharedBaseObj<VkImageResourceView> view;
    capture.image->GetImageView(view);
    if (!view) {
        return;
    }
    const VkSharedBaseObj<VkImageResource>& res = view->GetImageResource();
    VkDevice device = m_vkDevCtx->getDevice();
    void* mapped = nullptr;
    if ((m_vkDevCtx->MapMemory(device, res->GetDeviceMemory(),
                               res->GetImageDeviceMemoryOffset(),
                               res->GetImageDeviceMemorySize(), 0,
                               &mapped) != VK_SUCCESS) ||
        (mapped == nullptr)) {
        return;
    }

    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    const uint32_t w = capture.width;
    const uint32_t h = capture.height;

    VkImage img = res->GetImage();
    VkImageSubresource sub = {};
    VkSubresourceLayout ly = {};
    VkSubresourceLayout lc = {};
    sub.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    m_vkDevCtx->GetImageSubresourceLayout(device, img, &sub, &ly);
    sub.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    m_vkDevCtx->GetImageSubresourceLayout(device, img, &sub, &lc);

    uint32_t meanY = 0, meanU = 0, meanV = 0;
    ScorePlanesQ8(base, ly, lc, w, h, m_scratch, meanY, meanU, meanV);
    m_vkDevCtx->UnmapMemory(device, res->GetDeviceMemory());

    if ((w == 0) || (h == 0)) {
        return;
    }
    ApplyVerdictLocked(capture.registrationId, meanY, meanU, meanV);
}

void VkVideoEncoderContentProbe::ScorePlanesQ8(
    const uint8_t* base, const VkSubresourceLayout& lumaLayout,
    const VkSubresourceLayout& chromaLayout, uint32_t width, uint32_t height,
    std::vector<uint8_t>& scratch, uint32_t& outMeanYQ8, uint32_t& outMeanUQ8,
    uint32_t& outMeanVQ8)
{
    outMeanYQ8 = 0;
    outMeanUQ8 = 0;
    outMeanVQ8 = 0;
    if ((base == nullptr) || (width == 0) || (height == 0)) {
        return;
    }
    const uint32_t cw = (width + 1) / 2;
    const uint32_t ch = (height + 1) / 2;

    // See kRowStride's comment: the bulk memcpy into cached scratch plus a
    // strided row set is what keeps this off the ~200 fps -> 3.6 fps cliff a
    // byte-wise walk of the same memory already fell off once in this tree.
    scratch.resize((size_t)std::max(width, 2u * cw) + 64u);
    uint8_t* row = scratch.data();

    uint64_t sumY = 0, nY = 0;
    for (uint32_t y = 0; y < height; y += kRowStride) {
        memcpy(row, base + lumaLayout.offset + ((size_t)y * lumaLayout.rowPitch),
               width);
        for (uint32_t x = 0; x < width; x++) {
            sumY += row[x];
        }
        nY += width;
    }
    uint64_t sumU = 0, sumV = 0, nC = 0;
    for (uint32_t y = 0; y < ch; y += kRowStride) {
        memcpy(row,
               base + chromaLayout.offset + ((size_t)y * chromaLayout.rowPitch),
               (size_t)2 * cw);
        for (uint32_t x = 0; x < cw; x++) {
            sumU += row[(2 * x) + 0];
            sumV += row[(2 * x) + 1];
        }
        nC += cw;
    }
    if (nY != 0) {
        outMeanYQ8 = (uint32_t)((sumY * 256u) / nY);
    }
    if (nC != 0) {
        outMeanUQ8 = (uint32_t)((sumU * 256u) / nC);
        outMeanVQ8 = (uint32_t)((sumV * 256u) / nC);
    }
}

// ===== THE PREDICATE =====
//
// The UNION of the two measured damage modes, and it is a union on purpose.
// A chroma-only scorer -- which is what this tree's PSNR readback has,
// `if ((u == 0) && (v == 0)) zeroUV++` with no luma term at all -- reports an
// ALL_ZERO buffer and a CHROMA_ZERO buffer identically, and that conflation
// has produced a wrong conclusion on this defect twice. The two need the same
// reaction and different explanations, so they get different states.
//
// THE UNCOVERED QUADRANT, named rather than left to be discovered: luma dead
// with LIVE chroma scores CLEAN here. That is deliberate. It is not one of the
// two modes this driver defect has ever produced (the chroma plane is either
// dead with luma alive, or everything is dead), and a frame with a
// legitimately black luma plane over live chroma is a real thing a producer
// can send. Adding it would trade a false negative nobody has observed for a
// false positive that reroutes a working buffer.
//
// AND A FULLY LEGAL BLACK FRAME DOES NOT TRIP IT, which is the whole reason a
// content test is admissible here: black is U = V = 128, so meanU and meanV
// are 32768 in these units -- sixty-four times the threshold.
VkVideoEncoderContentProbe::State
VkVideoEncoderContentProbe::ClassifyPlaneMeansQ8(uint32_t meanYQ8,
                                                 uint32_t meanUQ8,
                                                 uint32_t meanVQ8)
{
    const bool yDead = (meanYQ8 < kDeadPlaneMeanQ8);
    const bool uDead = (meanUQ8 < kDeadPlaneMeanQ8);
    const bool vDead = (meanVQ8 < kDeadPlaneMeanQ8);
    if (yDead && uDead && vDead) {
        return STATE_DAMAGED_ALL;
    }
    if (!yDead && (uDead || vDead)) {
        return STATE_DAMAGED_CHROMA;
    }
    return STATE_CLEAN;
}

void VkVideoEncoderContentProbe::ApplyVerdict(uint64_t registrationId,
                                              uint32_t meanYQ8,
                                              uint32_t meanUQ8,
                                              uint32_t meanVQ8)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ApplyVerdictLocked(registrationId, meanYQ8, meanUQ8, meanVQ8);
}

void VkVideoEncoderContentProbe::ApplyVerdictLocked(uint64_t registrationId,
                                                    uint32_t meanYQ8,
                                                    uint32_t meanUQ8,
                                                    uint32_t meanVQ8)
{
    auto it = m_registrations.find(registrationId);
    if ((it == m_registrations.end()) || (it->second.state != STATE_ARMED)) {
        // Not armed, already scored, or retired between the record and the
        // fence. A second verdict for one registration must never move the
        // session counters: the probe's unit is the buffer, not the frame.
        return;
    }
    Verdict verdict;
    verdict.registrationId = registrationId;
    verdict.meanY = meanYQ8;
    verdict.meanU = meanUQ8;
    verdict.meanV = meanVQ8;
    verdict.state = ClassifyPlaneMeansQ8(meanYQ8, meanUQ8, meanVQ8);

    it->second.state = verdict.state;
    m_verdicts[registrationId] = verdict;
    m_probedCount++;
    if (verdict.state == STATE_CLEAN) {
        m_lastCleanVerdict = verdict;
    } else {
        m_damagedCount++;
        m_damagedOrder.push_back(registrationId);
    }
}

void VkVideoEncoderContentProbe::GetSnapshot(Verdict& outVerdict,
                                             uint32_t& outProbedCount,
                                             uint32_t& outDamagedCount,
                                             uint32_t& outArmedCount) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    outProbedCount = m_probedCount;
    outDamagedCount = m_damagedCount;
    // COUNTED HERE, NOT TRACKED AS A RUNNING TALLY. Every transition out of
    // STATE_ARMED already exists in three places (ApplyVerdictLocked's
    // verdict, RecordCapture's two NOT_APPLICABLE latches) and
    // ForgetRegistration removes entries outright; a counter incremented and
    // decremented across four sites is a counter that drifts. m_registrations
    // is small by construction -- it is one entry per REGISTERED BUFFER, five
    // to eleven on the owner's measured sessions, not one per frame -- so
    // walking it under a lock already held costs nothing worth naming.
    outArmedCount = 0;
    for (const auto& entry : m_registrations) {
        if (entry.second.state == STATE_ARMED) {
            outArmedCount++;
        }
    }
    // OLDEST STILL-REGISTERED DAMAGED FIRST. A caller reacts by retiring that
    // registration, ForgetRegistration drops it from m_damagedOrder, and the
    // next poll surfaces the next one. A caller that does NOT react sees the
    // same entry again -- so a verdict is never consumed by being read, and
    // two buffers damaged between two polls both get reported. "Most recent"
    // would have lost the older one permanently, because a registration is
    // probed exactly once and there is no second look coming.
    for (uint64_t id : m_damagedOrder) {
        auto v = m_verdicts.find(id);
        if (v != m_verdicts.end()) {
            outVerdict = v->second;
            return;
        }
    }
    outVerdict = m_lastCleanVerdict;
}

void VkVideoEncoderContentProbe::Deinit()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pool = nullptr;
    m_registrations.clear();
    m_verdicts.clear();
    m_damagedOrder.clear();
    m_scratch.clear();
    m_scratch.shrink_to_fit();
}
