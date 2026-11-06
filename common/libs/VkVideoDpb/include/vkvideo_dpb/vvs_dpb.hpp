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

/*
 * vvs_dpb.hpp: the C++17 interface of vkvideo-dpb, a header-only layer over vvs_dpb.h.
 *
 * The C header is the contract; this layer adds ownership and makes a call for another codec a
 * compile error instead of a VK_ERROR_VALIDATION_FAILED. It uses the C structures as they are, so
 * nothing is converted between the two. No exceptions, no RTTI, no static or global variables.
 *
 * Every call returns the C call's VkResult, with the meaning vvs_dpb.h gives it, and a call that fails
 * changes nothing. One object is used from one thread at a time, as one C context is.
 *
 *   vvs::dpb::H264Encode dpb;                    // or H265Encode, AV1Encode
 *   dpb.Create(createInfo);                      // VvsDpbCreateInfo, as for vvsDpbCreate
 *   dpb.Sequence(sequenceInfo, &requirements);   // the codec's VvsDpb<Codec>SequenceInfo
 *   dpb.Picture(pictureInfo, frame.dpbResult);   // per picture, in encode order
 *   vvs::dpb::SetEncodeSlots(frame.dpbResult.slots, encodeInfo);
 *   // the context is destroyed with the object
 */
#ifndef VVS_DPB_HPP_
#define VVS_DPB_HPP_ 1

#include <cstdint>
#include <utility>

#include "vvs_dpb.h"

namespace vvs {
namespace dpb {

/* Result storage. Its pointers point into itself, so it is neither copied nor moved: keep it where
 * the picture lives, in the encoder's frame. It starts with every member zero. */
struct PictureResult : VvsDpbPictureResult {
    PictureResult() : VvsDpbPictureResult() {}
    PictureResult(const PictureResult&) = delete;
    PictureResult& operator=(const PictureResult&) = delete;
};

/* Syntax storage; the Std picture information points into it, so it is neither copied nor moved
 * while the picture's commands are recorded. It starts with every member zero. */
struct H264EncodeSyntax : VvsDpbH264EncodeSyntax {
    H264EncodeSyntax() : VvsDpbH264EncodeSyntax() {}
    H264EncodeSyntax(const H264EncodeSyntax&) = delete;
    H264EncodeSyntax& operator=(const H264EncodeSyntax&) = delete;
};

struct H265EncodeSyntax : VvsDpbH265EncodeSyntax {
    H265EncodeSyntax() : VvsDpbH265EncodeSyntax() {}
    H265EncodeSyntax(const H265EncodeSyntax&) = delete;
    H265EncodeSyntax& operator=(const H265EncodeSyntax&) = delete;
};

/* A checkpoint is plain data: copy it, keep it in an array, drop it. */
using Checkpoint = VvsDpbCheckpoint;

/* A context, owned. The lifecycle common to every codec. An object starts empty (no context);
 * the codec classes below create one. The destructor and a move assignment destroy the context the
 * object holds, with the allocator it was created with: no resource callback is called, so a client
 * that wants its resources back calls Reset() first. */
class Context {
   public:
    Context() = default;
    ~Context() { vvsDpbDestroy(m_dpb, m_pAllocator); }

