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

#include "VkVideoEncoder/VkVideoEncoderH264.h"
#include "VkVideoCore/VulkanVideoCapabilities.h"
#include "Logger.h"

VkResult CreateVideoEncoderH264(const VulkanDeviceContext* vkDevCtx,
                                VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                VkSharedBaseObj<VkVideoEncoder>& encoder)
{
    VkSharedBaseObj<VkVideoEncoderH264> vkEncoderH264(new VkVideoEncoderH264(vkDevCtx));
    if (vkEncoderH264) {

        VkResult result = vkEncoderH264->InitEncoderCodec(encoderConfig);
        if (result != VK_SUCCESS) {
            return result;
        }

        encoder = vkEncoderH264;
        return VK_SUCCESS;
    }

    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult VkVideoEncoderH264::InitEncoderCodec(VkSharedBaseObj<EncoderConfig>& encoderConfig)
{
    auto* h264Config = encoderConfig->GetEncoderConfigh264();
    assert(h264Config);
    m_encoderConfig = std::shared_ptr<EncoderConfigH264>(encoderConfig, h264Config);

    if (m_encoderConfig->codec != VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) {
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    VkResult result = InitEncoder(encoderConfig);
    if (result != VK_SUCCESS) {
        LOG_ERROR("InitEncoder() failed with ret(%d)\n", result);
        return result;
    }

    // The device QP window, recorded where the capabilities are known to
    // be populated -- InitEncoder above is what runs
    // EncoderConfigH264::InitDeviceCapabilities. A mid-stream clamp is
    // checked against this on the caller thread, which cannot safely
    // reach the config.
    m_deviceQpWindowMin = m_encoderConfig->h264EncodeCapabilities.minQp;
    m_deviceQpWindowMax = m_encoderConfig->h264EncodeCapabilities.maxQp;

    m_encoderConfig->GetRateControlParameters(&m_rateControlInfo, m_rateControlLayersInfo, &m_h264.m_rateControlInfoH264, m_h264.m_rateControlLayersInfoH264);

    m_encoderConfig->InitSpsPpsParameters(&m_h264.m_spsInfo, &m_h264.m_ppsInfo,
            m_encoderConfig->InitVuiParameters(&m_h264.m_vuiInfo, &m_h264.m_hrdParameters));

    // The DPB: the session's slots and active references bound it, and its pictures are images of the
    // DPB image pool. Every picture is set up, references or not.
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
    const VvsDpbH264SequenceInfo dpbSequenceInfo{&m_h264.m_spsInfo, &m_h264.m_ppsInfo, nullptr};
    result = m_dpb.Sequence(dpbSequenceInfo, nullptr);
    if (result != VK_SUCCESS) {
        LOG_ERROR("InitEncoderCodec: the DPB refused the SPS and PPS (%d)\n", result);
        return result;
    }


    // create SPS and PPS set
    VideoSessionParametersInfo videoSessionParametersInfo(*m_videoSession,
                                                          &m_h264.m_spsInfo,
                                                          &m_h264.m_ppsInfo,
                                                          m_encoderConfig->qualityLevel,
                                                          m_encoderConfig->enableQpMap,
                                                          m_qpMapTexelSize);

    VkVideoSessionParametersCreateInfoKHR* encodeSessionParametersCreateInfo = videoSessionParametersInfo.getVideoSessionParametersInfo();
    VkVideoSessionParametersKHR sessionParameters;
    result = m_vkDevCtx->CreateVideoSessionParametersKHR(*m_vkDevCtx,
                                                         encodeSessionParametersCreateInfo,
                                                         nullptr,
                                                         &sessionParameters);
    if(result != VK_SUCCESS) {
        LOG_ERROR("EncodeFrame: Failed to get create video session parameters.\n");
        return result;
    }

    result = VulkanVideoSessionParameters::Create(m_vkDevCtx, m_videoSession,
                                                  sessionParameters, m_videoSessionParameters);
    if(result != VK_SUCCESS) {
        LOG_ERROR("EncodeFrame: Failed to get create video session object.\n");
        return result;
    }

    return VK_SUCCESS;
}

VkResult VkVideoEncoderH264::InitRateControl(VkCommandBuffer cmdBuf, uint32_t qp)
{
    VkVideoBeginCodingInfoKHR encodeBeginInfo = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    encodeBeginInfo.videoSession = *m_videoSession;
    encodeBeginInfo.videoSessionParameters = *m_videoSessionParameters;

    VkVideoEncodeH264FrameSizeKHR encodeH264FrameSize;
    encodeH264FrameSize.frameISize = 0;

    VkVideoEncodeH264QpKHR encodeH264Qp;
    encodeH264Qp.qpI = qp;

    VkVideoEncodeH264RateControlLayerInfoKHR encodeH264RateControlLayerInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR};
    encodeH264RateControlLayerInfo.useMinQp = VK_TRUE;
    encodeH264RateControlLayerInfo.minQp = encodeH264Qp;
    encodeH264RateControlLayerInfo.useMaxQp = VK_TRUE;
    encodeH264RateControlLayerInfo.maxQp = encodeH264Qp;
    encodeH264RateControlLayerInfo.useMaxFrameSize = VK_TRUE;
    encodeH264RateControlLayerInfo.maxFrameSize = encodeH264FrameSize;

    VkVideoEncodeRateControlLayerInfoKHR encodeRateControlLayerInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR};
    encodeRateControlLayerInfo.pNext = &encodeH264RateControlLayerInfo;

    VkVideoCodingControlInfoKHR codingControlInfo = {VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
    codingControlInfo.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    codingControlInfo.pNext = &encodeRateControlLayerInfo;

    VkVideoEndCodingInfoKHR encodeEndInfo = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};

    // Reset the video session before first use and apply QP values.
    m_vkDevCtx->CmdBeginVideoCodingKHR(cmdBuf, &encodeBeginInfo);
    m_vkDevCtx->CmdControlVideoCodingKHR(cmdBuf, &codingControlInfo);
    m_vkDevCtx->CmdEndVideoCodingKHR(cmdBuf, &encodeEndInfo);

    return VK_SUCCESS;
}

