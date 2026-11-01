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

// vk-video-encoder-dpb-trace drives the encoder's DPB, the DPB library vkvideo-dpb, through a GOP
// sequence with no Vulkan device, making the calls the encoder makes for each picture, and writes
// what it decides: the setup slot, the reference lists or reference names, and the slots and Std
// reference information of the references.
//
// The harness stands in for the encoder where it feeds the DPB, and follows it line by line:
//   - the configuration: EncoderConfigH264::InitSpsPpsParameters and InitDpbCount
//     (VkEncoderConfigH264.cpp), EncoderConfigH265::InitDpbCount, InitializeSpsRefPicSet and the
//     reference fields of InitParamameters (VkEncoderConfigH265.cpp), EncoderConfigAV1::InitDpbCount
//     and InitSequenceHeader (VkEncoderConfigAV1.cpp), and the DPB size VkVideoEncoder::InitEncoder
//     passes on (VkVideoEncoder.cpp);
//   - the order: VkVideoEncoder::EncodeFrame takes each input frame's GOP position, and
//     EnqueueFrame and InsertOrdered (VkVideoEncoder.h) defer it in GOP encode order until an IDR,
//     the last frame, a reference frame or a GOP without B frames flushes the queue, and
//     VkVideoEncoderAV1::InsertOrdered adds the show_existing_frame of a reordered AV1 frame;
//   - each picture: VkVideoEncoderH264::EncodeFrame and ProcessDpb, VkVideoEncoderH265::EncodeFrame
//     and ProcessDpb, VkVideoEncoderAV1::EncodeFrame, InitializeFrameHeader and ProcessDpb.
// A change to any of them is a change this harness has to follow.
//
// scripts/encoder_dpb_model.py checks the trace against the H.264 and H.265 reference picture
// rules and the AV1 reference buffers. See ../README.md.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "VkVideoEncoder/VkEncoderConfigAV1.h"
#include "VkVideoEncoder/VkVideoGopStructure.h"
#include "vkvideo_dpb/vvs_dpb.hpp"

namespace {

struct Settings {
    std::string codec;
    uint32_t frames = 60;
    uint32_t gopFrameCount = 16;
    int32_t idrPeriod = 60;
    uint32_t bFrames = 3;
    bool closedGop = false;
    uint32_t refsL0 = 1;
    uint32_t refsL1 = 1;
    // H.264: the DPB size the level allows for the picture size (levelLimits[].maxDPB), at most 16.
    uint32_t levelDpbSize = 16;
    // The device's VkVideoCapabilitiesKHR::maxDpbSlots.
    uint32_t maxDpbSlots = 17;
    // AV1: the device's VkVideoCapabilitiesKHR::maxActiveReferencePictures and the reference
    // capabilities of VkVideoEncodeAV1CapabilitiesKHR. The defaults are those of a device that
    // allows every reference name.
    uint32_t maxActiveRefs = 7;
    uint32_t av1Single[2] = {7, 0x7f};       // maxSingleReferenceCount, singleReferenceNameMask
    uint32_t av1Unidir[3] = {4, 4, 0xf};     // count, group 1 count, name mask
    uint32_t av1Bidir[4] = {7, 4, 3, 0x7f};  // count, group 1 count, group 2 count, name mask
    // VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR.
    bool av1CdfOnly = true;
};

// One trace line: a tag and key=value tokens, arrays as [a,b], flags as a|b or "-".
class Line {
   public:
    explicit Line(const char* tag) : m_text(tag) {}
    Line& Num(const char* key, int64_t value) {
        m_text += std::string(" ") + key + "=" + std::to_string(value);
        return *this;
    }
    Line& Str(const char* key, const char* value) {
        m_text += std::string(" ") + key + "=" + value;
        return *this;
    }
    template <typename T>
    Line& Array(const char* key, const T* values, size_t count) {
        m_text += std::string(" ") + key + "=[";
        for (size_t i = 0; i < count; i++) {
            m_text += ((i != 0) ? "," : "") + std::to_string(static_cast<int64_t>(values[i]));
        }
        m_text += "]";
        return *this;
    }
    // A flag's value is copied in, so a Std bit-field can be passed as it is.
    struct Flag {
        const char* name;
        uint32_t value;
    };
    Line& Flags(const char* key, std::initializer_list<Flag> flags) {
        std::string set;
        for (const Flag& flag : flags) {
            if (flag.value != 0) {
                set += (set.empty() ? "" : "|") + std::string(flag.name);
            }
        }
        return Str(key, set.empty() ? "-" : set.c_str());
    }
    void Emit(FILE* out) const { fprintf(out, "%s\n", m_text.c_str()); }

   private:
    std::string m_text;
};

const char* FrameTypeName(VkVideoGopStructure::FrameType type) { return VkVideoGopStructure::GetFrameTypeName(type); }

// A picture of the sequence, from its input to its DPB processing.
struct Frame {
    explicit Frame(uint32_t positionInInputOrder) : gop(positionInInputOrder) {}
    VkVideoGopStructure::GopPosition gop;
    uint32_t inputNum = 0;
    bool isIdr = false;
    bool isReference = false;
    bool lastFrame = false;
    uint64_t picOrderCntVal = 0;
    StdVideoEncodeH264PictureInfo h264Info{};
    StdVideoEncodeH264SliceHeader h264Slice{};
    StdVideoEncodeH265PictureInfo h265Info{};
    // AV1: VkVideoEncodeFrameInfoAV1's members.
    StdVideoEncodeAV1PictureInfo av1Info{};
    bool showExistingFrame = false;
};

using FrameQueue = std::vector<std::unique_ptr<Frame>>;

class CodecDriver {
   public:
    virtual ~CodecDriver() = default;
    virtual void EncodeFrame(Frame& frame) = 0;
    virtual void ProcessDpb(Frame& frame, uint32_t encodeNum) = 0;

    // VkVideoEncoder::InsertOrdered: before the first deferred frame whose GOP encode order is not
    // lower.
    virtual void InsertOrdered(FrameQueue& deferred, std::unique_ptr<Frame> frame) {
        auto at = std::find_if(deferred.begin(), deferred.end(), [&](const std::unique_ptr<Frame>& other) {
            return other->gop.encodeOrder >= frame->gop.encodeOrder;
        });
        deferred.insert(at, std::move(frame));
    }
};

// The library's picture resources in the trace: each picture gets its own resource index, and
// owner[index] is the input number of the picture it was handed to, so a reference's pic is the
// picture the DPB set up in its slot. A release and a retain change nothing.
struct Owners {
    std::vector<int32_t> owner;
    uint32_t input = 0;
    VkVideoPictureResourceInfoKHR resource{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};

