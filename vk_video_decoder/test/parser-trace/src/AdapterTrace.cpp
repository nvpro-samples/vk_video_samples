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

// vk-video-adapter-trace runs the decoder sample's parser adapter, VulkanVideoParser.cpp, over an
// H.264, H.265, AV1 or VP9 stream with no Vulkan device. NvVideoParser parses the stream, the
// adapter assigns the DPB slots and builds each picture's decode parameters, and a recording
// decoder in place of VkVideoDecoder writes what the decode would submit: the setup slot, the
// reference slots with their Std reference information, the codec's slot arrays, and the
// begin-coding list VkVideoDecoder builds from them. The recording decoder and its picture pool
// make the choices of VkVideoDecoder and VulkanVideoFrameBuffer that reach the adapter: how many
// pictures there are, which free picture a new one takes, and when a new video session resets
// the DPB.
//
// scripts/slot_oracle.py checks the trace against the DPB slot rules of the Vulkan specification.
// See ../README.md.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "StreamInput.h"
#include "TraceClient.h"
#include "vkvideo_parser/VulkanVideoParser.h"

using namespace parser_trace;

namespace {

// VkVideoDecoder: the decode images in flight beyond the parser's minimum number of pictures, and
// VulkanVideoFrameBuffer::maxImages.
constexpr uint32_t kDecodeImagesInFlight = 8;
constexpr uint32_t kMaxImages = 32;
constexpr uint32_t kMaxActiveReferencePictures = VkParserPerFrameDecodeParameters::MAX_DPB_REF_SLOTS;

// The pictures the adapter decodes into, managed as VulkanVideoFrameBuffer manages them: a picture
// is free when nothing holds it, and a new picture takes the free one decoded longest ago. The
// consumer takes a displayed picture at once, so display holds no picture.
class PicturePool : public IVulkanVideoFrameBufferParserCb {
   public:
    explicit PicturePool(FILE* out) : m_out(out) {}

    void Grow(uint32_t count) {
        while (m_pictures.size() < count) {
            m_pictures.push_back(std::make_unique<vkPicBuffBase>());
        }
    }

    uint32_t Size() const { return static_cast<uint32_t>(m_pictures.size()); }

    void SetDecodeOrder(int32_t picIdx, uint64_t decodeOrder) {
        if ((picIdx >= 0) && (static_cast<size_t>(picIdx) < m_pictures.size())) {
            m_pictures[picIdx]->m_decodeOrder = decodeOrder;
        }
    }

    // VulkanVideoFrameBuffer::ReservePictureBuffer, with its search bound: a free picture is taken
    // only if its decode order is below picture 0's plus 1000.
    vkPicBuffBase* ReservePictureBuffer() override {
        int32_t found = -1;
        if (!m_pictures.empty()) {
            int64_t minDecodeOrder = static_cast<int64_t>(m_pictures[0]->m_decodeOrder) + 1000;
            for (uint32_t picIdx = 0; picIdx < m_pictures.size(); picIdx++) {
                const int64_t decodeOrder = static_cast<int64_t>(m_pictures[picIdx]->m_decodeOrder);
                if (m_pictures[picIdx]->IsAvailable() && (decodeOrder < minDecodeOrder)) {
                    found = static_cast<int32_t>(picIdx);
                    minDecodeOrder = decodeOrder;
                }
            }
        }
        if (found < 0) {
            Line("reserve-failed").Num("pool", Size()).Emit(m_out);
            return nullptr;
        }
        vkPicBuffBase* picture = m_pictures[found].get();
        picture->Reset();
        picture->AddRef();
        picture->m_picIdx = found;
        return picture;
    }

    int32_t QueueDecodedPictureForDisplay(int8_t picId, VulkanVideoDisplayPictureInfo* pDispInfo) override {
        Line("disp").Num("idx", picId).Num("pts", (pDispInfo != nullptr) ? pDispInfo->timestamp : 0).Emit(m_out);
        m_displayed++;
        return picId;
    }

    uint64_t Displayed() const { return m_displayed; }