VkResult VkVideoEncoderH264::ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                        uint32_t frameIdx, uint32_t ofTotalFrames)
{
    VkVideoEncodeFrameInfoH264* pFrameInfo = GetEncodeFrameInfoH264(encodeFrameInfo);

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("process DPB", 3, encodeFrameInfo, frameIdx, ofTotalFrames);
    }

    const VkVideoGopStructure::FrameType picType = encodeFrameInfo->gopPosition.pictureType;
    const bool isIntraRefreshFrame = m_encoderConfig->gopStructure.IsIntraRefreshFrame(encodeFrameInfo->gopPosition);
    const bool intraRefresh = m_encoderConfig->enableIntraRefresh && isIntraRefreshFrame;

    // The DPB library decides the references and writes frame_num, the PicOrderCnt the decoding
    // process derives from this one, the reference lists and the slots.
    pFrameInfo->stdPictureInfo.PicOrderCnt = encodeFrameInfo->picOrderCntVal;
    VvsDpbH264EncodePictureInfo dpbPictureInfo{};
    // The number of dirty intra-refresh regions in the current picture
    dpbPictureInfo.dirtyIntraRefreshRegions =
        intraRefresh ? (m_encoderConfig->intraRefreshCycleDuration - encodeFrameInfo->gopPosition.intraRefreshIndex - 1)
                     : VVS_DPB_NO_INTRA_REFRESH;
    dpbPictureInfo.pStdPictureInfo = &pFrameInfo->stdPictureInfo;
    dpbPictureInfo.pSyntax = &pFrameInfo->dpbSyntax;
    m_dpbPictures.BeginPicture(pFrameInfo);
    const VkResult result = m_dpb.Picture(dpbPictureInfo, pFrameInfo->dpbResult);
    // The frame holds the images its commands read and write until it completes.
    m_dpbPictures.EndPicture(pFrameInfo, (result == VK_SUCCESS) ? &pFrameInfo->dpbResult.slots : nullptr);
    if (result != VK_SUCCESS) {
        LOG_ERROR("ProcessDpb: the DPB refused the picture (%d)\n", result);
        return result;
    }
    const VvsDpbPictureSlots& slots = pFrameInfo->dpbResult.slots;

    const StdVideoEncodeH264ReferenceListsInfo& refLists = pFrameInfo->dpbSyntax.refLists;
    pFrameInfo->stdSliceHeader[0].flags.num_ref_idx_active_override_flag = false;
    if (picType == VkVideoGopStructure::FRAME_TYPE_B) {
        pFrameInfo->stdSliceHeader[0].flags.num_ref_idx_active_override_flag =
            ((refLists.num_ref_idx_l0_active_minus1 != m_h264.m_ppsInfo.num_ref_idx_l0_default_active_minus1) ||
             (refLists.num_ref_idx_l1_active_minus1 != m_h264.m_ppsInfo.num_ref_idx_l1_default_active_minus1));
    } else if (picType == VkVideoGopStructure::FRAME_TYPE_P) {
        pFrameInfo->stdSliceHeader[0].flags.num_ref_idx_active_override_flag =
            (refLists.num_ref_idx_l0_active_minus1 != m_h264.m_ppsInfo.num_ref_idx_l0_default_active_minus1);
    }

    // Populate all Std slice header entries. For now, they are all identical.
    for (uint32_t i = 1; i < pFrameInfo->pictureInfo.naluSliceEntryCount; i++) {
        pFrameInfo->stdSliceHeader[i] = pFrameInfo->stdSliceHeader[0];
    }

    if (intraRefresh && (m_encoderConfig->intraRefreshMode == EncoderConfig::REFRESH_PER_PARTITION)) {
        // When using per-picture partition intra-refresh, mark the slice corresponding
        // to the currently refreshed intra-refresh region as an intra slice.
        for (uint32_t i = 0; i < pFrameInfo->pictureInfo.naluSliceEntryCount; i++) {
            if (i != encodeFrameInfo->gopPosition.intraRefreshIndex)
                continue;

            pFrameInfo->stdSliceHeader[i].slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
        }
    }

    vvs::dpb::SetEncodeSlots(slots, pFrameInfo->encodeInfo);
    pFrameInfo->pBeginCodingReferenceSlots = slots.referenceSlots;
    pFrameInfo->beginCodingReferenceSlotCount = slots.beginCodingSlotCount;
    pFrameInfo->encodeInfo.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;

    return VK_SUCCESS;
}

