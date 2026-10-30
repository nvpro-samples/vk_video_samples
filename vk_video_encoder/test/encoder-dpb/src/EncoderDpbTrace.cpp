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

// vk-video-encoder-dpb-trace drives the encoder's DPB classes, VkEncDpbH264, VkEncDpbH265 and
// VkEncDpbAV1 from the encoder library, through a GOP sequence with no Vulkan device, making the
// calls the encoder makes for each picture, and writes what they decide: the setup slot, the
// reference lists or reference names, and the slots and Std reference information of the
// references.
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
//     and ProcessDpb, VkVideoEncoderAV1::EncodeFrame, StartOfVideoCodingEncodeOrder,
//     InitializeFrameHeader and ProcessDpb.
// A change to any of them is a change this harness has to follow.
//
// scripts/encoder_dpb_model.py checks the trace against the H.264 and H.265 reference picture
// rules and the AV1 reference buffers. See ../README.md.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "VkVideoEncoder/VkEncoderDpbAV1.h"
#include "VkVideoEncoder/VkEncoderDpbH264.h"
#include "VkVideoEncoder/VkEncoderDpbH265.h"
#include "VkVideoEncoder/VkVideoGopStructure.h"

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
    uint64_t timeStamp = 0;
    uint64_t picOrderCntVal = 0;
    StdVideoEncodeH264PictureInfo h264Info{};
    StdVideoEncodeH264SliceHeader h264Slice{};
    StdVideoEncodeH265PictureInfo h265Info{};
    // AV1: VkVideoEncodeFrameInfoAV1's members.
    StdVideoEncodeAV1PictureInfo av1Info{};
    bool showExistingFrame = false;
    bool overlayFrame = false;
    bool shownKeyFrameOrSwitch = false;
    int32_t frameToShowBufId = -1;
    uint64_t frameEncodeEncodeOrderNum = uint64_t(-1);
};

// The encoder's DPB images: one per picture, named by the picture's input number in the trace.
class Images {
   public:
    VkSharedBaseObj<VulkanVideoImagePoolNode> New(uint32_t inputNum) {
        VkSharedBaseObj<VulkanVideoImagePoolNode> node(new VulkanVideoImagePoolNode());
        m_nodes.push_back(node);
        m_owner[node.get()] = static_cast<int32_t>(inputNum);
        return node;
    }
    int32_t Owner(const VkSharedBaseObj<VulkanVideoImagePoolNode>& node) const {
        auto it = m_owner.find(node.get());
        return (it != m_owner.end()) ? it->second : -1;
    }

