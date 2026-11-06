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
 * The H.264 encode planner. Section numbers are those of ITU-T H.264. Vulkan encodes frames, so
 * there are no fields and no complementary field pairs; the library writes frame_num itself, so
 * there are no gaps in frame_num.
 */
#include "vvs_dpb_internal.h"

/* 8.2.4.1: FrameNumWrap of a short-term reference picture, for a picture with this frame_num. */
static int32_t FrameNumWrap(uint32_t frameNum, uint32_t currentFrameNum, uint32_t maxFrameNum) {
    return (frameNum > currentFrameNum) ? (int32_t)frameNum - (int32_t)maxFrameNum : (int32_t)frameNum;
}

static bool IsLongTerm(const VvsDpbSlotState* pSlot) {
    return (pSlot->flags & (VVS_DPB_SLOT_REFERENCE | VVS_DPB_SLOT_LONG_TERM)) == (VVS_DPB_SLOT_REFERENCE | VVS_DPB_SLOT_LONG_TERM);
}

/* Appends the long-term reference frames by ascending LongTermPicNum, which is LongTermFrameIdx for a
 * frame (8-30). */
static uint32_t AppendLongTerm(const VvsDpbState* pState, uint32_t* pList, uint32_t count) {
    uint32_t slots[VVS_DPB_MAX_SLOTS];
    int32_t keys[VVS_DPB_MAX_SLOTS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        if (IsLongTerm(&pState->slots[i])) {
            slots[n] = i;
            keys[n] = pState->slots[i].codecValue;
            n++;
        }
    }
    vvsDpbSortSlots(slots, keys, n, false);
    for (uint32_t i = 0; i < n; i++) {
        pList[count++] = slots[i];
    }
    return count;
}

/* 8.2.4.2.1: the initial list 0 of a P frame, short-term frames by descending PicNum, then long-term
 * frames. */
static uint32_t InitialListP(const VvsDpbState* pState, uint32_t frameNum, uint32_t maxFrameNum, uint32_t* pList) {
    int32_t keys[VVS_DPB_MAX_SLOTS];
    uint32_t count = 0;
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        if (vvsDpbIsShortTerm(&pState->slots[i])) {
            pList[count] = i;
            keys[count] = FrameNumWrap(pState->slots[i].stdReferenceInfo.h264.FrameNum, frameNum, maxFrameNum);
            count++;
        }
    }
    vvsDpbSortSlots(pList, keys, count, true);
    return AppendLongTerm(pState, pList, count);
}

/* 8.2.4.2.3: the initial list 0 (list1 false) or list 1 of a B frame: the short-term frames before the
 * picture by descending PicOrderCnt and those after it by ascending PicOrderCnt, list 0 the ones
 * before first and list 1 the ones after; then the long-term frames. */
static uint32_t InitialListB(const VvsDpbState* pState, int32_t picOrderCnt, bool list1, uint32_t* pList) {
    uint32_t before[VVS_DPB_MAX_SLOTS];
    uint32_t after[VVS_DPB_MAX_SLOTS];
    int32_t beforeKeys[VVS_DPB_MAX_SLOTS];
    int32_t afterKeys[VVS_DPB_MAX_SLOTS];
    uint32_t beforeCount = 0;
    uint32_t afterCount = 0;
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        if (!vvsDpbIsShortTerm(&pState->slots[i])) {
            continue;
        }
        const int32_t poc = pState->slots[i].stdReferenceInfo.h264.PicOrderCnt;
        if (poc <= picOrderCnt) {
            before[beforeCount] = i;
            beforeKeys[beforeCount++] = poc;
        } else {
            after[afterCount] = i;
            afterKeys[afterCount++] = poc;
        }
    }
    vvsDpbSortSlots(before, beforeKeys, beforeCount, true);
    vvsDpbSortSlots(after, afterKeys, afterCount, false);
    uint32_t count = 0;
    const uint32_t* pFirst = list1 ? after : before;
    const uint32_t firstCount = list1 ? afterCount : beforeCount;
    const uint32_t* pSecond = list1 ? before : after;
    const uint32_t secondCount = list1 ? beforeCount : afterCount;
    for (uint32_t i = 0; i < firstCount; i++) {
        pList[count++] = pFirst[i];
    }
    for (uint32_t i = 0; i < secondCount; i++) {
        pList[count++] = pSecond[i];
    }
    return AppendLongTerm(pState, pList, count);
}

/* 8.2.5.3: when the references fill max_num_ref_frames, the short-term reference with the smallest
 * FrameNumWrap stops being one. */
