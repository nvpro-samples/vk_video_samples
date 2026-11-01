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
 * The H.265 encode planner. Section numbers are those of ITU-T H.265. The planner keeps short-term
 * reference pictures only; the client gives PicOrderCntVal (8.3.1).
 */
#include "vvs_dpb_internal.h"

/* The most pictures a picture's RefPicSetStCurrBefore, RefPicSetStCurrAfter and RefPicSetLtCurr
 * hold together: NumPicTotalCurr (7.4.7.2). */
#define VVS_DPB_H265_MAX_TOTAL_CURR 8U

static int32_t SlotPoc(const VvsDpbState* pState, uint32_t slot) {
    return pState->slots[slot].stdReferenceInfo.h265.PicOrderCntVal;
}

static bool Bit(uint32_t bits, uint32_t i) { return (i < 16U) && (((bits >> i) & 1U) != 0U); }

/* Appends a picture to one half of a derived set; false when the half is full or the delta does not
 * fit. */
static bool Append(int16_t* pDeltas, uint16_t* pUsed, uint32_t* pCount, int32_t deltaPoc, bool used) {
    if ((*pCount >= STD_VIDEO_H265_MAX_DPB_SIZE) || (deltaPoc < INT16_MIN) || (deltaPoc > INT16_MAX)) {
        return false;
    }
    pDeltas[*pCount] = (int16_t)deltaPoc;
    if (used) {
        *pUsed = (uint16_t)(*pUsed | (1U << *pCount));
    }
    (*pCount)++;
    return true;
}

/* 7.4.8: the set an SPS's st_ref_pic_set(stRpsIdx) describes. False for a set that is not valid. */
static bool DeriveShortTermSet(const StdVideoH265ShortTermRefPicSet* pSets, const VvsDpbH265ShortTermSet* pDerived,
                               uint32_t stRpsIdx, VvsDpbH265ShortTermSet* pSet) {
    const StdVideoH265ShortTermRefPicSet* pRps = &pSets[stRpsIdx];
    memset(pSet, 0, sizeof(*pSet));
    uint32_t negative = 0;
    uint32_t positive = 0;
    bool valid = true;
    if ((stRpsIdx == 0U) || (pRps->flags.inter_ref_pic_set_prediction_flag == 0U)) {
        int32_t poc = 0;
        for (uint32_t i = 0; valid && (i < pRps->num_negative_pics); i++) {
            poc -= (int32_t)pRps->delta_poc_s0_minus1[i] + 1;
            valid = Append(pSet->deltaPocS0, &pSet->usedS0, &negative, poc, Bit(pRps->used_by_curr_pic_s0_flag, i));
        }
        poc = 0;
        for (uint32_t i = 0; valid && (i < pRps->num_positive_pics); i++) {
            poc += (int32_t)pRps->delta_poc_s1_minus1[i] + 1;
            valid = Append(pSet->deltaPocS1, &pSet->usedS1, &positive, poc, Bit(pRps->used_by_curr_pic_s1_flag, i));
        }
    } else {
        /* Predicted from the set before it (an SPS has no delta_idx_minus1): (7-61) and (7-62). A
         * use_delta_flag that is not present, where used_by_curr_pic_flag is 1, is 1. */
        const VvsDpbH265ShortTermSet* pRef = &pDerived[stRpsIdx - 1U];
        const uint32_t numDeltaPocs = (uint32_t)pRef->numNegativePics + pRef->numPositivePics;
        const int32_t deltaRps = (1 - 2 * (int32_t)pRps->flags.delta_rps_sign) * ((int32_t)pRps->abs_delta_rps_minus1 + 1);
        const uint32_t used = pRps->used_by_curr_pic_flag;
        const uint32_t useDelta = (uint32_t)pRps->use_delta_flag | used;
        for (uint32_t k = pRef->numPositivePics; valid && (k > 0U); k--) {
            const uint32_t j = pRef->numNegativePics + k - 1U;
            const int32_t dPoc = pRef->deltaPocS1[k - 1U] + deltaRps;
            if ((dPoc < 0) && Bit(useDelta, j)) {
                valid = Append(pSet->deltaPocS0, &pSet->usedS0, &negative, dPoc, Bit(used, j));
            }
        }
        if (valid && (deltaRps < 0) && Bit(useDelta, numDeltaPocs)) {
            valid = Append(pSet->deltaPocS0, &pSet->usedS0, &negative, deltaRps, Bit(used, numDeltaPocs));
        }
        for (uint32_t j = 0; valid && (j < pRef->numNegativePics); j++) {
            const int32_t dPoc = pRef->deltaPocS0[j] + deltaRps;
            if ((dPoc < 0) && Bit(useDelta, j)) {
                valid = Append(pSet->deltaPocS0, &pSet->usedS0, &negative, dPoc, Bit(used, j));
            }
        }
        for (uint32_t k = pRef->numNegativePics; valid && (k > 0U); k--) {
            const uint32_t j = k - 1U;
            const int32_t dPoc = pRef->deltaPocS0[j] + deltaRps;
            if ((dPoc > 0) && Bit(useDelta, j)) {
                valid = Append(pSet->deltaPocS1, &pSet->usedS1, &positive, dPoc, Bit(used, j));
            }
        }
        if (valid && (deltaRps > 0) && Bit(useDelta, numDeltaPocs)) {
            valid = Append(pSet->deltaPocS1, &pSet->usedS1, &positive, deltaRps, Bit(used, numDeltaPocs));
        }
        for (uint32_t k = 0; valid && (k < pRef->numPositivePics); k++) {
            const uint32_t j = pRef->numNegativePics + k;
            const int32_t dPoc = pRef->deltaPocS1[k] + deltaRps;
            if ((dPoc > 0) && Bit(useDelta, j)) {
                valid = Append(pSet->deltaPocS1, &pSet->usedS1, &positive, dPoc, Bit(used, j));
            }
        }
    }
    pSet->numNegativePics = (uint8_t)negative;
    pSet->numPositivePics = (uint8_t)positive;
    return valid && ((negative + positive) <= STD_VIDEO_H265_MAX_DPB_SIZE);
}