    static VkResult VKAPI_CALL Acquire(void* pUserData, const VvsDpbPictureInfo*, uint32_t* pResourceIndex,
                                       const VkVideoPictureResourceInfoKHR** ppResource) {
        Owners* pOwners = static_cast<Owners*>(pUserData);
        *pResourceIndex = static_cast<uint32_t>(pOwners->owner.size());
        pOwners->owner.push_back(static_cast<int32_t>(pOwners->input));
        *ppResource = &pOwners->resource;
        return VK_SUCCESS;
    }
    static void VKAPI_CALL Keep(void*, uint32_t) {}
};

// VkVideoEncoderH264 with EncoderConfigH264.
class H264Driver : public CodecDriver {
   public:
    H264Driver(const Settings& settings, const VkVideoGopStructure& gop, FILE* out) : m_out(out) {
        // EncoderConfigH264::InitDpbCount, then VkVideoEncoder::InitEncoder's min(.., 16u, maxDpbSlots),
        // which it writes back to dpbCount.
        const uint32_t numRefFrames = settings.refsL0 + settings.refsL1;
        const uint32_t dpbCount = ((numRefFrames > 0) ? std::min(numRefFrames, settings.levelDpbSize) : settings.levelDpbSize) + 1;
        m_maxDpbPicturesCount = std::min({dpbCount, 16u, settings.maxDpbSlots});

        // EncoderConfigH264::InitSpsPpsParameters: the fields the DPB and the reference lists read.
        m_sps.flags.frame_mbs_only_flag = 1;
        m_sps.log2_max_frame_num_minus4 = 4;
        m_sps.log2_max_pic_order_cnt_lsb_minus4 = 4;
        m_sps.max_num_ref_frames = static_cast<uint8_t>(settings.refsL0 + settings.refsL1);
        m_pps.num_ref_idx_l0_default_active_minus1 = static_cast<uint8_t>((settings.refsL0 > 0) ? settings.refsL0 - 1 : 0);
        m_pps.num_ref_idx_l1_default_active_minus1 = static_cast<uint8_t>((settings.refsL1 > 0) ? settings.refsL1 - 1 : 0);
        if ((m_sps.max_num_ref_frames == 0) || (m_sps.max_num_ref_frames <= m_pps.num_ref_idx_l0_default_active_minus1)) {
            m_sps.max_num_ref_frames = static_cast<uint8_t>(m_pps.num_ref_idx_l0_default_active_minus1 + 1);
            if (gop.GetConsecutiveBFrameCount() > 0) {
                m_sps.max_num_ref_frames =
                    static_cast<uint8_t>(m_sps.max_num_ref_frames + m_pps.num_ref_idx_l1_default_active_minus1 + 1U);
            }
        }
        m_sps.max_num_ref_frames =
            std::min(m_sps.max_num_ref_frames, static_cast<uint8_t>(std::max(static_cast<int32_t>(m_maxDpbPicturesCount) - 1, 0)));
        m_sps.pic_order_cnt_type = (gop.GetConsecutiveBFrameCount() > 0) ? STD_VIDEO_H264_POC_TYPE_0 : STD_VIDEO_H264_POC_TYPE_2;

        {
            // VkVideoEncoderH264::InitEncoderCodec: the session's slots bound the DPB, and every
            // picture is set up. The device's maxActiveReferencePictures is 16.
            m_profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
            VvsDpbCreateInfo createInfo{};
            createInfo.apiVersion = VVS_DPB_API_VERSION;
            createInfo.flags = VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT;
            createInfo.pVideoProfile = &m_profile;
            createInfo.maxDpbSlots = m_maxDpbPicturesCount;
            createInfo.maxActiveReferencePictures = 16;
            createInfo.slotPolicy = VVS_DPB_SLOT_POLICY_LOWEST_FREE;
            createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_CALLBACK;
            createInfo.resources.callbacks.pUserData = &m_owners;
            createInfo.resources.callbacks.pfnAcquireResource = Owners::Acquire;
            createInfo.resources.callbacks.pfnRetainResource = Owners::Keep;
            createInfo.resources.callbacks.pfnReleaseResource = Owners::Keep;
            const VvsDpbH264SequenceInfo sequenceInfo{&m_sps, &m_pps, nullptr};
            m_libraryResult = m_library.Create(createInfo);
            if (m_libraryResult == VK_SUCCESS) {
                m_libraryResult = m_library.Sequence(sequenceInfo, nullptr);
            }
        }
        Line("config")
            .Str("codec", "h264")
            .Num("dpbSize", m_maxDpbPicturesCount)
            .Num("max_num_ref_frames", m_sps.max_num_ref_frames)
            .Num("pic_order_cnt_type", m_sps.pic_order_cnt_type)
            .Num("log2_max_frame_num", m_sps.log2_max_frame_num_minus4 + 4)
            .Num("log2_max_pic_order_cnt_lsb", m_sps.log2_max_pic_order_cnt_lsb_minus4 + 4)
            .Num("num_ref_idx_l0_default", m_pps.num_ref_idx_l0_default_active_minus1 + 1)
            .Num("num_ref_idx_l1_default", m_pps.num_ref_idx_l1_default_active_minus1 + 1)
            .Emit(m_out);
    }

    // VkVideoEncoderH264::EncodeFrame: the picture information ProcessDpb reads.
    void EncodeFrame(Frame& frame) override {
        frame.picOrderCntVal = 2 * frame.gop.inputOrder;
        StdVideoH264PictureType type = STD_VIDEO_H264_PICTURE_TYPE_INVALID;
        switch (frame.gop.pictureType) {
            case VkVideoGopStructure::FRAME_TYPE_IDR:
            case VkVideoGopStructure::FRAME_TYPE_INTRA_REFRESH:
                frame.h264Slice.slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
                type = STD_VIDEO_H264_PICTURE_TYPE_IDR;
                break;
            case VkVideoGopStructure::FRAME_TYPE_I:
                frame.h264Slice.slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
                type = STD_VIDEO_H264_PICTURE_TYPE_I;
                break;
            case VkVideoGopStructure::FRAME_TYPE_P:
                frame.h264Slice.slice_type = STD_VIDEO_H264_SLICE_TYPE_P;
                type = STD_VIDEO_H264_PICTURE_TYPE_P;
                break;
            case VkVideoGopStructure::FRAME_TYPE_B:
                frame.h264Slice.slice_type = STD_VIDEO_H264_SLICE_TYPE_B;
                type = STD_VIDEO_H264_PICTURE_TYPE_B;
                break;
            default:
                break;
        }
        frame.h264Info.flags.IdrPicFlag = (frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR);
        frame.h264Info.flags.is_reference = frame.isReference;
        frame.h264Info.primary_pic_type = type;
        if (frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) {
            frame.h264Info.idr_pic_id = static_cast<uint16_t>(m_idrPicId & 1);
            m_idrPicId++;
        }
    }