static void SlidingWindow(VvsDpbState* pState, const VvsDpbH264Sequence* pSequence, uint32_t frameNum, uint32_t maxFrameNum) {
    uint32_t shortTermCount = 0;
    uint32_t longTermCount = 0;
    uint32_t oldest = VVS_DPB_NO_SLOT;
    int32_t oldestFrameNumWrap = INT32_MAX;
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        const VvsDpbSlotState* pSlot = &pState->slots[i];
        if (IsLongTerm(pSlot)) {
            longTermCount++;
        } else if (vvsDpbIsShortTerm(pSlot)) {
            shortTermCount++;
            const int32_t frameNumWrap = FrameNumWrap(pSlot->stdReferenceInfo.h264.FrameNum, frameNum, maxFrameNum);
            if (frameNumWrap < oldestFrameNumWrap) {
                oldestFrameNumWrap = frameNumWrap;
                oldest = i;
            }
        }
    }
    if (((shortTermCount + longTermCount) >= pSequence->maxNumRefFrames) && (oldest != VVS_DPB_NO_SLOT)) {
        pState->slots[oldest].flags &= ~VVS_DPB_SLOT_REFERENCE;
    }
}

VVS_DPB_API VkResult vvsDpbH264Sequence(VvsDpb dpb, const VvsDpbH264SequenceInfo* pSequenceInfo,
                                        VvsDpbSequenceRequirements* pRequirements) {
    if ((dpb == VK_NULL_HANDLE) || (dpb->operation != VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) || (pSequenceInfo == NULL) ||
        (pSequenceInfo->pStdSps == NULL) || (pSequenceInfo->pStdPps == NULL)) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    const StdVideoH264SequenceParameterSet* pSps = pSequenceInfo->pStdSps;
    const StdVideoH264PictureParameterSet* pPps = pSequenceInfo->pStdPps;
    if (((pSps->pic_order_cnt_type != STD_VIDEO_H264_POC_TYPE_0) && (pSps->pic_order_cnt_type != STD_VIDEO_H264_POC_TYPE_2)) ||
        (pSps->log2_max_frame_num_minus4 > 12U) || (pSps->log2_max_pic_order_cnt_lsb_minus4 > 12U)) {
        return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
    }
    if (vvsDpbGopHasPatterns(pSequenceInfo->pGopInfo)) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    const uint32_t references = vvsDpbMax(pSps->max_num_ref_frames, 1U);
    const uint32_t list0Count = pPps->num_ref_idx_l0_default_active_minus1 + 1U;
    const uint32_t list1Count = pPps->num_ref_idx_l1_default_active_minus1 + 1U;
    VvsDpbSequenceRequirements requirements;
    requirements.maxReferencePictures = references;
    /* The references stay while the picture is encoded, so the picture takes one more slot, and a
     * rewind of one picture takes none: the picture never takes the slot of a reference. */
    requirements.maxDpbSlots = references + 1U + ((dpb->rewindDepth > 0U) ? dpb->rewindDepth - 1U : 0U);
    /* A P picture references list 0; a B picture one list 0 entry and one or list 1's. */
    requirements.maxActiveReferencePictures =
        vvsDpbMax(vvsDpbMin(references, list0Count), vvsDpbMin(references, 1U + ((list0Count > 1U) ? 1U : list1Count)));
    if ((references > VVS_DPB_MAX_ACTIVE_REFERENCES) || (requirements.maxDpbSlots > VVS_DPB_MAX_SLOTS) ||
        ((dpb->boundSlots != 0U) && (requirements.maxDpbSlots > dpb->boundSlots)) ||
        ((dpb->boundActiveReferences != 0U) && (requirements.maxActiveReferencePictures > dpb->boundActiveReferences))) {
        return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
    }

    VvsDpbH264Sequence* pSequence = &dpb->sequence.h264;
    pSequence->maxNumRefFrames = references;
    pSequence->log2MaxFrameNum = pSps->log2_max_frame_num_minus4 + 4U;
    pSequence->log2MaxPicOrderCntLsb = pSps->log2_max_pic_order_cnt_lsb_minus4 + 4U;
    pSequence->picOrderCntType = (uint32_t)pSps->pic_order_cnt_type;
    pSequence->numRefIdxL0DefaultActive = list0Count;
    pSequence->numRefIdxL1DefaultActive = list1Count;
    vvsDpbSequenceCommit(dpb, &requirements, pSequenceInfo->pGopInfo);
    if (pRequirements != NULL) {
        *pRequirements = requirements;
    }
    return VK_SUCCESS;
}

