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

#include <chrono>
#include <cstring>
#include <limits>
#include "VkVideoEncoder/VkVideoEncoderAV1.h"
#include "VkVideoCore/VulkanVideoCapabilities.h"
#include "Logger.h"


#define FrameIsKey(frameType)   (frameType == STD_VIDEO_AV1_FRAME_TYPE_KEY)
#define FrameIsIntraOnly(frameType) (frameType == STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY)
#define FrameIsIntra(frameType) ((FrameIsKey(frameType)) || (FrameIsIntraOnly(frameType)))
#define FrameIsInter(frameType) ((frameType == STD_VIDEO_AV1_FRAME_TYPE_INTER))
#define FrameIsSwitch(frameType) ((frameType == STD_VIDEO_AV1_FRAME_TYPE_SWITCH))

#define MAKE_FOURCC( ch0, ch1, ch2, ch3 )                               \
                ( (uint32_t)(uint8_t)(ch0) | ( (uint32_t)(uint8_t)(ch1) << 8 ) |    \
                ( (uint32_t)(uint8_t)(ch2) << 16 ) | ( (uint32_t)(uint8_t)(ch3) << 24 ) )

static inline void mem_put_le32(uint8_t* mem, int val)
{
    mem[0] = (uint8_t)((val >>  0) & 0xff);
    mem[1] = (uint8_t)((val >>  8) & 0xff);
    mem[2] = (uint8_t)((val >> 16) & 0xff);
    mem[3] = (uint8_t)((val >> 24) & 0xff);
}

static inline void mem_put_le16(uint8_t* mem, int val)
{
    mem[0] = (uint8_t)((val >>  0) & 0xff);
    mem[1] = (uint8_t)((val >>  8) & 0xff);
}

VkResult CreateVideoEncoderAV1(const VulkanDeviceContext* vkDevCtx,
                               VkSharedBaseObj<EncoderConfig>& encoderconfig,
                               VkSharedBaseObj<VkVideoEncoder>& encoder)
{
    VkSharedBaseObj<VkVideoEncoderAV1> vkEncoderAV1(new VkVideoEncoderAV1(vkDevCtx));
    if (vkEncoderAV1) {
        VkResult result = vkEncoderAV1->InitEncoderCodec(encoderconfig);
        if (result != VK_SUCCESS) {
            return result;
        }

        encoder = vkEncoderAV1;
        return VK_SUCCESS;
    }

    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult VkVideoEncoderAV1::InitEncoderCodec(VkSharedBaseObj<EncoderConfig>& encoderConfig)
{
    auto* av1Config = encoderConfig->GetEncoderConfigAV1();
    assert(av1Config);
    m_encoderConfig = std::shared_ptr<EncoderConfigAV1>(encoderConfig, av1Config);

    if (m_encoderConfig->codec != VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) {
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    VkResult result = InitEncoder(encoderConfig);
    if (result != VK_SUCCESS) {
        LOG_ERROR("InitEncoder() failed with ret(%d)\n", result);
        return result;
    }

    const auto& encodeCaps = m_encoderConfig->av1EncodeCapabilities;
    if (encoderConfig->gopStructure.GetConsecutiveBFrameCount() > 0 &&
        encodeCaps.maxSingleReferenceCount < 2 &&
        encodeCaps.maxUnidirectionalCompoundReferenceCount == 0 &&
        encodeCaps.maxBidirectionalCompoundReferenceCount == 0) {
        LOG_S_INFO << "B-frames were requested but the implementation does not support multiple reference frames!" << std::endl;
        assert(!"B-frames not supported");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // The DPB manager needs a slot for each of the eight reference frame buffers.
    if (m_maxDpbPicturesCount < STD_VIDEO_AV1_NUM_REF_FRAMES) {
        LOG_S_INFO << "The AV1 encoder needs " << STD_VIDEO_AV1_NUM_REF_FRAMES << " DPB slots, and the implementation has "
                   << m_maxDpbPicturesCount << "!" << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    m_encoderConfig->GetRateControlParameters(&m_rateControlInfo, m_rateControlLayersInfo, &m_stateAV1.m_rateControlInfoAV1, m_stateAV1.m_rateControlLayersInfoAV1);

    m_encoderConfig->InitSequenceHeader(&m_stateAV1.m_sequenceHeader, m_stateAV1.m_operatingPointsInfo);

    // The DPB: the session's slots and active references bound it, its pictures are images of the DPB
    // image pool, and every frame is set up. The GOP says whether it has B pictures.
    VvsDpbCreateInfo dpbCreateInfo{};
    dpbCreateInfo.apiVersion = VVS_DPB_API_VERSION;
    dpbCreateInfo.flags = VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT;
    if (m_encoderConfig->enableIntraRefresh) {
        dpbCreateInfo.flags |= VVS_DPB_CREATE_INTRA_REFRESH_BIT;
    }
    dpbCreateInfo.pVideoProfile = m_encoderConfig->videoCoreProfile.GetProfile();
    dpbCreateInfo.maxDpbSlots = m_maxDpbPicturesCount;
    dpbCreateInfo.maxActiveReferencePictures = m_encoderConfig->videoCapabilities.maxActiveReferencePictures;
    dpbCreateInfo.slotPolicy = VVS_DPB_SLOT_POLICY_LOWEST_FREE;
    dpbCreateInfo.resources = m_dpbPictures.Resources(&m_dpbImagePool);
    result = m_dpb.Create(dpbCreateInfo);
    if (result != VK_SUCCESS) {
        LOG_ERROR("InitEncoderCodec: the DPB context was not created (%d)\n", result);
        return result;
    }
    VvsDpbGopInfo dpbGopInfo{};
    dpbGopInfo.gopStructure = VVS_DPB_GOP_STRUCTURE_IRREGULAR;
    dpbGopInfo.gopFrameCount = m_encoderConfig->gopStructure.GetGopFrameCount();
    dpbGopInfo.consecutiveBipredictiveFrameCount = m_encoderConfig->gopStructure.GetConsecutiveBFrameCount();
    const VvsDpbAV1SequenceInfo dpbSequenceInfo{&m_stateAV1.m_sequenceHeader, &m_encoderConfig->av1EncodeCapabilities,
                                                m_encoderConfig->enableIntraRefresh ? &m_encoderConfig->intraRefreshCapabilities : nullptr,
                                                &dpbGopInfo};
    result = m_dpb.Sequence(dpbSequenceInfo, nullptr);
    if (result != VK_SUCCESS) {
        LOG_ERROR("InitEncoderCodec: the DPB refused the sequence header (%d)\n", result);
        return result;
    }

    VideoSessionParametersInfoAV1 videoSessionParametersInfo(*m_videoSession, &m_stateAV1.m_sequenceHeader,
                                                          nullptr/*decoderModelInfo*/,
                                                          1, m_stateAV1.m_operatingPointsInfo /*operatingPointsInfo*/,
                                                          encoderConfig->qualityLevel,
                                                          encoderConfig->enableQpMap, m_qpMapTexelSize);
    VkVideoSessionParametersCreateInfoKHR* encodeSessionParametersCreateInfo = videoSessionParametersInfo.getVideoSessionParametersInfo();
    VkVideoSessionParametersKHR sessionParameters;
    result = m_vkDevCtx->CreateVideoSessionParametersKHR(*m_vkDevCtx,
                                                         encodeSessionParametersCreateInfo,
                                                         nullptr,
                                                         &sessionParameters);
    if (result != VK_SUCCESS) {
        LOG_ERROR("EncodeFrame: Failed to get create video session parameters.\n");
        return result;
    }

    result = VulkanVideoSessionParameters::Create(m_vkDevCtx, m_videoSession,
                                                  sessionParameters, m_videoSessionParameters);
    if (result != VK_SUCCESS) {
        LOG_ERROR("EncodeFrame: Failed to get create video session object.\n");
        return result;
    }

    return VK_SUCCESS;
}

VkResult VkVideoEncoderAV1::InitRateControl(VkCommandBuffer cmdBuf, uint32_t qp)
{
    return VK_SUCCESS;
}

VkResult VkVideoEncoderAV1::EncodeVideoSessionParameters(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);

    assert(pFrameInfo->videoSession);
    assert(pFrameInfo->videoSessionParameters);

    VkVideoEncodeSessionParametersGetInfoKHR getInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
        nullptr,
        *pFrameInfo->videoSessionParameters
    };

    VkVideoEncodeSessionParametersFeedbackInfoKHR feedbackInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,
        nullptr,
    };

    size_t bufferSize = sizeof(encodeFrameInfo->bitstreamHeaderBuffer);
    VkResult result = m_vkDevCtx->GetEncodedVideoSessionParametersKHR(*m_vkDevCtx,
                                                                      &getInfo,
                                                                      &feedbackInfo,
                                                                      &bufferSize,
                                                                      encodeFrameInfo->bitstreamHeaderBuffer);
    if (result != VK_SUCCESS) {
        return result;
    }
    encodeFrameInfo->bitstreamHeaderBufferSize = bufferSize;

    // HDR10 STATIC METADATA, appended to the sequence header OBU.
    //
    // Both output arms consume this buffer and both put it in the right
    // place: BuildFrameObuSequence (file arm) copies it in first, and the
    // capture arm of WriteBitstreamToFile inserts it after the temporal
    // delimiter and before the frame OBU. So the temporal unit reads
    // TD, sequence header, metadata, frame -- which is the order a decoder
    // needs and the order that makes the metadata apply to the frames that
    // follow it.
    //
    if (m_encoderConfig->hdrMetadata.Any()) {
        bool truncated = false;
        const size_t used = encodeFrameInfo->bitstreamHeaderOffset +
                            encodeFrameInfo->bitstreamHeaderBufferSize;
        const size_t obuBytes = VkEncBuildAv1HdrMetadataObus(
            m_encoderConfig->hdrMetadata,
            encodeFrameInfo->bitstreamHeaderBuffer + used,
            sizeof(encodeFrameInfo->bitstreamHeaderBuffer) - used,
            &truncated);
        if (truncated) {
            LOG_ERROR("EncodeVideoSessionParameters: the HDR10 metadata "
                    "OBUs do not fit in the %zu-byte non-VCL header buffer "
                    "after %zu bytes of sequence header.\n",
                    sizeof(encodeFrameInfo->bitstreamHeaderBuffer), used);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        encodeFrameInfo->bitstreamHeaderBufferSize += obuBytes;
    }

    return result;

}
VkResult VkVideoEncoderAV1::StartOfVideoCodingEncodeOrder(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo, uint32_t frameIdx, uint32_t ofTotalFrames)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);
    if (!pFrameInfo->bShowExistingFrame) {
        encodeFrameInfo->frameEncodeEncodeOrderNum = m_encodeEncodeFrameNum++;
        if (m_encoderConfig->verboseFrameStruct) {
            DumpStateInfo("start encoding AV1 regular frame", 2, encodeFrameInfo, frameIdx, ofTotalFrames);
        }
    } else if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("start encoding AV1 show existing", 2, encodeFrameInfo, frameIdx, ofTotalFrames);
    }

    return VK_SUCCESS;
}