    // VkVideoEncoderH264::ProcessDpb, without the image pool and the intra refresh. Each picture gets
    // its own resource index, so a reference's pic is the input number of the picture the DPB set up
    // in it.
    void ProcessDpb(Frame& frame, uint32_t encodeNum) override {
        StdVideoEncodeH264PictureInfo stdPictureInfo = frame.h264Info;
        stdPictureInfo.PicOrderCnt = static_cast<int32_t>(frame.picOrderCntVal);
        VvsDpbH264EncodePictureInfo pictureInfo{};
        pictureInfo.dirtyIntraRefreshRegions = VVS_DPB_NO_INTRA_REFRESH;
        pictureInfo.pStdPictureInfo = &stdPictureInfo;
        pictureInfo.pSyntax = &m_syntax;
        m_owners.input = frame.inputNum;
        const VkResult result = (m_libraryResult == VK_SUCCESS) ? m_library.Picture(pictureInfo, m_result) : m_libraryResult;

        Line("pic")
            .Num("index", encodeNum)
            .Num("input", frame.inputNum)
            .Str("type", FrameTypeName(frame.gop.pictureType))
            .Num("poc", static_cast<int64_t>(frame.picOrderCntVal))
            .Num("ref", frame.h264Info.flags.is_reference)
            .Emit(m_out);
        if (result != VK_SUCCESS) {
            Line("  refused").Num("result", result).Emit(m_out);
            return;
        }
        Line("  h264")
            .Num("frame_num", stdPictureInfo.frame_num)
            .Num("PicOrderCnt", stdPictureInfo.PicOrderCnt)
            .Num("idr_pic_id", stdPictureInfo.idr_pic_id)
            .Emit(m_out);
        const StdVideoEncodeH264ReferenceListsInfo& lists = m_syntax.refLists;
        const uint32_t count0 =
            (lists.RefPicList0[0] == STD_VIDEO_H264_NO_REFERENCE_PICTURE) ? 0U : lists.num_ref_idx_l0_active_minus1 + 1U;
        const uint32_t count1 =
            (lists.RefPicList1[0] == STD_VIDEO_H264_NO_REFERENCE_PICTURE) ? 0U : lists.num_ref_idx_l1_active_minus1 + 1U;
        Line("  lists").Array("l0", lists.RefPicList0, count0).Array("l1", lists.RefPicList1, count1).Emit(m_out);

        const VvsDpbPictureSlots& slots = m_result.slots;
        for (uint32_t i = 0; i < slots.referenceSlotCount; i++) {
            TraceReferenceInfo(
                Line("  ref").Num("slot", slots.referenceSlots[i].slotIndex).Num("pic", m_owners.owner[slots.resourceIndices[i]]),
                slots.stdReferenceInfos[i].h264)
                .Emit(m_out);
        }
        StdVideoEncodeH264ReferenceInfo setupInfo{};
        int32_t setupSlot = -1;
        if (slots.pSetupReferenceSlot != nullptr) {
            setupSlot = slots.pSetupReferenceSlot->slotIndex;
            setupInfo = slots.stdReferenceInfos[slots.referenceSlotCount].h264;
        }
        TraceReferenceInfo(Line("  setup").Num("slot", setupSlot), setupInfo).Emit(m_out);
    }

   private:
    // Every member of the H.264 Std reference information.
    static Line& TraceReferenceInfo(Line& line, const StdVideoEncodeH264ReferenceInfo& info) {
        return line.Num("FrameNum", info.FrameNum)
            .Num("PicOrderCnt", info.PicOrderCnt)
            .Num("long_term_pic_num", info.long_term_pic_num)
            .Num("long_term_frame_idx", info.long_term_frame_idx)
            .Num("primary_pic_type", info.primary_pic_type)
            .Num("temporal_id", info.temporal_id)
            .Flags("flags", {{"lt", info.flags.used_for_long_term_reference}});
    }

    FILE* m_out;
    StdVideoH264SequenceParameterSet m_sps{};
    StdVideoH264PictureParameterSet m_pps{};
    uint32_t m_maxDpbPicturesCount = 0;
    uint32_t m_idrPicId = 0;
    // The library: the context, the storage it writes a picture's syntax and slots into, and the input
    // number of the picture each resource index was handed to.
    VkVideoProfileInfoKHR m_profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    vvs::dpb::H264Encode m_library;
    VkResult m_libraryResult = VK_SUCCESS;
    vvs::dpb::H264EncodeSyntax m_syntax;
    vvs::dpb::PictureResult m_result;
    Owners m_owners;
};

// VkVideoEncoderH265 with EncoderConfigH265.
class H265Driver : public CodecDriver {
   public:
    H265Driver(const Settings& settings, const VkVideoGopStructure&, FILE* out) : m_out(out) {
        // EncoderConfigH265::InitDpbCount (VerifyDpbSize keeps 5 at the levels the encoder picks),
        // then VkVideoEncoder::InitEncoder's min(.., 16u, maxDpbSlots), which it writes back to
        // dpbCount.
        const uint32_t dpbCount = std::min({5u, 16u, settings.maxDpbSlots});
        m_maxDpbPicturesCount = dpbCount;

        // EncoderConfigH265::InitParamameters and InitializeSpsRefPicSet.
        m_decPicBufMgr.max_dec_pic_buffering_minus1[0] = static_cast<uint8_t>(dpbCount - 1);
        m_sps.sps_max_sub_layers_minus1 = 0;
        m_sps.pDecPicBufMgr = &m_decPicBufMgr;
        m_sps.log2_max_pic_order_cnt_lsb_minus4 = 4;
        m_sps.flags.long_term_ref_pics_present_flag = 0;
        m_spsRps.delta_idx_minus1 = 0;
        m_spsRps.num_negative_pics = static_cast<uint8_t>(dpbCount - 1);
        m_spsRps.used_by_curr_pic_s0_flag =
            static_cast<uint16_t>((1u << std::min<uint32_t>(m_spsRps.num_negative_pics, settings.refsL0)) - 1);
        m_sps.num_short_term_ref_pic_sets = 1;
        m_sps.pShortTermRefPicSet = &m_spsRps;
        m_pps.num_ref_idx_l0_default_active_minus1 = static_cast<uint8_t>((settings.refsL0 > 0) ? settings.refsL0 - 1 : 0);
        m_pps.num_ref_idx_l1_default_active_minus1 = static_cast<uint8_t>((settings.refsL1 > 0) ? settings.refsL1 - 1 : 0);

        {
            // VkVideoEncoderH265::InitEncoderCodec: the session's slots bound the DPB, and every
            // picture is set up. The device's maxActiveReferencePictures is 16.
            m_profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
            VvsDpbCreateInfo createInfo{};
            createInfo.apiVersion = VVS_DPB_API_VERSION;
            createInfo.flags = VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT;
            createInfo.pVideoProfile = &m_profile;
            createInfo.maxDpbSlots = m_maxDpbPicturesCount;
            createInfo.maxActiveReferencePictures = 16;
            createInfo.slotPolicy = VVS_DPB_SLOT_POLICY_LOWEST_FREE;
            createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_CALLBACK;
            createInfo.resources.callbacks.pUserData = &m_owners;
            createInfo.resources.callbacks.pfnAcquireResource = Owners::Acquire;
            createInfo.resources.callbacks.pfnRetainResource = Owners::Keep;
            createInfo.resources.callbacks.pfnReleaseResource = Owners::Keep;
            const VvsDpbH265SequenceInfo sequenceInfo{&m_sps, &m_pps, nullptr};
            m_libraryResult = m_library.Create(createInfo);
            if (m_libraryResult == VK_SUCCESS) {
                m_libraryResult = m_library.Sequence(sequenceInfo, nullptr);
            }
        }
        Line("config")
            .Str("codec", "h265")
            .Num("dpbSize", m_maxDpbPicturesCount)
            .Num("log2_max_pic_order_cnt_lsb", m_sps.log2_max_pic_order_cnt_lsb_minus4 + 4)
            .Num("num_ref_idx_l0_default", (settings.refsL0 > 0) ? settings.refsL0 : 1)
            .Num("num_ref_idx_l1_default", (settings.refsL1 > 0) ? settings.refsL1 : 1)
            .Emit(m_out);
        TraceRps("  sps-rps", m_spsRps);
    }