static bool SameShortTermSet(const VvsDpbH265ShortTermSet* pA, const VvsDpbH265ShortTermSet* pB) {
    if ((pA->numNegativePics != pB->numNegativePics) || (pA->numPositivePics != pB->numPositivePics) ||
        (pA->usedS0 != pB->usedS0) || (pA->usedS1 != pB->usedS1)) {
        return false;
    }
    for (uint32_t i = 0; i < pA->numNegativePics; i++) {
        if (pA->deltaPocS0[i] != pB->deltaPocS0[i]) {
            return false;
        }
    }
    for (uint32_t i = 0; i < pA->numPositivePics; i++) {
        if (pA->deltaPocS1[i] != pB->deltaPocS1[i]) {
            return false;
        }
    }
    return true;
}

/* Fills a list from the pictures the current picture uses, as 8.3.4 builds RefPicListTemp0 (list 1
 * false) and RefPicListTemp1: the first group, then the second, repeated until the list is full. */
static void FillList(const uint32_t* pFirst, uint32_t firstCount, const uint32_t* pSecond, uint32_t secondCount, uint32_t count,
                     uint8_t* pList) {
    uint32_t n = 0;
    while (n < count) {
        for (uint32_t i = 0; (i < firstCount) && (n < count); i++) {
            pList[n++] = (uint8_t)pFirst[i];
        }
        for (uint32_t i = 0; (i < secondCount) && (n < count); i++) {
            pList[n++] = (uint8_t)pSecond[i];
        }
    }
}

/* The references a B picture uses before and after it, of at most VVS_DPB_H265_MAX_TOTAL_CURR: as
 * many after it as list 0's default count leaves, at least one; the rest before it. */
static uint32_t UsedAfterB(uint32_t list0Count) {
    return (list0Count < VVS_DPB_H265_MAX_TOTAL_CURR) ? VVS_DPB_H265_MAX_TOTAL_CURR - list0Count : 1U;
}