VVS_DPB_API VkResult vvsDpbEncodeH264Picture(VvsDpb dpb, const VvsDpbH264EncodePictureInfo* pPictureInfo,
                                             VvsDpbPictureResult* pResult) {
    if ((pPictureInfo == NULL) || (pPictureInfo->pStdPictureInfo == NULL) || (pPictureInfo->pSyntax == NULL) || (pResult == NULL)) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    VkResult result = vvsDpbPictureBegin(dpb, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, pPictureInfo->dirtyIntraRefreshRegions);
    if (result != VK_SUCCESS) {
        return result;
    }
    StdVideoEncodeH264PictureInfo* pStd = pPictureInfo->pStdPictureInfo;
    const VvsDpbH264Sequence* pSequence = &dpb->sequence.h264;
    VvsDpbState* pState = &dpb->state;
    VvsDpbH264State* pH264 = &pState->codec.h264;
    const bool idr = pStd->flags.IdrPicFlag != 0;
    if (pState->keyPictureNeeded && !idr) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    const bool reference = idr || (pStd->flags.is_reference != 0);
    const bool longTerm = idr && (pStd->flags.long_term_reference_flag != 0);
    const uint32_t maxFrameNum = 1U << pSequence->log2MaxFrameNum;

    /* frame_num (7.4.3): the reference pictures since the IDR picture. */
    if (idr) {
        pH264->frameNumCount = 0;
    }
    const uint32_t frameNum = pH264->frameNumCount & (maxFrameNum - 1U);
    if (reference) {
        pH264->frameNumCount++;
    }

    /* The order count the decoding process derives (8.2.1). */
    int32_t picOrderCnt = 0;
    if (pSequence->picOrderCntType == (uint32_t)STD_VIDEO_H264_POC_TYPE_0) {
        const int32_t maxPicOrderCntLsb = (int32_t)(1U << pSequence->log2MaxPicOrderCntLsb);
        const int32_t lsb = (int32_t)((uint32_t)pStd->PicOrderCnt & (uint32_t)(maxPicOrderCntLsb - 1));
        if (idr) {
            pH264->prevPicOrderCntMsb = 0;
            pH264->prevPicOrderCntLsb = 0;
        }
        int32_t msb = pH264->prevPicOrderCntMsb; /* (8-3) */
        if ((lsb < pH264->prevPicOrderCntLsb) && ((pH264->prevPicOrderCntLsb - lsb) >= (maxPicOrderCntLsb / 2))) {
            msb = pH264->prevPicOrderCntMsb + maxPicOrderCntLsb;
        } else if ((lsb > pH264->prevPicOrderCntLsb) && ((lsb - pH264->prevPicOrderCntLsb) > (maxPicOrderCntLsb / 2))) {
            msb = pH264->prevPicOrderCntMsb - maxPicOrderCntLsb;
        }
        picOrderCnt = msb + lsb;
        if (reference) {
            pH264->prevPicOrderCntMsb = msb;
            pH264->prevPicOrderCntLsb = lsb;
        }
    } else {
        int32_t frameNumOffset = pH264->prevFrameNumOffset; /* (8-11) */
        if (idr) {
            frameNumOffset = 0;
        } else if (pH264->prevFrameNum > frameNum) {
            frameNumOffset += (int32_t)maxFrameNum;
        }
        if (!idr) { /* (8-12) */
            picOrderCnt = 2 * (frameNumOffset + (int32_t)frameNum) - (reference ? 0 : 1);
        }
        pH264->prevFrameNumOffset = frameNumOffset;
        pH264->prevFrameNum = frameNum;
    }

    /* The initial lists (8.2.4.2) of the references before the picture, at today's counts. */
    uint32_t list0[VVS_DPB_MAX_SLOTS];
    uint32_t list1[VVS_DPB_MAX_SLOTS];
    uint32_t list0Count = 0;
    uint32_t list1Count = 0;
    if (pStd->primary_pic_type == STD_VIDEO_H264_PICTURE_TYPE_P) {
        list0Count = vvsDpbMin(InitialListP(pState, frameNum, maxFrameNum, list0), pSequence->numRefIdxL0DefaultActive);
    } else if (pStd->primary_pic_type == STD_VIDEO_H264_PICTURE_TYPE_B) {
        const uint32_t initial0 = InitialListB(pState, picOrderCnt, false, list0);
        const uint32_t initial1 = InitialListB(pState, picOrderCnt, true, list1);
        if ((initial1 > 1U) && (initial0 == initial1) && (memcmp(list0, list1, initial0 * sizeof(list0[0])) == 0)) {
            const uint32_t first = list1[0];
            list1[0] = list1[1];
            list1[1] = first;
        }
        list0Count = vvsDpbMin(initial0, 1U);
        list1Count = vvsDpbMin(initial1, (pSequence->numRefIdxL0DefaultActive > 1U) ? 1U : pSequence->numRefIdxL1DefaultActive);
    }

    /* The setup slot. The references stay references while the picture is encoded (8.2.5 marks after
     * it), so it takes a slot that holds none; an IDR picture drops them first. */
    if (idr) {
        for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
            pState->slots[i].flags &= ~(VVS_DPB_SLOT_REFERENCE | VVS_DPB_SLOT_LONG_TERM);
        }
        pH264->maxLongTermFrameIdx = longTerm ? 0 : -1;
    }
    const bool setup = reference || ((dpb->flags & VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT) != 0);
    uint32_t setupSlot = VVS_DPB_NO_SLOT;
    if (setup) {
        setupSlot = vvsDpbChooseSetupSlot(dpb, 0);
        if (setupSlot == VVS_DPB_NO_SLOT) {
            vvsDpbPictureAbort(dpb);
            return VK_ERROR_TOO_MANY_OBJECTS;
        }
        result = vvsDpbAcquireResource(dpb, setupSlot, pPictureInfo->pPictureInfo);
        if (result != VK_SUCCESS) {
            vvsDpbPictureAbort(dpb);
            return result;
        }
    }

    /* Nothing fails from here on. The marking of a reference picture that is not an IDR picture. */
    if (reference && !idr) {
        SlidingWindow(pState, pSequence, frameNum, maxFrameNum);
    }
    if (setup) {
        VvsDpbSlotState* pSlot = &pState->slots[setupSlot];
        pSlot->flags = (reference ? VVS_DPB_SLOT_REFERENCE : 0U) | (longTerm ? VVS_DPB_SLOT_LONG_TERM : 0U);
        if (pPictureInfo->pPictureInfo != NULL) {
            pSlot->pictureInfo = *pPictureInfo->pPictureInfo;
            pSlot->flags |= VVS_DPB_SLOT_PICTURE_INFO;
        } else {
            memset(&pSlot->pictureInfo, 0, sizeof(pSlot->pictureInfo));
        }
        pSlot->dirtyIntraRefreshRegions = vvsDpbDirtyRegions(pPictureInfo->dirtyIntraRefreshRegions);
        pSlot->codecValue = longTerm ? 0 : -1;
        StdVideoEncodeH264ReferenceInfo* pReference = &pSlot->stdReferenceInfo.h264;
        memset(pReference, 0, sizeof(*pReference));
        pReference->flags.used_for_long_term_reference = longTerm ? 1U : 0U;
        pReference->primary_pic_type = pStd->primary_pic_type;
        pReference->FrameNum = frameNum;
        pReference->PicOrderCnt = picOrderCnt;
        pReference->long_term_pic_num = 0;
        pReference->long_term_frame_idx = longTerm ? 0U : 0xFFFFU;
        pReference->temporal_id = pStd->temporal_id;
    }
    if (idr) {
        pState->keyPictureNeeded = 0;
    }

    /* The syntax. */
    VvsDpbH264EncodeSyntax* pSyntax = pPictureInfo->pSyntax;
    StdVideoEncodeH264ReferenceListsInfo* pLists = &pSyntax->refLists;
    memset(pLists, 0, sizeof(*pLists));
    memset(pLists->RefPicList0, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(pLists->RefPicList0));
    memset(pLists->RefPicList1, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(pLists->RefPicList1));
    for (uint32_t i = 0; i < list0Count; i++) {
        pLists->RefPicList0[i] = (uint8_t)list0[i];
    }
    for (uint32_t i = 0; i < list1Count; i++) {
        pLists->RefPicList1[i] = (uint8_t)list1[i];
    }
    pLists->num_ref_idx_l0_active_minus1 = (uint8_t)((list0Count > 0U) ? list0Count - 1U : 0U);
    pLists->num_ref_idx_l1_active_minus1 = (uint8_t)((list1Count > 0U) ? list1Count - 1U : 0U);
    pLists->pRefList0ModOperations = pSyntax->refList0ModOperations;
    pLists->pRefList1ModOperations = pSyntax->refList1ModOperations;
    pLists->pRefPicMarkingOperations = pSyntax->refPicMarkingOperations;
    pStd->frame_num = frameNum;
    pStd->PicOrderCnt = picOrderCnt;
    pStd->flags.adaptive_ref_pic_marking_mode_flag = 0;
    pStd->pRefLists = pLists;

    /* The slots: list 0's references, then list 1's. */
    uint32_t references[2U * VVS_DPB_MAX_SLOTS];
    uint32_t referenceCount = 0;
    for (uint32_t i = 0; i < list0Count; i++) {
        references[referenceCount++] = list0[i];
    }
    for (uint32_t i = 0; i < list1Count; i++) {
        references[referenceCount++] = list1[i];
    }
    vvsDpbResultSetReferences(dpb, references, referenceCount, pResult);
    vvsDpbPictureCommit(dpb, setupSlot, pResult);
    return VK_SUCCESS;
}