   private:
    FILE* m_out;
    std::vector<std::unique_ptr<vkPicBuffBase>> m_pictures;
    uint64_t m_displayed = 0;
};

// The video session VkVideoDecoder::StartVideoSequence keeps or creates. A new session starts with
// every DPB slot inactive (the decoder resets it before its first decode).
struct Session {
    bool exists = false;
    VkVideoCodecOperationFlagBitsKHR codec = VK_VIDEO_CODEC_OPERATION_NONE_KHR;
    VkVideoChromaSubsamplingFlagBitsKHR chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_INVALID_KHR;
    VkVideoComponentBitDepthFlagsKHR lumaBitDepth = 0;
    VkVideoComponentBitDepthFlagsKHR chromaBitDepth = 0;
    uint32_t codecProfile = 0;
    bool canUseFields = false;
    VkExtent2D maxCodedExtent = {};
    uint32_t maxDpbSlots = 0;
    uint32_t maxActiveReferencePictures = 0;
};

const char* CodecName(VkVideoCodecOperationFlagBitsKHR codec) {
    switch (codec) {
        case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
            return "h264";
        case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
            return "h265";
        case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
            return "av1";
        case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR:
            return "vp9";
        default:
            return "unknown";
    }
}

// The Std reference information in the pNext chain of a reference slot, or nullptr.
template <typename DpbSlotInfo>
const DpbSlotInfo* FindDpbSlotInfo(const VkVideoReferenceSlotInfoKHR& slot, VkStructureType type) {
    const auto* next = static_cast<const VkBaseInStructure*>(slot.pNext);
    while (next != nullptr) {
        if (next->sType == type) {
            return reinterpret_cast<const DpbSlotInfo*>(next);
        }
        next = next->pNext;
    }
    return nullptr;
}

// The recording decoder: what VkVideoDecoder would submit for each picture, as trace lines.
class RecordingDecoder : public IVulkanVideoDecoderHandler {
   public:
    RecordingDecoder(FILE* out, VkVideoCodecOperationFlagBitsKHR codec, PicturePool& pool, uint32_t offsetAlignment,
                     uint32_t sizeAlignment)
        : m_out(out), m_codec(codec), m_pool(pool), m_offsetAlignment(offsetAlignment), m_sizeAlignment(sizeAlignment) {}

    // VkVideoDecoder::StartVideoSequence: the number of pictures, and the session.
    int32_t StartVideoSequence(VkParserDetectedVideoFormat* format) override {
        const uint32_t numDecodeSurfaces =
            std::min(kMaxImages, std::max(m_pool.Size(), format->minNumDecodeSurfaces + kDecodeImagesInFlight));
        m_pool.Grow(numDecodeSurfaces);

        const VkExtent2D maxCodedExtent = {
            std::max(static_cast<uint32_t>(format->display_area.right - format->display_area.left), format->coded_width),
            std::max(static_cast<uint32_t>(format->display_area.bottom - format->display_area.top), format->coded_height)};
        const uint32_t maxDpbSlots = format->maxNumDpbSlots;
        // VulkanVideoSession::IsCompatible, asked with the counts StartVideoSequence passes it.
        const bool compatible =
            m_session.exists && (m_session.codec == format->codec) && (m_session.chromaSubsampling == format->chromaSubsampling) &&
            (m_session.lumaBitDepth == format->lumaBitDepth) && (m_session.chromaBitDepth == format->chromaBitDepth) &&
            (m_session.codecProfile == format->codecProfile) && (m_session.canUseFields == (format->canUseFields != 0)) &&
            (maxCodedExtent.width <= m_session.maxCodedExtent.width) &&
            (maxCodedExtent.height <= m_session.maxCodedExtent.height) && (maxDpbSlots <= m_session.maxDpbSlots) &&
            (std::max(maxDpbSlots, kMaxActiveReferencePictures) <= m_session.maxActiveReferencePictures);
        if (!compatible) {
            m_session.exists = true;
            m_session.codec = format->codec;
            m_session.chromaSubsampling = format->chromaSubsampling;
            m_session.lumaBitDepth = format->lumaBitDepth;
            m_session.chromaBitDepth = format->chromaBitDepth;
            m_session.codecProfile = format->codecProfile;
            m_session.canUseFields = (format->canUseFields != 0);
            m_session.maxCodedExtent = maxCodedExtent;
            m_session.maxDpbSlots = maxDpbSlots;
            m_session.maxActiveReferencePictures = std::min(maxDpbSlots, kMaxActiveReferencePictures);
        }

        Line("seq")
            .Num("index", static_cast<int64_t>(m_sequences++))
            .Str("codec", CodecName(format->codec))
            .Num("width", format->coded_width)
            .Num("height", format->coded_height)
            .Num("chroma", format->chromaSubsampling)
            .Num("bitDepth", format->bit_depth_luma_minus8 + 8)
            .Num("profile", format->codecProfile)
            .Flags("flags", {{"update", static_cast<uint32_t>(format->sequenceUpdate)},
                             {"reconfigureFormat", static_cast<uint32_t>(format->sequenceReconfigureFormat)},
                             {"reconfigureExtent", static_cast<uint32_t>(format->sequenceReconfigureCodedExtent)},
                             {"progressive", static_cast<uint32_t>(format->progressive_sequence)},
                             {"fields", static_cast<uint32_t>(format->canUseFields)}})
            .Num("minDecodeSurfaces", format->minNumDecodeSurfaces)
            .Num("maxDpbSlots", maxDpbSlots)
            .Num("pictures", numDecodeSurfaces)
            .Emit(m_out);
        Line("session")
            .Str("state", compatible ? "kept" : "new")
            .Num("maxDpbSlots", m_session.maxDpbSlots)
            .Num("maxActiveReferencePictures", m_session.maxActiveReferencePictures)
            .Emit(m_out);
        return static_cast<int32_t>(numDecodeSurfaces);
    }