    // VkVideoEncoderH265::EncodeFrame: the picture information ProcessDpb reads.
    void EncodeFrame(Frame& frame) override {
        frame.picOrderCntVal = frame.gop.inputOrder;
        StdVideoH265PictureType type = STD_VIDEO_H265_PICTURE_TYPE_INVALID;
        switch (frame.gop.pictureType) {
            case VkVideoGopStructure::FRAME_TYPE_P:
                type = STD_VIDEO_H265_PICTURE_TYPE_P;
                break;
            case VkVideoGopStructure::FRAME_TYPE_B:
                type = STD_VIDEO_H265_PICTURE_TYPE_B;
                break;
            case VkVideoGopStructure::FRAME_TYPE_I:
                type = STD_VIDEO_H265_PICTURE_TYPE_I;
                break;
            case VkVideoGopStructure::FRAME_TYPE_IDR:
            case VkVideoGopStructure::FRAME_TYPE_INTRA_REFRESH:
                type = STD_VIDEO_H265_PICTURE_TYPE_IDR;
                break;
            default:
                break;
        }
        frame.h265Info.flags.is_reference = frame.isReference;
        frame.h265Info.flags.short_term_ref_pic_set_sps_flag = 1;
        frame.h265Info.flags.IrapPicFlag = ((frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) ||
                                            (frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_I))
                                               ? 1
                                               : 0;
        frame.h265Info.flags.pic_output_flag = 1;
        frame.h265Info.flags.no_output_of_prior_pics_flag = 0;
        frame.h265Info.pic_type = type;
        frame.h265Info.PicOrderCntVal = static_cast<int32_t>(frame.picOrderCntVal);
        frame.h265Info.TemporalId = 0;
    }

    // VkVideoEncoderH265::ProcessDpb, without the image pool and the intra refresh. Each picture gets
    // its own resource index, so a reference's pic is the input number of the picture the DPB set up
    // in it.
    void ProcessDpb(Frame& frame, uint32_t encodeNum) override {
        StdVideoEncodeH265PictureInfo& info = frame.h265Info;
        // The pictures the DPB holds before the picture: the slots the RPS names.
        uint32_t count = VVS_DPB_MAX_SLOTS;
        VvsDpbReferenceFrame held[VVS_DPB_MAX_SLOTS];
        if ((m_libraryResult != VK_SUCCESS) || (m_library.GetReferenceFrames(&count, held) < VK_SUCCESS)) {
            count = 0;
        }
        VvsDpbH265EncodePictureInfo pictureInfo{};
        pictureInfo.dirtyIntraRefreshRegions = VVS_DPB_NO_INTRA_REFRESH;
        pictureInfo.pStdPictureInfo = &info;
        pictureInfo.pSyntax = &m_syntax;
        m_owners.input = frame.inputNum;
        const VkResult result = (m_libraryResult == VK_SUCCESS) ? m_library.Picture(pictureInfo, m_result) : m_libraryResult;

        const VkVideoGopStructure::FrameType picType = frame.gop.pictureType;
        TracePicture(frame, encodeNum);
        if (result != VK_SUCCESS) {
            Line("  refused").Num("result", result).Emit(m_out);
            return;
        }
        TraceH265(info);
        const StdVideoH265ShortTermRefPicSet* rps = info.flags.short_term_ref_pic_set_sps_flag
                                                        ? &m_sps.pShortTermRefPicSet[info.short_term_ref_pic_set_idx]
                                                        : info.pShortTermRefPicSet;
        TraceRps("  rps", *rps);

        // The slots of the RPS's pictures, as 8.3.2 finds them by PicOrderCntVal.
        auto slotOf = [&](int32_t poc) {
            for (uint32_t i = 0; i < count; i++) {
                if (held[i].stdReferenceInfo.h265.PicOrderCntVal == poc) {
                    return static_cast<int8_t>(held[i].slotIndex);
                }
            }
            return static_cast<int8_t>(-1);
        };
        std::vector<int8_t> stCurrBefore, stCurrAfter, stFoll;
        int32_t poc = info.PicOrderCntVal;
        for (uint32_t i = 0; i < rps->num_negative_pics; i++) {
            poc -= rps->delta_poc_s0_minus1[i] + 1;
            (((rps->used_by_curr_pic_s0_flag >> i) & 1) ? stCurrBefore : stFoll).push_back(slotOf(poc));
        }
        poc = info.PicOrderCntVal;
        for (uint32_t i = 0; i < rps->num_positive_pics; i++) {
            poc += rps->delta_poc_s1_minus1[i] + 1;
            (((rps->used_by_curr_pic_s1_flag >> i) & 1) ? stCurrAfter : stFoll).push_back(slotOf(poc));
        }
        Line("  rps-slots")
            .Array("stCurrBefore", stCurrBefore.data(), stCurrBefore.size())
            .Array("stCurrAfter", stCurrAfter.data(), stCurrAfter.size())
            .Array("ltCurr", stFoll.data(), 0)
            .Array("stFoll", stFoll.data(), stFoll.size())
            .Array("ltFoll", stFoll.data(), 0)
            .Emit(m_out);
        if ((picType == VkVideoGopStructure::FRAME_TYPE_P) || (picType == VkVideoGopStructure::FRAME_TYPE_B)) {
            const StdVideoEncodeH265ReferenceListsInfo& lists = m_syntax.refLists;
            const uint32_t count0 = lists.num_ref_idx_l0_active_minus1 + 1u;
            const uint32_t count1 = (picType == VkVideoGopStructure::FRAME_TYPE_B) ? lists.num_ref_idx_l1_active_minus1 + 1u : 0u;
            Line("  lists").Array("l0", lists.RefPicList0, count0).Array("l1", lists.RefPicList1, count1).Emit(m_out);
        }
        const VvsDpbPictureSlots& slots = m_result.slots;
        for (uint32_t i = 0; i < slots.referenceSlotCount; i++) {
            TraceReferenceInfo(
                Line("  ref").Num("slot", slots.referenceSlots[i].slotIndex).Num("pic", m_owners.owner[slots.resourceIndices[i]]),
                slots.stdReferenceInfos[i].h265)
                .Emit(m_out);
        }
        StdVideoEncodeH265ReferenceInfo setupInfo{};
        int32_t setupSlot = -1;
        if (slots.pSetupReferenceSlot != nullptr) {
            setupSlot = slots.pSetupReferenceSlot->slotIndex;
            setupInfo = slots.stdReferenceInfos[slots.referenceSlotCount].h265;
        }
        TraceReferenceInfo(Line("  setup").Num("slot", setupSlot), setupInfo).Emit(m_out);
    }

