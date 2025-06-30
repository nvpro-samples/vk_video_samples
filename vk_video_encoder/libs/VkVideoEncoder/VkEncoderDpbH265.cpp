/*
 * Copyright 2023 NVIDIA Corporation.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#include <algorithm>

#include "VkEncoderDpbH265.h"

template<typename T>
static inline T clampl(T value, T minbound) {
    return (value < minbound) ? minbound : value;
}

VkEncDpbH265::VkEncDpbH265()
    : m_curDpbIndex(0)
    , m_dpbSize(0)
    , m_numPocStCurrBefore(0)
    , m_numPocStCurrAfter(0)
    , m_numPocStFoll(0)
    , m_numPocLtCurr(0)
    , m_numPocLtFoll(0)
    , m_lastIDRTimeStamp(0)
    , m_picOrderCntCRA(0)
    , m_refreshPending(false)
    , m_longTermFlags(0)
    , m_useMultipleRefs()
{
        for (uint32_t i = 0; i < STD_VIDEO_H265_MAX_DPB_SIZE; i++) {
            m_stDpb[i] = DpbEntryH265();
        }
}

bool VkEncDpbH265::DpbSequenceStart(int32_t dpbSize, bool useMultipleReferences)
{
    assert(dpbSize >= 0);
    m_dpbSize = std::min<int8_t>((int8_t)dpbSize, STD_VIDEO_H265_MAX_DPB_SIZE);

    for (uint32_t i = 0; i < STD_VIDEO_H265_MAX_DPB_SIZE; i++) {
        m_stDpb[i].state = 0;
        m_stDpb[i].marking = 0;
        m_stDpb[i].output = 0;
        m_stDpb[i].dpbImageView = nullptr;
    }

    // The device supports use of multiple references when encoding a frame,
    // so make use of that ability.
    m_useMultipleRefs = useMultipleReferences;

    return true;
}


int8_t VkEncDpbH265::DpbPictureStart(uint64_t frameId, const StdVideoEncodeH265PictureInfo *pPicInfo,
                                      const StdVideoH265ShortTermRefPicSet *pShortTermRefPicSet,
                                      const StdVideoH265LongTermRefPicsSps* pLongTermRefPicsSps,
                                      int32_t maxPicOrderCntLsb,
                                      uint64_t timeStamp,
                                      RefPicSet* pRefPicSet) {
    bool isIrapPic = pPicInfo->flags.IrapPicFlag;
    bool NoRaslOutputFlag = false;
    if (isIrapPic) {
        // If the current picture is an IDR picture, a BLA picture, the first picture in the bitstream
        // in decoding order, or the first picture that follows an end of sequence NAL unit in decoding order,
        // the variable NoRaslOutputFlag is set equal to 1.
        //
        // We're not dealing with BLA pictures in our encoder and we always output the first
        // picture as an IDR, so the above conditions state that NoRaslOutputFlag has to be
        // set for IDR pictures.
        NoRaslOutputFlag = (pPicInfo->pic_type == STD_VIDEO_H265_PICTURE_TYPE_IDR);
    }

    ApplyReferencePictureSet(pPicInfo,
                             pShortTermRefPicSet, pLongTermRefPicsSps,
                             maxPicOrderCntLsb, pRefPicSet);

    if (isIrapPic && NoRaslOutputFlag) {
        // Strictly speaking, we should be using the NALU type when setting NoOutputOfPriorPicsFlag
        // but it doesn't matter in our case because I-frames are coded with the CRA_NUT NALU type.
        int32_t NoOutputOfPriorPicsFlag = (pPicInfo->pic_type == STD_VIDEO_H265_PICTURE_TYPE_I) ? 1 : pPicInfo->flags.no_output_of_prior_pics_flag;
        if (NoOutputOfPriorPicsFlag) {
            // Empty all the frame buffers when NoOutputOfPriorPicsFlag == true
            for (int32_t i = 0; i < m_dpbSize; i++) {
                m_stDpb[i].state = 0;
                m_stDpb[i].marking = 0;
                m_stDpb[i].output = 0;
                m_stDpb[i].dpbImageView = nullptr;
            }
        } else {
            // For NoOutputOfPriorPicsFlag == false, empty all frame buffers marked as "not needed for output"
            FlushDpb();
        }
    } else {
        for (int32_t i = 0; i < m_dpbSize; i++) {
            if ((m_stDpb[i].marking == 0) && (m_stDpb[i].output == 0)) {
                m_stDpb[i].state = 0;
                m_stDpb[i].dpbImageView = nullptr;
            }
        }
        while (IsDpbFull()) {
            DpbBumping();
        }
    }

    // select decoded picture buffer
    for (m_curDpbIndex = 0; m_curDpbIndex < m_dpbSize; m_curDpbIndex++) {
        if (m_stDpb[m_curDpbIndex].state == 0)
            break;
    }
    if (m_curDpbIndex >= m_dpbSize) {
        assert(!"Dpb index out of bounds");
        return false;
    }

    DpbEntryH265* pCurDpbEntry = &m_stDpb[m_curDpbIndex];
    pCurDpbEntry->frameId = frameId;
    pCurDpbEntry->picOrderCntVal = pPicInfo->PicOrderCntVal;
    pCurDpbEntry->output = !!pPicInfo->flags.pic_output_flag;
    pCurDpbEntry->corrupted = false;
    pCurDpbEntry->temporalId = pPicInfo->TemporalId;
    if (isIrapPic && NoRaslOutputFlag) {
        m_lastIDRTimeStamp = timeStamp;
    }

    for (int32_t i = 0; i < m_dpbSize; i++) {
        pCurDpbEntry->refPicOrderCnt[i] = m_stDpb[i].picOrderCntVal;
        if (m_stDpb[i].marking == 2) {
            pCurDpbEntry->longTermRefPic |= 1 << i;
        } else {
            pCurDpbEntry->longTermRefPic &= ~(1 << i);
        }
    }

    return m_curDpbIndex;
}

void VkEncDpbH265::DpbPictureEnd(VkSharedBaseObj<VulkanVideoImagePoolNode>& dpbImageView, uint32_t numTemporalLayers, bool isReference) {

    // For temporal SVC , we unmark the ref frames in Dpb having same temporal id as the current frame
    if (numTemporalLayers > 1) {
        for (int32_t i = 0; i < m_dpbSize; i++) {
            if ((m_stDpb[i].state == 1) && (m_stDpb[i].marking != 0) &&
                    (m_stDpb[i].temporalId == m_stDpb[m_curDpbIndex].temporalId)) {
                m_stDpb[i].marking = 0;
            }
        }
    }

    m_stDpb[m_curDpbIndex].dpbImageView = dpbImageView;
    m_stDpb[m_curDpbIndex].state = 1;
    m_stDpb[m_curDpbIndex].marking = isReference ? 1 : 0;
}

bool VkEncDpbH265::IsDpbFull() {
    int32_t numDpbPictures = 0;
    for (int32_t i = 0; i < m_dpbSize; i++) {
        if (m_stDpb[i].state == 1) {
            numDpbPictures++;
        }
    }
    return numDpbPictures >= m_dpbSize;
}

bool VkEncDpbH265::IsDpbEmpty() {
    int32_t numDpbPictures = 0;
    for (int32_t i = 0; i < m_dpbSize; i++) {
        if (m_stDpb[i].state == 1)
            numDpbPictures++;
    }
    return numDpbPictures == 0;
}

void VkEncDpbH265::FlushDpb() {
    // mark all reference pictures as "unused for reference"
    for (int32_t i = 0; i < m_dpbSize; i++) {
        m_stDpb[i].marking = 0;
    }
    // empty frame buffers marked as "not needed for output" and "unused for reference"
    for (int32_t i = 0; i < m_dpbSize; i++) {
        if ((m_stDpb[i].state == 1) && (m_stDpb[i].output == 0) && (m_stDpb[i].marking == 0)) {
            m_stDpb[i].state = 0; // empty
            m_stDpb[i].dpbImageView = nullptr;
        }
    }
    while (!IsDpbEmpty())
        DpbBumping();
}

void VkEncDpbH265::DpbBumping() {
    int32_t dpbIndxWithMinPoc = -1;
    uint32_t minPoc = 0;

    for (int32_t i = 0; i < m_dpbSize; i++) {
        if ((m_stDpb[i].state == 1) && m_stDpb[i].output) {
            if ((dpbIndxWithMinPoc < 0) || (m_stDpb[i].picOrderCntVal < minPoc)) {
                minPoc = m_stDpb[i].picOrderCntVal;
                dpbIndxWithMinPoc = i;
            }
        }
    }

    if (dpbIndxWithMinPoc < 0) {
        assert(!"Invalid Dpb state");
        return;
    }

    m_stDpb[dpbIndxWithMinPoc].output = 0;
    if (m_stDpb[dpbIndxWithMinPoc].marking == 0) {
        m_stDpb[dpbIndxWithMinPoc].state = 0;
        m_stDpb[dpbIndxWithMinPoc].dpbImageView = nullptr;
    }
}

bool VkEncDpbH265::GetRefPicture(int8_t dpbIndex, VkSharedBaseObj<VulkanVideoImagePoolNode>& dpbImageView)
{
    assert(dpbIndex < STD_VIDEO_H265_MAX_DPB_SIZE);

    if (!(dpbIndex < STD_VIDEO_H265_MAX_DPB_SIZE)) {
        return false;
    }

    dpbImageView = m_stDpb[dpbIndex].dpbImageView;
    return (dpbImageView != nullptr) ? true : false;
}

void VkEncDpbH265::FillStdReferenceInfo(uint8_t dpbIndex, StdVideoEncodeH265ReferenceInfo *pRefInfo)
{
    assert(dpbIndex < STD_VIDEO_H265_MAX_DPB_SIZE);

    const DpbEntryH265 *entry = &m_stDpb[dpbIndex];

    pRefInfo->flags.unused_for_reference = (entry->marking == 0);

    pRefInfo->PicOrderCntVal = entry->picOrderCntVal;
    pRefInfo->TemporalId = (uint8_t)entry->temporalId;

    // TODO: fill in pRefInfo->pic_type
}

// Can mark STR frames as unused for reference
// Can mark STR frames as used for long term reference
// Can mark LTR frames as unused for reference
void VkEncDpbH265::ApplyReferencePictureSet(const StdVideoEncodeH265PictureInfo *pPicInfo,
                                            const StdVideoH265ShortTermRefPicSet *pShortTermRefPicSet,
                                            const StdVideoH265LongTermRefPicsSps* pLongTermRefPicsSps,
                                            int32_t maxPicOrderCntLsb,
                                            RefPicSet* pRefPicSet) {

    uint32_t pocStCurrBefore[STD_VIDEO_H265_MAX_NUM_LIST_REF], pocStCurrAfter[STD_VIDEO_H265_MAX_NUM_LIST_REF],
             pocStFoll[STD_VIDEO_H265_MAX_NUM_LIST_REF], pocLtCurr[STD_VIDEO_H265_MAX_NUM_LIST_REF], pocLtFoll[STD_VIDEO_H265_MAX_NUM_LIST_REF];
    int32_t currDeltaPocMsbPresentFlag[STD_VIDEO_H265_MAX_NUM_LIST_REF], follDeltaPocMsbPresentFlag[STD_VIDEO_H265_MAX_NUM_LIST_REF];
    bool noRaslOutputFlag = false;
    bool isIrapPic = false;

    uint32_t picOrderCntVal = pPicInfo->PicOrderCntVal;

    isIrapPic = pPicInfo->flags.IrapPicFlag;
    if (isIrapPic) {
        noRaslOutputFlag = (pPicInfo->pic_type == STD_VIDEO_H265_PICTURE_TYPE_IDR);
    }

    if (isIrapPic && noRaslOutputFlag) {
        for (int32_t i = 0; i < m_dpbSize; i++) {
            m_stDpb[i].marking = 0;
        }
    }

    // int32_t maxPicOrderCntLsb = 1 << (pSps->log2_max_pic_order_cnt_lsb_minus4 + 4);

    if (pPicInfo->pic_type == STD_VIDEO_H265_PICTURE_TYPE_IDR) {
        m_numPocStCurrBefore = 0;
        m_numPocStCurrAfter  = 0;
        m_numPocStFoll       = 0;
        m_numPocLtCurr       = 0;
        m_numPocLtFoll       = 0;
    } else {

        const StdVideoEncodeH265LongTermRefPics *pLongTermRefPics = pPicInfo->pLongTermRefPics;

        // Derived delta POC values
        int32_t DeltaPocS0[STD_VIDEO_H265_MAX_DPB_SIZE], DeltaPocS1[STD_VIDEO_H265_MAX_DPB_SIZE];

        uint32_t numLongTermRefPics = 0;
        int32_t numRefPics = pShortTermRefPicSet->num_negative_pics + pShortTermRefPicSet->num_positive_pics;
        if ((pLongTermRefPicsSps != nullptr) && (pLongTermRefPics != nullptr)) {
            numLongTermRefPics = pLongTermRefPics->num_long_term_sps + pLongTermRefPics->num_long_term_pics;

            numRefPics += numLongTermRefPics;
        }

        if (numRefPics > (m_dpbSize - 1)) {
            printf("too many reference frames (%d, max is %d)\n", numRefPics, (m_dpbSize - 1));
        }

        assert(numRefPics <= STD_VIDEO_H265_MAX_NUM_LIST_REF);

        int8_t i = 0, j = 0, k = 0;

        for (i = 0; i < pShortTermRefPicSet->num_negative_pics; i++) {
            DeltaPocS0[i] = (i == 0) ? -(pShortTermRefPicSet->delta_poc_s0_minus1[i] + 1) :
                            DeltaPocS0[i - 1] - (pShortTermRefPicSet->delta_poc_s0_minus1[i] + 1);
        }
        for (; i < STD_VIDEO_H265_MAX_DPB_SIZE; i++) {
            DeltaPocS0[i] = -1;
        }

        for (i = 0; i < pShortTermRefPicSet->num_positive_pics; i++) {
            DeltaPocS1[i] = (i == 0) ? (pShortTermRefPicSet->delta_poc_s1_minus1[i] + 1) :
                            DeltaPocS1[i - 1] + (pShortTermRefPicSet->delta_poc_s1_minus1[i] + 1);
        }
        for (; i < STD_VIDEO_H265_MAX_DPB_SIZE; i++) {
            DeltaPocS1[i] = -1;
        }

        for (i = 0; i < pShortTermRefPicSet->num_negative_pics; i++) {
            if ((pShortTermRefPicSet->used_by_curr_pic_s0_flag >> i) & 0x1)
                pocStCurrBefore[j++] = picOrderCntVal + DeltaPocS0[i];
            else
                pocStFoll[k++] = picOrderCntVal + DeltaPocS0[i];
        }
        m_numPocStCurrBefore = j;

        j = 0;
        for (i = 0; i < pShortTermRefPicSet->num_positive_pics; i++) {
            if ((pShortTermRefPicSet->used_by_curr_pic_s1_flag >> i) & 0x1)
                pocStCurrAfter[j++] = picOrderCntVal + DeltaPocS1[i];
            else
                pocStFoll[k++] = picOrderCntVal + DeltaPocS1[i];
        }

        m_numPocStCurrAfter = j;
        m_numPocStFoll = k;

        uint32_t pocLsbLt[STD_VIDEO_H265_MAX_NUM_LIST_REF]{};
        uint32_t usedByCurrPicLt[STD_VIDEO_H265_MAX_NUM_LIST_REF]{};
        uint32_t deltaPocMSBCycleLt[STD_VIDEO_H265_MAX_NUM_LIST_REF]{};

        if (pLongTermRefPics && (pLongTermRefPics->num_long_term_sps > 0)) {
            assert(pLongTermRefPicsSps);
        }

        for (i = 0; i < (int32_t)numLongTermRefPics; i++) {
            if (i < pLongTermRefPics->num_long_term_sps) {
                uint32_t index = pLongTermRefPics->lt_idx_sps[i];

                pocLsbLt[i] = pLongTermRefPicsSps->lt_ref_pic_poc_lsb_sps[index];
                usedByCurrPicLt[i] = (pLongTermRefPicsSps->used_by_curr_pic_lt_sps_flag >> index) & 0x1;
            } else {
                pocLsbLt[i] = pLongTermRefPics->poc_lsb_lt[i];
                usedByCurrPicLt[i] = (pLongTermRefPics->used_by_curr_pic_lt_flag >> i) & 0x1;
            }

            if (i == 0 || i == pLongTermRefPics->num_long_term_sps)
                deltaPocMSBCycleLt[i] = pLongTermRefPics->delta_poc_msb_cycle_lt[i];
            else
                deltaPocMSBCycleLt[i] = pLongTermRefPics->delta_poc_msb_cycle_lt[i] + deltaPocMSBCycleLt[i-1];
        }

        j = 0;
        k = 0;
        for (i = 0; i < (int32_t)numLongTermRefPics; i++) {
            int32_t pocLt = pocLsbLt[i];
            if (pLongTermRefPics->delta_poc_msb_present_flag[i]) {
                uint32_t slice_pic_order_cnt_lsb = picOrderCntVal & (maxPicOrderCntLsb - 1);

                pocLt += picOrderCntVal - deltaPocMSBCycleLt[i] * maxPicOrderCntLsb - slice_pic_order_cnt_lsb;
            }

            if (usedByCurrPicLt[i]) {
                pocLtCurr[j] = pocLt;
                currDeltaPocMsbPresentFlag[j++] = pLongTermRefPics->delta_poc_msb_present_flag[i];
            } else {
                pocLtFoll[k] = pocLt;
                follDeltaPocMsbPresentFlag[k++] = pLongTermRefPics->delta_poc_msb_present_flag[i];
            }
        }
        m_numPocLtCurr = j;
        m_numPocLtFoll = k;
    }

    // set all entries to "no reference picture"
    memset(pRefPicSet, -1, sizeof(RefPicSet));

    for (int32_t i = 0; i < m_numPocLtCurr; i++) {
        uint32_t mask = !currDeltaPocMsbPresentFlag[i] ? (maxPicOrderCntLsb - 1) : ~0;
        // if there is a reference picture picX in the Dpb with slice_pic_order_cnt_lsb equal to pocLtCurr[i]
        // if there is a reference picture picX in the Dpb with PicOrderCntVal equal to pocLtCurr[i]
        for (int8_t j = 0; j < m_dpbSize; j++) {
            if ((m_stDpb[j].state == 1) && (m_stDpb[j].marking != 0) &&
                    (m_stDpb[j].picOrderCntVal & mask) == pocLtCurr[i]) {
                pRefPicSet->ltCurr[i] = j;
                break;
            }
        }
        if (pRefPicSet->ltCurr[i] < 0)
            printf("long-term reference picture not available (POC=%d)\n", pocLtCurr[i]);
    }

    for (int32_t i = 0; i < m_numPocLtFoll; i++) {
        uint32_t mask = !follDeltaPocMsbPresentFlag[i] ? maxPicOrderCntLsb - 1 : ~0;
        // if there is a reference picture picX in the Dpb with slice_pic_order_cnt_lsb equal to PocLtFoll[i]
        // if there is a reference picture picX in the Dpb with PicOrderCntVal to PocLtFoll[i]
        for (int8_t j = 0; j < m_dpbSize; j++) {
            if ((m_stDpb[j].state == 1) && (m_stDpb[j].marking != 0) && ((m_stDpb[j].picOrderCntVal & mask) == pocLtFoll[i])) {
                pRefPicSet->ltFoll[i] = j;
                break;
            }
        }
    }

    for (int32_t i = 0; i < m_numPocLtCurr; i++) {
        if (pRefPicSet->ltCurr[i] != -1) {
            // encoder driver should have already done the reference picture marking process
            if (m_stDpb[pRefPicSet->ltCurr[i]].marking != 2) {
                assert(!"Forcing reference picture marking to be used as long term");
                m_stDpb[pRefPicSet->ltCurr[i]].marking = 2;
            }
        }
    }

    for (int32_t i = 0; i < m_numPocLtFoll; i++) {
        if (pRefPicSet->ltFoll[i] != -1) {
            // encoder driver should have already done the reference picture marking process
            if (m_stDpb[pRefPicSet->ltCurr[i]].marking != 2) {
                assert(!"Forcing reference picture marking to be used as long term");
                m_stDpb[pRefPicSet->ltCurr[i]].marking = 2;
            }
        }
    }

    for (int32_t i = 0; i < m_numPocStCurrBefore; i++) {
        // if there is a short-term reference picture picX in the Dpb with PicOrderCntVal equal to PocStCurrBefore[i]
        for (int8_t j = 0; j < m_dpbSize; j++) {
            if (((m_stDpb[j].state == 1) && (m_stDpb[j].marking == 1)) && (m_stDpb[j].picOrderCntVal == pocStCurrBefore[i])) {
                pRefPicSet->stCurrBefore[i] = j;
                break;
            }
        }
        if (pRefPicSet->stCurrBefore[i] < 0)
            printf("short-term reference picture not available (POC=%d)\n", pocStCurrBefore[i]);
    }

    for (int32_t i = 0; i < m_numPocStCurrAfter; i++) {
        // if there is a short-term reference picture picX in the Dpb with PicOrderCntVal equal to PocStCurrAfter[i]
        for (int8_t j = 0; j < m_dpbSize; j++) {
            if ((m_stDpb[j].state == 1) && (m_stDpb[j].marking == 1) && (m_stDpb[j].picOrderCntVal == pocStCurrAfter[i])) {
                pRefPicSet->stCurrAfter[i] = j;
                break;
            }
        }
        if (pRefPicSet->stCurrAfter[i] < 0)
            printf("short-term reference picture not available (POC=%d)\n", pocStCurrAfter[i]);
    }

    for (int32_t i = 0; i < m_numPocStFoll; i++) {
        // if there is a short-term reference picture picX in the Dpb with PicOrderCntVal equal to PocStFoll[i]
        for (int8_t j = 0; j < m_dpbSize; j++) {
            if ((m_stDpb[j].state == 1) && (m_stDpb[j].marking == 1) && (m_stDpb[j].picOrderCntVal == pocStFoll[i])) {
                pRefPicSet->stFoll[i] = j;
                break;
            }
        }
    }

    // All reference pictures in the decoded picture buffer that are not included in RefPicSetLtCurr, RefPicSetLtFoll, RefPicSetStCurrBefore, RefPicSetStCurrAfter or RefPicSetStFoll are marked as "unused for reference".
    bool inUse[STD_VIDEO_H265_MAX_DPB_SIZE];
    for (int32_t i = 0; i < m_dpbSize; i++)
        inUse[i] = false;

    for (int32_t i = 0; i < m_numPocLtCurr; i++) {
        if (pRefPicSet->ltCurr[i] != -1)
            inUse[pRefPicSet->ltCurr[i]] = true;
    }

    for (int32_t i = 0; i < m_numPocLtFoll; i++) {
        if (pRefPicSet->ltFoll[i] != -1)
            inUse[pRefPicSet->ltFoll[i]] = true;
    }

    for (int32_t i = 0; i < m_numPocStCurrBefore; i++) {
        if (pRefPicSet->stCurrBefore[i] != -1)
            inUse[pRefPicSet->stCurrBefore[i]] = true;
    }

    for (int32_t i = 0; i < m_numPocStCurrAfter; i++) {
        if (pRefPicSet->stCurrAfter[i] != -1)
            inUse[pRefPicSet->stCurrAfter[i]] = true;
    }

    for (int32_t i = 0; i < m_numPocStFoll; i++) {
        if (pRefPicSet->stFoll[i] != -1)
            inUse[pRefPicSet->stFoll[i]] = true;
    }

    for (int32_t i = 0; i < m_dpbSize; i++) {
        if (!inUse[i]) {
            if (m_stDpb[i].marking != 0) {
                m_stDpb[i].marking = 0;
            }
        }
    }
}

void VkEncDpbH265::SetupReferencePictureListLx(StdVideoH265PictureType picType,
                                               const RefPicSet* pRefPicSet,
                                               StdVideoEncodeH265ReferenceListsInfo *pRefLists,
                                               uint32_t numRefL0, uint32_t numRefL1) {
    int32_t isLongTerm[32] = { 0 };
    uint8_t numPocTotalCurr = (uint8_t)(m_numPocStCurrBefore + m_numPocStCurrAfter + m_numPocLtCurr);
    assert(numPocTotalCurr <= 8);

    assert(pRefLists); // P- and B-frames must have non-NULL reference lists.

    pRefLists->num_ref_idx_l0_active_minus1 = (uint8_t)((numRefL0 > 0) ? (numRefL0 - 1) : 0);
    pRefLists->num_ref_idx_l1_active_minus1 = (uint8_t)((numRefL1 > 0) ? (numRefL1) - 1 : 0);

    m_longTermFlags = 0;
    // The value of num_ref_idx_l0_active_minus1 should not be updated here when WP is enabled.
    // The correct value for num_ref_idx_l0_active_minus1 for WP is calculated in ModifyRefPicListForWP()
    if (m_useMultipleRefs) {
        if ((pRefLists->num_ref_idx_l0_active_minus1 + 1) > m_numPocStCurrBefore) {
            pRefLists->num_ref_idx_l0_active_minus1 = (uint8_t)(((m_numPocStCurrBefore - 1) >= 0) ? (m_numPocStCurrBefore - 1) : 0);
        }

        if ((picType == STD_VIDEO_H265_PICTURE_TYPE_B) && ((pRefLists->num_ref_idx_l1_active_minus1 + 1) > m_numPocStCurrAfter)) {
            pRefLists->num_ref_idx_l1_active_minus1 = (uint8_t)(((m_numPocStCurrAfter - 1) >= 0) ?  (m_numPocStCurrAfter - 1) : 0);
        }
    }

    for (int32_t refidx = 0; refidx < STD_VIDEO_H265_MAX_NUM_LIST_REF; refidx++) {
        pRefLists->RefPicList0[refidx] = STD_VIDEO_H265_NO_REFERENCE_PICTURE;
        pRefLists->RefPicList1[refidx] = STD_VIDEO_H265_NO_REFERENCE_PICTURE;
    }

    if ((picType == STD_VIDEO_H265_PICTURE_TYPE_P) || (picType == STD_VIDEO_H265_PICTURE_TYPE_B)) {
        uint8_t nNumRpsCurrTempList0 = std::max<uint8_t>((uint8_t)(pRefLists->num_ref_idx_l0_active_minus1 + 1), numPocTotalCurr);
        assert(nNumRpsCurrTempList0 <= STD_VIDEO_H265_MAX_NUM_LIST_REF);
        int8_t RefPicListTemp0[STD_VIDEO_H265_MAX_NUM_LIST_REF];
        uint8_t rIdx = 0;

        memset(RefPicListTemp0, 0, sizeof(RefPicListTemp0));

        while (rIdx < nNumRpsCurrTempList0) {
            for (int8_t i = 0; (i < m_numPocStCurrBefore) && (rIdx < nNumRpsCurrTempList0); rIdx++, i++) {
                RefPicListTemp0[rIdx] = pRefPicSet->stCurrBefore[i];
                isLongTerm[rIdx] = 0;
            }
            for (int8_t i = 0;  (i < m_numPocStCurrAfter) && (rIdx < nNumRpsCurrTempList0); rIdx++, i++) {
                RefPicListTemp0[rIdx] = pRefPicSet->stCurrAfter[i];
                isLongTerm[rIdx] = 0;
            }
            for (int8_t i = 0; (i < m_numPocLtCurr) && (rIdx < nNumRpsCurrTempList0); rIdx++, i++) {
                RefPicListTemp0[rIdx] = pRefPicSet->ltCurr[i];
                isLongTerm[rIdx] = 1;
            }
        }

        assert(pRefLists->num_ref_idx_l0_active_minus1 < STD_VIDEO_H265_MAX_NUM_LIST_REF);
        for (rIdx = 0; rIdx <= pRefLists->num_ref_idx_l0_active_minus1; rIdx++) {
            pRefLists->RefPicList0[rIdx] = pRefLists->flags.ref_pic_list_modification_flag_l0 ? RefPicListTemp0[pRefLists->list_entry_l0[rIdx]] : RefPicListTemp0[rIdx];
            m_longTermFlags |= pRefLists->flags.ref_pic_list_modification_flag_l0 ? (isLongTerm[pRefLists->list_entry_l0[rIdx]] << rIdx) : (isLongTerm[rIdx] << rIdx);
        }
    }

    if (picType == STD_VIDEO_H265_PICTURE_TYPE_B) {
        uint8_t nNumRpsCurrTempList1 = std::max<uint8_t>((uint8_t)(pRefLists->num_ref_idx_l1_active_minus1 + 1), numPocTotalCurr);
        assert(nNumRpsCurrTempList1 <= STD_VIDEO_H265_MAX_NUM_LIST_REF);
        int8_t RefPicListTemp1[STD_VIDEO_H265_MAX_NUM_LIST_REF];
        uint8_t rIdx = 0, i = 0;
        while (rIdx < nNumRpsCurrTempList1) {
            assert(m_numPocStCurrAfter < STD_VIDEO_H265_MAX_NUM_LIST_REF);
            for (i = 0; (i < m_numPocStCurrAfter) && (rIdx < nNumRpsCurrTempList1); rIdx++, i++) {
                RefPicListTemp1[rIdx] = pRefPicSet->stCurrAfter[i];
                isLongTerm[16 + rIdx] = 0;
            }
            assert(m_numPocStCurrBefore < STD_VIDEO_H265_MAX_NUM_LIST_REF);
            for (i = 0;  (i < m_numPocStCurrBefore) && (rIdx < nNumRpsCurrTempList1); rIdx++, i++) {
                RefPicListTemp1[rIdx] = pRefPicSet->stCurrBefore[i];
                isLongTerm[16 + rIdx] = 0;
            }
            assert(m_numPocLtCurr < STD_VIDEO_H265_MAX_NUM_LIST_REF);
            for (i = 0; (i < m_numPocLtCurr) && (rIdx<nNumRpsCurrTempList1); rIdx++, i++) {
                RefPicListTemp1[rIdx] = pRefPicSet->ltCurr[i];
                isLongTerm[16 + rIdx] = 1;
            }
        }

        assert(pRefLists->num_ref_idx_l1_active_minus1 < STD_VIDEO_H265_MAX_NUM_LIST_REF);
        for (rIdx = 0; rIdx <= pRefLists->num_ref_idx_l1_active_minus1; rIdx++) {
            pRefLists->RefPicList1[rIdx] = pRefLists->flags.ref_pic_list_modification_flag_l1 ? RefPicListTemp1[pRefLists->list_entry_l1[rIdx]] : RefPicListTemp1[rIdx];
            m_longTermFlags |= pRefLists->flags.ref_pic_list_modification_flag_l1 ? (isLongTerm[16 + pRefLists->list_entry_l1[rIdx]] << (16 + rIdx)) : (isLongTerm[16 + rIdx] << (16 + rIdx));
        }
    }
}

void VkEncDpbH265::InitializeShortTermRPSPFrame(int32_t numPocLtCurr,
                                                const StdVideoH265ShortTermRefPicSet *pSpsShortTermRps,
                                                uint8_t spsNumShortTermRefPicSets,
                                                StdVideoEncodeH265PictureInfo *pPicInfo,
                                                StdVideoH265ShortTermRefPicSet *pShortTermRefPicSet,
                                                uint32_t numRefL0, uint32_t numRefL1) {

    StdVideoH265ShortTermRefPicSet tmpSTRPS{};

    uint32_t shortTermRefPicPOCL0[STD_VIDEO_H265_MAX_DPB_SIZE];
    uint32_t deltaPocS0[STD_VIDEO_H265_MAX_DPB_SIZE];
    int32_t  usedByCurrPicS0[STD_VIDEO_H265_MAX_DPB_SIZE]{};
    uint32_t shortTermRefPicPOCL1[STD_VIDEO_H265_MAX_DPB_SIZE];
    uint32_t deltaPocS1[STD_VIDEO_H265_MAX_DPB_SIZE];
    int32_t  usedByCurrPicS1[STD_VIDEO_H265_MAX_DPB_SIZE]{};

    int32_t numNegativeRefPics = 0;
    int32_t numPositiveRefPics = 0;
    int32_t numLongTermRefPic = 0;
    int32_t numPocStCurrBefore = 0;
    int32_t numPocStCurrAfter = 0;
    int32_t maxStRefPicsCurr = 0;
    int32_t shortTermRefPicTemporalIdL0[16];
    int32_t curTemporalId = pPicInfo->TemporalId;
    bool    tsaPicture = false;

    uint32_t curPOC = pPicInfo->PicOrderCntVal;

    bool isIrapPic = pPicInfo->flags.IrapPicFlag;

    const StdVideoEncodeH265LongTermRefPics *pLongTermRefPics = pPicInfo->pLongTermRefPics;
    if ((pLongTermRefPics != nullptr) && (pLongTermRefPics->num_long_term_sps > 0)) {
        numLongTermRefPic = pLongTermRefPics->num_long_term_sps + pLongTermRefPics->num_long_term_pics;
    }
    for (int32_t i = 0; i < m_dpbSize; i++) {
        if ((m_stDpb[i].marking == 1) &&
                (m_stDpb[i].picOrderCntVal < curPOC) &&
                (!m_stDpb[i].corrupted) &&
                ((m_stDpb[i].temporalId < curTemporalId) || (!tsaPicture && (m_stDpb[i].temporalId == curTemporalId)))) {
            shortTermRefPicPOCL0[numNegativeRefPics] = m_stDpb[i].picOrderCntVal;
            shortTermRefPicTemporalIdL0[numNegativeRefPics] = m_stDpb[i].temporalId;
            deltaPocS0[numNegativeRefPics] = shortTermRefPicPOCL0[numNegativeRefPics] - curPOC;
            numNegativeRefPics++;
        }
        if (m_useMultipleRefs) {
            if ((m_stDpb[i].marking == 1) &&
                    (m_stDpb[i].picOrderCntVal > curPOC) &&
                    (!m_stDpb[i].corrupted) &&
                    ((m_stDpb[i].temporalId < curTemporalId) || (!tsaPicture && (m_stDpb[i].temporalId == curTemporalId)))) {
                shortTermRefPicPOCL1[numPositiveRefPics] = m_stDpb[i].picOrderCntVal;
                deltaPocS1[numPositiveRefPics] = shortTermRefPicPOCL1[numPositiveRefPics] - curPOC;
                numPositiveRefPics++;
            }
        }
    }

    // sort the negative pictures in decreasing order of POC value
    for(int32_t i = 0; i < numNegativeRefPics; i++) {
        for(int32_t j = 0; j < numNegativeRefPics - 1; j++) {
            if(shortTermRefPicPOCL0[j] < shortTermRefPicPOCL0[j+1]) {
                std::swap(shortTermRefPicPOCL0[j], shortTermRefPicPOCL0[j+1]);
                std::swap(deltaPocS0[j], deltaPocS0[j+1]);
                std::swap(shortTermRefPicTemporalIdL0[j], shortTermRefPicTemporalIdL0[j+1]);
            }
        }
    }

    // sort the positive pictures in increasing order of POC value
    if (m_useMultipleRefs) {
        for (int32_t i = 0; i < numPositiveRefPics; i++) {
            for (int32_t j = 0; j < numPositiveRefPics - 1; j++) {
                if (shortTermRefPicPOCL1[j] > shortTermRefPicPOCL1[j + 1]) {
                    std::swap(shortTermRefPicPOCL1[j], shortTermRefPicPOCL1[j + 1]);
                    std::swap(deltaPocS1[j], deltaPocS1[j + 1]);
                }
            }
        }

        // check if we exceed max num ref frames, try removing older  short term negative ref pics
        // since the negative list is sorted in decreasing order of POC , just decrease the numNegativeRefPics
        while ((numPocLtCurr + numNegativeRefPics + numPositiveRefPics) > (m_dpbSize - 1)) {
            // mark the oldest short term as unused for reference
            if (numNegativeRefPics > 0) {
                numNegativeRefPics--;
            } else if (numPositiveRefPics > 0) {
                numPositiveRefPics--;
            }
        }
    } else {
        while ((numLongTermRefPic + numNegativeRefPics + numPositiveRefPics) > (m_dpbSize - 1)) {
            // mark the oldest short term as unused for reference
            numNegativeRefPics--;
        }
    }

    // HEVC spec only allows max total number of reference frame to be used by current picture to be 8
    // mark older short term references unused for reference for the current frame, but still keep it in Dpb
    // They can used later for error recovery processes like invalidate reference frames
    const static int32_t maxAllowedNumRefFrames = 8;
    int32_t numPositiveRefPicsUsed = 0, numNegativeRefPicsUsed = 0;
    if (m_useMultipleRefs) {
        if (pPicInfo->pic_type == STD_VIDEO_H265_PICTURE_TYPE_B) {
            maxStRefPicsCurr = clampl((maxAllowedNumRefFrames - numPocLtCurr), 2);
            numPositiveRefPicsUsed = clampl((maxStRefPicsCurr - (int32_t)numRefL0), 1);
            numNegativeRefPicsUsed = maxStRefPicsCurr - numPositiveRefPicsUsed;
        } else {
            maxStRefPicsCurr = clampl((maxAllowedNumRefFrames - numPocLtCurr), 1);
            numNegativeRefPicsUsed = maxStRefPicsCurr;
        }
    } else {
        maxStRefPicsCurr = std::min(1, maxAllowedNumRefFrames - numPocLtCurr);
        numNegativeRefPicsUsed = maxStRefPicsCurr;
    }

    if (!isIrapPic) {
        for (int32_t i = 0; i < numNegativeRefPics; i++) {
            if (i < numNegativeRefPicsUsed && i < (int32_t)numRefL0) {
                assert(shortTermRefPicTemporalIdL0[i] <= curTemporalId);
                usedByCurrPicS0[i] = 1;
                numPocStCurrBefore++;
            } else {
                usedByCurrPicS0[i] = 0;
            }
        }

        if (m_useMultipleRefs) {
            for (int32_t i = 0; i < numPositiveRefPics; i++) {
                if (i < numPositiveRefPicsUsed && i < (int32_t)numRefL1) {
                    usedByCurrPicS1[i] = 1;
                    numPocStCurrAfter++;
                } else {
                    usedByCurrPicS1[i] = 0;
                }
            }
        }
    }

    if (isIrapPic) {
        assert((numPocStCurrBefore + numPocStCurrAfter + numPocLtCurr) == 0);
    } else {
        if ((numPocStCurrBefore + numPocStCurrAfter + numPocLtCurr) == 0) {
            assert(!"Invalid configuration - no reference pictures selected for inter picture");
            return;
        }
    }
    if (numNegativeRefPics || numPositiveRefPics) {
        tmpSTRPS.flags.inter_ref_pic_set_prediction_flag         = 0;
        tmpSTRPS.num_negative_pics                               = (uint8_t)numNegativeRefPics;
        tmpSTRPS.num_positive_pics                               = (uint8_t)numPositiveRefPics;
        int32_t prevDelta = 0;
        for (int32_t numStRefL0 = 0; numStRefL0 < tmpSTRPS.num_negative_pics; numStRefL0++) {
            tmpSTRPS.delta_poc_s0_minus1[numStRefL0] = (uint8_t)(prevDelta - deltaPocS0[numStRefL0] - 1);
            tmpSTRPS.used_by_curr_pic_s0_flag |= (uint16_t)((usedByCurrPicS0[numStRefL0] & 1) << numStRefL0);
            // tmpSTRPS.DeltaPocS0[numStRefL0]                      = iDeltaPocS0[numStRefL0];
            // tmpSTRPS.UsedByCurrPicS0[numStRefL0]                 = iUsedByCurrPicS0[numStRefL0];
            prevDelta = deltaPocS0[numStRefL0];
        }
        prevDelta = 0;
        if (m_useMultipleRefs) {
            for (int32_t numStRefL1 = 0; numStRefL1 < tmpSTRPS.num_positive_pics; numStRefL1++) {
                tmpSTRPS.delta_poc_s1_minus1[numStRefL1] = (uint8_t)(deltaPocS1[numStRefL1] - prevDelta - 1);
                tmpSTRPS.used_by_curr_pic_s1_flag |= (uint16_t)((usedByCurrPicS1[numStRefL1] & 1) << numStRefL1);
                // tmpSTRPS.DeltaPocS1[numStRefL1] = iDeltaPocS1[numStRefL1];
                // tmpSTRPS.UsedByCurrPicS1[numStRefL1] = iUsedByCurrPicS1[numStRefL1];
                prevDelta = deltaPocS1[numStRefL1];
            }
        }
    }
    int32_t iSPSSTRpsIdx = -1;
    bool bFound = false;
    for (int32_t i = 0; i < spsNumShortTermRefPicSets; i++) {
        // check if strps matches with one signalled in SPS
        if ((pSpsShortTermRps[i].num_negative_pics  == tmpSTRPS.num_negative_pics) &&
                (pSpsShortTermRps[i].num_positive_pics == tmpSTRPS.num_positive_pics)) {
            bFound = true;

            uint32_t used_by_curr_pic_sx_flag_xored =
                pSpsShortTermRps[i].used_by_curr_pic_s0_flag ^ tmpSTRPS.used_by_curr_pic_s0_flag;

            for (int32_t j = 0; j < pSpsShortTermRps[i].num_negative_pics; j++) {
                if ((pSpsShortTermRps[i].delta_poc_s0_minus1[j] != tmpSTRPS.delta_poc_s0_minus1[j]) ||
                        ((used_by_curr_pic_sx_flag_xored >> j) & 0x1)) {
                    bFound = false;
                    break;
                }
            }
            if (m_useMultipleRefs) {
                if (bFound == true) {
                    used_by_curr_pic_sx_flag_xored =
                        pSpsShortTermRps[i].used_by_curr_pic_s1_flag ^ tmpSTRPS.used_by_curr_pic_s1_flag;

                    for (int32_t j = 0; j < pSpsShortTermRps[i].num_positive_pics; j++) {
                        if ((pSpsShortTermRps[i].delta_poc_s1_minus1[j] != tmpSTRPS.delta_poc_s1_minus1[j]) ||
                                ((used_by_curr_pic_sx_flag_xored >> j) & 0x1)) {
                            bFound = false;
                            break;
                        }
                    }
                }
            }
        }
        if (bFound == true) {
            iSPSSTRpsIdx = i;
            break;
        }
    }

    if (iSPSSTRpsIdx >= 0) {
        pPicInfo->flags.short_term_ref_pic_set_sps_flag = 1;
        pPicInfo->short_term_ref_pic_set_idx = (uint8_t)iSPSSTRpsIdx;
    } else {
        pPicInfo->flags.short_term_ref_pic_set_sps_flag = 0;
        *pShortTermRefPicSet = tmpSTRPS;
    }
}

void VkEncDpbH265::ReferencePictureMarking(int32_t curPOC, StdVideoH265PictureType picType,
                                           bool longTermRefPicsPresentFlag) {
    if (picType == STD_VIDEO_H265_PICTURE_TYPE_IDR) {
        for (int32_t i = 0; i < m_dpbSize; i++)
            m_stDpb[i].marking = 0;

    } else {
        // TL pictures can't use LD pictures as reference
        if ((m_refreshPending == true) && (curPOC > m_picOrderCntCRA)) { // CRA reference marking pending
            for (int32_t i = 0; i < m_dpbSize; i++) {
                if (m_stDpb[i].picOrderCntVal != (uint32_t)m_picOrderCntCRA)
                    m_stDpb[i].marking = 0;
            }
            m_refreshPending = false;
        }

        if (picType == STD_VIDEO_H265_PICTURE_TYPE_I) { // CRA picture found
            m_refreshPending = true;
            m_picOrderCntCRA = curPOC;
        }

        if (m_useMultipleRefs) {
            int32_t numLongTermRefPics = 0;
            int32_t numShortTermRefPics = 0;
            int32_t numCorruptedRefPics = 0;
            int32_t minPOCSTIdx = -1;
            uint32_t minPOCSTVal = UINT_MAX;
            int32_t minPocLTIdx = -1;
            uint32_t minPocLTVal = UINT_MAX;
            int32_t minPOCCorruptedIdx = -1;
            uint32_t minPocCorruptedVal = UINT_MAX;

            for (int32_t i = 0; i < m_dpbSize; i++) {
                if ((m_stDpb[i].state == 1) && (m_stDpb[i].marking == 1) && !m_stDpb[i].corrupted) {
                    numShortTermRefPics++;
                    if (m_stDpb[i].picOrderCntVal < minPOCSTVal) {
                        minPOCSTVal = m_stDpb[i].picOrderCntVal;
                        minPOCSTIdx = i;
                    }
                }
                if ((m_stDpb[i].state == 1) && (m_stDpb[i].marking == 2) && !m_stDpb[i].corrupted) {
                    numLongTermRefPics++;
                    if (m_stDpb[i].picOrderCntVal < minPocLTVal) {
                        minPocLTVal = m_stDpb[i].picOrderCntVal;
                        minPocLTIdx = i;
                    }
                }
                if ((m_stDpb[i].state == 1) && m_stDpb[i].corrupted) {
                    numCorruptedRefPics++;
                    if (m_stDpb[i].picOrderCntVal < minPocCorruptedVal) {
                        minPocCorruptedVal = m_stDpb[i].picOrderCntVal;
                        minPOCCorruptedIdx = i;
                    }
                }
            }

            if (!longTermRefPicsPresentFlag) {
                if (((numShortTermRefPics + numLongTermRefPics + numCorruptedRefPics) > (m_dpbSize - 1))) {
                    if (numCorruptedRefPics && (minPocCorruptedVal < minPOCSTVal) &&
                            (minPOCCorruptedIdx >= 0) && (minPOCCorruptedIdx < m_dpbSize)) {
                        m_stDpb[minPOCCorruptedIdx].marking = 0;
                    } else if ((numShortTermRefPics) && (minPOCSTIdx >= 0) && (minPOCSTIdx < m_dpbSize)) {
                        m_stDpb[minPOCSTIdx].marking = 0;
                    } else if ((numLongTermRefPics) && (minPocLTIdx >= 0) && (minPocLTIdx < m_dpbSize)) {
                        m_stDpb[minPocLTIdx].marking = 0;
                    }
                }
            } else {
                assert(picType != STD_VIDEO_H265_PICTURE_TYPE_B);
                // In order to achieve a balance between the number of LTR and STR frames, the number of LTR frames should not exceed 50%
                // of the active references in Dpb
                int32_t num_active_ref_frames = numShortTermRefPics + numLongTermRefPics + numCorruptedRefPics;
                int32_t max_allowed_ltr_frames = 0; // std::min(m_stHEVCEncInitParams.iMaxNumLongTermRefPics, num_active_ref_frames / 2);
                if (num_active_ref_frames > (m_dpbSize - 1)) {
                    // If number of LTR in Dpb > max_allowed_ltr_frames, mark the earliest
                    // LTR as unused for reference else mark the STR as unused for reference
                    // This logic will help in maintaining separate queues for LTR and STR frames
                    if (numCorruptedRefPics && (minPocCorruptedVal < minPOCSTVal) && (minPOCCorruptedIdx >= 0) && (minPOCCorruptedIdx < m_dpbSize)) {
                        m_stDpb[minPOCCorruptedIdx].marking = 0;
                    } else if ((numLongTermRefPics > max_allowed_ltr_frames) && (minPocLTIdx < m_dpbSize) && (minPocLTIdx >= 0)) {
                        m_stDpb[minPocLTIdx].marking = 0;
                    } else if (numShortTermRefPics && (minPOCSTIdx < m_dpbSize) && (minPOCSTIdx >= 0)) {
                        m_stDpb[minPOCSTIdx].marking = 0;
                    }
                }
            }
        }
    }
}

void VkEncDpbH265::InitializeRPS(const StdVideoH265ShortTermRefPicSet *pSpsShortTermRps,
                                 uint8_t spsNumShortTermRefPicSets,
                                 StdVideoEncodeH265PictureInfo *pPicInfo,
                                 StdVideoH265ShortTermRefPicSet *pShortTermRefPicSet,
                                 uint32_t numRefL0, uint32_t numRefL1) {
    int32_t numPocLtCurr = 0;

    InitializeShortTermRPSPFrame(numPocLtCurr, pSpsShortTermRps, spsNumShortTermRefPicSets,
                                 pPicInfo, pShortTermRefPicSet, numRefL0, numRefL1);
}