VkResult VkVideoEncoderAV1::ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                       uint32_t frameIdx, uint32_t ofTotalFrames)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("process DPB", 3, encodeFrameInfo, frameIdx, ofTotalFrames);
    }

    InitializeFrameHeader(&m_stateAV1.m_sequenceHeader, pFrameInfo);

    if (pFrameInfo->bShowExistingFrame) {
        // The buffer that holds the frame to show, and the frame's own fields for its header.
        uint8_t frameToShowMapIdx = 0;
        const VkResult result = m_dpb.ShowExistingFrame(pFrameInfo->picOrderCntVal, &frameToShowMapIdx, pFrameInfo->dpbResult);
        if (result != VK_SUCCESS) {
            LOG_ERROR("ProcessDpb: the DPB holds no frame to show (%d)\n", result);
            return result;
        }
        pFrameInfo->frameToShowBufId = frameToShowMapIdx;
        VvsDpbReferenceFrame frames[VVS_DPB_MAX_SLOTS];
        uint32_t count = VVS_DPB_MAX_SLOTS;
        m_dpb.GetReferenceFrames(&count, frames);
        for (uint32_t i = 0; i < count; i++) {
            if (((frames[i].av1BufferMask >> frameToShowMapIdx) & 1U) != 0) {
                pFrameInfo->stdPictureInfo.frame_type = frames[i].stdReferenceInfo.av1.frame_type;
                pFrameInfo->stdPictureInfo.current_frame_id = frames[i].stdReferenceInfo.av1.RefFrameId;
            }
        }
        return VK_SUCCESS;
    }

    const bool isIntraRefreshFrame = m_encoderConfig->gopStructure.IsIntraRefreshFrame(encodeFrameInfo->gopPosition);
    const bool intraRefresh = m_encoderConfig->enableIntraRefresh && isIntraRefreshFrame;

    // The DPB library decides the references, their names and buffers, the frame header's reference
    // fields, the prediction mode, and the slots.
    VvsDpbPictureInfo dpbFrameInfo{};
    switch (pFrameInfo->gopPosition.pictureType) {
        case VkVideoGopStructure::FRAME_TYPE_IDR:
            dpbFrameInfo.pictureType = VVS_DPB_PICTURE_TYPE_KEY;
            break;
        case VkVideoGopStructure::FRAME_TYPE_I:
            dpbFrameInfo.pictureType = VVS_DPB_PICTURE_TYPE_INTRA;
            break;
        case VkVideoGopStructure::FRAME_TYPE_B:
            dpbFrameInfo.pictureType = VVS_DPB_PICTURE_TYPE_BIPREDICTIVE;
            break;
        default:
            dpbFrameInfo.pictureType = VVS_DPB_PICTURE_TYPE_PREDICTIVE;
            break;
    }
    dpbFrameInfo.displayOrderIndex = pFrameInfo->picOrderCntVal;
    dpbFrameInfo.encodeOrderIndex = pFrameInfo->frameEncodeEncodeOrderNum;
    VvsDpbAV1EncodePictureInfo dpbPictureInfo{};
    dpbPictureInfo.pPictureInfo = &dpbFrameInfo;
    // The number of dirty intra-refresh regions in the current picture
    dpbPictureInfo.dirtyIntraRefreshRegions =
        intraRefresh ? (m_encoderConfig->intraRefreshCycleDuration - encodeFrameInfo->gopPosition.intraRefreshIndex - 1)
                     : VVS_DPB_NO_INTRA_REFRESH;
    dpbPictureInfo.reference = m_encoderConfig->gopStructure.IsFrameReference(encodeFrameInfo->gopPosition) ? VK_TRUE : VK_FALSE;
    dpbPictureInfo.pPictureInfoAV1 = &pFrameInfo->pictureInfo;
    dpbPictureInfo.pStdPictureInfo = &pFrameInfo->stdPictureInfo;
    m_dpbPictures.BeginPicture(pFrameInfo);
    const VkResult result = m_dpb.Picture(dpbPictureInfo, pFrameInfo->dpbResult);
    // The frame holds the images its commands read and write until it completes.
    m_dpbPictures.EndPicture(pFrameInfo, (result == VK_SUCCESS) ? &pFrameInfo->dpbResult.slots : nullptr);
    if (result != VK_SUCCESS) {
        LOG_ERROR("ProcessDpb: the DPB refused the frame (%d)\n", result);
        return result;
    }

    // A B picture the DPB codes without a reference after it is a P picture.
    if ((pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B) &&
        (pFrameInfo->pictureInfo.rateControlGroup != VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_BIPREDICTIVE_KHR)) {
        pFrameInfo->gopPosition.pictureType = VkVideoGopStructure::FRAME_TYPE_P;
    }

    const VvsDpbPictureSlots& slots = pFrameInfo->dpbResult.slots;
    vvs::dpb::SetEncodeSlots(slots, pFrameInfo->encodeInfo);
    pFrameInfo->pBeginCodingReferenceSlots = slots.referenceSlots;
    pFrameInfo->beginCodingReferenceSlotCount = slots.beginCodingSlotCount;

    return VK_SUCCESS;
}