   private:
    std::vector<VkSharedBaseObj<VulkanVideoImagePoolNode>> m_nodes;  // keeps each node, so no address repeats
    std::map<const VulkanVideoImagePoolNode*, int32_t> m_owner;
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

// VkVideoEncoderH264 with EncoderConfigH264.
class H264Driver : public CodecDriver {
   public:
    H264Driver(const Settings& settings, const VkVideoGopStructure& gop, FILE* out)
        : m_out(out), m_dpb(VkEncDpbH264::CreateInstance()) {
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

        m_dpb->DpbSequenceStart(static_cast<int32_t>(m_maxDpbPicturesCount));
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

    // As ~VkVideoEncoderH264: DpbDestroy() deletes the DPB itself.
    ~H264Driver() override { m_dpb->DpbDestroy(); }

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

    // VkVideoEncoderH264::ProcessDpb, without the image pool and the intra refresh.
    void ProcessDpb(Frame& frame, uint32_t encodeNum) override {
        const VkVideoGopStructure::FrameType picType = frame.gop.pictureType;
        const bool isReference = frame.h264Info.flags.is_reference;

        PicInfoH264 pictureInfo{};
        memcpy(&pictureInfo, &frame.h264Info, sizeof(frame.h264Info));
        if (pictureInfo.flags.IdrPicFlag) {
            m_frameNumSyntax = 0;
        }
        pictureInfo.frame_num = m_frameNumSyntax & ((1 << (m_sps.log2_max_frame_num_minus4 + 4)) - 1);
        pictureInfo.PicOrderCnt =
            static_cast<int32_t>(frame.picOrderCntVal & ((1 << (m_sps.log2_max_pic_order_cnt_lsb_minus4 + 4)) - 1));
        pictureInfo.timeStamp = frame.timeStamp;
        if (isReference) {
            m_frameNumSyntax++;
        }

        VkSharedBaseObj<VulkanVideoImagePoolNode> setupImage = m_images.New(frame.inputNum);
        const int8_t startSlot = m_dpb->DpbPictureStart(&pictureInfo, &m_sps);

        StdVideoEncodeH264ReferenceListsInfo refListsInfo{};
        StdVideoEncodeH264SliceHeader slice = frame.h264Slice;
        if ((m_pps.num_ref_idx_l0_default_active_minus1 > 0) && (picType == VkVideoGopStructure::FRAME_TYPE_B)) {
            slice.flags.num_ref_idx_active_override_flag = true;
            refListsInfo.num_ref_idx_l0_active_minus1 = 0;
        }
        NvVideoEncodeH264DpbSlotInfoLists<STD_VIDEO_H264_MAX_NUM_LIST_REF> refLists;
        m_dpb->GetRefPicList(&pictureInfo, &refLists, &m_sps, &m_pps, &slice, &refListsInfo);

        StdVideoEncodeH264PictureInfo stdPictureInfo = frame.h264Info;
        stdPictureInfo.frame_num = m_dpb->GetUpdatedFrameNumAndPicOrderCnt(stdPictureInfo.PicOrderCnt);

        Line("pic")
            .Num("index", encodeNum)
            .Num("input", frame.inputNum)
            .Str("type", FrameTypeName(picType))
            .Num("poc", static_cast<int64_t>(frame.picOrderCntVal))
            .Num("ref", isReference)
            .Emit(m_out);
        Line("  h264")
            .Num("frame_num", stdPictureInfo.frame_num)
            .Num("PicOrderCnt", stdPictureInfo.PicOrderCnt)
            .Num("idr_pic_id", stdPictureInfo.idr_pic_id)
            .Num("start", startSlot)
            .Emit(m_out);
        Line("  lists")
            .Array("l0", refLists.refPicList[0], refLists.refPicListCount[0])
            .Array("l1", refLists.refPicList[1], refLists.refPicListCount[1])
            .Emit(m_out);

        // The references the encode binds: each slot of the lists once.
        std::vector<int8_t> bound;
        for (uint32_t list = 0; list < 2; list++) {
            for (uint32_t i = 0; i < refLists.refPicListCount[list]; i++) {
                const int8_t slot = static_cast<int8_t>(refLists.refPicList[list][i]);
                if (std::find(bound.begin(), bound.end(), slot) != bound.end()) {
                    continue;
                }
                bound.push_back(slot);
                VkSharedBaseObj<VulkanVideoImagePoolNode> image;
                const bool available = m_dpb->GetRefPicture(slot, image);
                StdVideoEncodeH264ReferenceInfo info{};
                m_dpb->FillStdReferenceInfo(static_cast<uint8_t>(slot), &info);
                TraceReferenceInfo(Line("  ref").Num("slot", slot).Num("pic", available ? m_images.Owner(image) : -1), info)
                    .Emit(m_out);
            }
        }

        const int8_t setupSlot = m_dpb->DpbPictureEnd(&pictureInfo, setupImage, &m_sps, &slice, &refListsInfo, 10);
        // The reference information of the setup slot, filled from the slot as the encoder does.
        StdVideoEncodeH264ReferenceInfo setupInfo{};
        if ((setupSlot >= 0) && (setupSlot < m_dpb->GetMaxDPBSize())) {
            m_dpb->FillStdReferenceInfo(static_cast<uint8_t>(setupSlot), &setupInfo);
        }
        TraceReferenceInfo(Line("  setup").Num("slot", setupSlot), setupInfo).Emit(m_out);
    }

   private:
    // Every member of the structure FillStdReferenceInfo fills.
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
    VkEncDpbH264* m_dpb;
    Images m_images;
    StdVideoH264SequenceParameterSet m_sps{};
    StdVideoH264PictureParameterSet m_pps{};
    uint32_t m_maxDpbPicturesCount = 0;
    uint32_t m_frameNumSyntax = 0;
    uint32_t m_idrPicId = 0;
};

// VkVideoEncoderH265 with EncoderConfigH265.
class H265Driver : public CodecDriver {
   public:
    H265Driver(const Settings& settings, const VkVideoGopStructure&, FILE* out) : m_out(out), m_settings(settings) {
        // EncoderConfigH265::InitDpbCount (VerifyDpbSize keeps 5 at the levels the encoder picks),
        // then VkVideoEncoder::InitEncoder's min(.., 16u, maxDpbSlots), which it writes back to
        // dpbCount.
        const uint32_t dpbCount = std::min({5u, 16u, settings.maxDpbSlots});
        m_maxDpbPicturesCount = dpbCount;

        // EncoderConfigH265::InitParamameters and InitializeSpsRefPicSet.
        m_sps.log2_max_pic_order_cnt_lsb_minus4 = 4;
        m_sps.flags.long_term_ref_pics_present_flag = 0;
        m_spsRps.delta_idx_minus1 = 0;
        m_spsRps.num_negative_pics = static_cast<uint8_t>(dpbCount - 1);
        m_spsRps.used_by_curr_pic_s0_flag =
            static_cast<uint16_t>((1u << std::min<uint32_t>(m_spsRps.num_negative_pics, settings.refsL0)) - 1);
        m_sps.num_short_term_ref_pic_sets = 1;
        m_sps.pShortTermRefPicSet = &m_spsRps;

        m_dpb.DpbSequenceStart(static_cast<int32_t>(m_maxDpbPicturesCount), (settings.refsL0 > 0) || (settings.refsL1 > 0));
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

    // VkVideoEncoderH265::ProcessDpb, without the image pool and the intra refresh.
    void ProcessDpb(Frame& frame, uint32_t encodeNum) override {
        uint32_t numRefL0 = m_settings.refsL0;
        uint32_t numRefL1 = m_settings.refsL1;
        const VkVideoGopStructure::FrameType picType = frame.gop.pictureType;
        if ((picType == VkVideoGopStructure::FRAME_TYPE_P) || (picType == VkVideoGopStructure::FRAME_TYPE_B)) {
            numRefL0 = (numRefL0 == 0) ? 1 : numRefL0;
            if (picType == VkVideoGopStructure::FRAME_TYPE_B) {
                numRefL1 = (numRefL1 == 0) ? 1 : numRefL1;
            }
        }

        StdVideoEncodeH265PictureInfo& info = frame.h265Info;
        m_dpb.ReferencePictureMarking(static_cast<int32_t>(frame.picOrderCntVal), static_cast<StdVideoH265PictureType>(picType),
                                      m_sps.flags.long_term_ref_pics_present_flag);

        StdVideoH265ShortTermRefPicSet pictureRps{};
        if (!info.flags.no_output_of_prior_pics_flag) {
            info.pShortTermRefPicSet = &pictureRps;
            m_dpb.InitializeRPS(m_sps.pShortTermRefPicSet, m_sps.num_short_term_ref_pic_sets, &info, &pictureRps, numRefL0,
                                numRefL1);
        } else {
            info.pShortTermRefPicSet = nullptr;
        }
        const int32_t maxPicOrderCntLsb = 1 << (m_sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
        const StdVideoH265ShortTermRefPicSet* rps = !info.flags.short_term_ref_pic_set_sps_flag
                                                        ? info.pShortTermRefPicSet
                                                        : &m_sps.pShortTermRefPicSet[info.short_term_ref_pic_set_idx];

        VkSharedBaseObj<VulkanVideoImagePoolNode> setupImage = m_images.New(frame.inputNum);
        VkEncDpbH265::RefPicSet refPicSet{};
        const int8_t setupSlot =
            m_dpb.DpbPictureStart(&info, rps, nullptr, maxPicOrderCntLsb, frame.timeStamp, &refPicSet);

        StdVideoEncodeH265ReferenceListsInfo refLists{};
        const bool inter = (picType == VkVideoGopStructure::FRAME_TYPE_P) || (picType == VkVideoGopStructure::FRAME_TYPE_B);
        if (inter) {
            m_dpb.SetupReferencePictureListLx(static_cast<StdVideoH265PictureType>(picType), &refPicSet, &refLists, numRefL0,
                                              numRefL1);
        }
        m_dpb.DpbPictureEnd(setupImage, 1, info.flags.is_reference);

        Line("pic")
            .Num("index", encodeNum)
            .Num("input", frame.inputNum)
            .Str("type", FrameTypeName(picType))
            .Num("poc", static_cast<int64_t>(frame.picOrderCntVal))
            .Num("ref", info.flags.is_reference)
            .Emit(m_out);
        Line("  h265")
            .Num("PicOrderCntVal", info.PicOrderCntVal)
            .Flags("flags", {{"irap", info.flags.IrapPicFlag},
                             {"no_output_of_prior_pics", info.flags.no_output_of_prior_pics_flag},
                             {"sps_rps", info.flags.short_term_ref_pic_set_sps_flag}})
            .Num("short_term_ref_pic_set_idx", info.short_term_ref_pic_set_idx)
            .Emit(m_out);
        if (rps != nullptr) {
            TraceRps("  rps", *rps);
        }
        // Each RPS list holds its slots first, then -1.
        auto used = [](const int8_t* slots) {
            size_t count = 0;
            while ((count < STD_VIDEO_H265_MAX_NUM_LIST_REF) && (slots[count] >= 0)) {
                count++;
            }
            return count;
        };
        Line("  rps-slots")
            .Array("stCurrBefore", refPicSet.stCurrBefore, used(refPicSet.stCurrBefore))
            .Array("stCurrAfter", refPicSet.stCurrAfter, used(refPicSet.stCurrAfter))
            .Array("ltCurr", refPicSet.ltCurr, used(refPicSet.ltCurr))
            .Array("stFoll", refPicSet.stFoll, used(refPicSet.stFoll))
            .Array("ltFoll", refPicSet.ltFoll, used(refPicSet.ltFoll))
            .Emit(m_out);
        if (inter) {
            const uint32_t count0 = refLists.num_ref_idx_l0_active_minus1 + 1u;
            const uint32_t count1 =
                (picType == VkVideoGopStructure::FRAME_TYPE_B) ? refLists.num_ref_idx_l1_active_minus1 + 1u : 0u;
            Line("  lists").Array("l0", refLists.RefPicList0, count0).Array("l1", refLists.RefPicList1, count1).Emit(m_out);

            std::vector<uint8_t> bound;
            const uint8_t* lists[2] = {refLists.RefPicList0, refLists.RefPicList1};
            const uint32_t counts[2] = {count0, count1};
            for (uint32_t list = 0; list < 2; list++) {
                for (uint32_t i = 0; i < counts[list]; i++) {
                    const uint8_t slot = lists[list][i];
                    if (std::find(bound.begin(), bound.end(), slot) != bound.end()) {
                        continue;
                    }
                    bound.push_back(slot);
                    VkSharedBaseObj<VulkanVideoImagePoolNode> image;
                    const bool available = m_dpb.GetRefPicture(static_cast<int8_t>(slot), image);
                    StdVideoEncodeH265ReferenceInfo refInfo{};
                    m_dpb.FillStdReferenceInfo(slot, &refInfo);
                    TraceReferenceInfo(Line("  ref").Num("slot", slot).Num("pic", available ? m_images.Owner(image) : -1), refInfo)
                        .Emit(m_out);
                }
            }
        }
        // The reference information of the setup slot, filled from the slot as the encoder does.
        StdVideoEncodeH265ReferenceInfo setupInfo{};
        if (setupSlot >= 0) {
            m_dpb.FillStdReferenceInfo(static_cast<uint8_t>(setupSlot), &setupInfo);
        }
        TraceReferenceInfo(Line("  setup").Num("slot", setupSlot), setupInfo).Emit(m_out);
    }

   private:
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
    Settings m_settings;
    VkEncDpbH265 m_dpb;
    Images m_images;
    StdVideoH265SequenceParameterSet m_sps{};
    StdVideoH265ShortTermRefPicSet m_spsRps{};
    uint32_t m_maxDpbPicturesCount = 0;
};

// VkVideoEncoderAV1 with EncoderConfigAV1.
class AV1Driver : public CodecDriver {
   public:
    AV1Driver(const Settings& settings, const VkVideoGopStructure& gop, FILE* out)
        : m_out(out), m_config(new EncoderConfigAV1()), m_dpb(VkEncDpbAV1::CreateInstance()) {
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

        m_dpb->DpbSequenceStart(m_config, m_maxDpbPicturesCount);
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

    // As ~VkVideoEncoderAV1.
    ~AV1Driver() override { m_dpb->DpbDestroy(); }

    // VkVideoEncoderAV1::EncodeFrame: the members ProcessDpb reads.
    void EncodeFrame(Frame& frame) override {
        frame.picOrderCntVal = frame.gop.inputOrder;
        frame.showExistingFrame = false;
        frame.overlayFrame = false;
        if (frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_B) {
            m_numBFramesToEncode++;
        }
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
        companion->timeStamp = frame->inputNum;
        companion->overlayFrame = true;
        companion->showExistingFrame = true;
        deferred.insert(at, std::move(frame));
        if (reordered) {
            deferred.push_back(std::move(companion));
        }
    }

    // VkVideoEncoderAV1::StartOfVideoCodingEncodeOrder, then ProcessDpb, without the image pool, the
    // rate control and the intra refresh. The encoder recognizes a reference it already binds by
    // its image index (GetImageIndex()); each image here is its own node, so the node stands for
    // the index.
    void ProcessDpb(Frame& frame, uint32_t encodeNum) override {
        if (!frame.showExistingFrame) {
            frame.frameEncodeEncodeOrderNum = m_encodeEncodeFrameNum++;
        }

        uint32_t flags = 0;
        if (frame.gop.pictureType != VkVideoGopStructure::FRAME_TYPE_B) {
            if ((frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_I) && (frame.gop.inputOrder == frame.gop.encodeOrder)) {
                flags = 1 << STD_VIDEO_AV1_REFERENCE_NAME_INTRA_FRAME;
            } else {
                flags = (m_config->gopStructure.GetConsecutiveBFrameCount() == 0)
                            ? 0
                            : (m_numBFramesToEncode == 0 ? (1 << STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME)
                                                         : (1 << STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME));
            }
        }
        const bool isReference = m_config->gopStructure.IsFrameReference(frame.gop);
        StdVideoAV1ReferenceName refName = m_dpb->AssignReferenceFrameType(frame.gop.pictureType, flags, isReference);
        InitializeFrameHeader(frame, refName);
        if (!frame.showExistingFrame) {
            m_dpb->SetupReferenceFrameGroups(frame.gop.pictureType, frame.av1Info.frame_type,
                                             static_cast<uint32_t>(frame.picOrderCntVal));
            frame.av1Info.primary_ref_frame = static_cast<uint8_t>(m_dpb->ResolvePrimaryRefFrame());
            if ((frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_B) && (m_dpb->GetNumRefsL1() == 0)) {
                frame.gop.pictureType = VkVideoGopStructure::FRAME_TYPE_P;
                m_numBFramesToEncode--;
            }
        }
        const VkVideoEncoderAV1FrameUpdateType frameUpdateType = m_dpb->GetFrameUpdateType(refName, frame.overlayFrame);
        const int8_t dpbIndx =
            m_dpb->DpbPictureStart(frame.av1Info.frame_type, refName, static_cast<uint32_t>(frame.picOrderCntVal),
                                   frame.av1Info.current_frame_id, frame.showExistingFrame, frame.frameToShowBufId);
        m_dpb->ConfigureRefBufUpdate(frame.shownKeyFrameOrSwitch, frame.showExistingFrame, frameUpdateType);

        Line("pic")
            .Num("index", encodeNum)
            .Num("input", frame.inputNum)
            .Str("type", FrameTypeName(frame.gop.pictureType))
            .Num("poc", static_cast<int64_t>(frame.picOrderCntVal))
            .Num("ref", isReference)
            .Num("show_existing", frame.showExistingFrame)
            .Emit(m_out);

        VkSharedBaseObj<VulkanVideoImagePoolNode> setupImage;
        if (frame.showExistingFrame) {
            frame.av1Info.refresh_frame_flags =
                static_cast<uint8_t>(m_dpb->GetRefreshFrameFlags(frame.shownKeyFrameOrSwitch, frame.showExistingFrame));
            TraceHeader(frame, refName, nullptr, false);
            m_dpb->DpbPictureEnd(dpbIndx, setupImage, &m_seqHdr, frame.showExistingFrame, frame.shownKeyFrameOrSwitch,
                                 frame.av1Info.flags.error_resilient_mode, frame.overlayFrame, refName, frameUpdateType);
            Line("  shown").Num("slot", dpbIndx).Num("buffer", frame.frameToShowBufId).Emit(m_out);
            return;
        }
        if (frame.frameEncodeEncodeOrderNum != uint64_t(-1)) {
            m_dpb->InvalidateStaleReferenceFrames(static_cast<uint32_t>(frame.frameEncodeEncodeOrderNum),
                                                  static_cast<uint32_t>(frame.picOrderCntVal), &m_seqHdr);
        }
        frame.av1Info.refresh_frame_flags =
            static_cast<uint8_t>(m_dpb->GetRefreshFrameFlags(frame.shownKeyFrameOrSwitch, frame.showExistingFrame));

        setupImage = m_images.New(frame.inputNum);
        StdVideoEncodeAV1ReferenceInfo setupInfo{};
        m_dpb->FillStdReferenceInfo(static_cast<uint8_t>(dpbIndx), &setupInfo);

        // The references, by reference name, each image bound once.
        int32_t names[STD_VIDEO_AV1_REFS_PER_FRAME];
        std::fill(std::begin(names), std::end(names), -1);
        std::vector<std::pair<int32_t, VkSharedBaseObj<VulkanVideoImagePoolNode>>> bound;
        auto isBound = [&](const VkSharedBaseObj<VulkanVideoImagePoolNode>& image) {
            return std::any_of(bound.begin(), bound.end(), [&](const auto& entry) { return entry.second.get() == image.get(); });
        };
        bool primaryRefCdfOnly = (frame.av1Info.primary_ref_frame != STD_VIDEO_AV1_PRIMARY_REF_NONE);
        for (int32_t groupId = 0; groupId < 2; groupId++) {
            for (int32_t i = 0; i < m_dpb->GetNumRefsInGroup(groupId); i++) {
                const int32_t refNameMinus1 = m_dpb->GetRefNameMinus1(groupId, i);
                const int32_t dpbIdx = m_dpb->GetDpbIdx(groupId, i);
                names[refNameMinus1] = dpbIdx;
                VkSharedBaseObj<VulkanVideoImagePoolNode> image;
                if (!m_dpb->GetDpbPictureResource(dpbIdx, image)) {
                    Line("  missing").Num("slot", dpbIdx).Num("name", refNameMinus1).Emit(m_out);
                    continue;
                }
                if (isBound(image)) {
                    continue;
                }
                bound.emplace_back(dpbIdx, image);
                if (refNameMinus1 == frame.av1Info.primary_ref_frame) {
                    primaryRefCdfOnly = false;
                }
            }
        }
        if ((frame.av1Info.primary_ref_frame != STD_VIDEO_AV1_PRIMARY_REF_NONE) && primaryRefCdfOnly) {
            const int32_t dpbIdx = m_dpb->GetDpbIdx(frame.av1Info.primary_ref_frame);
            VkSharedBaseObj<VulkanVideoImagePoolNode> image;
            if (!m_dpb->GetDpbPictureResource(dpbIdx, image)) {
                Line("  missing").Num("slot", dpbIdx).Num("name", frame.av1Info.primary_ref_frame).Emit(m_out);
            } else {
                if (!isBound(image)) {
                    bound.emplace_back(dpbIdx, image);
                }
                names[frame.av1Info.primary_ref_frame] = dpbIdx;
            }
        }
        TraceHeader(frame, refName, names, primaryRefCdfOnly);
        for (const auto& entry : bound) {
            StdVideoEncodeAV1ReferenceInfo info{};
            m_dpb->FillStdReferenceInfo(static_cast<uint8_t>(entry.first), &info);
            TraceReferenceInfo(Line("  ref").Num("slot", entry.first).Num("pic", m_images.Owner(entry.second)), info).Emit(m_out);
        }

        m_dpb->DpbPictureEnd(dpbIndx, setupImage, &m_seqHdr, frame.showExistingFrame, frame.shownKeyFrameOrSwitch,
                             frame.av1Info.flags.error_resilient_mode, frame.overlayFrame, refName, frameUpdateType);
        TraceReferenceInfo(Line("  setup").Num("slot", dpbIndx), setupInfo).Emit(m_out);

        if (frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_B) {
            m_numBFramesToEncode--;
        }
    }

   private:
    // VkVideoEncoderAV1::InitializeFrameHeader: the fields of the frame header the DPB decides.
    void InitializeFrameHeader(Frame& frame, StdVideoAV1ReferenceName& refName) {
        StdVideoEncodeAV1PictureInfo& info = frame.av1Info;
        info = StdVideoEncodeAV1PictureInfo{};
        const int32_t frameIdBits = m_seqHdr.delta_frame_id_length_minus_2 + 2 + m_seqHdr.additional_frame_id_length_minus_1 + 1;
        const int32_t orderHintBits = m_seqHdr.order_hint_bits_minus_1 + 1;
        if (frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) {
            info.frame_type = STD_VIDEO_AV1_FRAME_TYPE_KEY;
        } else if (frame.gop.pictureType == VkVideoGopStructure::FRAME_TYPE_I) {
            info.frame_type = STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY;
        } else {
            info.frame_type = STD_VIDEO_AV1_FRAME_TYPE_INTER;
        }
        info.current_frame_id = static_cast<uint32_t>(frame.gop.encodeOrder % (1ULL << frameIdBits));
        info.order_hint = static_cast<uint8_t>(frame.picOrderCntVal % (1 << orderHintBits));
        if (frame.overlayFrame) {
            frame.frameToShowBufId = m_dpb->GetOverlayRefBufId(static_cast<int32_t>(frame.picOrderCntVal));
            const int32_t refBufDpbId = m_dpb->GetRefBufDpbId(frame.frameToShowBufId);
            refName = m_dpb->GetRefName(refBufDpbId);
            info.frame_type = m_dpb->GetFrameType(refBufDpbId);
            info.current_frame_id = m_dpb->GetFrameId(refBufDpbId);
        }
        info.flags.show_frame =
            (((refName == STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME) || (refName == STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME) ||
              (refName == STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME)) &&
             !frame.overlayFrame)
                ? 0
                : 1;
        info.flags.showable_frame = info.flags.show_frame ? ((info.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY) ? 0 : 1) : 1;
        if ((info.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY) && info.flags.show_frame) {
            info.flags.error_resilient_mode = 1;
        }
        frame.shownKeyFrameOrSwitch = ((info.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY) && info.flags.show_frame) ||
                                      (info.frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH);
        if (!frame.showExistingFrame &&
            ((info.frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTER) || (info.frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH))) {
            for (int32_t ref = STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME; ref <= STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME; ref++) {
                info.ref_frame_idx[ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] =
                    static_cast<int8_t>(m_dpb->GetRefBufId(static_cast<StdVideoAV1ReferenceName>(ref)));
            }
            for (uint32_t bufIdx = 0; bufIdx < STD_VIDEO_AV1_NUM_REF_FRAMES; bufIdx++) {
                const int32_t dpbIdx = m_dpb->GetRefBufDpbId(static_cast<int32_t>(bufIdx));
                info.ref_order_hint[bufIdx] = static_cast<uint8_t>(m_dpb->GetPicOrderCntVal(dpbIdx) % (1 << ORDER_HINT_BITS));
            }
        }
        info.primary_ref_frame = static_cast<uint8_t>(
            m_dpb->GetPrimaryRefFrame(info.frame_type, refName, info.flags.error_resilient_mode, frame.overlayFrame));
    }

    void TraceHeader(const Frame& frame, StdVideoAV1ReferenceName refName, const int32_t* names, bool cdfOnly) const {
        const StdVideoEncodeAV1PictureInfo& info = frame.av1Info;
        Line line("  av1");
        line.Num("frame_type", info.frame_type)
            .Num("ref_name", refName)
            .Num("refresh", info.refresh_frame_flags)
            .Flags("flags", {{"show_frame", info.flags.show_frame},
                             {"showable_frame", info.flags.showable_frame},
                             {"error_resilient_mode", info.flags.error_resilient_mode},
                             {"overlay", frame.overlayFrame}})
            .Num("order_hint", info.order_hint)
            .Num("current_frame_id", info.current_frame_id)
            .Num("primary_ref_frame", info.primary_ref_frame)
            .Array("ref_frame_idx", info.ref_frame_idx, STD_VIDEO_AV1_REFS_PER_FRAME)
            .Array("ref_order_hint", info.ref_order_hint, STD_VIDEO_AV1_NUM_REF_FRAMES);
        if (names != nullptr) {
            line.Array("names", names, STD_VIDEO_AV1_REFS_PER_FRAME).Num("cdf_only", cdfOnly);
        }
        line.Emit(m_out);
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
    VkEncDpbAV1* m_dpb;
    Images m_images;
    StdVideoAV1SequenceHeader m_seqHdr{};
    uint32_t m_maxDpbPicturesCount = 0;
    uint32_t m_numBFramesToEncode = 0;
    int32_t m_lastKeyFrameOrderHint = 0;
    uint64_t m_encodeEncodeFrameNum = 0;
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
            "Drives the encoder's DPB class through a GOP sequence as the encoder does, and writes the\n"
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
        frame->timeStamp = i;
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
