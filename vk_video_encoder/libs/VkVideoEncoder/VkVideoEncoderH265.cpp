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

#include "VkVideoEncoder/VkVideoEncoderH265.h"
#include "VkVideoCore/VulkanVideoCapabilities.h"
#include "Logger.h"

VkResult CreateVideoEncoderH265(const VulkanDeviceContext* vkDevCtx,
                                VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                VkSharedBaseObj<VkVideoEncoder>& encoder)
{
    VkSharedBaseObj<VkVideoEncoderH265> vkEncoderH265(new VkVideoEncoderH265(vkDevCtx));
    if (vkEncoderH265) {

        VkResult result = vkEncoderH265->InitEncoderCodec(encoderConfig);
        if (result != VK_SUCCESS) {
            return result;
        }

        encoder = vkEncoderH265;
        return VK_SUCCESS;
    }

    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult VkVideoEncoderH265::InitEncoderCodec(VkSharedBaseObj<EncoderConfig>& encoderConfig)
{
    auto* h265Config = encoderConfig->GetEncoderConfigh265();
    assert(h265Config);
    m_encoderConfig = std::shared_ptr<EncoderConfigH265>(encoderConfig, h265Config);

    if (m_encoderConfig->codec != VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR) {
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    VkResult result = InitEncoder(encoderConfig);
    if (result != VK_SUCCESS) {
        LOG_ERROR("InitEncoder() failed with ret(%d)\n", result);
        return result;
    }

    if (m_encoderConfig->verbose) {
        LOG_S_INFO << ", numRefL0: "    << (uint32_t)m_encoderConfig->numRefL0
                  << ", numRefL1: "    << (uint32_t)m_encoderConfig->numRefL1 << std::endl;
    }

    // The device QP window; see the H.264 counterpart. InitEncoder above
    // is what runs EncoderConfigH265::InitDeviceCapabilities, so the
    // capabilities are populated by here.
    m_deviceQpWindowMin = m_encoderConfig->h265EncodeCapabilities.minQp;
    m_deviceQpWindowMax = m_encoderConfig->h265EncodeCapabilities.maxQp;

    m_encoderConfig->GetRateControlParameters(&m_rateControlInfo, m_rateControlLayersInfo, &m_rateControlInfoH265, m_rateControlLayersInfoH265);

    m_encoderConfig->InitParamameters(&m_vps, &m_sps, &m_pps,
            m_encoderConfig->InitVuiParameters(&m_sps.vuiInfo,
                                                   &m_sps.hrdParameters,
                                                   &m_sps.subLayerHrdParametersNal));

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
    const VvsDpbH265SequenceInfo dpbSequenceInfo{&m_sps.sps, &m_pps, nullptr};
    result = m_dpb.Sequence(dpbSequenceInfo, nullptr);
    if (result != VK_SUCCESS) {
        LOG_ERROR("InitEncoderCodec: the DPB refused the SPS and PPS (%d)\n", result);
        return result;
    }


    VkVideoEncodeH265SessionParametersAddInfoKHR encodeH265SessionParametersAddInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR};

    encodeH265SessionParametersAddInfo.stdVPSCount = 1;
    encodeH265SessionParametersAddInfo.pStdVPSs = &m_vps.vpsInfo;
    encodeH265SessionParametersAddInfo.stdSPSCount = 1;
    encodeH265SessionParametersAddInfo.pStdSPSs = &m_sps.sps;
    encodeH265SessionParametersAddInfo.stdPPSCount = 1;
    encodeH265SessionParametersAddInfo.pStdPPSs = &m_pps;

    VkVideoEncodeQualityLevelInfoKHR qualityLevelInfo;
    qualityLevelInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
    qualityLevelInfo.pNext = nullptr;
    qualityLevelInfo.qualityLevel = encoderConfig->qualityLevel;

    VkVideoEncodeH265SessionParametersCreateInfoKHR encodeH265SessionParametersCreateInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR,
        &qualityLevelInfo, 1 /* maxStdVPSCount */, 1 /* maxSpsStdCount */, 1 /* maxPpsStdCount */,
        &encodeH265SessionParametersAddInfo
    };

    VkVideoSessionParametersCreateInfoKHR encodeSessionParametersCreateInfo = {
        VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR, &encodeH265SessionParametersCreateInfo};
    encodeSessionParametersCreateInfo.videoSession = *m_videoSession;
    encodeSessionParametersCreateInfo.flags = 0;

    // Encodes with a quantization map need parameters created map-compatible, for the map
    // texel size the session uses (VUID-vkCmdEncodeVideoKHR-pNext-10315).
    VkVideoEncodeQuantizationMapSessionParametersCreateInfoKHR quantizationMapSessionParametersCreateInfo {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUANTIZATION_MAP_SESSION_PARAMETERS_CREATE_INFO_KHR, nullptr, m_qpMapTexelSize };
    if (encoderConfig->enableQpMap) {
        qualityLevelInfo.pNext = &quantizationMapSessionParametersCreateInfo;
        encodeSessionParametersCreateInfo.flags = VK_VIDEO_SESSION_PARAMETERS_CREATE_QUANTIZATION_MAP_COMPATIBLE_BIT_KHR;
    }

    VkVideoSessionParametersKHR sessionParameters;
    result = m_vkDevCtx->CreateVideoSessionParametersKHR(*m_vkDevCtx,
                                                         &encodeSessionParametersCreateInfo,
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

VkResult VkVideoEncoderH265::InitRateControl(VkCommandBuffer cmdBuf, uint32_t qp)
{
    return VK_NOT_READY;
}

VkResult VkVideoEncoderH265::ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                        uint32_t frameIdx, uint32_t ofTotalFrames)
{
    VkVideoEncodeFrameInfoH265* pFrameInfo = GetEncodeFrameInfoH265(encodeFrameInfo);

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("process DPB", 3, encodeFrameInfo, frameIdx, ofTotalFrames);
    }

    const bool isIntraRefreshFrame = m_encoderConfig->gopStructure.IsIntraRefreshFrame(encodeFrameInfo->gopPosition);
    const bool intraRefresh = m_encoderConfig->enableIntraRefresh && isIntraRefreshFrame;

    // The DPB library decides the references and writes the RPS, the reference lists and the slots.
    VvsDpbH265EncodePictureInfo dpbPictureInfo{};
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

    const StdVideoEncodeH265ReferenceListsInfo* pRefLists = pFrameInfo->stdPictureInfo.pRefLists;
    if ((pRefLists != nullptr) &&
        ((m_pps.num_ref_idx_l0_default_active_minus1 != pRefLists->num_ref_idx_l0_active_minus1) ||
         (m_pps.num_ref_idx_l1_default_active_minus1 != pRefLists->num_ref_idx_l1_active_minus1))) {
        pFrameInfo->stdSliceSegmentHeader[0].flags.num_ref_idx_active_override_flag = 1;
    }

    // Populate all Std slice segment header entries. For now, they are all identical.
    for (uint32_t i = 1; i < pFrameInfo->pictureInfo.naluSliceSegmentEntryCount; i++) {
        pFrameInfo->stdSliceSegmentHeader[i] = pFrameInfo->stdSliceSegmentHeader[0];
    }

    if (intraRefresh && (m_encoderConfig->intraRefreshMode == EncoderConfig::REFRESH_PER_PARTITION)) {
        // When using per-picture partition intra-refresh, mark the slice corresponding
        // to the currently refreshed intra-refresh region as an intra slice.
        for (uint32_t i = 0; i < pFrameInfo->pictureInfo.naluSliceSegmentEntryCount; i++) {
            if (i != encodeFrameInfo->gopPosition.intraRefreshIndex)
                continue;

            pFrameInfo->stdSliceSegmentHeader[i].slice_type = STD_VIDEO_H265_SLICE_TYPE_I;
        }
    }

    vvs::dpb::SetEncodeSlots(slots, pFrameInfo->encodeInfo);
    pFrameInfo->pBeginCodingReferenceSlots = slots.referenceSlots;
    pFrameInfo->beginCodingReferenceSlotCount = slots.beginCodingSlotCount;
    pFrameInfo->encodeInfo.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;

    return VK_SUCCESS;
}