VkResult VkVideoEncoderAV1::CreateFrameInfoBuffersQueue(uint32_t numPoolNodes)
{
    VkSharedBaseObj<VulkanBufferPool<VkVideoEncodeFrameInfoAV1>> _cmdBufPool(new VulkanBufferPool<VkVideoEncodeFrameInfoAV1>());

    if (_cmdBufPool) {
        _cmdBufPool->Init(numPoolNodes);
        m_frameInfoBuffersQueue = _cmdBufPool;
        return VK_SUCCESS;
    }

    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult VkVideoEncoderAV1::EncodeFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);

    if (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) {
        VkResult result = EncodeVideoSessionParameters(encodeFrameInfo);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    encodeFrameInfo->picOrderCntVal = encodeFrameInfo->gopPosition.inputOrder;

    pFrameInfo->bShowExistingFrame = false;
    pFrameInfo->bOverlayFrame = false;
    if (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) {
        assert(encodeFrameInfo->picOrderCntVal == 0);
        m_lastKeyFrameOrderHint = encodeFrameInfo->picOrderCntVal;
    }
    pFrameInfo->picOrderCntVal = encodeFrameInfo->picOrderCntVal - m_lastKeyFrameOrderHint;

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("input", 1, encodeFrameInfo);

        if (encodeFrameInfo->lastFrame) {
            LOG_S_INFO << "#### It is the last frame: " << encodeFrameInfo->frameInputOrderNum
                      << " of type " << VkVideoGopStructure::GetFrameTypeName(encodeFrameInfo->gopPosition.pictureType)
                      << " ###"
                      << std::endl << std::flush;
        }
    }

    pFrameInfo->encodeInfo.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    assert(pFrameInfo->encodeInfo.srcPictureResource.codedOffset.x == 0);
    assert(pFrameInfo->encodeInfo.srcPictureResource.codedOffset.y == 0);
    pFrameInfo->encodeInfo.srcPictureResource.codedExtent.width = m_encoderConfig->encodeWidth;
    pFrameInfo->encodeInfo.srcPictureResource.codedExtent.height = m_encoderConfig->encodeHeight;
    VkVideoPictureResourceInfoKHR* pSrcPictureResource = encodeFrameInfo->srcEncodeImageResource->GetPictureResourceInfo();
    pFrameInfo->encodeInfo.srcPictureResource.imageViewBinding = pSrcPictureResource->imageViewBinding;
    pFrameInfo->encodeInfo.srcPictureResource.baseArrayLayer = pSrcPictureResource->baseArrayLayer;

    //if (encodeFrameInfo->frameEncodeInputOrderNum == 0) {
    //    pFrameInfo->encodeInfo.flags |= VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    //}

    if (m_rateControlInfo.rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
        switch (encodeFrameInfo->gopPosition.pictureType) {
            case VkVideoGopStructure::FRAME_TYPE_IDR:
            case VkVideoGopStructure::FRAME_TYPE_I:
                pFrameInfo->pictureInfo.constantQIndex = (uint8_t)encodeFrameInfo->constQp.qpIntra;
                break;
            case VkVideoGopStructure::FRAME_TYPE_P:
                pFrameInfo->pictureInfo.constantQIndex = (uint8_t)encodeFrameInfo->constQp.qpInterP;
                break;
            case VkVideoGopStructure::FRAME_TYPE_B:
                pFrameInfo->pictureInfo.constantQIndex = (uint8_t)encodeFrameInfo->constQp.qpInterB;
                break;
            default:
                assert(!"Invalid picture type");
                break;
        }
        if (pFrameInfo->stdPictureInfo.pQuantization != nullptr) {
            assert(pFrameInfo->stdPictureInfo.pQuantization == &pFrameInfo->stdQuantInfo);
            pFrameInfo->stdQuantInfo.base_q_idx = (uint8_t)pFrameInfo->pictureInfo.constantQIndex;
        }
    }

    return VK_SUCCESS;
}