   private:
    void TracePicture(const Frame& frame, uint32_t encodeNum) const {
        Line("pic")
            .Num("index", encodeNum)
            .Num("input", frame.inputNum)
            .Str("type", FrameTypeName(frame.gop.pictureType))
            .Num("poc", static_cast<int64_t>(frame.picOrderCntVal))
            .Num("ref", frame.h265Info.flags.is_reference)
            .Emit(m_out);
    }

    void TraceH265(const StdVideoEncodeH265PictureInfo& info) const {
        Line("  h265")
            .Num("PicOrderCntVal", info.PicOrderCntVal)
            .Flags("flags", {{"irap", info.flags.IrapPicFlag},
                             {"no_output_of_prior_pics", info.flags.no_output_of_prior_pics_flag},
                             {"sps_rps", info.flags.short_term_ref_pic_set_sps_flag}})
            .Num("short_term_ref_pic_set_idx", info.short_term_ref_pic_set_idx)
            .Emit(m_out);
    }

    // Every member of the structure FillStdReferenceInfo fills.
    static Line& TraceReferenceInfo(Line& line, const StdVideoEncodeH265ReferenceInfo& info) {
        return line.Num("PicOrderCntVal", info.PicOrderCntVal)
            .Num("pic_type", info.pic_type)
            .Num("TemporalId", info.TemporalId)
            .Flags("flags", {{"lt", info.flags.used_for_long_term_reference}, {"unused", info.flags.unused_for_reference}});
    }

    void TraceRps(const char* tag, const StdVideoH265ShortTermRefPicSet& rps) const {
        Line(tag)
            .Num("num_negative_pics", rps.num_negative_pics)
            .Num("num_positive_pics", rps.num_positive_pics)
            .Array("delta_poc_s0_minus1", rps.delta_poc_s0_minus1, rps.num_negative_pics)
            .Array("delta_poc_s1_minus1", rps.delta_poc_s1_minus1, rps.num_positive_pics)
            .Num("used_s0", rps.used_by_curr_pic_s0_flag)
            .Num("used_s1", rps.used_by_curr_pic_s1_flag)
            .Flags("flags", {{"inter", rps.flags.inter_ref_pic_set_prediction_flag}})
            .Emit(m_out);
    }

    FILE* m_out;
    StdVideoH265DecPicBufMgr m_decPicBufMgr{};
    StdVideoH265SequenceParameterSet m_sps{};
    StdVideoH265ShortTermRefPicSet m_spsRps{};
    StdVideoH265PictureParameterSet m_pps{};
    uint32_t m_maxDpbPicturesCount = 0;
    // The library: the context, the storage it writes a picture's syntax and slots into, and the input
    // number of the picture each resource index was handed to.
    VkVideoProfileInfoKHR m_profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    vvs::dpb::H265Encode m_library;
    VkResult m_libraryResult = VK_SUCCESS;
    vvs::dpb::H265EncodeSyntax m_syntax;
    vvs::dpb::PictureResult m_result;
    Owners m_owners;
};

// VkVideoEncoderAV1 with EncoderConfigAV1.
class AV1Driver : public CodecDriver {
   public:
    AV1Driver(const Settings& settings, const VkVideoGopStructure& gop, FILE* out) : m_out(out), m_config(new EncoderConfigAV1()) {
        m_config->gopStructure = gop;
        m_config->enableIntraRefresh = false;
        // The device's reference capabilities (Settings).
        m_config->videoCapabilities.maxActiveReferencePictures = settings.maxActiveRefs;
        VkVideoEncodeAV1CapabilitiesKHR& caps = m_config->av1EncodeCapabilities;
        caps.maxSingleReferenceCount = settings.av1Single[0];
        caps.singleReferenceNameMask = settings.av1Single[1];
        caps.maxUnidirectionalCompoundReferenceCount = settings.av1Unidir[0];
        caps.maxUnidirectionalCompoundGroup1ReferenceCount = settings.av1Unidir[1];
        caps.unidirectionalCompoundReferenceNameMask = settings.av1Unidir[2];
        caps.maxBidirectionalCompoundReferenceCount = settings.av1Bidir[0];
        caps.maxBidirectionalCompoundGroup1ReferenceCount = settings.av1Bidir[1];
        caps.maxBidirectionalCompoundGroup2ReferenceCount = settings.av1Bidir[2];
        caps.bidirectionalCompoundReferenceNameMask = settings.av1Bidir[3];
        caps.flags = settings.av1CdfOnly ? VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR : 0;

        // EncoderConfigAV1::InitDpbCount, then VkVideoEncoder::InitEncoder's min(.., 16u, maxDpbSlots).
        m_maxDpbPicturesCount = AV1DpbSize(settings);
        // EncoderConfigAV1::InitSequenceHeader: the fields the DPB and the frame header read.
        m_seqHdr.flags.frame_id_numbers_present_flag = 0;
        m_seqHdr.additional_frame_id_length_minus_1 = FRAME_ID_BITS - DELTA_FRAME_ID_BITS - 1;
        m_seqHdr.delta_frame_id_length_minus_2 = DELTA_FRAME_ID_BITS - 2;
        m_seqHdr.flags.enable_order_hint = 1;
        m_seqHdr.order_hint_bits_minus_1 = ORDER_HINT_BITS - 1;

        {
            // VkVideoEncoderAV1::InitEncoderCodec: the session's slots and active references bound the
            // DPB, every picture is set up, and the GOP says whether it has B pictures.
            m_profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
            VvsDpbCreateInfo createInfo{};
            createInfo.apiVersion = VVS_DPB_API_VERSION;
            createInfo.flags = VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT;
            createInfo.pVideoProfile = &m_profile;
            createInfo.maxDpbSlots = m_maxDpbPicturesCount;
            createInfo.maxActiveReferencePictures = settings.maxActiveRefs;
            createInfo.slotPolicy = VVS_DPB_SLOT_POLICY_LOWEST_FREE;
            createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_CALLBACK;
            createInfo.resources.callbacks.pUserData = &m_owners;
            createInfo.resources.callbacks.pfnAcquireResource = Owners::Acquire;
            createInfo.resources.callbacks.pfnRetainResource = Owners::Keep;
            createInfo.resources.callbacks.pfnReleaseResource = Owners::Keep;
            VvsDpbGopInfo gopInfo{};
            gopInfo.gopStructure = VVS_DPB_GOP_STRUCTURE_IRREGULAR;
            gopInfo.gopFrameCount = gop.GetGopFrameCount();
            gopInfo.consecutiveBipredictiveFrameCount = gop.GetConsecutiveBFrameCount();
            const VvsDpbAV1SequenceInfo sequenceInfo{&m_seqHdr, &caps, nullptr, &gopInfo};
            m_libraryResult = m_library.Create(createInfo);
            if (m_libraryResult == VK_SUCCESS) {
                m_libraryResult = m_library.Sequence(sequenceInfo, nullptr);
            }
        }
        Line config("config");
        config.Str("codec", "av1")
            .Num("dpbSize", m_maxDpbPicturesCount)
            .Num("order_hint_bits", m_seqHdr.order_hint_bits_minus_1 + 1)
            .Num("max_active_refs", settings.maxActiveRefs)
            .Array("single", settings.av1Single, 2)
            .Array("unidir", settings.av1Unidir, 3)
            .Array("bidir", settings.av1Bidir, 4);
        // The line names the CDF-only capability only for a device without it.
        if (!settings.av1CdfOnly) {
            config.Num("cdf_only_primary", 0);
        }
        config.Emit(m_out);
    }