VkResult VkVideoEncoderH265::EncodeVideoSessionParameters(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoH265* pFrameInfo = GetEncodeFrameInfoH265(encodeFrameInfo);

    assert(pFrameInfo->stdPictureInfo.sps_video_parameter_set_id >= 0);
    assert(pFrameInfo->stdPictureInfo.pps_seq_parameter_set_id >= 0);
    assert(pFrameInfo->stdPictureInfo.pps_pic_parameter_set_id >= 0);
    assert(pFrameInfo->videoSessionParameters);

    VkVideoEncodeH265SessionParametersGetInfoKHR sessionParametersGetInfoH265 = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_KHR,
        nullptr,
        VK_TRUE, /* writeStdVPS */
        VK_TRUE, /* writeStdSPS */
        VK_TRUE, /* writeStdPPS */
        pFrameInfo->stdPictureInfo.sps_video_parameter_set_id,
        pFrameInfo->stdPictureInfo.pps_seq_parameter_set_id,
        pFrameInfo->stdPictureInfo.pps_pic_parameter_set_id
    };

    VkVideoEncodeSessionParametersGetInfoKHR sessionParametersGetInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
        &sessionParametersGetInfoH265,
        *pFrameInfo->videoSessionParameters,
    };

    VkVideoEncodeH265SessionParametersFeedbackInfoKHR sessionParametersFeedbackInfoH265 = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,
        nullptr,
    };

    VkVideoEncodeSessionParametersFeedbackInfoKHR sessionParametersFeedbackInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,
        &sessionParametersFeedbackInfoH265,
    };

    size_t bufferSize = sizeof(encodeFrameInfo->bitstreamHeaderBuffer);
    VkResult result = m_vkDevCtx->GetEncodedVideoSessionParametersKHR(*m_vkDevCtx,
                                                                      &sessionParametersGetInfo,
                                                                      &sessionParametersFeedbackInfo,
                                                                      &bufferSize,
                                                                      encodeFrameInfo->bitstreamHeaderBuffer);
    if (result != VK_SUCCESS) {
        return result;
    }
    encodeFrameInfo->bitstreamHeaderBufferSize = bufferSize;

    // HDR10 STATIC METADATA, appended to the parameter sets the driver just
    // wrote.
    //
    // HERE, and not on the per-frame path, for two reasons. The access-unit
    // order a decoder requires is VPS, SPS, PPS, prefix SEI, slice -- so the
    // bytes belong immediately after what this function produced. And this
    // function runs for EVERY IDR (see the note in
    // VkVideoEncoder::EncodeFrame about bitstreamHeaderBufferSize), so the
    // colour volume repeats at every random-access point instead of once at
    // the head of the stream where a seek or a mid-stream join would miss it.
    //
    // Vulkan Video has no std structure for either payload and
    // GetEncodedVideoSessionParametersKHR writes parameter sets only, so the
    // NAL is built by hand -- see VkVideoEncoderHdrMetadata.cpp.
    if (m_encoderConfig->hdrMetadata.Any()) {
        bool truncated = false;
        const size_t used = encodeFrameInfo->bitstreamHeaderOffset +
                            encodeFrameInfo->bitstreamHeaderBufferSize;
        const size_t seiBytes = VkEncBuildH265HdrSeiNal(
            m_encoderConfig->hdrMetadata,
            encodeFrameInfo->bitstreamHeaderBuffer + used,
            sizeof(encodeFrameInfo->bitstreamHeaderBuffer) - used,
            &truncated);
        if (truncated) {
            // FATAL. A caller that asked for HDR10 and got a stream without
            // it has no way to notice: every counter, every completion edge
            // and every byte count is identical. Refusing is the only signal
            // this failure has.
            LOG_ERROR("EncodeVideoSessionParameters: the HDR10 SEI does "
                    "not fit in the %zu-byte non-VCL header buffer after %zu "
                    "bytes of parameter sets.\n",
                    sizeof(encodeFrameInfo->bitstreamHeaderBuffer), used);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        // AND THE RATE CONTROLLER IS TOLD. bitstreamHeaderBufferSize is
        // what VkVideoEncoder::EncodeFrameCommon reserves via
        // dstBufferOffset and reports as precedingExternallyEncodedBytes,
        // and it runs AFTER this function (the codec arm fills the buffer
        // first). So growing the count here debits the SEI's bytes from the
        // IDR's frame budget exactly as the parameter sets' bytes already
        // were -- rather than leaving the RC to overshoot by another ~47
        // bytes on every IDR.
        encodeFrameInfo->bitstreamHeaderBufferSize += seiBytes;
    }

    return result;
}