VkResult VkVideoEncoderAV1::CodecHandleRateControlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);

    // Fill-in the codec-specific parts next
    for (uint32_t layerIndx = 0; layerIndx < ARRAYSIZE(m_stateAV1.m_rateControlLayersInfoAV1); layerIndx++) {
        pFrameInfo->rateControlLayersInfoAV1[layerIndx] = m_stateAV1.m_rateControlLayersInfoAV1[layerIndx];
        pFrameInfo->rateControlLayersInfoAV1[layerIndx].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_LAYER_INFO_KHR;
        pFrameInfo->rateControlLayersInfo[layerIndx].pNext = &pFrameInfo->rateControlLayersInfoAV1[layerIndx];
    }

    pFrameInfo->rateControlInfoAV1 = m_stateAV1.m_rateControlInfoAV1;
    pFrameInfo->rateControlInfoAV1.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_INFO_KHR;
    pFrameInfo->rateControlInfoAV1.temporalLayerCount = m_encoderConfig->gopStructure.GetTemporalLayerCount();

    if (pFrameInfo->pControlCmdChain != nullptr) {
        pFrameInfo->rateControlInfoAV1.pNext = pFrameInfo->pControlCmdChain;
    }

    pFrameInfo->pControlCmdChain = (VkBaseInStructure*)&pFrameInfo->rateControlInfoAV1;

    return VK_SUCCESS;
}

