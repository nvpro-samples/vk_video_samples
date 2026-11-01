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
 */
#ifndef VVS_DPB_HPP_
#define VVS_DPB_HPP_ 1

#include <cstdint>
#include <utility>

#include "vvs_dpb.h"

namespace vvs {
namespace dpb {

/* Result storage. Its pointers point into itself, so it is neither copied nor moved: keep it where
 * the picture lives, in the encoder's frame. */
struct PictureResult : VvsDpbPictureResult {
    PictureResult() : VvsDpbPictureResult() {}
    PictureResult(const PictureResult&) = delete;
    PictureResult& operator=(const PictureResult&) = delete;
};

/* Syntax storage; the Std picture information points into it. */
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

/* A context, owned. The lifecycle common to every codec. */
class Context {
   public:
    Context() = default;
    ~Context() { vvsDpbDestroy(m_dpb, m_pAllocator); }

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

    explicit operator bool() const { return m_dpb != nullptr; }
    VvsDpb Handle() const { return m_dpb; }

    VkResult Reset(const VvsDpbResetInfo* pResetInfo = nullptr, VvsDpbResourceList* pReleases = nullptr) {
        return vvsDpbReset(m_dpb, pResetInfo, pReleases);
    }
    void SaveCheckpoint(Checkpoint& checkpoint) const { vvsDpbSaveCheckpoint(m_dpb, &checkpoint); }
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
    VkResult GetReferenceFrames(uint32_t* pCount, VvsDpbReferenceFrame* pFrames) const {
        return vvsDpbGetReferenceFrames(m_dpb, pCount, pFrames);
    }

   protected:
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

/* The calls specialized to each codec's API: one class per codec operation. */

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
    VkResult ShowExistingFrame(uint64_t displayOrderIndex, uint8_t* pFrameToShowMapIdx, PictureResult& result) {
        return vvsDpbEncodeAV1ShowExistingFrame(m_dpb, displayOrderIndex, pFrameToShowMapIdx, &result);
    }
};

/* The results into the video commands, without copying. */

inline void SetBeginCodingSlots(const VvsDpbPictureSlots& slots, VkVideoBeginCodingInfoKHR& beginInfo) {
    vvsDpbSetBeginCodingSlots(&slots, &beginInfo);
}

inline void SetEncodeSlots(const VvsDpbPictureSlots& slots, VkVideoEncodeInfoKHR& encodeInfo) {
    vvsDpbSetEncodeSlots(&slots, &encodeInfo);
}

}  // namespace dpb
}  // namespace vvs

#endif /* VVS_DPB_HPP_ */