VkResult VkVideoEncoderH265::CreateFrameInfoBuffersQueue(uint32_t numPoolNodes)
{
    VkSharedBaseObj<VulkanBufferPool<VkVideoEncodeFrameInfoH265>> _cmdBuffPool(new VulkanBufferPool<VkVideoEncodeFrameInfoH265>());

    if (_cmdBuffPool) {
        _cmdBuffPool->Init(numPoolNodes);
        m_frameInfoBuffersQueue = _cmdBuffPool;
        return VK_SUCCESS;
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult VkVideoEncoderH265::EncodeFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)

{
    VkVideoEncodeFrameInfoH265* pFrameInfo = GetEncodeFrameInfoH265(encodeFrameInfo);

    assert(pFrameInfo->videoSession);
    assert(pFrameInfo->videoSessionParameters);

    encodeFrameInfo->picOrderCntVal = encodeFrameInfo->gopPosition.inputOrder;

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
    encodeFrameInfo->encodeInfo.srcPictureResource.baseArrayLayer = pSrcPictureResource->baseArrayLayer;

    pFrameInfo->pictureInfo.naluSliceSegmentEntryCount = m_encoderConfig->sliceCount;

    pFrameInfo->stdPictureInfo.sps_video_parameter_set_id = m_vps.vpsInfo.vps_video_parameter_set_id;
    pFrameInfo->stdPictureInfo.pps_seq_parameter_set_id   = m_sps.sps.sps_seq_parameter_set_id;
    pFrameInfo->stdPictureInfo.pps_pic_parameter_set_id   = m_pps.pps_pic_parameter_set_id;

    VkResult result = VK_SUCCESS;

    // In capture mode (disableFileOutput: the bitstream is delivered in
    // memory) EVERY IDR chunk must be independently decodable: a consumer
    // of keyframe chunks expects in-band VPS/SPS/PPS on each keyframe,
    // including mid-stream forced IDRs. The file-based sample keeps the
    // original headers-once-at-stream-start behavior.
    if ((encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) &&
            ((encodeFrameInfo->frameEncodeInputOrderNum == 0) ||
             (m_encoderConfig->disableFileOutput != 0)
             /*|| pEncodeConfigH265->repeatSPSPPS || m_bReconfigForcedIDR*/)) {

        result = EncodeVideoSessionParameters(encodeFrameInfo);
        if (result != VK_SUCCESS ) {
            assert(result == VK_SUCCESS);
            return result;
        }
    }

    StdVideoH265PictureType stdPictureType = STD_VIDEO_H265_PICTURE_TYPE_INVALID;
    StdVideoH265SliceType sliceType = STD_VIDEO_H265_SLICE_TYPE_I;
    switch (encodeFrameInfo->gopPosition.pictureType) {
        case VkVideoGopStructure::FRAME_TYPE_P:
            stdPictureType = STD_VIDEO_H265_PICTURE_TYPE_P;
            sliceType = STD_VIDEO_H265_SLICE_TYPE_P;
            break;
        case VkVideoGopStructure::FRAME_TYPE_B:
            stdPictureType = STD_VIDEO_H265_PICTURE_TYPE_B;
            sliceType = STD_VIDEO_H265_SLICE_TYPE_B;
            break;
        case VkVideoGopStructure::FRAME_TYPE_I:
            stdPictureType = STD_VIDEO_H265_PICTURE_TYPE_I;
            sliceType = STD_VIDEO_H265_SLICE_TYPE_I;
            break;
        case VkVideoGopStructure::FRAME_TYPE_IDR:
            stdPictureType = STD_VIDEO_H265_PICTURE_TYPE_IDR;
            sliceType = STD_VIDEO_H265_SLICE_TYPE_I;
            break;
        case VkVideoGopStructure::FRAME_TYPE_INTRA_REFRESH:
            stdPictureType = STD_VIDEO_H265_PICTURE_TYPE_IDR;
            sliceType = STD_VIDEO_H265_SLICE_TYPE_I;
            break;
        default:
            assert(!"Invalid picture type");
            return VK_ERROR_INITIALIZATION_FAILED;
    }

    memset(pFrameInfo->stdSliceSegmentHeader, 0, sizeof(pFrameInfo->stdSliceSegmentHeader));

    pFrameInfo->stdSliceSegmentHeader[0].slice_type = sliceType;
    pFrameInfo->stdSliceSegmentHeader[0].MaxNumMergeCand = 5;
    pFrameInfo->stdSliceSegmentHeader[0].flags.first_slice_segment_in_pic_flag = 1;
    pFrameInfo->stdSliceSegmentHeader[0].flags.dependent_slice_segment_flag = 0;
    pFrameInfo->stdSliceSegmentHeader[0].flags.slice_sao_luma_flag = 1;
    pFrameInfo->stdSliceSegmentHeader[0].flags.slice_sao_chroma_flag = 1;
    pFrameInfo->stdSliceSegmentHeader[0].flags.num_ref_idx_active_override_flag = 0;
    pFrameInfo->stdSliceSegmentHeader[0].flags.mvd_l1_zero_flag = 0;
    pFrameInfo->stdSliceSegmentHeader[0].flags.cabac_init_flag = 0;
    pFrameInfo->stdSliceSegmentHeader[0].flags.cu_chroma_qp_offset_enabled_flag = 1;
    pFrameInfo->stdSliceSegmentHeader[0].flags.deblocking_filter_override_flag = 1;
    pFrameInfo->stdSliceSegmentHeader[0].flags.slice_deblocking_filter_disabled_flag = 0;
    pFrameInfo->stdSliceSegmentHeader[0].flags.collocated_from_l0_flag = 0;
    pFrameInfo->stdSliceSegmentHeader[0].flags.slice_loop_filter_across_slices_enabled_flag = 1;

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

        for (uint32_t i = 0; i < pFrameInfo->pictureInfo.naluSliceSegmentEntryCount; i++) {
            pFrameInfo->naluSliceSegmentInfo[i].constantQp = constantQp;
        }
        if (getenv("VKENC_DEBUG_PSNR")) {
            LOG_INFO("[QPDBG] picType=%d constantQp=%d (qpI=%d qpP=%d qpB=%d) rcMode=%d\n",
                    (int)encodeFrameInfo->gopPosition.pictureType, constantQp,
                    encodeFrameInfo->constQp.qpIntra, encodeFrameInfo->constQp.qpInterP,
                    encodeFrameInfo->constQp.qpInterB, (int)m_rateControlInfo.rateControlMode);
        }
    } else if (getenv("VKENC_DEBUG_PSNR")) {
        LOG_INFO("[QPDBG] rcMode=%d NOT DISABLED (picType=%d) -> QP not forced\n",
                (int)m_rateControlInfo.rateControlMode, (int)encodeFrameInfo->gopPosition.pictureType);
    }

    pFrameInfo->stdPictureInfo.flags.is_reference = m_encoderConfig->gopStructure.IsFrameReference(encodeFrameInfo->gopPosition);
    pFrameInfo->stdPictureInfo.flags.short_term_ref_pic_set_sps_flag = 1;
    pFrameInfo->stdPictureInfo.flags.IrapPicFlag = ((encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) ||
                                                    (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_I)) ? 1 : 0;
    pFrameInfo->stdPictureInfo.flags.pic_output_flag = 1;
    // An IDR outputs the pictures still waiting in the decoder's DPB (C.5.2.2): with B frames one
    // picture waits for reordering at every IDR, and the flag would make the decoder discard it.
    pFrameInfo->stdPictureInfo.flags.no_output_of_prior_pics_flag = 0;
    pFrameInfo->stdPictureInfo.pic_type = stdPictureType;
    pFrameInfo->stdPictureInfo.pps_seq_parameter_set_id = m_sps.sps.sps_seq_parameter_set_id;
    pFrameInfo->stdPictureInfo.pps_pic_parameter_set_id = m_pps.pps_pic_parameter_set_id;
    pFrameInfo->stdPictureInfo.PicOrderCntVal = encodeFrameInfo->picOrderCntVal;
    pFrameInfo->stdPictureInfo.TemporalId = 0;

    return result;
}