VkResult VkVideoEncoderH264::EncodeVideoSessionParameters(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoH264* pFrameInfo = GetEncodeFrameInfoH264(encodeFrameInfo);

    assert(pFrameInfo->stdPictureInfo.seq_parameter_set_id >= 0);
    assert(pFrameInfo->stdPictureInfo.pic_parameter_set_id >= 0);
    assert(pFrameInfo->videoSession);
    assert(pFrameInfo->videoSessionParameters);

    VkVideoEncodeH264SessionParametersGetInfoKHR h264GetInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR,
        nullptr,
        VK_TRUE,
        VK_TRUE,
        pFrameInfo->stdPictureInfo.seq_parameter_set_id,
        pFrameInfo->stdPictureInfo.pic_parameter_set_id,
    };

    VkVideoEncodeSessionParametersGetInfoKHR getInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
        &h264GetInfo,
        *pFrameInfo->videoSessionParameters,
    };

    VkVideoEncodeH264SessionParametersFeedbackInfoKHR h264FeedbackInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,
        nullptr,
    };

    VkVideoEncodeSessionParametersFeedbackInfoKHR feedbackInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,
        &h264FeedbackInfo,
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

    return result;
}

VkResult VkVideoEncoderH264::CreateFrameInfoBuffersQueue(uint32_t numPoolNodes)
{
    VkSharedBaseObj<VulkanBufferPool<VkVideoEncodeFrameInfoH264>> _cmdBuffPool(new VulkanBufferPool<VkVideoEncodeFrameInfoH264>());

    if (_cmdBuffPool) {
        _cmdBuffPool->Init(numPoolNodes);
        m_frameInfoBuffersQueue = _cmdBuffPool;
        return VK_SUCCESS;
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult VkVideoEncoderH264::EncodeFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoH264* pFrameInfo = GetEncodeFrameInfoH264(encodeFrameInfo);

    encodeFrameInfo->picOrderCntVal = 2 * encodeFrameInfo->gopPosition.inputOrder;

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("input", 1, encodeFrameInfo);

        if (encodeFrameInfo->lastFrame) {
            LOG_S_INFO << "#### It is the last frame: " << encodeFrameInfo->frameInputOrderNum
                      << " of type " << VkVideoGopStructure::GetFrameTypeName(encodeFrameInfo->gopPosition.pictureType)
                      << " ###"
                      << std::endl << std::flush;
        }
    }

    pFrameInfo->encodeInfo.flags = 0;
    assert(pFrameInfo->encodeInfo.srcPictureResource.codedOffset.x == 0);
    assert(pFrameInfo->encodeInfo.srcPictureResource.codedOffset.y == 0);
    pFrameInfo->encodeInfo.srcPictureResource.codedExtent.width = m_encoderConfig->encodeWidth;
    pFrameInfo->encodeInfo.srcPictureResource.codedExtent.height = m_encoderConfig->encodeHeight;
    VkVideoPictureResourceInfoKHR* pSrcPictureResource = encodeFrameInfo->srcEncodeImageResource->GetPictureResourceInfo();
    encodeFrameInfo->encodeInfo.srcPictureResource.imageViewBinding = pSrcPictureResource->imageViewBinding;
    encodeFrameInfo->encodeInfo.srcPictureResource.baseArrayLayer   = pSrcPictureResource->baseArrayLayer;

    pFrameInfo->pictureInfo.naluSliceEntryCount = m_encoderConfig->sliceCount;

    pFrameInfo->stdPictureInfo.seq_parameter_set_id = m_h264.m_spsInfo.seq_parameter_set_id;
    pFrameInfo->stdPictureInfo.pic_parameter_set_id = m_h264.m_ppsInfo.pic_parameter_set_id;

    memset(pFrameInfo->stdSliceHeader, 0, sizeof(pFrameInfo->stdSliceHeader));

    StdVideoH264PictureType stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_INVALID;
    switch (encodeFrameInfo->gopPosition.pictureType) {
        case VkVideoGopStructure::FRAME_TYPE_IDR:
        case VkVideoGopStructure::FRAME_TYPE_INTRA_REFRESH:
            pFrameInfo->stdSliceHeader[0].slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_IDR;
            break;
        case VkVideoGopStructure::FRAME_TYPE_I:
            pFrameInfo->stdSliceHeader[0].slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_I;
            break;
        case VkVideoGopStructure::FRAME_TYPE_P:
            pFrameInfo->stdSliceHeader[0].slice_type = STD_VIDEO_H264_SLICE_TYPE_P;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_P;
            break;
        case VkVideoGopStructure::FRAME_TYPE_B:
            pFrameInfo->stdSliceHeader[0].slice_type = STD_VIDEO_H264_SLICE_TYPE_B;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_B;
            break;
        default:
            assert(!"Invalid value");
            break;
    }

    pFrameInfo->stdPictureInfo.flags.IdrPicFlag = (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR);
    pFrameInfo->stdPictureInfo.flags.is_reference = m_encoderConfig->gopStructure.IsFrameReference(encodeFrameInfo->gopPosition);
    pFrameInfo->stdPictureInfo.flags.long_term_reference_flag = pFrameInfo->islongTermReference;
    pFrameInfo->stdPictureInfo.primary_pic_type = stdPictureType;
    pFrameInfo->stdPictureInfo.flags.no_output_of_prior_pics_flag = false;        // TODO: replace this by a check for the corresponding slh flag
    pFrameInfo->stdPictureInfo.flags.adaptive_ref_pic_marking_mode_flag = false;  // TODO: replace this by a check for the corresponding slh flag

    pFrameInfo->stdSliceHeader[0].disable_deblocking_filter_idc = m_encoderConfig->disable_deblocking_filter_idc;
     // FIXME: set cabac_init_idc based on a query
     pFrameInfo->stdSliceHeader[0].cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0;

    if (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) {
        pFrameInfo->stdPictureInfo.idr_pic_id = m_IDRPicId & 1;
        m_IDRPicId++;
    }

    // In capture mode (disableFileOutput: the bitstream is delivered in
    // memory) EVERY IDR chunk must be independently decodable: a consumer
    // of keyframe chunks (a muxer, a validator) expects in-band SPS/PPS on
    // each keyframe, including mid-stream forced IDRs. The file-based sample
    // keeps the original headers-once-at-stream-start behavior.
    if ((encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) &&
            ((encodeFrameInfo->frameEncodeInputOrderNum == 0) ||
             (m_encoderConfig->disableFileOutput != 0))) {
        VkResult result = EncodeVideoSessionParameters(encodeFrameInfo);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    // XXX: We don't really test encoder state reset at the moment.
    // For simplicity, only indicate that the state is to be reset for the
    // first IDR picture.
    // FIXME: The reset must use a RESET control command.
    //if (encodeFrameInfo->frameEncodeOrderNum == 0) {
    //    pFrameInfo->encodeInfo.flags |= VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    //}

    if (m_rateControlInfo.rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
        int32_t constantQp = 0;

        switch (encodeFrameInfo->gopPosition.pictureType) {
            case VkVideoGopStructure::FRAME_TYPE_IDR:
            case VkVideoGopStructure::FRAME_TYPE_I:
                constantQp = encodeFrameInfo->constQp.qpIntra;
                break;
            case VkVideoGopStructure::FRAME_TYPE_P:
                constantQp = encodeFrameInfo->constQp.qpInterP;
                break;
            case VkVideoGopStructure::FRAME_TYPE_B:
                constantQp = encodeFrameInfo->constQp.qpInterB;
                break;
            default:
                assert(!"Invalid picture type");
                break;
        }

        for (uint32_t i = 0; i < pFrameInfo->pictureInfo.naluSliceEntryCount; i++) {
            pFrameInfo->naluSliceInfo[i].constantQp = constantQp;
        }
    }

    return VK_SUCCESS;
}

void VkVideoEncoderH264::RefreshCodecRateControlParameters()
{
    // READ THROUGH THE BASE CONFIG POINTER, not the H.264 member. This
    // runs from VkVideoEncoder::ApplyPendingRateControlUpdate, which just
    // wrote the new clamp on VkVideoEncoder::m_encoderConfig; reading the
    // same pointer is what guarantees the fill sees that write. In a
    // device-initialized session the two name one object anyway --
    // InitEncoderCodec builds the H.264 member as an aliasing handle on
    // the very config InitEncoder stores -- so this is the same object by
    // a route that also holds for a session that never ran
    // InitEncoderCodec.
    if (!VkVideoEncoder::m_encoderConfig) {
        return;
    }
    EncoderConfigH264* config =
        VkVideoEncoder::m_encoderConfig->GetEncoderConfigh264();
    if (config == nullptr) {
        return;
    }
    // RESET THE LAYER STRUCT TO ITS CODEC-INIT STATE FIRST, because the
    // fill only ever RAISES useMinQp/useMaxQp -- it has no else branch that
    // lowers them. At codec-init that is harmless: the struct arrives
    // zero-initialised but for its sType, so an unset clamp leaves the flag
    // down. Re-invoked in place it is not: a clamp that was set once and is
    // then cleared would keep its flag raised and go on clamping at a value
    // the caller withdrew, which is the accepted-and-ignored shape in
    // reverse and worse. The brace-init below is the state the constructor
    // gives this member, and the fill plus CodecHandleRateControlCmd are
    // its only other writers, so nothing else is lost by rebuilding it.
    for (uint32_t layerIndx = 0;
         layerIndx < ARRAYSIZE(m_h264.m_rateControlLayersInfoH264);
         layerIndx++) {
        m_h264.m_rateControlLayersInfoH264[layerIndx] =
            VkVideoEncodeH264RateControlLayerInfoKHR{
                VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR};
    }
    config->GetRateControlParameters(&m_rateControlInfo,
                                     m_rateControlLayersInfo,
                                     &m_h264.m_rateControlInfoH264,
                                     m_h264.m_rateControlLayersInfoH264);
}

void VkVideoEncoderH264::GetResolvedQpClampForTest(uint32_t* pUseMinQp,
                                                   int32_t*  pMinQpI,
                                                   uint32_t* pUseMaxQp,
                                                   int32_t*  pMaxQpI) const
{
    const VkVideoEncodeH264RateControlLayerInfoKHR& layer =
        m_h264.m_rateControlLayersInfoH264[0];
    *pUseMinQp = (layer.useMinQp == VK_TRUE) ? 1u : 0u;
    *pMinQpI   = layer.minQp.qpI;
    *pUseMaxQp = (layer.useMaxQp == VK_TRUE) ? 1u : 0u;
    *pMaxQpI   = layer.maxQp.qpI;
}

VkResult VkVideoEncoderH264::CodecHandleRateControlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoH264* pFrameInfo = GetEncodeFrameInfoH264(encodeFrameInfo);

    for (uint32_t layerIndx = 0; layerIndx < ARRAYSIZE(m_h264.m_rateControlLayersInfoH264); layerIndx++) {
        pFrameInfo->rateControlLayersInfoH264[layerIndx] = m_h264.m_rateControlLayersInfoH264[layerIndx];
        pFrameInfo->rateControlLayersInfoH264[layerIndx].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR;
        pFrameInfo->rateControlLayersInfo[layerIndx].pNext = &pFrameInfo->rateControlLayersInfoH264[layerIndx];
    }

    pFrameInfo->rateControlInfoH264 = m_h264.m_rateControlInfoH264;
    pFrameInfo->rateControlInfoH264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR;
    pFrameInfo->rateControlInfoH264.temporalLayerCount = m_encoderConfig->gopStructure.GetTemporalLayerCount();

    if (pFrameInfo->pControlCmdChain != nullptr) {
        pFrameInfo->rateControlInfoH264.pNext = pFrameInfo->pControlCmdChain;
    }

    pFrameInfo->pControlCmdChain = (VkBaseInStructure*)&pFrameInfo->rateControlInfoH264;

    return VK_SUCCESS;
}
