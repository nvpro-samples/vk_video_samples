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
    m_encoderConfig = encoderConfig->GetEncoderConfigh265();
    assert(m_encoderConfig);

    if (m_encoderConfig->codec != VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR) {
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    VkResult result = InitEncoder(encoderConfig);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "\nERROR: InitEncoder() failed with ret(%d)\n", result);
        return result;
    }

    // Initialize DPB
    m_dpb.DpbSequenceStart(m_maxDpbPicturesCount, (m_encoderConfig->numRefL0 > 0) || (m_encoderConfig->numRefL1 > 0));

    if (m_encoderConfig->verbose) {
        std::cout << ", numRefL0: "    << (uint32_t)m_encoderConfig->numRefL0
                  << ", numRefL1: "    << (uint32_t)m_encoderConfig->numRefL1 << std::endl;
    }

    m_encoderConfig->GetRateControlParameters(&m_rateControlInfo, m_rateControlLayersInfo, &m_rateControlInfoH265, m_rateControlLayersInfoH265);

    m_encoderConfig->InitParamameters(&m_vps, &m_sps, &m_pps,
            m_encoderConfig->InitVuiParameters(&m_sps.vuiInfo,
                                                   &m_sps.hrdParameters,
                                                   &m_sps.subLayerHrdParametersNal));


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

    VkVideoSessionParametersKHR sessionParameters;
    result = m_vkDevCtx->CreateVideoSessionParametersKHR(*m_vkDevCtx,
                                                         &encodeSessionParametersCreateInfo,
                                                         nullptr,
                                                         &sessionParameters);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nEncodeFrame Error: Failed to get create video session parameters.\n");
        return result;
    }

    result = VulkanVideoSessionParameters::Create(m_vkDevCtx, m_videoSession,
                                                  sessionParameters, m_videoSessionParameters);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nEncodeFrame Error: Failed to get create video session object.\n");
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

    // TODO: Optimize this below very complex and inefficient DPB management code.

    uint32_t numRefL0 = m_encoderConfig->numRefL0;
    uint32_t numRefL1 = m_encoderConfig->numRefL1;

    if ((encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_P) ||
        (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B)) {

        numRefL0 = (numRefL0 == 0) ? 1 : numRefL0;

        if (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B) {
            numRefL1 = (numRefL1 == 0) ? 1 : numRefL1;
        }
    }

    m_dpb.ReferencePictureMarking(encodeFrameInfo->picOrderCntVal,
                                  (StdVideoH265PictureType)encodeFrameInfo->gopPosition.pictureType,
                                  m_sps.sps.flags.long_term_ref_pics_present_flag);


    if (!pFrameInfo->stdPictureInfo.flags.no_output_of_prior_pics_flag) {

        pFrameInfo->stdPictureInfo.pShortTermRefPicSet = &pFrameInfo->stdShortTermRefPicSet;
        m_dpb.InitializeRPS(m_sps.sps.pShortTermRefPicSet, m_sps.sps.num_short_term_ref_pic_sets,
                            &pFrameInfo->stdPictureInfo, &pFrameInfo->stdShortTermRefPicSet,
                            numRefL0, numRefL1);
    } else {
        pFrameInfo->stdPictureInfo.pShortTermRefPicSet = nullptr;
    }

    int32_t maxPicOrderCntLsb = 1 << (m_sps.sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
    const StdVideoH265ShortTermRefPicSet *pShortTermRefPicSet =
            !pFrameInfo->stdPictureInfo.flags.short_term_ref_pic_set_sps_flag ?
                pFrameInfo->stdPictureInfo.pShortTermRefPicSet :
                &m_sps.sps.pShortTermRefPicSet[pFrameInfo->stdPictureInfo.short_term_ref_pic_set_idx];

    const StdVideoH265LongTermRefPicsSps* pLongTermRefPicsSps = nullptr;
    if (m_sps.sps.flags.long_term_ref_pics_present_flag &&
            pFrameInfo->stdPictureInfo.pLongTermRefPics &&
            (pFrameInfo->stdPictureInfo.pLongTermRefPics->num_long_term_sps > 0)) {
        pLongTermRefPicsSps = m_sps.sps.pLongTermRefPicsSps;
        assert(pLongTermRefPicsSps);
    }

    bool success = m_dpbImagePool->GetAvailableImage(encodeFrameInfo->setupImageResource,
                                                     VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR);
    assert(success);
    assert(encodeFrameInfo->setupImageResource != nullptr);
    if (!success || (encodeFrameInfo->setupImageResource == nullptr)) {
	return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkVideoPictureResourceInfoKHR* setupImageViewPictureResource = encodeFrameInfo->setupImageResource->GetPictureResourceInfo();
    setupImageViewPictureResource->codedOffset = pFrameInfo->encodeInfo.srcPictureResource.codedOffset;
    setupImageViewPictureResource->codedExtent = pFrameInfo->encodeInfo.srcPictureResource.codedExtent;

    VkEncDpbH265::RefPicSet refPicSet{};
    int8_t targetDpbSlot = m_dpb.DpbPictureStart(encodeFrameInfo->frameEncodeInputOrderNum,
                                                 &pFrameInfo->stdPictureInfo,
                                                 pShortTermRefPicSet,
                                                 pLongTermRefPicsSps,
                                                 maxPicOrderCntLsb,
                                                 encodeFrameInfo->inputTimeStamp,
                                                 &refPicSet);
    assert(targetDpbSlot >= 0);

    if ((encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_P) ||
        (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B)) {

        m_dpb.SetupReferencePictureListLx((StdVideoH265PictureType)encodeFrameInfo->gopPosition.pictureType, &refPicSet,
                                          &pFrameInfo->stdReferenceListsInfo, numRefL0, numRefL1);

        pFrameInfo->stdPictureInfo.pRefLists = &pFrameInfo->stdReferenceListsInfo;

        if ((m_pps.num_ref_idx_l0_default_active_minus1 != pFrameInfo->stdPictureInfo.pRefLists->num_ref_idx_l0_active_minus1) ||
            (m_pps.num_ref_idx_l1_default_active_minus1 != pFrameInfo->stdPictureInfo.pRefLists->num_ref_idx_l1_active_minus1)) {

            pFrameInfo->stdSliceSegmentHeader.flags.num_ref_idx_active_override_flag = 1;
        }

    } else {
        pFrameInfo->stdPictureInfo.pRefLists = nullptr;
    }

    m_dpb.DpbPictureEnd(encodeFrameInfo->setupImageResource, 1 /* numTemporalLayers */, pFrameInfo->stdPictureInfo.flags.is_reference);

    // ***************** Start Update DPB info ************** //

    uint32_t numReferenceSlots = 0;
    assert(pFrameInfo->numDpbImageResources == 0);
    if (encodeFrameInfo->setupImageResource != nullptr) { // && pFrameInfo->stdPictureInfo.flags.is_reference
        // setup ref slot index 0
        pFrameInfo->referenceSlotsInfo[numReferenceSlots] = { VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
                                                              pFrameInfo->stdDpbSlotInfo, targetDpbSlot, setupImageViewPictureResource };

        pFrameInfo->setupReferenceSlotInfo = pFrameInfo->referenceSlotsInfo[numReferenceSlots];
        pFrameInfo->encodeInfo.pSetupReferenceSlot = &pFrameInfo->setupReferenceSlotInfo;

        numReferenceSlots++;

    } else {
        pFrameInfo->encodeInfo.pSetupReferenceSlot = nullptr;
    }
    pFrameInfo->numDpbImageResources = numReferenceSlots;

    if ((encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_P) ||
            (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B)) {

        for (uint32_t i = 0; i <= pFrameInfo->stdReferenceListsInfo.num_ref_idx_l0_active_minus1; i++) {

            uint8_t dpbIndex = pFrameInfo->stdReferenceListsInfo.RefPicList0[i];

            bool refPicAvailable = m_dpb.GetRefPicture(dpbIndex, pFrameInfo->dpbImageResources[numReferenceSlots]);
            assert(refPicAvailable);
	    if (!refPicAvailable) {
		return VK_ERROR_INITIALIZATION_FAILED;
	    }

            m_dpb.FillStdReferenceInfo(dpbIndex, &pFrameInfo->stdReferenceInfo[numReferenceSlots]);

            pFrameInfo->stdDpbSlotInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR;
            pFrameInfo->stdDpbSlotInfo[numReferenceSlots].pStdReferenceInfo = &pFrameInfo->stdReferenceInfo[numReferenceSlots];

            pFrameInfo->referenceSlotsInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].pNext = &pFrameInfo->stdDpbSlotInfo[numReferenceSlots];
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].slotIndex = dpbIndex;
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].pPictureResource =
                    pFrameInfo->dpbImageResources[numReferenceSlots]->GetPictureResourceInfo();

            numReferenceSlots++;
            assert(numReferenceSlots <= ARRAYSIZE(pFrameInfo->referenceSlotsInfo));
        }
        pFrameInfo->numDpbImageResources = numReferenceSlots;

        // TODO: iterate over L1 when coding B-frames
        if (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B) {
            for (uint32_t i = 0; i <= pFrameInfo->stdReferenceListsInfo.num_ref_idx_l1_active_minus1; i++) {

                uint8_t dpbIndex = pFrameInfo->stdReferenceListsInfo.RefPicList1[i];

                bool refPicAvailable = m_dpb.GetRefPicture(dpbIndex, pFrameInfo->dpbImageResources[numReferenceSlots]);
                assert(refPicAvailable);
                if (!refPicAvailable) {
                   return VK_ERROR_INITIALIZATION_FAILED;
                }
                m_dpb.FillStdReferenceInfo(dpbIndex, &pFrameInfo->stdReferenceInfo[numReferenceSlots]);

                pFrameInfo->stdDpbSlotInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR;
                pFrameInfo->stdDpbSlotInfo[numReferenceSlots].pStdReferenceInfo = &pFrameInfo->stdReferenceInfo[numReferenceSlots];

                pFrameInfo->referenceSlotsInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
                pFrameInfo->referenceSlotsInfo[numReferenceSlots].pNext = &pFrameInfo->stdDpbSlotInfo[numReferenceSlots];
                pFrameInfo->referenceSlotsInfo[numReferenceSlots].slotIndex = dpbIndex;
                pFrameInfo->referenceSlotsInfo[numReferenceSlots].pPictureResource =
                        pFrameInfo->dpbImageResources[numReferenceSlots]->GetPictureResourceInfo();

                numReferenceSlots++;
                assert(numReferenceSlots <= ARRAYSIZE(pFrameInfo->referenceSlotsInfo));
            }
            pFrameInfo->numDpbImageResources = numReferenceSlots;
        }
    }

    pFrameInfo->encodeInfo.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    //pFrameInfo->encodeInfo.flags = 0;
    // If the current picture is going to be a reference frame, the first
    // entry in the refSlots array contains information about the picture
    // resource associated with this frame. This entry should not be
    // provided in the list of reference resources for the current picture,
    // so skip refSlots[0].
    pFrameInfo->encodeInfo.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    encodeFrameInfo->encodeInfo.referenceSlotCount = numReferenceSlots - 1;
    encodeFrameInfo->encodeInfo.pReferenceSlots = pFrameInfo->referenceSlotsInfo + 1;

    // since encodeInfo.pReferenceSlots points to the address of the next element (+1), it's safe to set it one to -1
    // this is needed to explicity mark the unused element in BeginInfo for vkCmdBeginVideoCodingKHR() as inactive
    pFrameInfo->referenceSlotsInfo[0].slotIndex = -1;

    // ***************** End Update DPB info ************** //

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

    assert(encodeFrameInfo);
    assert(m_encoderConfig);
    assert(encodeFrameInfo->srcEncodeImageResource);

    encodeFrameInfo->frameEncodeInputOrderNum = m_encodeInputFrameNum++;

    bool isIdr = m_encoderConfig->gopStructure.GetPositionInGOP(m_gopState,
                                                                encodeFrameInfo->gopPosition,
                                                                (encodeFrameInfo->frameEncodeInputOrderNum == 0),
                                                                uint32_t(m_encoderConfig->numFrames - encodeFrameInfo->frameEncodeInputOrderNum));

    if (isIdr) {
        assert(encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR);
    }
    const bool isReference = m_encoderConfig->gopStructure.IsFrameReference(encodeFrameInfo->gopPosition);

    encodeFrameInfo->picOrderCntVal = encodeFrameInfo->gopPosition.inputOrder;

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("input", 1, encodeFrameInfo);

        if (encodeFrameInfo->lastFrame) {
            std::cout << "#### It is the last frame: " << encodeFrameInfo->frameInputOrderNum
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

    pFrameInfo->qualityLevel = m_encoderConfig->qualityLevel;
    pFrameInfo->videoSession = m_videoSession;
    pFrameInfo->videoSessionParameters = m_videoSessionParameters;

    pFrameInfo->stdPictureInfo.sps_video_parameter_set_id = m_vps.vpsInfo.vps_video_parameter_set_id;
    pFrameInfo->stdPictureInfo.pps_seq_parameter_set_id   = m_sps.sps.sps_seq_parameter_set_id;
    pFrameInfo->stdPictureInfo.pps_pic_parameter_set_id   = m_pps.pps_pic_parameter_set_id;

    VkResult result = VK_SUCCESS;


    VkDeviceSize size = GetBitstreamBuffer(encodeFrameInfo->outputBitstreamBuffer);
    assert((size > 0) && (encodeFrameInfo->outputBitstreamBuffer != nullptr));
    if ((size == 0) || (encodeFrameInfo->outputBitstreamBuffer == nullptr)) {
	return VK_ERROR_INITIALIZATION_FAILED;
    }
    pFrameInfo->encodeInfo.dstBuffer = encodeFrameInfo->outputBitstreamBuffer->GetBuffer();

    // For the actual (VCL) data, specify its insertion starting from the
    // provided offset into the bitstream buffer.
    encodeFrameInfo->encodeInfo.dstBufferOffset = 0; // FIXME: pEncPicParams->bitstreamBufferOffset;

    if (isIdr && (encodeFrameInfo->frameEncodeInputOrderNum == 0 /*|| pEncodeConfigH265->repeatSPSPPS || m_bReconfigForcedIDR*/)) {

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

    pFrameInfo->stdSliceSegmentHeader.slice_type = sliceType;
    pFrameInfo->stdSliceSegmentHeader.MaxNumMergeCand = 5;
    pFrameInfo->stdSliceSegmentHeader.flags.first_slice_segment_in_pic_flag = 1;
    pFrameInfo->stdSliceSegmentHeader.flags.dependent_slice_segment_flag = 0;
    pFrameInfo->stdSliceSegmentHeader.flags.slice_sao_luma_flag = 1;
    pFrameInfo->stdSliceSegmentHeader.flags.slice_sao_chroma_flag = 1;
    pFrameInfo->stdSliceSegmentHeader.flags.num_ref_idx_active_override_flag = 0;
    pFrameInfo->stdSliceSegmentHeader.flags.mvd_l1_zero_flag = 0;
    pFrameInfo->stdSliceSegmentHeader.flags.cabac_init_flag = 0;
    pFrameInfo->stdSliceSegmentHeader.flags.cu_chroma_qp_offset_enabled_flag = 1;
    pFrameInfo->stdSliceSegmentHeader.flags.deblocking_filter_override_flag = 1;
    pFrameInfo->stdSliceSegmentHeader.flags.slice_deblocking_filter_disabled_flag = 0;
    pFrameInfo->stdSliceSegmentHeader.flags.collocated_from_l0_flag = 0;
    pFrameInfo->stdSliceSegmentHeader.flags.slice_loop_filter_across_slices_enabled_flag = 0;

    if (m_rateControlInfo.rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
        switch (encodeFrameInfo->gopPosition.pictureType) {
            case VkVideoGopStructure::FRAME_TYPE_IDR:
            case VkVideoGopStructure::FRAME_TYPE_I:
                pFrameInfo->naluSliceSegmentInfo.constantQp = encodeFrameInfo->constQp.qpIntra;
                break;
            case VkVideoGopStructure::FRAME_TYPE_P:
                pFrameInfo->naluSliceSegmentInfo.constantQp = encodeFrameInfo->constQp.qpInterP;
                break;
            case VkVideoGopStructure::FRAME_TYPE_B:
                pFrameInfo->naluSliceSegmentInfo.constantQp = encodeFrameInfo->constQp.qpInterB;
                break;
            default:
                assert(!"Invalid picture type");
                break;
        }
    }

    pFrameInfo->stdPictureInfo.flags.is_reference = isReference;
    pFrameInfo->stdPictureInfo.flags.short_term_ref_pic_set_sps_flag = 1;
    pFrameInfo->stdPictureInfo.flags.IrapPicFlag = ((encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) ||
                                                    (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_I)) ? 1 : 0;
    pFrameInfo->stdPictureInfo.flags.pic_output_flag = 1;
    pFrameInfo->stdPictureInfo.flags.no_output_of_prior_pics_flag = (isIdr && (encodeFrameInfo->frameEncodeInputOrderNum != 0)) ? 1 : 0;
    pFrameInfo->stdPictureInfo.pic_type = stdPictureType;
    pFrameInfo->stdPictureInfo.pps_seq_parameter_set_id = m_sps.sps.sps_seq_parameter_set_id;
    pFrameInfo->stdPictureInfo.pps_pic_parameter_set_id = m_pps.pps_pic_parameter_set_id;
    pFrameInfo->stdPictureInfo.PicOrderCntVal = encodeFrameInfo->picOrderCntVal;
    pFrameInfo->stdPictureInfo.TemporalId = 0;


    if (m_sendControlCmd == true) {
        HandleCtrlCmd(encodeFrameInfo);
    }

    if (m_encoderConfig->enableQpMap) {
        ProcessQpMap(encodeFrameInfo);
    }

    EnqueueFrame(encodeFrameInfo, isIdr, isReference);
    return result;
}

VkResult VkVideoEncoderH265::HandleCtrlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoH265* pFrameInfo = GetEncodeFrameInfoH265(encodeFrameInfo);

    // Save the RateControlCmd request.
    const bool sendRateControlCmd = m_sendRateControlCmd;
    // Call the base class first to cover the bases
    VkVideoEncoder::HandleCtrlCmd(encodeFrameInfo);

    // Fill-n the codec-specific parts next
    if (sendRateControlCmd) {

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
    }

    return VK_SUCCESS;
}