void VkVideoEncoderH265::RefreshCodecRateControlParameters()
{
    // Through the base config pointer, for the reason spelled out on the
    // H.264 counterpart: that is the pointer
    // ApplyPendingRateControlUpdate just wrote the clamp on.
    if (!VkVideoEncoder::m_encoderConfig) {
        return;
    }
    EncoderConfigH265* config =
        VkVideoEncoder::m_encoderConfig->GetEncoderConfigh265();
    if (config == nullptr) {
        return;
    }
    // Reset to the codec-init state first; see the H.264 counterpart. The
    // fill raises useMinQp/useMaxQp and never lowers them, so re-invoking
    // it in place could not clear a clamp that had once been set.
    for (uint32_t layerIndx = 0;
         layerIndx < ARRAYSIZE(m_rateControlLayersInfoH265);
         layerIndx++) {
        m_rateControlLayersInfoH265[layerIndx] =
            VkVideoEncodeH265RateControlLayerInfoKHR{
                VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR};
    }
    config->GetRateControlParameters(&m_rateControlInfo,
                                     m_rateControlLayersInfo,
                                     &m_rateControlInfoH265,
                                     m_rateControlLayersInfoH265);
}

void VkVideoEncoderH265::GetResolvedQpClampForTest(uint32_t* pUseMinQp,
                                                   int32_t*  pMinQpI,
                                                   uint32_t* pUseMaxQp,
                                                   int32_t*  pMaxQpI) const
{
    const VkVideoEncodeH265RateControlLayerInfoKHR& layer =
        m_rateControlLayersInfoH265[0];
    *pUseMinQp = (layer.useMinQp == VK_TRUE) ? 1u : 0u;
    *pMinQpI   = layer.minQp.qpI;
    *pUseMaxQp = (layer.useMaxQp == VK_TRUE) ? 1u : 0u;
    *pMaxQpI   = layer.maxQp.qpI;
}