    /* Moving leaves the source empty. */
    Context(Context&& other) noexcept : m_dpb(std::exchange(other.m_dpb, nullptr)), m_pAllocator(other.m_pAllocator) {}
    Context& operator=(Context&& other) noexcept {
        if (this != &other) {
            vvsDpbDestroy(m_dpb, m_pAllocator);
            m_dpb = std::exchange(other.m_dpb, nullptr);
            m_pAllocator = other.m_pAllocator;
        }
        return *this;
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    /* Whether the object holds a context. */
    explicit operator bool() const { return m_dpb != nullptr; }
    /* The C handle, for the C calls; it stays owned by the object. */
    VvsDpb Handle() const { return m_dpb; }

    /* vvsDpbReset: drops every picture; pResetInfo NULL keeps the bound and the resources. */
    VkResult Reset(const VvsDpbResetInfo* pResetInfo = nullptr, VvsDpbResourceList* pReleases = nullptr) {
        return vvsDpbReset(m_dpb, pResetInfo, pReleases);
    }
    /* vvsDpbSaveCheckpoint: the context's current state, into the client's checkpoint. */
    void SaveCheckpoint(Checkpoint& checkpoint) const { vvsDpbSaveCheckpoint(m_dpb, &checkpoint); }
    /* vvsDpbRewind, with either target. */
    VkResult Rewind(const VvsDpbRewindInfo& rewindInfo, VvsDpbResourceList* pReleases = nullptr) {
        return vvsDpbRewind(m_dpb, &rewindInfo, pReleases);
    }
    // Undoes the last pictureCount picture calls, from the context's own history.
    VkResult RewindPictures(uint32_t pictureCount, VvsDpbResourceList* pReleases = nullptr) {
        const VvsDpbRewindInfo rewindInfo{pictureCount, nullptr};
        return Rewind(rewindInfo, pReleases);
    }
    // Returns to a saved checkpoint.
    VkResult RewindTo(const Checkpoint& checkpoint, VvsDpbResourceList* pReleases = nullptr) {
        const VvsDpbRewindInfo rewindInfo{0, &checkpoint};
        return Rewind(rewindInfo, pReleases);
    }
    /* vvsDpbGetReferenceFrames: the pictures the DPB holds, with Vulkan's two-call idiom. */
    VkResult GetReferenceFrames(uint32_t* pCount, VvsDpbReferenceFrame* pFrames) const {
        return vvsDpbGetReferenceFrames(m_dpb, pCount, pFrames);
    }

   protected:
    /* Creates a context for one codec operation: VK_ERROR_VALIDATION_FAILED, and the object unchanged,
     * for a profile of another operation. On success the object's previous context, if any, is
     * destroyed and the new one takes its place; pAllocator is kept for the destruction. */
    VkResult CreateFor(VkVideoCodecOperationFlagBitsKHR operation, const VvsDpbCreateInfo& createInfo,
                       const VkAllocationCallbacks* pAllocator) {
        if ((createInfo.pVideoProfile == nullptr) || (createInfo.pVideoProfile->videoCodecOperation != operation)) {
            return VK_ERROR_VALIDATION_FAILED;
        }
        VvsDpb dpb = nullptr;
        const VkResult result = vvsDpbCreate(&createInfo, pAllocator, &dpb);
        if (result == VK_SUCCESS) {
            vvsDpbDestroy(m_dpb, m_pAllocator);
            m_dpb = dpb;
            m_pAllocator = pAllocator;
        }
        return result;
    }

    VvsDpb m_dpb = nullptr;
    const VkAllocationCallbacks* m_pAllocator = nullptr;
};

/* The calls specialized to each codec's API: one class per codec operation. Create() takes the
 * VvsDpbCreateInfo of vvsDpbCreate and refuses a pVideoProfile of another operation; Sequence() and
 * Picture() take only the codec's own structures, so a call for another codec does not compile. */

/* H.264 encode: vvsDpbH264Sequence and vvsDpbEncodeH264Picture. */
class H264Encode : public Context {
   public:
    static constexpr VkVideoCodecOperationFlagBitsKHR kOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    VkResult Create(const VvsDpbCreateInfo& createInfo, const VkAllocationCallbacks* pAllocator = nullptr) {
        return CreateFor(kOperation, createInfo, pAllocator);
    }
    VkResult Sequence(const VvsDpbH264SequenceInfo& sequenceInfo, VvsDpbSequenceRequirements* pRequirements) {
        return vvsDpbH264Sequence(m_dpb, &sequenceInfo, pRequirements);
    }
    VkResult Picture(const VvsDpbH264EncodePictureInfo& pictureInfo, PictureResult& result) {
        return vvsDpbEncodeH264Picture(m_dpb, &pictureInfo, &result);
    }
};

/* H.265 encode: vvsDpbH265Sequence and vvsDpbEncodeH265Picture. */
class H265Encode : public Context {
   public:
    static constexpr VkVideoCodecOperationFlagBitsKHR kOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    VkResult Create(const VvsDpbCreateInfo& createInfo, const VkAllocationCallbacks* pAllocator = nullptr) {
        return CreateFor(kOperation, createInfo, pAllocator);
    }
    VkResult Sequence(const VvsDpbH265SequenceInfo& sequenceInfo, VvsDpbSequenceRequirements* pRequirements) {
        return vvsDpbH265Sequence(m_dpb, &sequenceInfo, pRequirements);
    }
    VkResult Picture(const VvsDpbH265EncodePictureInfo& pictureInfo, PictureResult& result) {
        return vvsDpbEncodeH265Picture(m_dpb, &pictureInfo, &result);
    }
};

/* AV1 encode: vvsDpbAV1Sequence, vvsDpbEncodeAV1Picture and vvsDpbEncodeAV1ShowExistingFrame. */
class AV1Encode : public Context {
   public:
    static constexpr VkVideoCodecOperationFlagBitsKHR kOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    VkResult Create(const VvsDpbCreateInfo& createInfo, const VkAllocationCallbacks* pAllocator = nullptr) {
        return CreateFor(kOperation, createInfo, pAllocator);
    }
    VkResult Sequence(const VvsDpbAV1SequenceInfo& sequenceInfo, VvsDpbSequenceRequirements* pRequirements) {
        return vvsDpbAV1Sequence(m_dpb, &sequenceInfo, pRequirements);
    }
    VkResult Picture(const VvsDpbAV1EncodePictureInfo& pictureInfo, PictureResult& result) {
        return vvsDpbEncodeAV1Picture(m_dpb, &pictureInfo, &result);
    }
    /* A frame header that shows the frame with this displayOrderIndex: *pFrameToShowMapIdx receives the
     * buffer for frame_to_show_map_idx. No video command encodes it. */
    VkResult ShowExistingFrame(uint64_t displayOrderIndex, uint8_t* pFrameToShowMapIdx, PictureResult& result) {
        return vvsDpbEncodeAV1ShowExistingFrame(m_dpb, displayOrderIndex, pFrameToShowMapIdx, &result);
    }
};

/* The results into the video commands, without copying. */

/* Points beginInfo's referenceSlotCount and pReferenceSlots at the picture's references and setup
 * resource. */
inline void SetBeginCodingSlots(const VvsDpbPictureSlots& slots, VkVideoBeginCodingInfoKHR& beginInfo) {
    vvsDpbSetBeginCodingSlots(&slots, &beginInfo);
}

/* Points encodeInfo's pSetupReferenceSlot, referenceSlotCount and pReferenceSlots at the picture's
 * setup slot and references. */
inline void SetEncodeSlots(const VvsDpbPictureSlots& slots, VkVideoEncodeInfoKHR& encodeInfo) {
    vvsDpbSetEncodeSlots(&slots, &encodeInfo);
}

}  // namespace dpb
}  // namespace vvs

#endif /* VVS_DPB_HPP_ */