    // EncoderConfigAV1::InitDpbCount, then VkVideoEncoder::InitEncoder's min(.., 16u, maxDpbSlots).
    // VkVideoEncoderAV1::InitEncoderCodec refuses fewer than STD_VIDEO_AV1_NUM_REF_FRAMES.
    static uint32_t AV1DpbSize(const Settings& settings) {
        return std::min({static_cast<uint32_t>(STD_VIDEO_AV1_NUM_REF_FRAMES + 1), 16u, settings.maxDpbSlots});
    }

    // VkVideoEncoderAV1::EncodeFrame: the members ProcessDpb reads.
    void EncodeFrame(Frame& frame) override {
        frame.picOrderCntVal = frame.gop.inputOrder;
        frame.showExistingFrame = false;
        if (frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) {
            m_lastKeyFrameOrderHint = static_cast<int32_t>(frame.picOrderCntVal);
        }
        frame.picOrderCntVal = frame.picOrderCntVal - m_lastKeyFrameOrderHint;
    }

    // VkVideoEncoderAV1::InsertOrdered: a frame never goes before a show_existing_frame, and a
    // frame inserted ahead of others gets a show_existing_frame, appended at the end, that shows
    // it in display order.
    void InsertOrdered(FrameQueue& deferred, std::unique_ptr<Frame> frame) override {
        auto at = std::find_if(deferred.begin(), deferred.end(), [&](const std::unique_ptr<Frame>& other) {
            return !other->showExistingFrame && (other->gop.encodeOrder >= frame->gop.encodeOrder);
        });
        const bool reordered = (at != deferred.end());
        auto companion = std::make_unique<Frame>(frame->gop.inputOrder);
        companion->gop = frame->gop;
        companion->picOrderCntVal = frame->picOrderCntVal;
        companion->inputNum = frame->inputNum;
        companion->showExistingFrame = true;
        deferred.insert(at, std::move(frame));
        if (reordered) {
            deferred.push_back(std::move(companion));
        }
    }

    // VkVideoEncoderAV1::ProcessDpb, without the image pool, the rate control and the intra refresh:
    // one library call per frame, or per show_existing_frame.
    // The frame header fields the encoder sets first: a frame encoded ahead of frames displayed before
    // it is hidden, and shown later.
    void ProcessDpb(Frame& frame, uint32_t encodeNum) override {
        StdVideoEncodeAV1PictureInfo& info = frame.av1Info;
        info = StdVideoEncodeAV1PictureInfo{};
        const VkVideoGopStructure::FrameType gopType = frame.gop.pictureType;
        if (frame.showExistingFrame) {
            uint8_t buffer = 0;
            const VkResult result = (m_libraryResult == VK_SUCCESS)
                                        ? m_library.ShowExistingFrame(frame.picOrderCntVal, &buffer, m_result)
                                        : m_libraryResult;
            TracePicture(frame, encodeNum, gopType);
            if (result != VK_SUCCESS) {
                Line("  refused").Num("result", result).Emit(m_out);
                return;
            }
            uint32_t count = VVS_DPB_MAX_SLOTS;
            VvsDpbReferenceFrame held[VVS_DPB_MAX_SLOTS];
            m_library.GetReferenceFrames(&count, held);
            int32_t slot = -1;
            for (uint32_t i = 0; i < count; i++) {
                if ((held[i].av1BufferMask >> buffer) & 1U) {
                    slot = held[i].slotIndex;
                    info.frame_type = held[i].stdReferenceInfo.av1.frame_type;
                    info.current_frame_id = held[i].stdReferenceInfo.av1.RefFrameId;
                }
            }
            info.order_hint = static_cast<uint8_t>(frame.picOrderCntVal % (1 << (m_seqHdr.order_hint_bits_minus_1 + 1)));
            TraceShowExisting(info);
            Line("  shown").Num("slot", slot).Num("buffer", buffer).Emit(m_out);
            return;
        }
        const int32_t frameIdBits = m_seqHdr.delta_frame_id_length_minus_2 + 2 + m_seqHdr.additional_frame_id_length_minus_1 + 1;
        VvsDpbPictureInfo pictureInfo{};
        if (gopType == VkVideoGopStructure::FRAME_TYPE_IDR) {
            info.frame_type = STD_VIDEO_AV1_FRAME_TYPE_KEY;
            pictureInfo.pictureType = VVS_DPB_PICTURE_TYPE_KEY;
        } else if (gopType == VkVideoGopStructure::FRAME_TYPE_I) {
            info.frame_type = STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY;
            pictureInfo.pictureType = VVS_DPB_PICTURE_TYPE_INTRA;
        } else {
            info.frame_type = STD_VIDEO_AV1_FRAME_TYPE_INTER;
            pictureInfo.pictureType = (gopType == VkVideoGopStructure::FRAME_TYPE_B) ? VVS_DPB_PICTURE_TYPE_BIPREDICTIVE
                                                                                     : VVS_DPB_PICTURE_TYPE_PREDICTIVE;
        }
        pictureInfo.displayOrderIndex = frame.picOrderCntVal;
        pictureInfo.encodeOrderIndex = m_encodeEncodeFrameNum++;
        info.current_frame_id = static_cast<uint32_t>(frame.gop.encodeOrder % (1ULL << frameIdBits));
        info.order_hint = static_cast<uint8_t>(frame.picOrderCntVal % (1 << (m_seqHdr.order_hint_bits_minus_1 + 1)));
        const bool hidden = (gopType != VkVideoGopStructure::FRAME_TYPE_B) && (gopType != VkVideoGopStructure::FRAME_TYPE_IDR) &&
                            (frame.gop.inputOrder != frame.gop.encodeOrder);
        info.flags.show_frame = hidden ? 0 : 1;
        info.flags.showable_frame = info.flags.show_frame ? ((info.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY) ? 0 : 1) : 1;
        if ((info.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY) && info.flags.show_frame) {
            info.flags.error_resilient_mode = 1;
        }
        VkVideoEncodeAV1PictureInfoKHR pictureInfoAV1{VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PICTURE_INFO_KHR};
        pictureInfoAV1.pStdPictureInfo = &info;
        VvsDpbAV1EncodePictureInfo dpbPictureInfo{};
        dpbPictureInfo.pPictureInfo = &pictureInfo;
        dpbPictureInfo.dirtyIntraRefreshRegions = VVS_DPB_NO_INTRA_REFRESH;
        dpbPictureInfo.reference = frame.isReference ? VK_TRUE : VK_FALSE;
        dpbPictureInfo.pPictureInfoAV1 = &pictureInfoAV1;
        dpbPictureInfo.pStdPictureInfo = &info;
        m_owners.input = frame.inputNum;
        const VkResult result = (m_libraryResult == VK_SUCCESS) ? m_library.Picture(dpbPictureInfo, m_result) : m_libraryResult;
        // A B picture the DPB codes without a reference after it is a P picture.
        VkVideoGopStructure::FrameType type = gopType;
        if ((result == VK_SUCCESS) && (type == VkVideoGopStructure::FRAME_TYPE_B) &&
            (pictureInfoAV1.rateControlGroup != VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_BIPREDICTIVE_KHR)) {
            type = VkVideoGopStructure::FRAME_TYPE_P;
        }
        TracePicture(frame, encodeNum, type);
        if (result != VK_SUCCESS) {
            Line("  refused").Num("result", result).Emit(m_out);
            return;
        }
        TraceHeader(info, pictureInfoAV1.referenceNameSlotIndices, pictureInfoAV1.primaryReferenceCdfOnly,
                    pictureInfoAV1.predictionMode, pictureInfoAV1.rateControlGroup);
        const VvsDpbPictureSlots& slots = m_result.slots;
        for (uint32_t i = 0; i < slots.referenceSlotCount; i++) {
            TraceReferenceInfo(
                Line("  ref").Num("slot", slots.referenceSlots[i].slotIndex).Num("pic", m_owners.owner[slots.resourceIndices[i]]),
                slots.stdReferenceInfos[i].av1)
                .Emit(m_out);
        }
        StdVideoEncodeAV1ReferenceInfo setupInfo{};
        int32_t setupSlot = -1;
        if (slots.pSetupReferenceSlot != nullptr) {
            setupSlot = slots.pSetupReferenceSlot->slotIndex;
            setupInfo = slots.stdReferenceInfos[slots.referenceSlotCount].av1;
        }
        TraceReferenceInfo(Line("  setup").Num("slot", setupSlot), setupInfo).Emit(m_out);
    }