    bool UpdatePictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>&, VkSharedBaseObj<VkVideoRefCountBase>&) override {
        return true;
    }

    int32_t DecodePictureWithParameters(VkParserPerFrameDecodeParameters* params, VkParserDecodePictureInfo*) override {
        const VkVideoDecodeInfoKHR& decode = params->decodeFrameInfo;
        m_pool.SetDecodeOrder(params->currPicIdx, m_decoded);

        Line("pic")
            .Num("index", static_cast<int64_t>(m_decoded))
            .Num("idx", params->currPicIdx)
            .Num("setup", (decode.pSetupReferenceSlot != nullptr) ? decode.pSetupReferenceSlot->slotIndex : -1)
            .Num("refs", decode.referenceSlotCount)
            .Emit(m_out);
        TracePicture(decode);
        if (decode.pSetupReferenceSlot != nullptr) {
            TraceSlot("setup", *params, *decode.pSetupReferenceSlot, true);
        }
        for (uint32_t i = 0; i < decode.referenceSlotCount; i++) {
            TraceSlot("ref", *params, decode.pReferenceSlots[i], true);
        }
        // VkVideoDecoder::DecodePictureWithParameters builds the begin-coding list this way: the
        // decode's reference slots and the setup slot after them (the adapter stores it at
        // pReferenceSlots[referenceSlotCount]), or the setup slot alone when there is no reference.
        const uint32_t beginCount = decode.referenceSlotCount + ((decode.pSetupReferenceSlot != nullptr) ? 1 : 0);
        const VkVideoReferenceSlotInfoKHR* begin =
            (decode.referenceSlotCount > 0) ? decode.pReferenceSlots : decode.pSetupReferenceSlot;
        for (uint32_t i = 0; i < beginCount; i++) {
            TraceSlot("begin", *params, begin[i], false);
        }
        m_decoded++;
        return params->currPicIdx;
    }

    VkDeviceSize GetBitstreamBuffer(VkDeviceSize size, VkDeviceSize minBitstreamBufferOffsetAlignment,
                                    VkDeviceSize minBitstreamBufferSizeAlignment, const uint8_t* pInitializeBufferMemory,
                                    VkDeviceSize initializeBufferMemorySize,
                                    VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer) override {
        return CreateHostBitstreamBuffer(size, std::max<VkDeviceSize>(minBitstreamBufferOffsetAlignment, m_offsetAlignment),
                                         std::max<VkDeviceSize>(minBitstreamBufferSizeAlignment, m_sizeAlignment),
                                         pInitializeBufferMemory, initializeBufferMemorySize, bitstreamBuffer);
    }

    uint64_t Decoded() const { return m_decoded; }