VVS_DPB_API VkResult vvsDpbH265Sequence(VvsDpb dpb, const VvsDpbH265SequenceInfo* pSequenceInfo,
                                        VvsDpbSequenceRequirements* pRequirements) {
    if ((dpb == VK_NULL_HANDLE) || (dpb->operation != VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR) || (pSequenceInfo == NULL) ||
        (pSequenceInfo->pStdSps == NULL) || (pSequenceInfo->pStdPps == NULL) || (pSequenceInfo->pStdSps->pDecPicBufMgr == NULL)) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    const StdVideoH265SequenceParameterSet* pSps = pSequenceInfo->pStdSps;
    const StdVideoH265PictureParameterSet* pPps = pSequenceInfo->pStdPps;
    if ((pSps->sps_max_sub_layers_minus1 >= STD_VIDEO_H265_SUBLAYERS_LIST_SIZE) ||
        (pSps->num_short_term_ref_pic_sets > STD_VIDEO_H265_MAX_SHORT_TERM_REF_PIC_SETS) ||
        ((pSps->num_short_term_ref_pic_sets > 0U) && (pSps->pShortTermRefPicSet == NULL))) {
        return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
    }
    if (vvsDpbGopHasPatterns(pSequenceInfo->pGopInfo)) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    /* The sets first, into storage of the call's own: a refused sequence changes nothing. */
    VvsDpbH265ShortTermSet sets[STD_VIDEO_H265_MAX_SHORT_TERM_REF_PIC_SETS];
    for (uint32_t i = 0; i < pSps->num_short_term_ref_pic_sets; i++) {
        if (!DeriveShortTermSet(pSps->pShortTermRefPicSet, sets, i, &sets[i])) {
            return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
        }
    }

    const uint32_t references = pSps->pDecPicBufMgr->max_dec_pic_buffering_minus1[pSps->sps_max_sub_layers_minus1];
    const uint32_t list0Count = pPps->num_ref_idx_l0_default_active_minus1 + 1U;
    const uint32_t list1Count = pPps->num_ref_idx_l1_default_active_minus1 + 1U;
    VvsDpbSequenceRequirements requirements;
    requirements.maxReferencePictures = references;
    /* The references a picture's RPS keeps stay while it is encoded, so it takes one more slot. A
     * picture may take the slot of a reference its own RPS drops, so a rewind of N pictures takes N
     * more. */
    requirements.maxDpbSlots = references + 1U + dpb->rewindDepth;
    /* A P picture uses the nearest pictures before it, up to list 0's default count; a B picture
     * those before and after it that UsedAfterB() leaves. */
    const uint32_t usedP = vvsDpbMin(list0Count, VVS_DPB_H265_MAX_TOTAL_CURR);
    const uint32_t usedB =
        vvsDpbMin(list0Count, VVS_DPB_H265_MAX_TOTAL_CURR - UsedAfterB(list0Count)) + vvsDpbMin(list1Count, UsedAfterB(list0Count));
    requirements.maxActiveReferencePictures = vvsDpbMin(references, vvsDpbMax(usedP, usedB));
    if ((references >= STD_VIDEO_H265_MAX_DPB_SIZE) || (requirements.maxDpbSlots > VVS_DPB_MAX_SLOTS) ||
        ((dpb->boundSlots != 0U) && (requirements.maxDpbSlots > dpb->boundSlots)) ||
        ((dpb->boundActiveReferences != 0U) && (requirements.maxActiveReferencePictures > dpb->boundActiveReferences))) {
        return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
    }

    VvsDpbH265Sequence* pSequence = &dpb->sequence.h265;
    pSequence->maxReferences = references;
    pSequence->numRefIdxL0DefaultActive = list0Count;
    pSequence->numRefIdxL1DefaultActive = list1Count;
    pSequence->shortTermSetCount = pSps->num_short_term_ref_pic_sets;
    memcpy(pSequence->shortTermSets, sets, pSps->num_short_term_ref_pic_sets * sizeof(sets[0]));
    vvsDpbSequenceCommit(dpb, &requirements, pSequenceInfo->pGopInfo);
    if (pRequirements != NULL) {
        *pRequirements = requirements;
    }
    return VK_SUCCESS;
}