void VkVideoEncoderAV1::InitializeFrameHeader(StdVideoAV1SequenceHeader* pSequenceHdr, VkVideoEncodeFrameInfoAV1* pFrameInfo)
{
    // No overlay frame support. Instead display the ARF, ARF2 using show_existing_frame=1
    // No switch frame support
    // No frame_refs_short_signalling

    StdVideoEncodeAV1PictureInfo* pStdPictureInfo = &pFrameInfo->stdPictureInfo;
    memset(pStdPictureInfo, 0, sizeof(StdVideoEncodeAV1PictureInfo));

    int32_t numPlanes = pSequenceHdr->pColorConfig && pSequenceHdr->pColorConfig->flags.mono_chrome ? 1 : 3;
    // unused: bool separate_uv_delta_q = pSequenceHdr->pColorConfig && pSequenceHdr->pColorConfig->flags.separate_uv_delta_q ? true : false;
    int32_t frameIdBits = pSequenceHdr->delta_frame_id_length_minus_2 + 2 +
                          pSequenceHdr->additional_frame_id_length_minus_1 + 1;
    int32_t orderHintBits = pSequenceHdr->order_hint_bits_minus_1 + 1;
    bool CodedLossless = false, AllLossless = false; // no lossless support

    if (pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) {
        pStdPictureInfo->frame_type = STD_VIDEO_AV1_FRAME_TYPE_KEY;
    } else if (pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_I) {
        pStdPictureInfo->frame_type = STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY;
    } else {
        pStdPictureInfo->frame_type = STD_VIDEO_AV1_FRAME_TYPE_INTER;
    }
    pStdPictureInfo->current_frame_id = (uint32_t)(pFrameInfo->gopPosition.encodeOrder % (1ULL << frameIdBits));
    // A coded extent padded up to minCodedExtent renders at the picture's extent.
    if ((m_encoderConfig->PictureWidth() != m_encoderConfig->encodeWidth) ||
        (m_encoderConfig->PictureHeight() != m_encoderConfig->encodeHeight)) {
        pStdPictureInfo->flags.render_and_frame_size_different = 1;
        pStdPictureInfo->render_width_minus_1 = (uint16_t)(m_encoderConfig->PictureWidth() - 1);
        pStdPictureInfo->render_height_minus_1 = (uint16_t)(m_encoderConfig->PictureHeight() - 1);
    }
    pStdPictureInfo->order_hint = (uint8_t)(pFrameInfo->picOrderCntVal % (1 << orderHintBits));

    // A frame encoded ahead of frames displayed before it is hidden, and shown later with
    // show_existing_frame. The DPB library writes the reference fields.
    const bool hidden = !pFrameInfo->bShowExistingFrame &&
                        (pFrameInfo->gopPosition.pictureType != VkVideoGopStructure::FRAME_TYPE_B) &&
                        (pFrameInfo->gopPosition.pictureType != VkVideoGopStructure::FRAME_TYPE_IDR) &&
                        (pFrameInfo->gopPosition.inputOrder != pFrameInfo->gopPosition.encodeOrder);
    pStdPictureInfo->flags.show_frame = hidden ? 0 : 1;
    pStdPictureInfo->flags.showable_frame = (pStdPictureInfo->flags.show_frame) ? (FrameIsKey(pStdPictureInfo->frame_type) ? 0 : 1) : 1;
    if ((pStdPictureInfo->frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY) && (pStdPictureInfo->flags.show_frame == 1)) {
        pStdPictureInfo->flags.error_resilient_mode = 1;
    }

    pStdPictureInfo->pTileInfo = nullptr;
    pStdPictureInfo->pQuantization = nullptr;
    pStdPictureInfo->pLoopFilter = nullptr;
    pStdPictureInfo->pCDEF = nullptr;
    pStdPictureInfo->pLoopRestoration = nullptr;

    if (m_encoderConfig->enableTiles) {
        pStdPictureInfo->pTileInfo = &pFrameInfo->stdTileInfo;

        if (m_encoderConfig->customTileConfig) {
            // copy tile config structure and udpate the pointers
            memcpy(&pFrameInfo->stdTileInfo, &m_encoderConfig->tileConfig, sizeof(StdVideoAV1TileInfo));
            pFrameInfo->stdTileInfo.pWidthInSbsMinus1 = nullptr;
            pFrameInfo->stdTileInfo.pHeightInSbsMinus1 = nullptr;

            if (!pFrameInfo->stdTileInfo.flags.uniform_tile_spacing_flag) {
                pFrameInfo->stdTileInfo.pHeightInSbsMinus1 = pFrameInfo->heightInSbsMinus1;
                pFrameInfo->stdTileInfo.pWidthInSbsMinus1 = pFrameInfo->widthInSbsMinus1;

                memset(pFrameInfo->heightInSbsMinus1, 0, sizeof(pFrameInfo->heightInSbsMinus1));
                memcpy(pFrameInfo->heightInSbsMinus1, m_encoderConfig->tileHeightInSbsMinus1, sizeof(uint16_t) * pFrameInfo->stdTileInfo.TileRows);

                memset(pFrameInfo->widthInSbsMinus1, 0, sizeof(pFrameInfo->widthInSbsMinus1));
                memcpy(pFrameInfo->widthInSbsMinus1, m_encoderConfig->tileWidthInSbsMinus1, sizeof(uint16_t) * pFrameInfo->stdTileInfo.TileCols);
            }
        } else {
            memset(&pFrameInfo->stdTileInfo, 0, sizeof(StdVideoAV1TileInfo));
            pFrameInfo->stdTileInfo.flags.uniform_tile_spacing_flag = 1;
            pFrameInfo->stdTileInfo.TileRows = 2;
            pFrameInfo->stdTileInfo.TileCols = 2;
        }
    }

    if (m_encoderConfig->enableQuant) {
        pStdPictureInfo->pQuantization = &pFrameInfo->stdQuantInfo;

        if (m_encoderConfig->customQuantConfig) {
            memcpy(&pFrameInfo->stdQuantInfo, &m_encoderConfig->quantConfig, sizeof(StdVideoAV1Quantization));
        } else {
            memset(&pFrameInfo->stdQuantInfo, 0, sizeof(StdVideoAV1Quantization));
            pFrameInfo->stdQuantInfo.base_q_idx = ((pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) ||
                                                   (pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_I)) ? 114 :
                                                        ((pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_P) ? 131 : 147);
        }
    }

    if (!CodedLossless && !pStdPictureInfo->flags.allow_intrabc && m_encoderConfig->enableLf) {
        pStdPictureInfo->pLoopFilter = &pFrameInfo->stdLfInfo;

        if (m_encoderConfig->customLfConfig) {
            memcpy(&pFrameInfo->stdLfInfo, &m_encoderConfig->lfConfig, sizeof(StdVideoAV1LoopFilter));
        } else {
            memset(&pFrameInfo->stdLfInfo, 0, sizeof(StdVideoAV1LoopFilter));
            pFrameInfo->stdLfInfo.loop_filter_level[0] = (pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) ? 11 :
                                                            ((pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_I) ? 15 :
                                                                ((pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_P) ? 18 : 23));
            pFrameInfo->stdLfInfo.loop_filter_level[1] = pFrameInfo->stdLfInfo.loop_filter_level[0];
            if (numPlanes > 1) {
                pFrameInfo->stdLfInfo.loop_filter_level[2] = pFrameInfo->stdLfInfo.loop_filter_level[0];
                pFrameInfo->stdLfInfo.loop_filter_level[3] = pFrameInfo->stdLfInfo.loop_filter_level[0];
            }

            pFrameInfo->stdLfInfo.flags.loop_filter_delta_enabled = 1;
            pFrameInfo->stdLfInfo.flags.loop_filter_delta_update = 1;
            pFrameInfo->stdLfInfo.update_ref_delta = 0xd1;
            pFrameInfo->stdLfInfo.loop_filter_ref_deltas[0] = 1;
            pFrameInfo->stdLfInfo.loop_filter_ref_deltas[4] = -1;
            pFrameInfo->stdLfInfo.loop_filter_ref_deltas[6] = -1;
            pFrameInfo->stdLfInfo.loop_filter_ref_deltas[7] = -1;
        }
    }

    if (!CodedLossless && !pStdPictureInfo->flags.allow_intrabc && m_encoderConfig->enableCdef) {
        pStdPictureInfo->pCDEF = &pFrameInfo->stdCdefInfo;

        if (m_encoderConfig->customCdefConfig) {
            memcpy(&pFrameInfo->stdCdefInfo, &m_encoderConfig->cdefConfig, sizeof(StdVideoAV1CDEF));
        } else {
            // The strengths each superblock chooses from, luma primary only, with damping 5: two at
            // quality levels 0 and 1, the fastest, and four at the others.
            memset(&pFrameInfo->stdCdefInfo, 0, sizeof(StdVideoAV1CDEF));
            pFrameInfo->stdCdefInfo.cdef_damping_minus_3 = 2;
            if (m_encoderConfig->qualityLevel <= 1) {
                pFrameInfo->stdCdefInfo.cdef_bits = 1;
                pFrameInfo->stdCdefInfo.cdef_y_pri_strength[1] = 4;
            } else {
                pFrameInfo->stdCdefInfo.cdef_bits = 2;
                pFrameInfo->stdCdefInfo.cdef_y_pri_strength[1] = 2;
                pFrameInfo->stdCdefInfo.cdef_y_pri_strength[2] = 4;
                pFrameInfo->stdCdefInfo.cdef_y_pri_strength[3] = 9;
            }
        }
    }

    if (!AllLossless && !pStdPictureInfo->flags.allow_intrabc && m_encoderConfig->UsesLoopRestoration()) {
        pStdPictureInfo->pLoopRestoration = &pFrameInfo->stdLrInfo;

        if (m_encoderConfig->customLrConfig) {
            memcpy(&pFrameInfo->stdLrInfo, &m_encoderConfig->lrConfig, sizeof(StdVideoAV1LoopRestoration));
        } else {
            memset(&pFrameInfo->stdLrInfo, 0, sizeof(StdVideoAV1LoopRestoration));
            pFrameInfo->stdLrInfo.FrameRestorationType[0] = STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_SGRPROJ;
            pFrameInfo->stdLrInfo.LoopRestorationSize[0] = 1; //log2(RESTORATION_TILESIZE_MAX >> 2) - 5
        }
        for (int32_t i = 0; i < numPlanes; i++) {
            if (pFrameInfo->stdLrInfo.FrameRestorationType[i] != STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE) {
                pStdPictureInfo->flags.UsesLr = 1;
                if (i > 0) {
                    pStdPictureInfo->flags.usesChromaLr = 1;
                }
            }
        }
    }

}

VkResult VkVideoEncoderAV1::RecordVideoCodingCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                                 uint32_t frameIdx, uint32_t ofTotalFrames)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);
    if (pFrameInfo->bShowExistingFrame) {
        if (m_encoderConfig->verboseFrameStruct) {
            DumpStateInfo(" skip  recording", 4, encodeFrameInfo, frameIdx, ofTotalFrames);
        }
        return VK_SUCCESS;
    }
    return VkVideoEncoder::RecordVideoCodingCmd(encodeFrameInfo, frameIdx, ofTotalFrames);
}