   private:
    // The codec's picture information that decides reference picture setup, and its slot arrays.
    void TracePicture(const VkVideoDecodeInfoKHR& decode) const {
        const auto* next = static_cast<const VkBaseInStructure*>(decode.pNext);
        for (; next != nullptr; next = next->pNext) {
            if (next->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR) {
                const StdVideoDecodeH264PictureInfo& std =
                    *reinterpret_cast<const VkVideoDecodeH264PictureInfoKHR*>(next)->pStdPictureInfo;
                Line("  h264")
                    .Num("frame_num", std.frame_num)
                    .Array("poc", std.PicOrderCnt, 2)
                    .Flags("flags", {{"field", std.flags.field_pic_flag},
                                     {"bottom", std.flags.bottom_field_flag},
                                     {"second", std.flags.complementary_field_pair},
                                     {"ref", std.flags.is_reference},
                                     {"intra", std.flags.is_intra},
                                     {"idr", std.flags.IdrPicFlag}})
                    .Emit(m_out);
                return;
            }
            if (next->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR) {
                const StdVideoDecodeH265PictureInfo& std =
                    *reinterpret_cast<const VkVideoDecodeH265PictureInfoKHR*>(next)->pStdPictureInfo;
                Line("  h265")
                    .Num("poc", std.PicOrderCntVal)
                    .Flags("flags",
                           {{"irap", std.flags.IrapPicFlag}, {"idr", std.flags.IdrPicFlag}, {"isref", std.flags.IsReference}})
                    .Array("before", std.RefPicSetStCurrBefore, STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE)
                    .Array("after", std.RefPicSetStCurrAfter, STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE)
                    .Array("lt", std.RefPicSetLtCurr, STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE)
                    .Emit(m_out);
                return;
            }
            if (next->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PICTURE_INFO_KHR) {
                const auto* info = reinterpret_cast<const VkVideoDecodeAV1PictureInfoKHR*>(next);
                const StdVideoDecodeAV1PictureInfo& std = *info->pStdPictureInfo;
                Line("  av1")
                    .Num("frame_type", std.frame_type)
                    .Num("order_hint", std.OrderHint)
                    .Hex("refresh", std.refresh_frame_flags)
                    .Array("names", info->referenceNameSlotIndices, VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR)
                    .Emit(m_out);
                return;
            }
            if (next->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PICTURE_INFO_KHR) {
                const auto* info = reinterpret_cast<const VkVideoDecodeVP9PictureInfoKHR*>(next);
                const StdVideoDecodeVP9PictureInfo& std = *info->pStdPictureInfo;
                Line("  vp9")
                    .Num("frame_type", std.frame_type)
                    .Flags("flags", {{"intra_only", std.flags.intra_only}})
                    .Hex("refresh", std.refresh_frame_flags)
                    .Array("names", info->referenceNameSlotIndices, VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR)
                    .Emit(m_out);
                return;
            }
        }
        Line("  picture-info").Str("state", "missing").Emit(m_out);
    }

    // The picture a slot's resource belongs to: the current picture for the setup resource, the
    // reference's picture for a reference resource, -1 for none, -2 for any other resource.
    static int32_t ResourcePicture(const VkParserPerFrameDecodeParameters& params, const VkVideoPictureResourceInfoKHR* resource) {
        if (resource == nullptr) {
            return -1;
        }
        if (resource == &params.dpbSetupPictureResource) {
            return params.currPicIdx;
        }
        const VkVideoPictureResourceInfoKHR* first = &params.pictureResources[0];
        if ((resource >= first) && (resource < first + VkParserPerFrameDecodeParameters::MAX_DPB_REF_AND_SETUP_SLOTS)) {
            return params.pGopReferenceImagesIndexes[resource - first];
        }
        return -2;
    }

    void TraceSlot(const char* tag, const VkParserPerFrameDecodeParameters& params, const VkVideoReferenceSlotInfoKHR& slot,
                   bool withStd) const {
        std::string text = std::string("  ") + tag;
        Line line(text.c_str());
        if (!withStd) {
            line.Str("role", (slot.pPictureResource == &params.dpbSetupPictureResource) ? "setup" : "ref");
        }
        line.Num("slot", slot.slotIndex).Num("res", ResourcePicture(params, slot.pPictureResource));
        if ((m_codec == VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) && (slot.pPictureResource != nullptr)) {
            line.Num("width", slot.pPictureResource->codedExtent.width).Num("height", slot.pPictureResource->codedExtent.height);
        }
        if (withStd) {
            TraceStd(line, slot);
        }
        line.Emit(m_out);
    }