VVS_DPB_API VkResult vvsDpbEncodeH265Picture(VvsDpb dpb, const VvsDpbH265EncodePictureInfo* pPictureInfo,
                                             VvsDpbPictureResult* pResult) {
    if ((pPictureInfo == NULL) || (pPictureInfo->pStdPictureInfo == NULL) || (pPictureInfo->pSyntax == NULL) || (pResult == NULL)) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    VkResult result = vvsDpbPictureBegin(dpb, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, pPictureInfo->dirtyIntraRefreshRegions);
    if (result != VK_SUCCESS) {
        return result;
    }
    StdVideoEncodeH265PictureInfo* pStd = pPictureInfo->pStdPictureInfo;
    const VvsDpbH265Sequence* pSequence = &dpb->sequence.h265;
    VvsDpbState* pState = &dpb->state;
    VvsDpbH265State* pH265 = &pState->codec.h265;
    const StdVideoH265PictureType picType = pStd->pic_type;
    const bool idr = picType == STD_VIDEO_H265_PICTURE_TYPE_IDR;
    const bool irap = pStd->flags.IrapPicFlag != 0U;
    const bool inter = (picType == STD_VIDEO_H265_PICTURE_TYPE_P) || (picType == STD_VIDEO_H265_PICTURE_TYPE_B);
    if ((pState->keyPictureNeeded && !idr) || (!inter && !idr && (picType != STD_VIDEO_H265_PICTURE_TYPE_I))) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    const bool reference = pStd->flags.is_reference != 0U;
    const int32_t poc = pStd->PicOrderCntVal;

    /* The references before the picture's RPS: an IDR picture drops them all. The first picture after
     * a CRA picture in output order drops the pictures before the CRA picture. The oldest short-term
     * reference goes when the references fill the DPB. */
    if (idr) {
        for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
            pState->slots[i].flags &= ~(VVS_DPB_SLOT_REFERENCE | VVS_DPB_SLOT_LONG_TERM);
        }
        pH265->craPending = 0;
    } else {
        if (pH265->craPending && (poc > pH265->craPicOrderCntVal)) {
            for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
                if (SlotPoc(pState, i) != pH265->craPicOrderCntVal) {
                    pState->slots[i].flags &= ~(VVS_DPB_SLOT_REFERENCE | VVS_DPB_SLOT_LONG_TERM);
                }
            }
            pH265->craPending = 0;
        }
        if (picType == STD_VIDEO_H265_PICTURE_TYPE_I) {
            pH265->craPending = 1;
            pH265->craPicOrderCntVal = poc;
        }
        uint32_t count = 0;
        uint32_t oldest = VVS_DPB_NO_SLOT;
        for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
            if ((pState->slots[i].flags & VVS_DPB_SLOT_REFERENCE) != 0U) {
                count++;
                if (vvsDpbIsShortTerm(&pState->slots[i]) &&
                    ((oldest == VVS_DPB_NO_SLOT) || (SlotPoc(pState, i) < SlotPoc(pState, oldest)))) {
                    oldest = i;
                }
            }
        }
        if ((count > pSequence->maxReferences) && (oldest != VVS_DPB_NO_SLOT)) {
            pState->slots[oldest].flags &= ~VVS_DPB_SLOT_REFERENCE;
        }
    }

    /* The RPS (7.4.8): every short-term reference of the picture's temporal sub-layer or a lower one,
     * those before it by descending PicOrderCntVal and those after it by ascending, the farthest
     * before it dropped first to fit the DPB. The picture uses the nearest ones. */
    uint32_t before[VVS_DPB_MAX_SLOTS];
    uint32_t after[VVS_DPB_MAX_SLOTS];
    int32_t beforeKeys[VVS_DPB_MAX_SLOTS];
    int32_t afterKeys[VVS_DPB_MAX_SLOTS];
    uint32_t beforeCount = 0;
    uint32_t afterCount = 0;
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        const VvsDpbSlotState* pSlot = &pState->slots[i];
        if (!vvsDpbIsShortTerm(pSlot) || (pSlot->stdReferenceInfo.h265.TemporalId > pStd->TemporalId)) {
            continue;
        }
        if (SlotPoc(pState, i) < poc) {
            before[beforeCount] = i;
            beforeKeys[beforeCount++] = SlotPoc(pState, i);
        } else if (SlotPoc(pState, i) > poc) {
            after[afterCount] = i;
            afterKeys[afterCount++] = SlotPoc(pState, i);
        }
    }
    vvsDpbSortSlots(before, beforeKeys, beforeCount, true);
    vvsDpbSortSlots(after, afterKeys, afterCount, false);
    while ((beforeCount + afterCount) > pSequence->maxReferences) {
        if (beforeCount > 0U) {
            beforeCount--;
        } else {
            afterCount--;
        }
    }
    const uint32_t list0Default = pSequence->numRefIdxL0DefaultActive;
    const uint32_t list1Default = pSequence->numRefIdxL1DefaultActive;
    uint32_t usedBefore = 0;
    uint32_t usedAfter = 0;
    if (!irap) {
        if (picType == STD_VIDEO_H265_PICTURE_TYPE_B) {
            usedBefore = vvsDpbMin(beforeCount, vvsDpbMin(list0Default, VVS_DPB_H265_MAX_TOTAL_CURR - UsedAfterB(list0Default)));
            usedAfter = vvsDpbMin(afterCount, vvsDpbMin(list1Default, UsedAfterB(list0Default)));
        } else {
            usedBefore = vvsDpbMin(beforeCount, vvsDpbMin(list0Default, VVS_DPB_H265_MAX_TOTAL_CURR));
        }
    }
    if (inter && ((usedBefore + usedAfter) == 0U)) {
        vvsDpbPictureAbort(dpb);
        return VK_ERROR_VALIDATION_FAILED;
    }

    VvsDpbH265EncodeSyntax* pSyntax = pPictureInfo->pSyntax;
    VvsDpbH265ShortTermSet set;
    memset(&set, 0, sizeof(set));
    set.numNegativePics = (uint8_t)beforeCount;
    set.numPositivePics = (uint8_t)afterCount;
    for (uint32_t i = 0; i < beforeCount; i++) {
        set.deltaPocS0[i] = (int16_t)(SlotPoc(pState, before[i]) - poc);
        set.usedS0 |= (uint16_t)(((i < usedBefore) ? 1U : 0U) << i);
    }
    for (uint32_t i = 0; i < afterCount; i++) {
        set.deltaPocS1[i] = (int16_t)(SlotPoc(pState, after[i]) - poc);
        set.usedS1 |= (uint16_t)(((i < usedAfter) ? 1U : 0U) << i);
    }

    /* 8.3.2: the references the RPS does not name stop being references. */
    uint32_t keep = 0;
    for (uint32_t i = 0; i < beforeCount; i++) {
        keep |= 1U << before[i];
    }
    for (uint32_t i = 0; i < afterCount; i++) {
        keep |= 1U << after[i];
    }
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        if ((keep & (1U << i)) == 0U) {
            pState->slots[i].flags &= ~(VVS_DPB_SLOT_REFERENCE | VVS_DPB_SLOT_LONG_TERM);
        }
    }

    /* The setup slot, one that holds no reference of the RPS. */
    const bool setup = reference || ((dpb->flags & VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT) != 0U);
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

    /* Nothing fails from here on. */
    if (setup) {
        VvsDpbSlotState* pSlot = &pState->slots[setupSlot];
        pSlot->flags = reference ? VVS_DPB_SLOT_REFERENCE : 0U;
        if (pPictureInfo->pPictureInfo != NULL) {
            pSlot->pictureInfo = *pPictureInfo->pPictureInfo;
            pSlot->flags |= VVS_DPB_SLOT_PICTURE_INFO;
        } else {
            memset(&pSlot->pictureInfo, 0, sizeof(pSlot->pictureInfo));
        }
        pSlot->dirtyIntraRefreshRegions = vvsDpbDirtyRegions(pPictureInfo->dirtyIntraRefreshRegions);
        pSlot->codecValue = 0;
        StdVideoEncodeH265ReferenceInfo* pReference = &pSlot->stdReferenceInfo.h265;
        memset(pReference, 0, sizeof(*pReference));
        if (!reference) {
            pReference->flags.unused_for_reference = 1;
        }
        pReference->pic_type = picType;
        pReference->PicOrderCntVal = poc;
        pReference->TemporalId = pStd->TemporalId;
    }
    if (idr) {
        pState->keyPictureNeeded = 0;
    }

    /* The RPS syntax, or an SPS set that equals it. */
    StdVideoH265ShortTermRefPicSet* pRps = &pSyntax->shortTermRefPicSet;
    memset(pRps, 0, sizeof(*pRps));
    pRps->num_negative_pics = set.numNegativePics;
    pRps->num_positive_pics = set.numPositivePics;
    pRps->used_by_curr_pic_s0_flag = set.usedS0;
    pRps->used_by_curr_pic_s1_flag = set.usedS1;
    int32_t previous = 0;
    for (uint32_t i = 0; i < set.numNegativePics; i++) {
        pRps->delta_poc_s0_minus1[i] = (uint16_t)(previous - set.deltaPocS0[i] - 1);
        previous = set.deltaPocS0[i];
    }
    previous = 0;
    for (uint32_t i = 0; i < set.numPositivePics; i++) {
        pRps->delta_poc_s1_minus1[i] = (uint16_t)(set.deltaPocS1[i] - previous - 1);
        previous = set.deltaPocS1[i];
    }
    pStd->flags.short_term_ref_pic_set_sps_flag = 0;
    pStd->short_term_ref_pic_set_idx = 0;
    for (uint32_t i = 0; i < pSequence->shortTermSetCount; i++) {
        if (SameShortTermSet(&pSequence->shortTermSets[i], &set)) {
            pStd->flags.short_term_ref_pic_set_sps_flag = 1;
            pStd->short_term_ref_pic_set_idx = (uint8_t)i;
            break;
        }
    }
    pStd->pShortTermRefPicSet = pRps;
    memset(&pSyntax->longTermRefPics, 0, sizeof(pSyntax->longTermRefPics));
    pStd->pLongTermRefPics = &pSyntax->longTermRefPics;

    /* The lists (8.3.4), at the PPS's default counts or the pictures the picture uses. */
    StdVideoEncodeH265ReferenceListsInfo* pLists = &pSyntax->refLists;
    memset(pLists, 0, sizeof(*pLists));
    memset(pLists->RefPicList0, STD_VIDEO_H265_NO_REFERENCE_PICTURE, sizeof(pLists->RefPicList0));
    memset(pLists->RefPicList1, STD_VIDEO_H265_NO_REFERENCE_PICTURE, sizeof(pLists->RefPicList1));
    uint32_t list0Count = 0;
    uint32_t list1Count = 0;
    if (inter) {
        list0Count = (list0Default > usedBefore) ? vvsDpbMax(usedBefore, 1U) : list0Default;
        list1Count = list1Default;
        FillList(before, usedBefore, after, usedAfter, list0Count, pLists->RefPicList0);
        if (picType == STD_VIDEO_H265_PICTURE_TYPE_B) {
            list1Count = (list1Default > usedAfter) ? vvsDpbMax(usedAfter, 1U) : list1Default;
            FillList(after, usedAfter, before, usedBefore, list1Count, pLists->RefPicList1);
        }
        pLists->num_ref_idx_l0_active_minus1 = (uint8_t)(list0Count - 1U);
        pLists->num_ref_idx_l1_active_minus1 = (uint8_t)(list1Count - 1U);
        pStd->pRefLists = pLists;
    } else {
        pStd->pRefLists = NULL;
    }

    /* The slots: list 0's references, then list 1's. */
    uint32_t references[2U * STD_VIDEO_H265_MAX_NUM_LIST_REF];
    uint32_t referenceCount = 0;
    for (uint32_t i = 0; i < list0Count; i++) {
        references[referenceCount++] = pLists->RefPicList0[i];
    }
    if (picType == STD_VIDEO_H265_PICTURE_TYPE_B) {
        for (uint32_t i = 0; i < list1Count; i++) {
            references[referenceCount++] = pLists->RefPicList1[i];
        }
    }
    vvsDpbResultSetReferences(dpb, references, referenceCount, pResult);
    vvsDpbPictureCommit(dpb, setupSlot, pResult);
    return VK_SUCCESS;
}