VkResult VkVideoEncoderAV1::SubmitVideoCodingCmds(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                                  uint32_t frameIdx, uint32_t ofTotalFrames)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);
    if (pFrameInfo->bShowExistingFrame) {
        if (m_encoderConfig->verboseFrameStruct) {
            DumpStateInfo("skip  submit", 5, encodeFrameInfo, frameIdx, ofTotalFrames);
        }
        return VK_SUCCESS;
    }
    return VkVideoEncoder::SubmitVideoCodingCmds(encodeFrameInfo, frameIdx, ofTotalFrames);
}

void VkVideoEncoderAV1::BuildFrameObuSequence(uint32_t frameIdx,
                                              const VkVideoEncodeFrameInfo* encodeFrameInfo,
                                              BitstreamReadback& bitstreamReadback)
{
    if (m_bitstream.size() <= frameIdx) {
        m_bitstream.resize(frameIdx + 1);
    }

    std::vector<uint8_t>& bitstream = m_bitstream[frameIdx];
    bitstream.clear();

    // Sequence Header OBU
    if (encodeFrameInfo->bitstreamHeaderBufferSize > 0) {
        const uint8_t* seqHdrData = encodeFrameInfo->bitstreamHeaderBuffer
                                  + encodeFrameInfo->bitstreamHeaderOffset;
        bitstream.insert(bitstream.end(), seqHdrData, seqHdrData + encodeFrameInfo->bitstreamHeaderBufferSize);

        if (m_encoderConfig->verboseFrameStruct) {
            LOG_S_INFO << "       == Non-VCL data SUCCESS"
                      << " Non-VCL data with size: " << encodeFrameInfo->bitstreamHeaderBufferSize
                      << ", Input Order: " << (uint32_t)encodeFrameInfo->gopPosition.inputOrder
                      << ", Encode Order: " << (uint32_t)encodeFrameInfo->gopPosition.encodeOrder
                      << std::endl << std::flush;
        }
    }

    // Frame Header OBU + Tile Group OBU
    // Encoder outputs a self-contained OBU.  So, just append it.
    // Consumes bitstreamReadback.bitstreamCopy: when no sequence header needs
    // to be prepended, the frame payload is moved (not copied) into staging.
    {
        const size_t frameSize = bitstreamReadback.bitstreamCopy.size();
        if (bitstream.empty()) {
            bitstream = std::move(bitstreamReadback.bitstreamCopy);
        } else if (frameSize > 0) {
            // Grow first, then copy into the known-sized tail, rather than
            // vector::insert(end(), first, last). GCC's -Wstringop-overflow
            // cannot bound the element count through insert's reallocate-and-
            // copy path and reports a bogus "writing between 2 and SIZE_MAX
            // bytes into a region of size 0" against the memmove inside
            // std::copy. After resize() the destination extent is explicit, so
            // the check has something to verify against. Same result, same
            // number of copies.
            const size_t offset = bitstream.size();
            bitstream.resize(offset + frameSize);
            std::memcpy(bitstream.data() + offset,
                        bitstreamReadback.bitstreamCopy.data(), frameSize);
        }

        if (m_encoderConfig->verboseFrameStruct) {
            LOG_S_INFO << "      == Output VCL data SUCCESS for " << frameIdx << " with size: " << frameSize
                      << ", Input Order: " << (uint32_t)encodeFrameInfo->gopPosition.inputOrder
                      << ", Encode Order: " << (uint32_t)encodeFrameInfo->gopPosition.encodeOrder
                      << std::endl << std::flush;
        }
    }

}

VkResult VkVideoEncoderAV1::FlushBatchedTemporalUnit(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    // Write IVF file header - written once at the start of the output file.
    if (encodeFrameInfo->frameInputOrderNum == 0) {
        uint8_t ivfFileHdr[32];
        mem_put_le32(ivfFileHdr     , MAKE_FOURCC('D', 'K', 'I', 'F'));
        mem_put_le16(ivfFileHdr +  4, 0);
        mem_put_le16(ivfFileHdr +  6, 32);
        mem_put_le32(ivfFileHdr +  8, MAKE_FOURCC('A', 'V', '0', '1'));
        mem_put_le16(ivfFileHdr + 12, m_encoderConfig->PictureWidth());
        mem_put_le16(ivfFileHdr + 14, m_encoderConfig->PictureHeight());
        mem_put_le32(ivfFileHdr + 16, m_encoderConfig->frameRateNumerator);
        mem_put_le32(ivfFileHdr + 20, m_encoderConfig->frameRateDenominator);
        mem_put_le32(ivfFileHdr + 24, m_encoderConfig->numFrames);
        mem_put_le32(ivfFileHdr + 28, 0);
        WriteDataToFile(ivfFileHdr, sizeof(ivfFileHdr));
    }

    // Calculate IVF frame size
    size_t tuSize = sizeof(tdObu);
    for (const auto& curFrameIdx : m_batchFramesIndxSetToAssemble) {
        tuSize += m_bitstream[curFrameIdx].size();

        if (m_encoderConfig->verboseFrameStruct) {
            LOG_S_INFO << ">>>>>> Assembly VCL index " << curFrameIdx << " has size: " << m_bitstream[curFrameIdx].size()
                      << std::endl << std::flush;
        }
    }

    if (m_encoderConfig->verboseFrameStruct) {
        LOG_S_INFO << ">>>>>> Assembly total VCL data is: "
                  << tuSize - sizeof(tdObu)
                  << std::endl << std::flush;
    }

    // Write IVF frame header
    encodeFrameInfo->inputTimeStamp = encodeFrameInfo->frameInputOrderNum;
    const uint64_t pts = encodeFrameInfo->inputTimeStamp;
    uint8_t ivfFrameHdr[12];
    mem_put_le32(ivfFrameHdr    , (uint32_t)tuSize);
    mem_put_le32(ivfFrameHdr + 4, (uint32_t)(pts & 0xffffffff));
    mem_put_le32(ivfFrameHdr + 8, (uint32_t)(pts >> 32));
    WriteDataToFile(ivfFrameHdr, sizeof(ivfFrameHdr));

    // Write Temporal delimiter
    WriteDataToFile(tdObu, sizeof(tdObu));

    // Write frame OBU sequences.  Use WriteDataToFile so the coded payload
    // is included in the CRC calculation (--crc), like the headers above.
    for (const auto& curFrameIdx : m_batchFramesIndxSetToAssemble) {
        const uint8_t* writeData = m_bitstream[curFrameIdx].data();
        size_t remainingBytes = m_bitstream[curFrameIdx].size();

        while (remainingBytes > 0) {
            const size_t bytesWritten = WriteDataToFile(writeData, remainingBytes);
            if (bytesWritten == 0) {
                LOG_S_ERROR << "Failed to write bitstream data for frame " << curFrameIdx << std::endl;
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }

            writeData += bytesWritten;
            remainingBytes -= bytesWritten;
        }

        std::vector<uint8_t>().swap(m_bitstream[curFrameIdx]);
    }
    fflush(m_encoderConfig->outputFileHandler.GetFileHandle());

    m_batchFramesIndxSetToAssemble.clear();

    return VK_SUCCESS;
}