   private:
    void TracePicture(const Frame& frame, uint32_t encodeNum, VkVideoGopStructure::FrameType type) const {
        Line("pic")
            .Num("index", encodeNum)
            .Num("input", frame.inputNum)
            .Str("type", FrameTypeName(type))
            .Num("poc", static_cast<int64_t>(frame.picOrderCntVal))
            .Num("ref", m_config->gopStructure.IsFrameReference(const_cast<Frame&>(frame).gop))
            .Num("show_existing", frame.showExistingFrame)
            .Emit(m_out);
    }

    // The frame header of a coded frame: the fields the DPB decides, and the prediction mode and rate
    // control group the references allow.
    void TraceHeader(const StdVideoEncodeAV1PictureInfo& info, const int32_t* names, bool cdfOnly,
                     VkVideoEncodeAV1PredictionModeKHR mode, VkVideoEncodeAV1RateControlGroupKHR group) const {
        Line("  av1")
            .Num("frame_type", info.frame_type)
            .Num("refresh", info.refresh_frame_flags)
            .Flags("flags", {{"show_frame", info.flags.show_frame},
                             {"showable_frame", info.flags.showable_frame},
                             {"error_resilient_mode", info.flags.error_resilient_mode}})
            .Num("order_hint", info.order_hint)
            .Num("current_frame_id", info.current_frame_id)
            .Num("primary_ref_frame", info.primary_ref_frame)
            .Array("ref_frame_idx", info.ref_frame_idx, STD_VIDEO_AV1_REFS_PER_FRAME)
            .Array("ref_order_hint", info.ref_order_hint, STD_VIDEO_AV1_NUM_REF_FRAMES)
            .Array("names", names, STD_VIDEO_AV1_REFS_PER_FRAME)
            .Num("cdf_only", cdfOnly)
            .Num("mode", mode)
            .Num("rc_group", group)
            .Emit(m_out);
    }

    // A show_existing_frame header: the shown frame's type and frame id.
    void TraceShowExisting(const StdVideoEncodeAV1PictureInfo& info) const {
        Line("  av1")
            .Num("frame_type", info.frame_type)
            .Num("order_hint", info.order_hint)
            .Num("current_frame_id", info.current_frame_id)
            .Emit(m_out);
    }

    static Line& TraceReferenceInfo(Line& line, const StdVideoEncodeAV1ReferenceInfo& info) {
        return line.Num("frame_type", info.frame_type)
            .Num("OrderHint", info.OrderHint)
            .Num("RefFrameId", info.RefFrameId)
            .Flags("flags", {{"disable_frame_end_update_cdf", info.flags.disable_frame_end_update_cdf},
                             {"segmentation_enabled", info.flags.segmentation_enabled}});
    }

    FILE* m_out;
    VkSharedBaseObj<EncoderConfigAV1> m_config;
    StdVideoAV1SequenceHeader m_seqHdr{};
    uint32_t m_maxDpbPicturesCount = 0;
    int32_t m_lastKeyFrameOrderHint = 0;
    uint64_t m_encodeEncodeFrameNum = 0;
    // The library: the context, the storage it writes a frame's slots into, and the input number of
    // the picture each resource index was handed to.
    VkVideoProfileInfoKHR m_profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    vvs::dpb::AV1Encode m_library;
    VkResult m_libraryResult = VK_SUCCESS;
    vvs::dpb::PictureResult m_result;
    Owners m_owners;
};

// "a:b:..." into count values (each decimal or 0x hexadecimal).
bool ParseCounts(const char* text, uint32_t* values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char* end = nullptr;
        values[i] = static_cast<uint32_t>(strtoul(text, &end, 0));
        if ((end == text) || (*end != ((i + 1 < count) ? ':' : '\0'))) {
            return false;
        }
        text = end + 1;
    }
    return true;
}

