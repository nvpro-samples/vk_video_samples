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
#include "VkVideoEncoder/VkVideoEncoderAV1.h"
#include "VkVideoCore/VulkanVideoCapabilities.h"


#define FrameIsKey(frameType)   (frameType == STD_VIDEO_AV1_FRAME_TYPE_KEY)
#define FrameIsIntraOnly(frameType) (frameType == STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY)
#define FrameIsIntra(frameType) ((FrameIsKey(frameType)) || (FrameIsIntraOnly(frameType)))
#define FrameIsInter(frameType) ((frameType == STD_VIDEO_AV1_FRAME_TYPE_INTER))
#define FrameIsSwitch(frameType) ((frameType == STD_VIDEO_AV1_FRAME_TYPE_SWITCH))

static StdVideoAV1ReferenceName refNameList[] =
{
    STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_LAST2_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_LAST3_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME
};

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
    m_encoderConfig = encoderConfig->GetEncoderConfigAV1();
    assert(m_encoderConfig);

    if (m_encoderConfig->codec != VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) {
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    VkResult result = InitEncoder(encoderConfig);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "\nERROR: InitEncoder() failed with ret(%d)\n", result);
        return result;
    }

    const auto& encodeCaps = m_encoderConfig->av1EncodeCapabilities;
    if (encoderConfig->gopStructure.GetConsecutiveBFrameCount() > 0 &&
        encodeCaps.maxSingleReferenceCount < 2 &&
        encodeCaps.maxUnidirectionalCompoundReferenceCount == 0 &&
        encodeCaps.maxBidirectionalCompoundReferenceCount == 0) {
        std::cout << "B-frames were requested but the implementation does not support multiple reference frames!" << std::endl;
        assert(!"B-frames not supported");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Initialize DPB
    m_dpbAV1 = VkEncDpbAV1::CreateInstance();
    assert(m_dpbAV1);
    m_dpbAV1->DpbSequenceStart(encodeCaps, m_maxDpbPicturesCount, encoderConfig->gopStructure.GetConsecutiveBFrameCount(),
                               encoderConfig->tuningMode, encoderConfig->qualityLevel);

    m_encoderConfig->GetRateControlParameters(&m_rateControlInfo, m_rateControlLayersInfo, &m_stateAV1.m_rateControlInfoAV1, m_stateAV1.m_rateControlLayersInfoAV1);

    m_encoderConfig->InitSequenceHeader(&m_stateAV1.m_sequenceHeader, m_stateAV1.m_operatingPointsInfo);

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
        fprintf(stderr, "\nEncodeFrame Error: Failed to get create video session parameters.\n");
        return result;
    }

    result = VulkanVideoSessionParameters::Create(m_vkDevCtx, m_videoSession,
                                                  sessionParameters, m_videoSessionParameters);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "\nEncodeFrame Error: Failed to get create video session object.\n");
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

    uint32_t flags = 0;
    if (pFrameInfo->gopPosition.pictureType != VkVideoGopStructure::FRAME_TYPE_B) {
        if ((pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_I) &&
            (encodeFrameInfo->gopPosition.inputOrder == encodeFrameInfo->gopPosition.encodeOrder)) {
                flags = 1 << STD_VIDEO_AV1_REFERENCE_NAME_INTRA_FRAME;
        } else {
            flags = (m_encoderConfig->gopStructure.GetConsecutiveBFrameCount() == 0) ? 0 :
                        (m_numBFramesToEncode == 0 ? (1 << STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME) :
                            (1 << STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME));
        }
    }
    StdVideoAV1ReferenceName refName = m_dpbAV1->AssignReferenceFrameType(pFrameInfo->gopPosition.pictureType, flags, pFrameInfo->bIsReference);
    InitializeFrameHeader(&m_stateAV1.m_sequenceHeader, pFrameInfo, refName);
    if (!pFrameInfo->bShowExistingFrame) {
        m_dpbAV1->SetupReferenceFrameGroups(pFrameInfo->gopPosition.pictureType, pFrameInfo->stdPictureInfo.frame_type, pFrameInfo->picOrderCntVal);
        // For B pictures, L1 must be non zero.  Switch to P picture if L1 is zero.
        if ((pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B) && (m_dpbAV1->GetNumRefsL1()  == 0)) {
            pFrameInfo->gopPosition.pictureType = VkVideoGopStructure::FRAME_TYPE_P;
            m_numBFramesToEncode--; // picture type changed from B to P.  Reduce the B frame count to encode.
        }
        // TODO: How about P pictures with L1 > 0? Should we change it to B?
    }
    VkVideoEncoderAV1FrameUpdateType frameUpdateType = m_dpbAV1->GetFrameUpdateType(refName, pFrameInfo->bOverlayFrame);

    int8_t dpbIndx = m_dpbAV1->DpbPictureStart(pFrameInfo->stdPictureInfo.frame_type, refName,
                                               pFrameInfo->picOrderCntVal,
                                               pFrameInfo->stdPictureInfo.current_frame_id,
                                               pFrameInfo->bShowExistingFrame, pFrameInfo->frameToShowBufId);

    assert(dpbIndx >= 0);

    m_dpbAV1->ConfigureRefBufUpdate(pFrameInfo->bShownKeyFrameOrSwitch, pFrameInfo->bShowExistingFrame, frameUpdateType);
    pFrameInfo->stdPictureInfo.refresh_frame_flags = (uint8_t)m_dpbAV1->GetRefreshFrameFlags(pFrameInfo->bShownKeyFrameOrSwitch, pFrameInfo->bShowExistingFrame);

    if (pFrameInfo->bShowExistingFrame) {
        m_dpbAV1->DpbPictureEnd(dpbIndx, encodeFrameInfo->setupImageResource/*nullptr*/, &m_stateAV1.m_sequenceHeader,
                                pFrameInfo->bShowExistingFrame, pFrameInfo->bShownKeyFrameOrSwitch,
                                pFrameInfo->stdPictureInfo.flags.error_resilient_mode,
                                pFrameInfo->bOverlayFrame,
                                refName, frameUpdateType);
        return VK_SUCCESS;
    }

    // setup recon picture (pSetupReferenceSlot)
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

    uint32_t numReferenceSlots = 0;
    assert(pFrameInfo->numDpbImageResources == 0);
    if (encodeFrameInfo->setupImageResource != nullptr) {
        assert(setupImageViewPictureResource);
        m_dpbAV1->FillStdReferenceInfo((uint8_t) dpbIndx, &pFrameInfo->stdReferenceInfo[numReferenceSlots]);
        pFrameInfo->dpbSlotInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR;
        pFrameInfo->dpbSlotInfo[numReferenceSlots].pNext = nullptr;
        pFrameInfo->dpbSlotInfo[numReferenceSlots].pStdReferenceInfo = &pFrameInfo->stdReferenceInfo[numReferenceSlots];

        pFrameInfo->referenceSlotsInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        pFrameInfo->referenceSlotsInfo[numReferenceSlots].pNext = &pFrameInfo->dpbSlotInfo[numReferenceSlots];
        pFrameInfo->referenceSlotsInfo[numReferenceSlots].slotIndex = dpbIndx;
        pFrameInfo->referenceSlotsInfo[numReferenceSlots].pPictureResource = setupImageViewPictureResource;

        pFrameInfo->setupReferenceSlotInfo = pFrameInfo->referenceSlotsInfo[numReferenceSlots];
        pFrameInfo->encodeInfo.pSetupReferenceSlot = &pFrameInfo->setupReferenceSlotInfo;

        numReferenceSlots++;
        assert(numReferenceSlots <= ARRAYSIZE(pFrameInfo->referenceSlotsInfo));
    } else {
        pFrameInfo->encodeInfo.pSetupReferenceSlot = nullptr;
    }

    // reference frames
    memset(pFrameInfo->pictureInfo.referenceNameSlotIndices, 0xff, sizeof(pFrameInfo->pictureInfo.referenceNameSlotIndices));
    bool primaryRefCdfOnly = true;
    for (uint32_t groupId = 0; groupId < 2; groupId++) {
        for (int32_t i = 0; i < m_dpbAV1->GetNumRefsInGroup(groupId); i++) {
            int32_t refNameMinus1 = m_dpbAV1->GetRefNameMinus1(groupId, i);

            int32_t dpbIdx = m_dpbAV1->GetDpbIdx(groupId, i);
            assert(dpbIdx == m_dpbAV1->GetDpbIdx(refNameMinus1));

            assert(pFrameInfo->pictureInfo.referenceNameSlotIndices[refNameMinus1] == -1);
            pFrameInfo->pictureInfo.referenceNameSlotIndices[refNameMinus1] = dpbIdx;

            VkSharedBaseObj<VulkanVideoImagePoolNode> dpbImageView;
            bool refPicAvailable = m_dpbAV1->GetDpbPictureResource(dpbIdx, dpbImageView);
            assert(refPicAvailable);
            if (!refPicAvailable) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            bool bDuplicate = false;
            for (uint32_t j = 0; j < numReferenceSlots; j++) {
                if ((pFrameInfo->dpbImageResources[j] != nullptr) &&
                        (pFrameInfo->dpbImageResources[j]->GetImageIndex() == dpbImageView->GetImageIndex())) {
                    bDuplicate = true;
                    break;
                }
            }

            if (bDuplicate) {
                continue;
            }

            m_dpbAV1->FillStdReferenceInfo((uint8_t)dpbIdx, &pFrameInfo->stdReferenceInfo[numReferenceSlots]);

            pFrameInfo->dpbSlotInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR;
            pFrameInfo->dpbSlotInfo[numReferenceSlots].pNext = nullptr;
            pFrameInfo->dpbSlotInfo[numReferenceSlots].pStdReferenceInfo = &pFrameInfo->stdReferenceInfo[numReferenceSlots];

            pFrameInfo->referenceSlotsInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].pNext = &pFrameInfo->dpbSlotInfo[numReferenceSlots];
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].slotIndex = dpbIdx;
            pFrameInfo->dpbImageResources[numReferenceSlots] = dpbImageView;
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].pPictureResource =
                    pFrameInfo->dpbImageResources[numReferenceSlots]->GetPictureResourceInfo();

            if (refNameMinus1 == pFrameInfo->stdPictureInfo.primary_ref_frame) {
                primaryRefCdfOnly = false;
            }

            numReferenceSlots++;
        }
    }

    // Do not include primary_ref_frame reference when primaryRefCdfOnly in predictionMode calculation
    // If curent picture is a key frame or intra frame, use INTRA prediction mode.
    // If both groups contain atleast 1 picture, use BIDIR_COMP prediction. (UNIDIR_COMP prediction also possible)
    // else if any of the group contains more than 1 picture, use UNIDIR_COMP (Refer section 5.11.25 of AV1 spec)
    // else use SINGLE
    const auto& encodeCaps = m_encoderConfig->av1EncodeCapabilities;
    bool bLastRefFramePresent = (pFrameInfo->pictureInfo.referenceNameSlotIndices[STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] != -1);
    bool bBwdRefFramePreset = (pFrameInfo->pictureInfo.referenceNameSlotIndices[STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] != -1);
    bool bAltRefFramePreset = (pFrameInfo->pictureInfo.referenceNameSlotIndices[STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] != -1);
    if ((pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_I) || (pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR)) {
        pFrameInfo->pictureInfo.predictionMode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_INTRA_ONLY_KHR;
    } else if ((m_dpbAV1->GetNumRefsInGroup1() > 0) && (m_dpbAV1->GetNumRefsInGroup2() > 0)) {
        pFrameInfo->pictureInfo.predictionMode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_BIDIRECTIONAL_COMPOUND_KHR;
    } else if ((bLastRefFramePresent && (m_dpbAV1->GetNumRefsInGroup1() >= 2)) || (bBwdRefFramePreset && bAltRefFramePreset)) {
        pFrameInfo->pictureInfo.predictionMode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_UNIDIRECTIONAL_COMPOUND_KHR;
    } else {
        pFrameInfo->pictureInfo.predictionMode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR;
    }

    // If any of the optimal reference modes are not supported, fall back to a simpler mode
    if (pFrameInfo->pictureInfo.predictionMode == VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_BIDIRECTIONAL_COMPOUND_KHR &&
        encodeCaps.maxBidirectionalCompoundReferenceCount == 0) {
        // TODO: try to remap the references to unidirectional, based on mask/counts
        pFrameInfo->pictureInfo.predictionMode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_UNIDIRECTIONAL_COMPOUND_KHR;
    }
    if (pFrameInfo->pictureInfo.predictionMode == VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_UNIDIRECTIONAL_COMPOUND_KHR &&
        encodeCaps.maxUnidirectionalCompoundReferenceCount == 0) {
        // TODO: try to remap the references to single reference, based on mask/count
        pFrameInfo->pictureInfo.predictionMode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR;
    }
    if (pFrameInfo->pictureInfo.predictionMode == VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR &&
        encodeCaps.maxSingleReferenceCount == 0) {
        pFrameInfo->pictureInfo.predictionMode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_INTRA_ONLY_KHR;
    }

    // If primary_ref_frame is not in reference list, add it explicity
    if ((pFrameInfo->stdPictureInfo.primary_ref_frame != STD_VIDEO_AV1_PRIMARY_REF_NONE) && (primaryRefCdfOnly)) {
        int32_t dpbIdx = m_dpbAV1->GetDpbIdx(pFrameInfo->stdPictureInfo.primary_ref_frame);

        VkSharedBaseObj<VulkanVideoImagePoolNode> dpbImageView;
        bool refPicAvailable = m_dpbAV1->GetDpbPictureResource(dpbIdx, dpbImageView);
        assert(refPicAvailable);
        if (!refPicAvailable) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        bool bDuplicate = false;
        for (uint32_t j = 0; j < numReferenceSlots; j++) {
            if ((pFrameInfo->dpbImageResources[j] != nullptr) &&
                    (pFrameInfo->dpbImageResources[j]->GetImageIndex() == dpbImageView->GetImageIndex())) {
                bDuplicate = true;
                break;
            }
        }
        if (bDuplicate) {
            // reference is already present.  So, update referenceNameSlotIndices
            assert(pFrameInfo->pictureInfo.referenceNameSlotIndices[pFrameInfo->stdPictureInfo.primary_ref_frame] == -1);
            pFrameInfo->pictureInfo.referenceNameSlotIndices[pFrameInfo->stdPictureInfo.primary_ref_frame] = dpbIdx;
        } else {
            // reference itself is not present.  Add it to the referenceSlotInfo and udpate referenceNameSlotIndices
            m_dpbAV1->FillStdReferenceInfo((uint8_t)dpbIdx, &pFrameInfo->stdReferenceInfo[numReferenceSlots]);

            pFrameInfo->dpbSlotInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR;
            pFrameInfo->dpbSlotInfo[numReferenceSlots].pNext = nullptr;
            pFrameInfo->dpbSlotInfo[numReferenceSlots].pStdReferenceInfo = &pFrameInfo->stdReferenceInfo[numReferenceSlots];

            pFrameInfo->referenceSlotsInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].pNext = &pFrameInfo->dpbSlotInfo[numReferenceSlots];
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].slotIndex = dpbIdx;
            pFrameInfo->dpbImageResources[numReferenceSlots] = dpbImageView;
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].pPictureResource =
                    pFrameInfo->dpbImageResources[numReferenceSlots]->GetPictureResourceInfo();

            assert(pFrameInfo->pictureInfo.referenceNameSlotIndices[pFrameInfo->stdPictureInfo.primary_ref_frame] == -1);
            pFrameInfo->pictureInfo.referenceNameSlotIndices[pFrameInfo->stdPictureInfo.primary_ref_frame] = dpbIdx;

            numReferenceSlots++;
        }
    }

    pFrameInfo->encodeInfo.referenceSlotCount = numReferenceSlots - 1;
    pFrameInfo->encodeInfo.pReferenceSlots = pFrameInfo->referenceSlotsInfo + 1;
    pFrameInfo->numDpbImageResources = numReferenceSlots;

    pFrameInfo->pictureInfo.primaryReferenceCdfOnly = primaryRefCdfOnly;
    if (pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_P) {
        pFrameInfo->pictureInfo.rateControlGroup = VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_PREDICTIVE_KHR;
    } else if (pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B) {
        pFrameInfo->pictureInfo.rateControlGroup = VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_BIPREDICTIVE_KHR;
    } else {
        pFrameInfo->pictureInfo.rateControlGroup = VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_INTRA_KHR;
    }

    m_dpbAV1->DpbPictureEnd(dpbIndx, encodeFrameInfo->setupImageResource, &m_stateAV1.m_sequenceHeader,
                            pFrameInfo->bShowExistingFrame, pFrameInfo->bShownKeyFrameOrSwitch,
                            pFrameInfo->stdPictureInfo.flags.error_resilient_mode,
                            pFrameInfo->bOverlayFrame,
                            refName, frameUpdateType);


    // this is needed to explicity mark the unused element in BeginInfo for vkCmdBeginVideoCodingKHR() as inactive
    pFrameInfo->referenceSlotsInfo[0].slotIndex = -1;

    if (pFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B) {
        assert(m_numBFramesToEncode != 0);
        m_numBFramesToEncode--;
    }

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

    assert(encodeFrameInfo);
    assert(m_encoderConfig);
    assert(encodeFrameInfo->srcEncodeImageResource);

    pFrameInfo->videoSession = m_videoSession;
    pFrameInfo->videoSessionParameters = m_videoSessionParameters;

    encodeFrameInfo->frameEncodeInputOrderNum = m_encodeInputFrameNum++;

    // GetPositionInGOP() method returns display position of the picture relative to last key frame picture.
    bool isIdr = m_encoderConfig->gopStructure.GetPositionInGOP(m_gopState,
                                                                encodeFrameInfo->gopPosition,
                                                                (encodeFrameInfo->frameEncodeInputOrderNum == 0),
                                                                uint32_t(m_encoderConfig->numFrames - encodeFrameInfo->frameEncodeInputOrderNum));
    if (isIdr) {
        assert(encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR);
        VkResult result = EncodeVideoSessionParameters(encodeFrameInfo);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    encodeFrameInfo->picOrderCntVal = encodeFrameInfo->gopPosition.inputOrder;

    pFrameInfo->bIsKeyFrame = (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_IDR);
    pFrameInfo->bIsReference = m_encoderConfig->gopStructure.IsFrameReference(encodeFrameInfo->gopPosition);
    pFrameInfo->bShowExistingFrame = false;
    pFrameInfo->bOverlayFrame = false;
    if (encodeFrameInfo->gopPosition.pictureType == VkVideoGopStructure::FRAME_TYPE_B) {
        m_numBFramesToEncode++;
    }
    if (pFrameInfo->bIsKeyFrame) {
        assert(encodeFrameInfo->picOrderCntVal == 0);
        m_lastKeyFrameOrderHint = encodeFrameInfo->picOrderCntVal;
    }
    pFrameInfo->picOrderCntVal = encodeFrameInfo->picOrderCntVal - m_lastKeyFrameOrderHint;

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("input", 1, encodeFrameInfo);

        if (encodeFrameInfo->lastFrame) {
            std::cout << "#### It is the last frame: " << encodeFrameInfo->frameInputOrderNum
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

    pFrameInfo->qualityLevel = m_encoderConfig->qualityLevel;

    //if (encodeFrameInfo->frameEncodeInputOrderNum == 0) {
    //    pFrameInfo->encodeInfo.flags |= VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    //}

    VkDeviceSize size = GetBitstreamBuffer(encodeFrameInfo->outputBitstreamBuffer);
    assert((size > 0) && (encodeFrameInfo->outputBitstreamBuffer != nullptr));
    if ((size == 0) || (encodeFrameInfo->outputBitstreamBuffer == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    pFrameInfo->encodeInfo.dstBuffer = encodeFrameInfo->outputBitstreamBuffer->GetBuffer();

    pFrameInfo->encodeInfo.dstBufferOffset = 0;

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

    if (m_sendControlCmd == true) {
        HandleCtrlCmd(encodeFrameInfo);
    }

    if (m_encoderConfig->enableQpMap) {
        ProcessQpMap(encodeFrameInfo);
    }

    EnqueueFrame(encodeFrameInfo, pFrameInfo->bIsKeyFrame, pFrameInfo->bIsReference);

    return VK_SUCCESS;
}

VkResult VkVideoEncoderAV1::HandleCtrlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);

    // Save the RateControlCmd request
    const bool sendRateControlCmd = m_sendRateControlCmd;
    // Call the base class first to cover the bases
    VkVideoEncoder::HandleCtrlCmd(encodeFrameInfo);

    // Fill-in the codec-specific parts next
    if (sendRateControlCmd) {
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
    }

    return VK_SUCCESS;
}

void VkVideoEncoderAV1::InitializeFrameHeader(StdVideoAV1SequenceHeader* pSequenceHdr, VkVideoEncodeFrameInfoAV1* pFrameInfo,
                                              StdVideoAV1ReferenceName& refName)
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
    pStdPictureInfo->order_hint = (uint8_t)(pFrameInfo->picOrderCntVal % (1 << orderHintBits));
    if (pFrameInfo->bOverlayFrame) {
        assert(pFrameInfo->bShowExistingFrame);
        pFrameInfo->frameToShowBufId = m_dpbAV1->GetOverlayRefBufId(pFrameInfo->picOrderCntVal);
        assert(pFrameInfo->frameToShowBufId != VkEncDpbAV1::INVALID_IDX);
        // Re-initialize
        int32_t refBufDpbId = m_dpbAV1->GetRefBufDpbId(pFrameInfo->frameToShowBufId);
        refName = m_dpbAV1->GetRefName(refBufDpbId);
        pStdPictureInfo->frame_type = m_dpbAV1->GetFrameType(refBufDpbId);
        pStdPictureInfo->current_frame_id = m_dpbAV1->GetFrameId(refBufDpbId);
    }

    pStdPictureInfo->flags.show_frame = (((refName == STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME) ||
                                          (refName == STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME) ||
                                          (refName == STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME)) &&
                                            (!pFrameInfo->bOverlayFrame)) ? 0 : 1;
    pStdPictureInfo->flags.showable_frame = (pStdPictureInfo->flags.show_frame) ? (FrameIsKey(pStdPictureInfo->frame_type) ? 0 : 1) : 1;
    if ((pStdPictureInfo->frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY) && (pStdPictureInfo->flags.show_frame == 1)) {
        pStdPictureInfo->flags.error_resilient_mode = 1;
    }
    pFrameInfo->bShownKeyFrameOrSwitch = (FrameIsKey(pStdPictureInfo->frame_type) && pStdPictureInfo->flags.show_frame) ||
                                            FrameIsSwitch(pStdPictureInfo->frame_type);

    if (!pFrameInfo->bShowExistingFrame) {
        if (FrameIsInter(pStdPictureInfo->frame_type) || FrameIsSwitch(pStdPictureInfo->frame_type)) {
            for (StdVideoAV1ReferenceName ref : refNameList) {

                if (pSequenceHdr->flags.frame_id_numbers_present_flag) {
                    int32_t dpbIdx = m_dpbAV1->GetRefFrameDpbId(ref);
                    if (dpbIdx == VkEncDpbAV1::INVALID_IDX){
                        assert(0);
                        continue;
                    }
                    int32_t deltaFrameIdMinus1 = ((pStdPictureInfo->current_frame_id - m_dpbAV1->GetFrameId(dpbIdx) + ( 1<< frameIdBits))) % (1 << frameIdBits) - 1;
                    assert((deltaFrameIdMinus1 >= 0) && (deltaFrameIdMinus1 < (1 << (pSequenceHdr->delta_frame_id_length_minus_2 + 2))));
                    pStdPictureInfo->delta_frame_id_minus_1[ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] =deltaFrameIdMinus1;
                }

                pStdPictureInfo->ref_frame_idx[ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] = (int8_t)m_dpbAV1->GetRefBufId(ref);
            }

            for (int32_t bufIdx = 0; bufIdx < STD_VIDEO_AV1_NUM_REF_FRAMES; bufIdx++) {
                int32_t dpbIdx = m_dpbAV1->GetRefBufDpbId(bufIdx);
                assert(dpbIdx != VkEncDpbAV1::INVALID_IDX);
                pStdPictureInfo->ref_order_hint[bufIdx] = (uint8_t)m_dpbAV1->GetPicOrderCntVal(dpbIdx);
            }
        }
    }
    pStdPictureInfo->primary_ref_frame = (uint8_t)m_dpbAV1->GetPrimaryRefFrame(pStdPictureInfo->frame_type, refName,
                                                                               pStdPictureInfo->flags.error_resilient_mode,
                                                                               pFrameInfo->bOverlayFrame);

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
            memset(&pFrameInfo->stdCdefInfo, 0, sizeof(StdVideoAV1CDEF));
            pFrameInfo->stdCdefInfo.cdef_damping_minus_3 = 2;
            pFrameInfo->stdCdefInfo.cdef_bits = 2;
            pFrameInfo->stdCdefInfo.cdef_y_pri_strength[0] = 0;
            pFrameInfo->stdCdefInfo.cdef_y_sec_strength[0] = 0;
            pFrameInfo->stdCdefInfo.cdef_y_pri_strength[1] = 2;
            pFrameInfo->stdCdefInfo.cdef_y_sec_strength[1] = 0;
            pFrameInfo->stdCdefInfo.cdef_y_pri_strength[2] = 4;
            pFrameInfo->stdCdefInfo.cdef_y_sec_strength[2] = 0;
            pFrameInfo->stdCdefInfo.cdef_y_pri_strength[3] = 9;
            pFrameInfo->stdCdefInfo.cdef_y_sec_strength[4] = 0;
        }
    }

    if (!AllLossless && !pStdPictureInfo->flags.allow_intrabc && m_encoderConfig->enableLr) {
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

VkResult VkVideoEncoderAV1::AssembleBitstreamData(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                                  uint32_t frameIdx, uint32_t ofTotalFrames)
{
    VkVideoEncodeFrameInfoAV1* pFrameInfo = GetEncodeFrameInfoAV1(encodeFrameInfo);

    if (pFrameInfo->bShowExistingFrame) {
        WriteShowExistingFrameHeader(encodeFrameInfo);
        return VK_SUCCESS;
    }

    assert(encodeFrameInfo->outputBitstreamBuffer != nullptr);
    assert(encodeFrameInfo->encodeCmdBuffer != nullptr);

    VkResult result = encodeFrameInfo->encodeCmdBuffer->SyncHostOnCmdBuffComplete(false, "encoderEncodeFence");
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nWait on encoder complete fence has failed with result 0x%x.\n", result);
        return result;
    }

    uint32_t querySlotId = (uint32_t)-1;
    VkQueryPool queryPool = encodeFrameInfo->encodeCmdBuffer->GetQueryPool(querySlotId);

    // Since we can use a single command buffer from multiple frames,
    // we can't just use the querySlotId from the command buffer.
    // Instead we use the input image index that should be unique for each frame.
    querySlotId = (uint32_t)encodeFrameInfo->srcEncodeImageResource->GetImageIndex();

    // get output results
    struct VulkanVideoEncodeStatus {
        uint32_t bitstreamStartOffset;
        uint32_t bitstreamSize;
        VkQueryResultStatusKHR status;
    } encodeResult{};

    // Fetch the coded VCL data and its information
    result = m_vkDevCtx->GetQueryPoolResults(*m_vkDevCtx, queryPool, querySlotId,
                                             1, sizeof(encodeResult), &encodeResult, sizeof(encodeResult),
                                             VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);

    assert(result == VK_SUCCESS);
    assert(encodeResult.status == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);

    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nRetrieveData Error: Failed to get vcl query pool results.\n");
        return result;
    }

    bool flushFrameData = (pFrameInfo->stdPictureInfo.flags.show_frame || pFrameInfo->bShowExistingFrame);

    VkDeviceSize maxSize;
    uint8_t* data = encodeFrameInfo->outputBitstreamBuffer->GetDataPtr(0, maxSize);

    if (!flushFrameData) {
        if (!(m_bitstream.size() > frameIdx)) {
            m_bitstream.resize(frameIdx + 1);
        }

        m_bitstream[frameIdx].resize(encodeResult.bitstreamSize);
        memcpy(m_bitstream[frameIdx].data(), data + encodeResult.bitstreamStartOffset, encodeResult.bitstreamSize);
    }

    if (m_encoderConfig->verboseFrameStruct) {
        std::cout << "       == Output VCL data SUCCESS for " << frameIdx << " with size: " << encodeResult.bitstreamSize
                  << " and offset: " << encodeResult.bitstreamStartOffset
                  << ", Input Order: " << (uint32_t)encodeFrameInfo->gopPosition.inputOrder
                  << ", Encode  Order: " << (uint32_t)encodeFrameInfo->gopPosition.encodeOrder << std::endl << std::flush;
    }

    m_batchFramesIndxSetToAssemble.insert(frameIdx);

    if (flushFrameData) {

        // IVF header
        if (encodeFrameInfo->frameInputOrderNum == 0) {
            uint8_t header[32];
            mem_put_le32(header     , MAKE_FOURCC('D', 'K', 'I', 'F'));
            mem_put_le16(header +  4, 0);
            mem_put_le16(header +  6, 32);
            mem_put_le32(header +  8, MAKE_FOURCC('A', 'V', '0', '1'));
            mem_put_le16(header + 12, m_encoderConfig->encodeWidth);
            mem_put_le16(header + 14, m_encoderConfig->encodeHeight);
            mem_put_le32(header + 16, m_encoderConfig->frameRateNumerator);
            mem_put_le32(header + 20, m_encoderConfig->frameRateDenominator);
            mem_put_le32(header + 24, m_encoderConfig->numFrames);
            mem_put_le32(header + 28, 0);
            fwrite(header, 1, sizeof(header), m_encoderConfig->outputFileHandler.GetFileHandle());
        }

        // IVF frame header
        size_t framesSize = 2 + encodeFrameInfo->bitstreamHeaderBufferSize; /* 2 is temporal delimiter size */
        for (const auto& curIndex : m_batchFramesIndxSetToAssemble) {

            size_t frameSize = 0;
            if (frameIdx == curIndex) {
                frameSize = encodeResult.bitstreamSize;
            } else {
                // Use the accumulated frame size
                frameSize = m_bitstream[curIndex].size();
            }

            framesSize += frameSize;

            if (m_encoderConfig->verboseFrameStruct) {
                std::cout << ">>>>>> Assembly VCL index " << curIndex << " has size: " << frameSize
                           << std::endl << std::flush;
            }
        }

        if (m_encoderConfig->verboseFrameStruct) {
            std::cout << ">>>>>> Assembly total VCL data at " << frameIdx << " is: "
                       << framesSize - (2 + encodeFrameInfo->bitstreamHeaderBufferSize)
                       << std::endl << std::flush;
        }

        encodeFrameInfo->inputTimeStamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::high_resolution_clock::now().time_since_epoch())
                                                  .count();

        encodeFrameInfo->inputTimeStamp = encodeFrameInfo->frameInputOrderNum;

        uint64_t pts = encodeFrameInfo->inputTimeStamp;
        uint8_t frameHeader[12];
        mem_put_le32(frameHeader    , (uint32_t)framesSize); // updated with correct size later on
        mem_put_le32(frameHeader + 4, (uint32_t)(pts & 0xffffffff));
        mem_put_le32(frameHeader + 8, (uint32_t)(pts >> 32));
        fwrite(frameHeader, 1, sizeof(frameHeader), m_encoderConfig->outputFileHandler.GetFileHandle());

        // Temporal delimiter
        uint8_t tdObu[2] = { 0x12, 0x00 };
        fwrite(tdObu, 1, sizeof(tdObu), m_encoderConfig->outputFileHandler.GetFileHandle());

        // sequence header
        if(encodeFrameInfo->bitstreamHeaderBufferSize > 0) {
            size_t nonVcl = fwrite(encodeFrameInfo->bitstreamHeaderBuffer + encodeFrameInfo->bitstreamHeaderOffset,
                        1, encodeFrameInfo->bitstreamHeaderBufferSize,
                        m_encoderConfig->outputFileHandler.GetFileHandle());

            if (m_encoderConfig->verboseFrameStruct) {
                std::cout << "       == Non-Vcl data " << (nonVcl ? "SUCCESS" : "FAIL")
                          << " File Output non-VCL data with size: " << encodeFrameInfo->bitstreamHeaderBufferSize
                          << ", Input Order: " << (uint32_t)encodeFrameInfo->gopPosition.inputOrder
                          << ", Encode  Order: " << (uint32_t)encodeFrameInfo->gopPosition.encodeOrder
                          << std::endl << std::flush;
            }
        }

        for (const auto& curIndex : m_batchFramesIndxSetToAssemble) {
            const uint8_t* writeData = (frameIdx == curIndex) ? (data + encodeResult.bitstreamStartOffset) : m_bitstream[curIndex].data();
            const size_t bytesToWrite = (frameIdx == curIndex) ? encodeResult.bitstreamSize : m_bitstream[curIndex].size();

            // Write data in chunks to handle partial writes
            size_t totalBytesWritten = 0;
            while (totalBytesWritten < bytesToWrite) {
                const size_t remainingBytes = bytesToWrite - totalBytesWritten;
                const size_t bytesWritten = fwrite(writeData + totalBytesWritten, 1, 
                                                 remainingBytes,
                                                 m_encoderConfig->outputFileHandler.GetFileHandle());

                if (bytesWritten == 0) {
                    std::cerr << "Failed to write bitstream data" << std::endl;
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                }

                totalBytesWritten += bytesWritten;
            }

            // Verify complete write
            if (totalBytesWritten != bytesToWrite) {
                std::cerr << "Warning: Incomplete write - expected " << bytesToWrite << " bytes but wrote " << totalBytesWritten << " bytes\n";
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        // reset the batch frames to assemble counter
        m_batchFramesIndxSetToAssemble.clear();
    }
    fflush(m_encoderConfig->outputFileHandler.GetFileHandle());

    return result;
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
    fwrite(frameHeader, 1, sizeof(frameHeader), m_encoderConfig->outputFileHandler.GetFileHandle());

    // Temporal delimiter
    uint8_t tdObu[2] = { 0x12, 0x00 };
    fwrite(tdObu, 1, sizeof(tdObu), m_encoderConfig->outputFileHandler.GetFileHandle());

    // frame header
    fwrite(header.data(), 1, header.size(), m_encoderConfig->outputFileHandler.GetFileHandle());
    fwrite(payload.data(), 1, payload.size(), m_encoderConfig->outputFileHandler.GetFileHandle());
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
            GetAvailablePoolNode(showExistingFrameInfo);
            assert(showExistingFrameInfo);

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