    void TraceStd(Line& line, const VkVideoReferenceSlotInfoKHR& slot) const {
        switch (m_codec) {
            case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: {
                const auto* info =
                    FindDpbSlotInfo<VkVideoDecodeH264DpbSlotInfoKHR>(slot, VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
                if ((info == nullptr) || (info->pStdReferenceInfo == nullptr)) {
                    line.Str("std", "missing");
                    return;
                }
                const StdVideoDecodeH264ReferenceInfo& std = *info->pStdReferenceInfo;
                line.Num("frame_num", std.FrameNum)
                    .Array("poc", std.PicOrderCnt, 2)
                    .Flags("flags", {{"top", std.flags.top_field_flag},
                                     {"bottom", std.flags.bottom_field_flag},
                                     {"lt", std.flags.used_for_long_term_reference},
                                     {"nonexisting", std.flags.is_non_existing}});
                return;
            }
            case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR: {
                const auto* info =
                    FindDpbSlotInfo<VkVideoDecodeH265DpbSlotInfoKHR>(slot, VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR);
                if ((info == nullptr) || (info->pStdReferenceInfo == nullptr)) {
                    line.Str("std", "missing");
                    return;
                }
                const StdVideoDecodeH265ReferenceInfo& std = *info->pStdReferenceInfo;
                line.Num("poc", std.PicOrderCntVal)
                    .Flags("flags", {{"lt", std.flags.used_for_long_term_reference}, {"unused", std.flags.unused_for_reference}});
                return;
            }
            case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR: {
                const auto* info =
                    FindDpbSlotInfo<VkVideoDecodeAV1DpbSlotInfoKHR>(slot, VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_DPB_SLOT_INFO_KHR);
                if ((info == nullptr) || (info->pStdReferenceInfo == nullptr)) {
                    line.Str("std", "missing");
                    return;
                }
                const StdVideoDecodeAV1ReferenceInfo& std = *info->pStdReferenceInfo;
                line.Num("frame_type", std.frame_type)
                    .Num("order_hint", std.OrderHint)
                    .Hex("sign_bias", std.RefFrameSignBias)
                    .Array("saved", std.SavedOrderHints, STD_VIDEO_AV1_NUM_REF_FRAMES)
                    .Flags("flags", {{"cdf", std.flags.disable_frame_end_update_cdf}, {"seg", std.flags.segmentation_enabled}});
                return;
            }
            default:
                return;  // VP9 has no Std reference information.
        }
    }

    FILE* m_out;
    VkVideoCodecOperationFlagBitsKHR m_codec;
    PicturePool& m_pool;
    uint32_t m_offsetAlignment;
    uint32_t m_sizeAlignment;
    Session m_session;
    uint64_t m_sequences = 0;
    uint64_t m_decoded = 0;
};

int Usage(const char* program) {
    fprintf(stderr,
            "Usage: %s --codec h264|h265|av1|vp9 --format bytestream|ivf|obu|annexb -i <stream> [-o <trace>]\n"
            "          [--offset-alignment N] [--size-alignment N]\n"
            "Runs the decoder sample's parser adapter over the stream with no Vulkan device and writes\n"
            "the DPB slots, reference information and begin-coding list of every decode.\n"
            "  bytestream  an H.264 or H.265 Annex B byte stream, given as the decoder sample gives an\n"
            "              elementary stream: the rest of the stream on each call, with partial parsing\n"
            "  ivf         one packet per IVF frame (AV1, VP9)\n"
            "  obu         the AV1 low-overhead OBU format, one packet per temporal unit\n"
            "  annexb      AV1 Annex B, one packet per temporal unit\n",
            program);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const char* codecName = nullptr;
    const char* format = nullptr;
    const char* input = nullptr;
    const char* output = nullptr;
    uint32_t offsetAlignment = 256;
    uint32_t sizeAlignment = 256;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            return Usage(argv[0]);
        }
        if (arg == "--codec") {
            codecName = argv[++i];
        } else if (arg == "--format") {
            format = argv[++i];
        } else if (arg == "-i") {
            input = argv[++i];
        } else if (arg == "-o") {
            output = argv[++i];
        } else if (arg == "--offset-alignment") {
            offsetAlignment = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--size-alignment") {
            sizeAlignment = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        } else {
            return Usage(argv[0]);
        }
    }
    if ((codecName == nullptr) || (format == nullptr) || (input == nullptr)) {
        return Usage(argv[0]);
    }
    const CodecInfo* codecInfo = FindCodec(codecName);
    if ((codecInfo == nullptr) || !FormatFits(*codecInfo, format)) {
        return Usage(argv[0]);
    }
    const bool byteStream = (strcmp(format, "bytestream") == 0);

    std::vector<uint8_t> file;
    if (!ReadFile(input, file)) {
        fprintf(stderr, "Cannot read %s\n", input);
        return 1;
    }
    std::vector<Packet> packets;
    std::string error;
    if (!ReadPackets(format, file, packets, error)) {
        fprintf(stderr, "%s: %s\n", input, error.c_str());
        return 1;
    }

    FILE* out = stdout;
    if (output != nullptr) {
        out = fopen(output, "w");
        if (out == nullptr) {
            fprintf(stderr, "Cannot write %s\n", output);
            return 1;
        }
    }

    Line trace("adapter-trace");
    trace.Num("version", 1).Str("codec", codecName).Str("format", format);
    if (byteStream) {
        trace.Num("bytes", static_cast<int64_t>(file.size()));
    } else {
        trace.Num("packets", static_cast<int64_t>(packets.size()));
    }
    trace.Num("offsetAlignment", offsetAlignment).Num("sizeAlignment", sizeAlignment).Emit(out);

    // The pool and the decoder outlive the adapter: the adapter and the parser release the
    // pictures they hold when they are destroyed.
    auto pool = std::make_shared<PicturePool>(out);
    auto decoder = std::make_shared<RecordingDecoder>(out, codecInfo->operation, *pool, offsetAlignment, sizeAlignment);
    int status = 0;
    {
        VkSharedBaseObj<IVulkanVideoDecoderHandler> decoderHandler(decoder);
        VkSharedBaseObj<IVulkanVideoFrameBufferParserCb> frameBufferCb(pool);
        VkSharedBaseObj<IVulkanVideoParser> adapter;
        // As VulkanVideoProcessor creates it: the two counts are not used.
        const VkResult result =
            vulkanCreateVideoParser(decoderHandler, frameBufferCb, codecInfo->operation, &codecInfo->stdExtension, 1, 1,
                                    2 * 1024 * 1024, offsetAlignment, sizeAlignment, 0, false, adapter);
        if ((result != VK_SUCCESS) || !adapter) {
            fprintf(stderr, "vulkanCreateVideoParser failed: %d\n", static_cast<int>(result));
            status = 1;
        } else {
            // As VulkanVideoProcessor::ParserProcessNextDataChunk gives the adapter a stream: an
            // elementary stream with partial parsing, advancing by the bytes consumed; a demuxed
            // stream one packet per call, with no flags; then an empty end-of-stream packet.
            if (byteStream) {
                size_t offset = 0;
                for (uint64_t call = 0; offset < file.size(); call++) {
                    VkParserSourceDataPacket packet = {};
                    packet.payload = file.data() + offset;
                    packet.payload_size = file.size() - offset;
                    size_t parsedBytes = 0;
                    const VkResult parsed = adapter->ParseVideoData(&packet, &parsedBytes, true);
                    Line("pkt")
                        .Num("index", static_cast<int64_t>(call))
                        .Num("offset", static_cast<int64_t>(offset))
                        .Num("parsed", static_cast<int64_t>(parsedBytes))
                        .Emit(out);
                    if (parsed != VK_SUCCESS) {
                        Line("pkt-error").Num("index", static_cast<int64_t>(call)).Emit(out);
                    }
                    if ((parsedBytes == 0) || (parsedBytes > packet.payload_size)) {
                        Line("pkt-stalled").Num("index", static_cast<int64_t>(call)).Emit(out);
                        break;
                    }
                    offset += parsedBytes;
                }
            }
            for (size_t i = 0; i < packets.size(); i++) {
                Line("pkt")
                    .Num("index", static_cast<int64_t>(i))
                    .Num("bytes", static_cast<int64_t>(packets[i].data.size()))
                    .Emit(out);
                VkParserSourceDataPacket packet = {};
                packet.payload = packets[i].data.data();
                packet.payload_size = packets[i].data.size();
                size_t parsedBytes = 0;
                if (adapter->ParseVideoData(&packet, &parsedBytes, false) != VK_SUCCESS) {
                    Line("pkt-error").Num("index", static_cast<int64_t>(i)).Emit(out);
                }
            }
            VkParserSourceDataPacket endOfStream = {};
            endOfStream.flags = VK_PARSER_PKT_ENDOFSTREAM;
            size_t parsedBytes = 0;
            adapter->ParseVideoData(&endOfStream, &parsedBytes, byteStream);
            Line("eos")
                .Num("pictures", static_cast<int64_t>(decoder->Decoded()))
                .Num("displayed", static_cast<int64_t>(pool->Displayed()))
                .Emit(out);
        }
    }

    if (out != stdout) {
        fclose(out);
    }
    return status;
}
