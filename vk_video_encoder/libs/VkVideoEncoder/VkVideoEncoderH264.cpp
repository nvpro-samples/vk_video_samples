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
    m_encoderConfig = encoderConfig->GetEncoderConfigh264();
    assert(m_encoderConfig);

    if (m_encoderConfig->codec != VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) {
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    VkResult result = InitEncoder(encoderConfig);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "\nERROR: InitEncoder() failed with ret(%d)\n", result);
        return result;
    }

    // Initialize DPB
    m_dpb264 = VkEncDpbH264::CreateInstance();
    assert(m_dpb264);
    m_dpb264->DpbSequenceStart(m_maxDpbPicturesCount);

    m_encoderConfig->GetRateControlParameters(&m_rateControlInfo, m_rateControlLayersInfo, &m_h264.m_rateControlInfoH264, m_h264.m_rateControlLayersInfoH264);

    m_encoderConfig->InitSpsPpsParameters(&m_h264.m_spsInfo, &m_h264.m_ppsInfo,
            m_encoderConfig->InitVuiParameters(&m_h264.m_vuiInfo, &m_h264.m_hrdParameters));


    // create SPS and PPS set
    VideoSessionParametersInfo videoSessionParametersInfo(*m_videoSession,
                                                          &m_h264.m_spsInfo,
                                                          &m_h264.m_ppsInfo,
                                                          m_encoderConfig->qualityLevel,
                                                          m_encoderConfig->enableQpMap,
                                                          m_qpMapTexelSize);

    VkVideoSessionParametersCreateInfoKHR* encodeSessionParametersCreateInfo = videoSessionParametersInfo.getVideoSessionParametersInfo();
    encodeSessionParametersCreateInfo->flags = 0;
    VkVideoSessionParametersKHR sessionParameters;
    result = m_vkDevCtx->CreateVideoSessionParametersKHR(*m_vkDevCtx,
                                                         encodeSessionParametersCreateInfo,
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

void VkVideoEncoderH264::POCBasedRefPicManagement(StdVideoEncodeH264RefPicMarkingEntry* m_mmco,
                                                  uint8_t& m_refPicMarkingOpCount) {
    int picNumX = -1;
    int currPicNum = -1;
    int maxPicNum = 1 << (m_h264.m_spsInfo.log2_max_frame_num_minus4 + 4);

    picNumX = m_dpb264->GetPicNumXWithMinPOC(0, 0, 0);

    // TODO: Check if this needs to be changed to m_dpb264->GetCurrentPicNum()
    currPicNum = m_dpb264->GetCurrentDpbEntry()->frame_num % maxPicNum;

    if (currPicNum > 0 && (picNumX >= 0)) {
        m_mmco[m_refPicMarkingOpCount].memory_management_control_operation = STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_UNMARK_SHORT_TERM;
        m_mmco[m_refPicMarkingOpCount++].difference_of_pic_nums_minus1 = (uint16_t)(currPicNum - picNumX - 1);
        m_mmco[m_refPicMarkingOpCount++].memory_management_control_operation = STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_END;
    }
}

void VkVideoEncoderH264::FrameNumBasedRefPicManagement(StdVideoEncodeH264RefPicMarkingEntry* m_mmco,
                                                       uint8_t& m_refPicMarkingOpCount)
{
    int picNumX = -1;
    int currPicNum = -1;
    int maxPicNum = 1 << (m_h264.m_spsInfo.log2_max_frame_num_minus4 + 4);

    picNumX = m_dpb264->GetPicNumXWithMinFrameNumWrap(0, 0, 0);

    // TODO: Check if this needs to be changed to m_dpb264->GetCurrentPicNum()
    currPicNum = m_dpb264->GetCurrentDpbEntry()->frame_num % maxPicNum;

    if (currPicNum > 0 && (picNumX >= 0)) {
        m_mmco[m_refPicMarkingOpCount].memory_management_control_operation = STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_UNMARK_SHORT_TERM;
        m_mmco[m_refPicMarkingOpCount++].difference_of_pic_nums_minus1 = (uint16_t)(currPicNum - picNumX - 1);
        m_mmco[m_refPicMarkingOpCount++].memory_management_control_operation = STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_END;
    }
}

VkResult VkVideoEncoderH264::SetupRefPicReorderingCommands(const PicInfoH264 *pPicInfo,
                                                           const StdVideoEncodeH264SliceHeader *slh,
                                                           StdVideoEncodeH264ReferenceListsInfoFlags* pFlags,
                                                           StdVideoEncodeH264RefListModEntry* m_ref_pic_list_modification_l0,
                                                           uint8_t& m_refList0ModOpCount)
{
    // Either the current picture requires no references, or the active
    // reference list does not contain corrupted pictures. Skip reordering.
    if (!m_dpb264->NeedToReorder()) {
        return VK_SUCCESS;
    }

    StdVideoEncodeH264RefListModEntry *refPicList0Mod = m_ref_pic_list_modification_l0;

    NvVideoEncodeH264DpbSlotInfoLists<STD_VIDEO_H264_MAX_NUM_LIST_REF> refLists;
    m_dpb264->GetRefPicList(pPicInfo, &refLists, &m_h264.m_spsInfo, &m_h264.m_ppsInfo, slh, nullptr, true);

    int maxPicNum = 1 << (m_h264.m_spsInfo.log2_max_frame_num_minus4 + 4);
    int picNumLXPred = m_dpb264->GetCurrentDpbEntry()->frame_num % maxPicNum;
    int numSTR = 0, numLTR = 0;
    m_dpb264->GetNumRefFramesInDPB(0, &numSTR, &numLTR);

    // Re-order the active list to skip all corrupted frames
    pFlags->ref_pic_list_modification_flag_l0 = true;
    m_refList0ModOpCount = 0;
    if (numSTR) {
        for (uint32_t i = 0; i < refLists.refPicListCount[0]; i++) {
            int diff = m_dpb264->GetPicNum(refLists.refPicList[0][i]) - picNumLXPred;
            if (diff <= 0) {
                refPicList0Mod[m_refList0ModOpCount].modification_of_pic_nums_idc =
                    STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_SHORT_TERM_SUBTRACT;
                refPicList0Mod[m_refList0ModOpCount].abs_diff_pic_num_minus1 = (uint16_t)(abs(diff) ? abs(diff) - 1 : maxPicNum - 1);
            } else {
                refPicList0Mod[m_refList0ModOpCount].modification_of_pic_nums_idc =
                    STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_SHORT_TERM_ADD;
                refPicList0Mod[m_refList0ModOpCount].abs_diff_pic_num_minus1 = (uint16_t)(abs(diff) - 1);
            }
            m_refList0ModOpCount++;
            picNumLXPred = m_dpb264->GetPicNum(refLists.refPicList[0][i]);
        }
    } else if (numLTR) {
        // If we end up supporting LTR, add code here.
    }

    refPicList0Mod[m_refList0ModOpCount++].modification_of_pic_nums_idc = STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_END;

    assert(m_refList0ModOpCount > 1);

    return VK_SUCCESS;
}

VkResult VkVideoEncoderH264::ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                        uint32_t frameIdx, uint32_t ofTotalFrames)
{
    VkVideoEncodeFrameInfoH264* pFrameInfo = GetEncodeFrameInfoH264(encodeFrameInfo);

    if (m_encoderConfig->verboseFrameStruct) {
        DumpStateInfo("process DPB", 3, encodeFrameInfo, frameIdx, ofTotalFrames);
    }

    // TODO: Optimize this below very complex and inefficient DPB management code.

    VkVideoGopStructure::FrameType picType = encodeFrameInfo->gopPosition.pictureType;
    bool isReference = pFrameInfo->stdPictureInfo.flags.is_reference;

    // FIXME: Move m_h264 to the h.264 specific encoder.
    PicInfoH264 pictureInfo{}; // temp picture
    memcpy(&pictureInfo, &pFrameInfo->stdPictureInfo, sizeof(pFrameInfo->stdPictureInfo));
    if (pictureInfo.flags.IdrPicFlag) {
        m_frameNumSyntax = 0;
    }
    pictureInfo.frame_num = m_frameNumSyntax & ((1 << (m_h264.m_spsInfo.log2_max_frame_num_minus4 + 4)) - 1);
    pictureInfo.PicOrderCnt = (encodeFrameInfo->picOrderCntVal) & ((1 << (m_h264.m_spsInfo.log2_max_pic_order_cnt_lsb_minus4 + 4)) - 1);
    pictureInfo.timeStamp = encodeFrameInfo->inputTimeStamp;
    if (isReference) {
        m_frameNumSyntax++;
    }

    bool success = m_dpbImagePool->GetAvailableImage(encodeFrameInfo->setupImageResource,
                                                     VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR);
    assert(success);
    if (!success) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    assert(encodeFrameInfo->setupImageResource != nullptr);
    VkVideoPictureResourceInfoKHR* setupImageViewPictureResource = encodeFrameInfo->setupImageResource->GetPictureResourceInfo();
    setupImageViewPictureResource->codedOffset = pFrameInfo->encodeInfo.srcPictureResource.codedOffset;
    setupImageViewPictureResource->codedExtent = pFrameInfo->encodeInfo.srcPictureResource.codedExtent;

    int8_t newDpbSlot = m_dpb264->DpbPictureStart(&pictureInfo,
                                                  &m_h264.m_spsInfo);
    assert(newDpbSlot >= 0);
    if (newDpbSlot < 0) {
	return VK_ERROR_INITIALIZATION_FAILED;
    }

    uint8_t refPicMarkingOpCount = 0;
    const uint32_t adaptiveRefPicManagementMode = 0; // FIXME
    if ((m_dpb264->GetNumRefFramesInDPB(0) >= m_h264.m_spsInfo.max_num_ref_frames) && isReference &&
        (adaptiveRefPicManagementMode > 0) && !pFrameInfo->stdPictureInfo.flags.IdrPicFlag) {
        // slh.flags.adaptive_ref_pic_marking_mode_flag = true;

        if (adaptiveRefPicManagementMode == 2) {
            POCBasedRefPicManagement(pFrameInfo->refPicMarkingEntry, refPicMarkingOpCount);
        } else if (adaptiveRefPicManagementMode == 1) {
            FrameNumBasedRefPicManagement(pFrameInfo->refPicMarkingEntry, refPicMarkingOpCount);
        }
    }

    // ref_pic_list_modification
    uint8_t refList0ModOpCount = 0;
    uint8_t refList1ModOpCount = 0;

    StdVideoEncodeH264ReferenceListsInfoFlags refMgmtFlags = StdVideoEncodeH264ReferenceListsInfoFlags();
    if ((m_dpb264->IsRefFramesCorrupted()) && ((picType == VkVideoGopStructure::FRAME_TYPE_P) || (picType == VkVideoGopStructure::FRAME_TYPE_B))) {
        SetupRefPicReorderingCommands(&pictureInfo, &pFrameInfo->stdSliceHeader, &refMgmtFlags, pFrameInfo->refList0ModOperations, refList0ModOpCount);
    }

    // Fill in the reference-related information for the current picture

    pFrameInfo->stdReferenceListsInfo.flags = refMgmtFlags;
    pFrameInfo->stdReferenceListsInfo.refPicMarkingOpCount = refPicMarkingOpCount;
    pFrameInfo->stdReferenceListsInfo.refList0ModOpCount = refList0ModOpCount;
    pFrameInfo->stdReferenceListsInfo.refList1ModOpCount = refList1ModOpCount;
    pFrameInfo->stdReferenceListsInfo.pRefList0ModOperations = pFrameInfo->refList0ModOperations;
    pFrameInfo->stdReferenceListsInfo.pRefList1ModOperations = pFrameInfo->refList1ModOperations;
    pFrameInfo->stdReferenceListsInfo.pRefPicMarkingOperations = pFrameInfo->refPicMarkingEntry;

    if ((m_h264.m_ppsInfo.num_ref_idx_l0_default_active_minus1 > 0) &&
            (picType == VkVideoGopStructure::FRAME_TYPE_B)) {
        // do not use multiple references for l0
        pFrameInfo->stdSliceHeader.flags.num_ref_idx_active_override_flag = true;
        pFrameInfo->stdReferenceListsInfo.num_ref_idx_l0_active_minus1 = 0;
    }

    NvVideoEncodeH264DpbSlotInfoLists<STD_VIDEO_H264_MAX_NUM_LIST_REF> refLists;
    m_dpb264->GetRefPicList(&pictureInfo, &refLists, &m_h264.m_spsInfo, &m_h264.m_ppsInfo, &pFrameInfo->stdSliceHeader, &pFrameInfo->stdReferenceListsInfo);
    assert(refLists.refPicListCount[0] <= 8);
    assert(refLists.refPicListCount[1] <= 8);

    memset(pFrameInfo->stdReferenceListsInfo.RefPicList0, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(pFrameInfo->stdReferenceListsInfo.RefPicList0));
    memset(pFrameInfo->stdReferenceListsInfo.RefPicList1, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(pFrameInfo->stdReferenceListsInfo.RefPicList1));

    memcpy(pFrameInfo->stdReferenceListsInfo.RefPicList0, refLists.refPicList[0], refLists.refPicListCount[0]);
    memcpy(pFrameInfo->stdReferenceListsInfo.RefPicList1, refLists.refPicList[1], refLists.refPicListCount[1]);

    pFrameInfo->stdReferenceListsInfo.num_ref_idx_l0_active_minus1 = refLists.refPicListCount[0] > 0 ? (uint8_t)(refLists.refPicListCount[0] - 1) : 0;
    pFrameInfo->stdReferenceListsInfo.num_ref_idx_l1_active_minus1 = refLists.refPicListCount[1] > 0 ? (uint8_t)(refLists.refPicListCount[1] - 1) : 0;

    pFrameInfo->stdSliceHeader.flags.num_ref_idx_active_override_flag = false;
    if (picType == VkVideoGopStructure::FRAME_TYPE_B) {
        pFrameInfo->stdSliceHeader.flags.num_ref_idx_active_override_flag =
            ((pFrameInfo->stdReferenceListsInfo.num_ref_idx_l0_active_minus1 != m_h264.m_ppsInfo.num_ref_idx_l0_default_active_minus1) ||
             (pFrameInfo->stdReferenceListsInfo.num_ref_idx_l1_active_minus1 != m_h264.m_ppsInfo.num_ref_idx_l1_default_active_minus1));
    } else if (picType == VkVideoGopStructure::FRAME_TYPE_P) {
        pFrameInfo->stdSliceHeader.flags.num_ref_idx_active_override_flag =
            (pFrameInfo->stdReferenceListsInfo.num_ref_idx_l0_active_minus1 != m_h264.m_ppsInfo.num_ref_idx_l0_default_active_minus1);
    }

    // Update the frame_num and PicOrderCnt picture parameters, if changed.
    pFrameInfo->stdPictureInfo.frame_num = m_dpb264->GetUpdatedFrameNumAndPicOrderCnt(pFrameInfo->stdPictureInfo.PicOrderCnt);

    // We need the reference slot for the target picture
    // Update the DPB
    int8_t targetDpbSlot = m_dpb264->DpbPictureEnd(&pictureInfo, encodeFrameInfo->setupImageResource,
                                                   &m_h264.m_spsInfo, &pFrameInfo->stdSliceHeader,
                                                   &pFrameInfo->stdReferenceListsInfo, MAX_MEM_MGMNT_CTRL_OPS_COMMANDS);
    if (targetDpbSlot >= VkEncDpbH264::MAX_DPB_SLOTS) {
        targetDpbSlot = static_cast<int8_t>((encodeFrameInfo->setupImageResource!=nullptr) + refLists.refPicListCount[0] + refLists.refPicListCount[1] + 1);
    }
    if (isReference) {
        assert(targetDpbSlot >= 0);
    }

    if ((picType == VkVideoGopStructure::FRAME_TYPE_P) || (picType == VkVideoGopStructure::FRAME_TYPE_B)) {
        pFrameInfo->stdPictureInfo.pRefLists = &pFrameInfo->stdReferenceListsInfo;
    }

    uint32_t numReferenceSlots = 0;
    assert(pFrameInfo->numDpbImageResources == 0);
    if (encodeFrameInfo->setupImageResource != nullptr) {

        assert(setupImageViewPictureResource);
        pFrameInfo->referenceSlotsInfo[numReferenceSlots] = { VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
                                                              pFrameInfo->stdDpbSlotInfo, targetDpbSlot, setupImageViewPictureResource };

        pFrameInfo->setupReferenceSlotInfo = pFrameInfo->referenceSlotsInfo[numReferenceSlots];
        pFrameInfo->encodeInfo.pSetupReferenceSlot = &pFrameInfo->setupReferenceSlotInfo;

        numReferenceSlots++;

        assert(numReferenceSlots <= ARRAYSIZE(pFrameInfo->referenceSlotsInfo));
    } else {
        pFrameInfo->encodeInfo.pSetupReferenceSlot = nullptr;
    }
    pFrameInfo->numDpbImageResources = numReferenceSlots;

    // It's not entirely correct to have two separate loops below, one for L0
    // and the other for L1. In each loop, elements are added to referenceSlotsInfo[]
    // without checking for duplication. Duplication could occur if the same
    // picture appears in both L0 and L1; AFAIK, we don't have a situation
    // today like that so the two loops work fine.
    // TODO: create a set out of the ref lists and then iterate over that to
    // build referenceSlotsInfo[].

    for (uint32_t listNum = 0; listNum < 2; listNum++) {

        for (uint32_t i = 0; i < refLists.refPicListCount[listNum]; i++) {

            int8_t slotIndex = refLists.refPicList[listNum][i];
            bool refPicAvailable = m_dpb264->GetRefPicture(slotIndex, pFrameInfo->dpbImageResources[numReferenceSlots]);
            assert(refPicAvailable);
            if (!refPicAvailable) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            m_dpb264->FillStdReferenceInfo(slotIndex, &pFrameInfo->stdReferenceInfo[numReferenceSlots]);

            pFrameInfo->stdDpbSlotInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
            pFrameInfo->stdDpbSlotInfo[numReferenceSlots].pStdReferenceInfo = &pFrameInfo->stdReferenceInfo[numReferenceSlots];

            pFrameInfo->referenceSlotsInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].pNext = &pFrameInfo->stdDpbSlotInfo[numReferenceSlots];
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].slotIndex = slotIndex;
            pFrameInfo->referenceSlotsInfo[numReferenceSlots].pPictureResource =
                        pFrameInfo->dpbImageResources[numReferenceSlots]->GetPictureResourceInfo();

            numReferenceSlots++;
            assert(numReferenceSlots <= ARRAYSIZE(pFrameInfo->referenceSlotsInfo));
        }
        pFrameInfo->numDpbImageResources = numReferenceSlots;
    }

    pFrameInfo->encodeInfo.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    //pFrameInfo->encodeInfo.flags = 0;
    // If the current picture is going to be a reference frame, the first
    // entry in the refSlots array contains information about the picture
    // resource associated with this frame. This entry should not be
    // provided in the list of reference resources for the current picture,
    // so skip refSlots[0].
    pFrameInfo->encodeInfo.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    pFrameInfo->encodeInfo.referenceSlotCount = numReferenceSlots - 1;
    pFrameInfo->encodeInfo.pReferenceSlots = pFrameInfo->referenceSlotsInfo + 1;

    if ((picType == VkVideoGopStructure::FRAME_TYPE_P) || (picType == VkVideoGopStructure::FRAME_TYPE_B)) {
        uint64_t timeStamp = m_dpb264->GetPictureTimestamp(pFrameInfo->referenceSlotsInfo[0].slotIndex);
        m_dpb264->SetCurRefFrameTimeStamp(timeStamp);
    } else {
        m_dpb264->SetCurRefFrameTimeStamp(0);
    }

    // since encodeInfo.pReferenceSlots points to the address of the next element (+1), it's safe to set it one to -1
    // this is needed to explicity mark the unused element in BeginInfo for vkCmdBeginVideoCodingKHR() as inactive
    pFrameInfo->referenceSlotsInfo[0].slotIndex = -1;

    assert(m_dpb264->GetNumRefFramesInDPB(0) <= m_h264.m_spsInfo.max_num_ref_frames);

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

    encodeFrameInfo->picOrderCntVal = 2 * encodeFrameInfo->gopPosition.inputOrder;

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
    encodeFrameInfo->encodeInfo.srcPictureResource.baseArrayLayer   = pSrcPictureResource->baseArrayLayer;

    pFrameInfo->qualityLevel = m_encoderConfig->qualityLevel;
    pFrameInfo->videoSession = m_videoSession;
    pFrameInfo->videoSessionParameters = m_videoSessionParameters;

    pFrameInfo->stdPictureInfo.seq_parameter_set_id = m_h264.m_spsInfo.seq_parameter_set_id;
    pFrameInfo->stdPictureInfo.pic_parameter_set_id = m_h264.m_ppsInfo.pic_parameter_set_id;

    StdVideoH264PictureType stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_INVALID;
    switch (encodeFrameInfo->gopPosition.pictureType) {
        case VkVideoGopStructure::FRAME_TYPE_IDR:
        case VkVideoGopStructure::FRAME_TYPE_INTRA_REFRESH:
            pFrameInfo->stdSliceHeader.slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_IDR;
            break;
        case VkVideoGopStructure::FRAME_TYPE_I:
            pFrameInfo->stdSliceHeader.slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_I;
            break;
        case VkVideoGopStructure::FRAME_TYPE_P:
            pFrameInfo->stdSliceHeader.slice_type = STD_VIDEO_H264_SLICE_TYPE_P;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_P;
            break;
        case VkVideoGopStructure::FRAME_TYPE_B:
            pFrameInfo->stdSliceHeader.slice_type = STD_VIDEO_H264_SLICE_TYPE_B;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_B;
            break;
        default:
            assert(!"Invalid value");
            break;
    }

    pFrameInfo->stdPictureInfo.flags.IdrPicFlag = isIdr;
    pFrameInfo->stdPictureInfo.flags.is_reference = isReference;
    pFrameInfo->stdPictureInfo.flags.long_term_reference_flag = pFrameInfo->islongTermReference;
    pFrameInfo->stdPictureInfo.primary_pic_type = stdPictureType;
    pFrameInfo->stdPictureInfo.flags.no_output_of_prior_pics_flag = false;        // TODO: replace this by a check for the corresponding slh flag
    pFrameInfo->stdPictureInfo.flags.adaptive_ref_pic_marking_mode_flag = false;  // TODO: replace this by a check for the corresponding slh flag

    pFrameInfo->stdSliceHeader.disable_deblocking_filter_idc = m_encoderConfig->disable_deblocking_filter_idc;
     // FIXME: set cabac_init_idc based on a query
     pFrameInfo->stdSliceHeader.cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0;

    if (isIdr) {
        pFrameInfo->stdPictureInfo.idr_pic_id = m_IDRPicId & 1;
        m_IDRPicId++;
    }

    if (isIdr && (encodeFrameInfo->frameEncodeInputOrderNum == 0)) {
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

    if (m_encoderConfig->enableQpMap) {
        ProcessQpMap(encodeFrameInfo);
    }

    // NOTE: dstBuffer resource acquisition can be deferred at the last moment before submit
    VkDeviceSize size = GetBitstreamBuffer(encodeFrameInfo->outputBitstreamBuffer);
    assert((size > 0) && (encodeFrameInfo->outputBitstreamBuffer != nullptr));
    if ((size == 0) || (encodeFrameInfo->outputBitstreamBuffer == nullptr)) {
	return VK_ERROR_INITIALIZATION_FAILED;
    }
    pFrameInfo->encodeInfo.dstBuffer = encodeFrameInfo->outputBitstreamBuffer->GetBuffer();

    // For the actual (VCL) data, specify its insertion starting from the
    // provided offset into the bitstream buffer.
    pFrameInfo->encodeInfo.dstBufferOffset = 0;

    if (m_rateControlInfo.rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
        switch (encodeFrameInfo->gopPosition.pictureType) {
            case VkVideoGopStructure::FRAME_TYPE_IDR:
            case VkVideoGopStructure::FRAME_TYPE_I:
                pFrameInfo->naluSliceInfo.constantQp = encodeFrameInfo->constQp.qpIntra;
                break;
            case VkVideoGopStructure::FRAME_TYPE_P:
                pFrameInfo->naluSliceInfo.constantQp = encodeFrameInfo->constQp.qpInterP;
                break;
            case VkVideoGopStructure::FRAME_TYPE_B:
                pFrameInfo->naluSliceInfo.constantQp = encodeFrameInfo->constQp.qpInterB;
                break;
            default:
                assert(!"Invalid picture type");
                break;
        }
    }

    if (m_sendControlCmd == true) {
        HandleCtrlCmd(encodeFrameInfo);
    }

    EnqueueFrame(encodeFrameInfo, isIdr, isReference);

    return VK_SUCCESS;
}

VkResult VkVideoEncoderH264::HandleCtrlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    VkVideoEncodeFrameInfoH264* pFrameInfo = GetEncodeFrameInfoH264(encodeFrameInfo);

    // Save the RateControlCmd request.
    const bool sendRateControlCmd = m_sendRateControlCmd;
    // Call the base class first to cover the bases
    VkVideoEncoder::HandleCtrlCmd(encodeFrameInfo);

    // Fill-n the codec-specific parts next
    if (sendRateControlCmd) {

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
    }

    return VK_SUCCESS;
}