VkResult VkVideoEncoderAV1::AssembleBitstreamData(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                                  uint32_t frameIdx, uint32_t ofTotalFrames)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);

    if (pFrameInfo->bShowExistingFrame) {
        WriteShowExistingFrameHeader(encodeFrameInfo);
        return VK_SUCCESS;
    }

    BitstreamReadback readback{};
    VkResult result = ReadbackBitstreamData(encodeFrameInfo, readback);
    if (result != VK_SUCCESS) {
        LOG_ERROR("AssembleBitstreamData: bitstream readback failed with result 0x%x.\n", result);
        assert(result == VK_SUCCESS);
        return result;
    }

    // Show-existing frames returned above; every remaining frame must have a
    // bitstream buffer, otherwise its coded data would be silently dropped.
    if (!readback.readbackDone) {
        LOG_ERROR("AssembleBitstreamData: no bitstream buffer to read back for frame %u.\n", frameIdx);
        assert(readback.readbackDone);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    BuildFrameObuSequence(frameIdx, encodeFrameInfo.get(), readback);

    m_batchFramesIndxSetToAssemble.insert(frameIdx);

    if (m_psnr && m_psnr->Enabled()) {
        m_psnr->ComputeFramePsnr(encodeFrameInfo.get());
    }

    if (!pFrameInfo->stdPictureInfo.flags.show_frame) {
        return VK_SUCCESS; // more AV1 frames to go in to the current temporal delimiter
    }

    return FlushBatchedTemporalUnit(encodeFrameInfo);

}

VkResult VkVideoEncoderAV1::ReadbackBitstreamData(
    VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
    BitstreamReadback& readback)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);

    if (pFrameInfo->bShowExistingFrame) {
        readback.readbackDone = false;
        return VK_SUCCESS;
    }

    VkResult result = VkVideoEncoder::ReadbackBitstreamData(encodeFrameInfo, readback);
    if (result != VK_SUCCESS) return result;

    if (readback.readbackDone) {
        VkDeviceSize maxSize;
        // The feedback query's bitstreamStartOffset is relative to the bound
        // bitstream buffer range, so honor dstBufferOffset too (currently
        // always 0, but keep the pointer math spec-correct).
        uint8_t* data = encodeFrameInfo->outputBitstreamBuffer->GetDataPtr(0, maxSize)
                      + encodeFrameInfo->encodeInfo.dstBufferOffset
                      + readback.bitstreamStartOffset;
        readback.bitstreamCopy.resize(readback.bitstreamSize);
        memcpy(readback.bitstreamCopy.data(), data, readback.bitstreamSize);
    }

    return VK_SUCCESS;
}

VkResult VkVideoEncoderAV1::WriteBitstreamToFile(
    VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
    uint32_t frameIdx, uint32_t ofTotalFrames,
    BitstreamReadback& readback)
{
    // Every frame that reaches assembly publishes exactly one completion
    // record, in both output modes -- including deferred (non-shown) frames
    // and show-existing calls, which are per-frame calls like any other. In
    // file-output mode a deferred frame's record carries its own assembly
    // turn's result even though its bytes reach the file at the batch
    // flush: the edge means the frame was committed to the output channel,
    // matching the per-frame pairing the pending-frame model requires.
    CapturedBitstream cap;
    cap.frameId = (encodeFrameInfo->externalFrameId != uint64_t(-1))
                      ? encodeFrameInfo->externalFrameId
                      : encodeFrameInfo->frameEncodeInputOrderNum;
    cap.isIdr = (encodeFrameInfo->gopPosition.pictureType ==
                 VkVideoGopStructure::FRAME_TYPE_IDR);
    cap.pictureType = static_cast<uint32_t>(
        encodeFrameInfo->gopPosition.pictureType);

    VkResult result = VK_SUCCESS;
    // Capture (in-memory) path. Capture the AV1 temporal unit into the
    // completion record (the FIFO the retrieval API drains) rather than
    // muxing IVF to a file. Empirically the NVIDIA driver does NOT emit the
    // leading Temporal-Delimiter OBU, so prepend it -- as the standalone
    // IVF writer does -- else the OBU stream cannot be split into temporal
    // units by a decoder.
    if (m_encoderConfig && m_encoderConfig->disableFileOutput) {
        static const uint8_t kAv1TdObu[2] = { 0x12, 0x00 };
        cap.bytes.insert(cap.bytes.end(), kAv1TdObu, kAv1TdObu + 2);
        if (encodeFrameInfo->bitstreamHeaderBufferSize > 0) {
            const uint8_t* hdr = encodeFrameInfo->bitstreamHeaderBuffer +
                                 encodeFrameInfo->bitstreamHeaderOffset;
            cap.bytes.insert(cap.bytes.end(), hdr,
                             hdr + encodeFrameInfo->bitstreamHeaderBufferSize);
        }
        if (readback.readbackDone && readback.bitstreamSize > 0) {
            const uint8_t* src;
            if (!readback.bitstreamCopy.empty()) {
                src = readback.bitstreamCopy.data();
            } else {
                VkDeviceSize maxSize;
                src = encodeFrameInfo->outputBitstreamBuffer->GetDataPtr(0, maxSize) +
                      readback.bitstreamStartOffset;
            }
            cap.bytes.insert(cap.bytes.end(), src, src + readback.bitstreamSize);
        }
    } else {
        result = WriteBitstreamToFileOutput(encodeFrameInfo, frameIdx, readback);
        cap.status = result;  // VK_SUCCESS, or the file-write failure code
    }
    PushCapturedBitstream(std::move(cap));
    return result;
}

// File-output arm of the AV1 override: the show-existing header write, the
// per-frame OBU staging (BuildFrameObuSequence) and deferred (non-shown) frame
// batching, then the flush of the completed temporal unit. The IVF mux itself
// lives in FlushBatchedTemporalUnit(). Private and non-virtual: it must never
// grow a second completion publish.
VkResult VkVideoEncoderAV1::WriteBitstreamToFileOutput(
    VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
    uint32_t frameIdx, BitstreamReadback& readback)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);

    if (pFrameInfo->bShowExistingFrame) {
        WriteShowExistingFrameHeader(encodeFrameInfo);
        return VK_SUCCESS;
    }

    BuildFrameObuSequence(frameIdx, encodeFrameInfo.get(), readback);

    m_batchFramesIndxSetToAssemble.insert(frameIdx);

    if (!pFrameInfo->stdPictureInfo.flags.show_frame) {
        return VK_SUCCESS;
    }

    return FlushBatchedTemporalUnit(encodeFrameInfo);
}