VkResult VkVideoEncoderH265::CodecHandleRateControlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoH265* pFrameInfo = GetEncodeFrameInfoH265(encodeFrameInfo);

    for (uint32_t layerIndx = 0; layerIndx < ARRAYSIZE(m_rateControlLayersInfoH265); layerIndx++) {
        pFrameInfo->rateControlLayersInfoH265[layerIndx] = m_rateControlLayersInfoH265[layerIndx];
        pFrameInfo->rateControlLayersInfoH265[layerIndx].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR;
        pFrameInfo->rateControlLayersInfo[layerIndx].pNext = &pFrameInfo->rateControlLayersInfoH265[layerIndx];
    }

    pFrameInfo->rateControlInfoH265 = m_rateControlInfoH265;
    pFrameInfo->rateControlInfoH265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR;
    pFrameInfo->rateControlInfoH265.subLayerCount = m_encoderConfig->gopStructure.GetTemporalLayerCount();

    if (pFrameInfo->pControlCmdChain != nullptr) {
        pFrameInfo->rateControlInfoH265.pNext = pFrameInfo->pControlCmdChain;
    }

    pFrameInfo->pControlCmdChain = (VkBaseInStructure*)&pFrameInfo->rateControlInfoH265;

    return VK_SUCCESS;
}