int Usage(const char* program) {
    fprintf(stderr,
            "Usage: %s --codec h264|h265|av1 [-o <trace>] [--frames N] [--gop N] [--idr N] [--bframes N] [--closed-gop]\n"
            "          [--refs-l0 N] [--refs-l1 N] [--level-dpb N] [--max-dpb-slots N] [--max-active-refs N]\n"
            "          [--av1-single N:MASK] [--av1-unidir N:G1:MASK] [--av1-bidir N:G1:G2:MASK] [--av1-cdf-only 0|1]\n"
            "       %s --asserts\n"
            "Drives the encoder's DPB through a GOP sequence as the encoder does, and writes the\n"
            "setup slot, reference lists or names, and references of every picture. --refs-l0, --refs-l1\n"
            "and --level-dpb apply to H.264 and H.265. --max-dpb-slots is the device's maxDpbSlots;\n"
            "--max-active-refs and the --av1- options give AV1 the device's maxActiveReferencePictures\n"
            "and reference capabilities (counts, group counts and name masks), --av1-cdf-only whether it\n"
            "takes a primary reference frame for its CDFs only (1). --asserts prints whether the encoder\n"
            "library was built with assertions.\n",
            program, program);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Settings settings;
    const char* output = nullptr;
    if ((argc == 2) && (std::string(argv[1]) == "--asserts")) {
#ifdef NDEBUG
        printf("asserts=0\n");
#else
        printf("asserts=1\n");
#endif
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--closed-gop") {
            settings.closedGop = true;
            continue;
        }
        if (i + 1 >= argc) {
            return Usage(argv[0]);
        }
        const char* value = argv[++i];
        if (arg == "--codec") {
            settings.codec = value;
        } else if (arg == "-o") {
            output = value;
        } else if (arg == "--frames") {
            settings.frames = static_cast<uint32_t>(strtoul(value, nullptr, 0));
        } else if (arg == "--gop") {
            settings.gopFrameCount = static_cast<uint32_t>(strtoul(value, nullptr, 0));
        } else if (arg == "--idr") {
            settings.idrPeriod = static_cast<int32_t>(strtol(value, nullptr, 0));
        } else if (arg == "--bframes") {
            settings.bFrames = static_cast<uint32_t>(strtoul(value, nullptr, 0));
        } else if (arg == "--refs-l0") {
            settings.refsL0 = static_cast<uint32_t>(strtoul(value, nullptr, 0));
        } else if (arg == "--refs-l1") {
            settings.refsL1 = static_cast<uint32_t>(strtoul(value, nullptr, 0));
        } else if (arg == "--level-dpb") {
            settings.levelDpbSize = static_cast<uint32_t>(strtoul(value, nullptr, 0));
        } else if (arg == "--max-dpb-slots") {
            settings.maxDpbSlots = static_cast<uint32_t>(strtoul(value, nullptr, 0));
        } else if (arg == "--max-active-refs") {
            settings.maxActiveRefs = static_cast<uint32_t>(strtoul(value, nullptr, 0));
        } else if (arg == "--av1-single") {
            if (!ParseCounts(value, settings.av1Single, 2)) {
                return Usage(argv[0]);
            }
        } else if (arg == "--av1-unidir") {
            if (!ParseCounts(value, settings.av1Unidir, 3)) {
                return Usage(argv[0]);
            }
        } else if (arg == "--av1-bidir") {
            if (!ParseCounts(value, settings.av1Bidir, 4)) {
                return Usage(argv[0]);
            }
        } else if (arg == "--av1-cdf-only") {
            settings.av1CdfOnly = (strtoul(value, nullptr, 0) != 0);
        } else {
            return Usage(argv[0]);
        }
    }
    if ((settings.codec != "h264") && (settings.codec != "h265") && (settings.codec != "av1")) {
        return Usage(argv[0]);
    }

    FILE* out = stdout;
    if (output != nullptr) {
        out = fopen(output, "w");
        if (out == nullptr) {
            fprintf(stderr, "Cannot write %s\n", output);
            return 1;
        }
    }
    Line("encoder-dpb-trace")
        .Num("version", 1)
        .Str("codec", settings.codec.c_str())
        .Num("frames", settings.frames)
        .Num("gop", settings.gopFrameCount)
        .Num("idr", settings.idrPeriod)
        .Num("bframes", settings.bFrames)
        .Num("closed", settings.closedGop)
        .Num("refs_l0", settings.refsL0)
        .Num("refs_l1", settings.refsL1)
        .Num("level_dpb", settings.levelDpbSize)
        .Num("max_dpb_slots", settings.maxDpbSlots)
        .Emit(out);

    // EncoderConfig's GOP, initialized as VkVideoEncoder::InitEncoder does.
    VkVideoGopStructure gop(settings.gopFrameCount, settings.idrPeriod, static_cast<uint8_t>(settings.bFrames), 1,
                            VkVideoGopStructure::FRAME_TYPE_P, VkVideoGopStructure::FRAME_TYPE_P, settings.closedGop, 0);
    gop.Init(settings.frames);

    std::unique_ptr<CodecDriver> driver;
    if (settings.codec == "h264") {
        driver = std::make_unique<H264Driver>(settings, gop, out);
    } else if (settings.codec == "h265") {
        driver = std::make_unique<H265Driver>(settings, gop, out);
    } else if (AV1Driver::AV1DpbSize(settings) < STD_VIDEO_AV1_NUM_REF_FRAMES) {
        // VkVideoEncoderAV1::InitEncoderCodec refuses the session.
        Line("refused").Num("dpbSize", AV1Driver::AV1DpbSize(settings)).Emit(out);
        if (out != stdout) {
            fclose(out);
        }
        return 1;
    } else {
        driver = std::make_unique<AV1Driver>(settings, gop, out);
    }

    // VkVideoEncoder::EncodeFrame and EnqueueFrame: the deferred queue in GOP encode order, held
    // until one reference frame (m_holdRefFramesInQueue 1) and flushed then, at an IDR, at the last
    // frame and on every frame of a GOP without B frames.
    VkVideoGopStructure::GopState gopState;
    FrameQueue deferred;
    uint32_t deferredRefFrames = 0;
    uint32_t encodeNum = 0;
    auto flush = [&]() {
        for (auto& frame : deferred) {
            driver->ProcessDpb(*frame, encodeNum++);
        }
        deferred.clear();
        deferredRefFrames = 0;
    };
    for (uint32_t i = 0; i < settings.frames; i++) {
        auto frame = std::make_unique<Frame>(gopState.positionInInputOrder);
        frame->inputNum = i;
        frame->isIdr = gop.GetPositionInGOP(gopState, frame->gop, i == 0, settings.frames - i);
        frame->isReference = gop.IsFrameReference(frame->gop);
        frame->lastFrame = (i + 1 == settings.frames);
        driver->EncodeFrame(*frame);

        if (frame->isIdr) {
            flush();
        }
        const bool isReference = frame->isReference;
        const bool lastFrame = frame->lastFrame;
        driver->InsertOrdered(deferred, std::move(frame));
        if (isReference) {
            deferredRefFrames++;
        }
        if (lastFrame || (gop.GetConsecutiveBFrameCount() == 0) || (isReference && (deferredRefFrames == 1))) {
            flush();
        }
    }
    flush();
    Line("end").Num("pictures", encodeNum).Emit(out);

    driver.reset();
    if (out != stdout) {
        fclose(out);
    }
    return 0;
}