void VkVideoEncoderAV1::WriteShowExistingFrameHeader(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);

    // Prepare frame header obu and write to the bitstream
    std::vector<uint8_t> payload;
    VkVideoEncoderAV1BitWriter payloadWriter(payload);
    payloadWriter.PutBits(1, 1); // show_existing_frame
    payloadWriter.PutBits(pFrameInfo->frameToShowBufId & 7, 3);
    bool decoderModelInfoPresentFlag = m_stateAV1.m_decoder_model_info_present_flag;
    bool equalPictureIntervalFlag = m_stateAV1.m_sequenceHeader.pTimingInfo ?
                                        m_stateAV1.m_sequenceHeader.pTimingInfo->flags.equal_picture_interval : 1;
    if (decoderModelInfoPresentFlag && !equalPictureIntervalFlag) {
        assert(0);
        uint32_t n = m_stateAV1.m_decoderModelInfo.frame_presentation_time_length_minus_1 + 1;
        uint32_t mask = (1 << n) - 1;
        payloadWriter.PutBits((int32_t)(pFrameInfo->inputTimeStamp & mask), n);
    }
    if (m_stateAV1.m_sequenceHeader.flags.frame_id_numbers_present_flag) {
        uint32_t n = m_stateAV1.m_sequenceHeader.delta_frame_id_length_minus_2 + 2 +
                     m_stateAV1.m_sequenceHeader.additional_frame_id_length_minus_1 + 1;
        payloadWriter.PutBits(pFrameInfo->stdPictureInfo.current_frame_id, n);
    }
    payloadWriter.PutTrailingBits();

    std::vector<uint8_t> header;
    VkVideoEncoderAV1BitWriter headerWriter(header);
    headerWriter.PutBits(0, 1); // obu_forbidden_bit
    headerWriter.PutBits(3 /* FRAME_HEADER*/, 4); // obu_type
    headerWriter.PutBits(0, 1); // obu_extension_flag
    headerWriter.PutBits(1, 1); // obu_has_size_field
    headerWriter.PutBits(0, 1); //obu_reserved_1bit
    headerWriter.PutLeb128((uint32_t)payload.size());

    // IVF frame header
    size_t frameSize = 2 + header.size() + payload.size(); /* 2 is temporal delimiter size */
    uint64_t pts = encodeFrameInfo->inputTimeStamp;
    uint8_t frameHeader[12];
    mem_put_le32(frameHeader    , (uint32_t)frameSize); // updated with correct size lateron
    mem_put_le32(frameHeader + 4, (uint32_t)(pts & 0xffffffff));
    mem_put_le32(frameHeader + 8, (uint32_t)(pts >> 32));
    WriteDataToFile(frameHeader, sizeof(frameHeader));

    // Temporal delimiter
    WriteDataToFile(tdObu, sizeof(tdObu));

    // frame header
    WriteDataToFile(header.data(), header.size());
    WriteDataToFile(payload.data(), payload.size());
    fflush(m_encoderConfig->outputFileHandler.GetFileHandle());
}

void VkVideoEncoderAV1::AppendShowExistingFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& current,
                                                VkSharedBaseObj<VkVideoEncodeFrameInfo>& node)
{
    assert(current);

    if (current->dependantFrames == nullptr) {
        current->dependantFrames = node;

        return;
    }

    AppendShowExistingFrame(current->dependantFrames, node);
}

// Insert frames in order from the reference frame first and B frames next in the list.
// Uses a simple ordering for now where B frame as reference are not supported yet.
void VkVideoEncoderAV1::InsertOrdered(VkSharedBaseObj<VkVideoEncodeFrameInfo>& current,
                                      VkSharedBaseObj<VkVideoEncodeFrameInfo>& prev,
                                      VkSharedBaseObj<VkVideoEncodeFrameInfo>& node) {

    bool bShowExistingFrame = false;
    if (current != nullptr) {
        VkVideoEncodeFrameInfoAV1* pCurrentFrameInfo = GetEncodeFrameInfoAV1(current);
        bShowExistingFrame = pCurrentFrameInfo->bShowExistingFrame;
    }

    if ((current == nullptr) || (!bShowExistingFrame && (current->gopPosition.encodeOrder >= node->gopPosition.encodeOrder))) {

        node->dependantFrames = current;

        if (prev != nullptr) {
            // If not inserting at the beginning, link the previous node to the new node
            prev->dependantFrames = node;
        } else {
            // If inserting at the beginning, update the head
            m_lastDeferredFrame = node;
        }

        // For out of order frames, insert display-frameheader in display order
        if (node->dependantFrames != nullptr) {
            VkSharedBaseObj<VkVideoEncodeFrameInfo> showExistingFrameInfo;
            // CHECKED, not asserted. This is the SECOND pool node this insert
            // needs -- the ext layer reserves exactly one per admitted input
            // frame -- so a miss is reachable, and assert() compiles out. In a
            // release build the miss left the handle null and the
            // GetEncodeFrameInfoAV1() below dereferenced it.
            if (!GetAvailablePoolNode(showExistingFrameInfo) ||
                !showExistingFrameInfo) {
                LOG_ERROR("[EncoderAV1] no pool node for the show_existing_frame "
                        "companion; emitting the reordered frame without it\n");
                return;
            }

            VkVideoEncodeFrameInfoAV1* pCurrentFrameInfo = GetEncodeFrameInfoAV1(showExistingFrameInfo);
            pCurrentFrameInfo->bOverlayFrame = true;
            pCurrentFrameInfo->bShowExistingFrame = true;
            pCurrentFrameInfo->gopPosition = node->gopPosition;
            pCurrentFrameInfo->picOrderCntVal = node->picOrderCntVal;
            pCurrentFrameInfo->frameInputOrderNum = node->frameInputOrderNum;
            pCurrentFrameInfo->inputTimeStamp = node->frameInputOrderNum;

            AppendShowExistingFrame(node->dependantFrames, showExistingFrameInfo);
            m_numDeferredFrames++;
        }

        return;
    }

    // Recursive case: Move to the next node, updating previous node pointer
    InsertOrdered(current->dependantFrames, current, node);
}
