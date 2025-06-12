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


// This is outside the driver
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <algorithm>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdint.h>

#include "VkEncoderDpbH264.h"

#define VK_DPB_DBG_PRINT(expr) printf expr

#define MARKING_UNUSED 0  // unused for reference
#define MARKING_SHORT 1   // used for short-term reference
#define MARKING_LONG 2    // used for long-term reference

#define INF_MIN ((int32_t)(1 << 31))
#define INF_MAX (~(1 << 31))

enum DpbStateH264 { DPB_EMPTY = 0, DPB_TOP, DPB_BOTTOM, DPB_FRAME };

// helper functions for refpic list intialization and reoirdering
static bool sort_check_short_term_P_frame(const DpbEntryH264 *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->topPicNum;
    return ((pDBP->top_field_marking == MARKING_SHORT) && (pDBP->bottom_field_marking == MARKING_SHORT));
}

static bool sort_check_short_term_P_field(const DpbEntryH264 *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->frameNumWrap;
    return (pDBP->top_field_marking == MARKING_SHORT) || (pDBP->bottom_field_marking == MARKING_SHORT);
}

static bool sort_check_short_term_B_frame(const DpbEntryH264 *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->picInfo.PicOrderCnt;
    return !((pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0) && pDBP->not_existing) && (pDBP->top_field_marking == MARKING_SHORT) &&
           (pDBP->bottom_field_marking == MARKING_SHORT);
}

static bool sort_check_short_term_B_field(const DpbEntryH264 *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->picInfo.PicOrderCnt;
    return !((pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0) && pDBP->not_existing) &&
           ((pDBP->top_field_marking == MARKING_SHORT) || (pDBP->bottom_field_marking == MARKING_SHORT));
}

static bool sort_check_long_term_frame(const DpbEntryH264 *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->topLongTermPicNum;
    return (pDBP->top_field_marking == MARKING_LONG) && (pDBP->bottom_field_marking == MARKING_LONG);
}

static bool sort_check_long_term_field(const DpbEntryH264 *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->longTermFrameIdx;
    return (pDBP->top_field_marking == MARKING_LONG) || (pDBP->bottom_field_marking == MARKING_LONG);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
VkEncDpbH264::VkEncDpbH264()
    : m_maxLongTermFrameIdx(0),
      m_max_dpb_size(0),
      m_prevPicOrderCntMsb(0),
      m_prevPicOrderCntLsb(0),
      m_prevFrameNumOffset(0),
      m_prevFrameNum(0),
      m_PrevRefFrameNum(0),
      m_currDpbIdx(0),
      m_lastIDRTimeStamp(0)
{
    memset(m_max_num_list, 0, sizeof(m_max_num_list));
}

VkEncDpbH264::~VkEncDpbH264() {}
VkEncDpbH264 *VkEncDpbH264::CreateInstance(void)
{
    VkEncDpbH264 *pDpb = new VkEncDpbH264();
    if (pDpb) {
        pDpb->DpbInit();
    }
    return pDpb;
}

void VkEncDpbH264::ReleaseFrame(VkSharedBaseObj<VulkanVideoImagePoolNode>&  dpbImageView)
{
    dpbImageView = nullptr;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VkEncDpbH264::DpbInit()
{
    m_max_dpb_size = 0;
    m_max_num_list[0] = 0;
    m_max_num_list[1] = 0;
    m_currDpbIdx = -1;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VkEncDpbH264::DpbDeinit()
{
    m_max_dpb_size = 0;
    m_currDpbIdx = 0;
    m_lastIDRTimeStamp = 0;
    m_currDpbIdx = -1;
};

void VkEncDpbH264::DpbDestroy()
{
    FlushDpb();
    DpbDeinit();

    delete this;
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
//
// The number of entries DPB_N should at least be equal to the max number of references (R) + decoded pictures that cannot
// be displayed yet + 1 (current picture to be reconstructed). At the end of the reconstruction of the current picture,
// if it is not a reference picture and can be displayed, the picture will not be part of the fullness of the DPB. The number
// of entries DPB_N = dpb_size (as viewed by H264 std) + 1
// returns -1 if err
int32_t VkEncDpbH264::DpbSequenceStart(int32_t userDpbSize)
{
    int32_t i;

    DpbDeinit();

    m_max_dpb_size = userDpbSize;

    for (i = 0; i < MAX_DPB_SLOTS + 1; i++) {
        m_DPB[i] = DpbEntryH264();
    }

    if (1)  //(!no_output_of_prior_pics_flag)
        FlushDpb();

    return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
int8_t VkEncDpbH264::DpbPictureStart(const PicInfoH264 *pPicInfo,
                                     const StdVideoH264SequenceParameterSet *sps)
{
    FillFrameNumGaps(pPicInfo, sps);

    // select decoded picture buffer

    // check if this is the second field of a complementary field pair
    //
    // 3.30 complementary non-reference field pair:
    // Two non-reference fields that are in consecutive access units in decoding order as
    // - two coded fields of opposite parity where
    // - the first field is not already a paired field.
    //
    // 3.31 complementary reference field pair:
    // Two reference fields that are in consecutive access units in decoding order as
    // - two coded fields and
    // - share the same value of the frame_num syntax element, where
    // - the second field in decoding order is not an IDR picture and
    // - does not include a memory_management_control_operation syntax element equal to 5.

    // TODO: what if there is no current picture?
    if (((m_DPB[m_currDpbIdx].state == DPB_TOP) || (m_DPB[m_currDpbIdx].state == DPB_BOTTOM)) &&  // contains a single field
            pPicInfo->field_pic_flag &&                                                      // current is a field
            (((m_DPB[m_currDpbIdx].state == DPB_TOP) && pPicInfo->bottom_field_flag) ||
             ((m_DPB[m_currDpbIdx].state == DPB_BOTTOM) && !pPicInfo->bottom_field_flag)) &&  // opposite parity
            ((!m_DPB[m_currDpbIdx].reference_picture &&                                          // first is a non-reference picture
              !pPicInfo->flags.is_reference)                                                             // current is a non-reference picture
             || (m_DPB[m_currDpbIdx].reference_picture &&                                        // first is reference picture
                 pPicInfo->flags.is_reference &&                                                         // current is reference picture
                 (m_DPB[m_currDpbIdx].picInfo.frame_num == pPicInfo->frame_num) &&           // same frame_num
                 !pPicInfo->flags.IdrPicFlag))) {                                                      // current is not an IDR picture
        // second field
        m_DPB[m_currDpbIdx].complementary_field_pair = true;
    } else {
        m_currDpbIdx = MAX_DPB_SLOTS;
        DpbEntryH264 *pCurDPBEntry = &m_DPB[m_currDpbIdx];
        if (pCurDPBEntry->state != DPB_EMPTY) {
            OutputPicture(m_currDpbIdx, true);
        }

        // initialize DPB frame buffer
        pCurDPBEntry->state = DPB_EMPTY;
        pCurDPBEntry->top_needed_for_output = pCurDPBEntry->bottom_needed_for_output = false;
        pCurDPBEntry->top_field_marking = pCurDPBEntry->bottom_field_marking = MARKING_UNUSED;
        pCurDPBEntry->reference_picture = pPicInfo->flags.is_reference;
        pCurDPBEntry->top_decoded_first = !pPicInfo->bottom_field_flag;
        pCurDPBEntry->complementary_field_pair = false;
        pCurDPBEntry->not_existing = false;
        pCurDPBEntry->picInfo.frame_num = pPicInfo->frame_num;
        pCurDPBEntry->timeStamp = pPicInfo->timeStamp;
        pCurDPBEntry->frame_is_corrupted = false;
        if (pPicInfo->flags.IdrPicFlag) {
            m_lastIDRTimeStamp = pPicInfo->timeStamp;
        }
    }

    CalculatePOC(pPicInfo, sps);
    CalculatePicNum(pPicInfo, sps);


    return m_currDpbIdx;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// per picture processing after decoding last slice
int8_t VkEncDpbH264::DpbPictureEnd(const PicInfoH264 *pPicInfo,
                                   VkSharedBaseObj<VulkanVideoImagePoolNode>&  dpbImageView,
                                   const StdVideoH264SequenceParameterSet *sps,
                                   const StdVideoEncodeH264SliceHeader *slh,
                                   const StdVideoEncodeH264ReferenceListsInfo *ref,
                                   uint32_t maxMemMgmntCtrlOpsCommands)
{
    DpbEntryH264 *pCurDPBEntry = &m_DPB[m_currDpbIdx];
    if (pCurDPBEntry->complementary_field_pair)  // second field of a CFP
        pCurDPBEntry->picInfo.PicOrderCnt = std::min(pCurDPBEntry->topFOC, pCurDPBEntry->bottomFOC);

    if (pPicInfo->flags.is_reference)  // reference picture
        DecodedRefPicMarking(pPicInfo, sps, slh, ref, maxMemMgmntCtrlOpsCommands);

    // C.4.4 Removal of pictures from the DPB before possible insertion of the current picture
    if (pPicInfo->flags.IdrPicFlag) { // IDR picture
        for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
            m_DPB[i].top_field_marking = MARKING_UNUSED;
            m_DPB[i].bottom_field_marking = MARKING_UNUSED;
            m_DPB[i].state = MARKING_UNUSED;
            ReleaseFrame(m_DPB[i].dpbImageView);
        }
        // TODO: infer no_output_of_prior_pics_flag if size has changed etc.
        if (pPicInfo->flags.no_output_of_prior_pics_flag) {
            for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
                m_DPB[i].state = DPB_EMPTY;  // empty
                ReleaseFrame(m_DPB[i].dpbImageView);
            }
        }
    }

    if ((pPicInfo->flags.IdrPicFlag && !pPicInfo->flags.no_output_of_prior_pics_flag)) {
        while (!IsDpbEmpty()) DpbBumping(false);
    }

    // C.4.5

    if (pPicInfo->flags.is_reference) { // reference picture
        // C.4.5.1
        if (pCurDPBEntry->state == DPB_EMPTY) {
            while (IsDpbFull()) {
                DpbBumping(true);
            }

            // find an empty DPB entry, copy current to it
            for (m_currDpbIdx = 0; m_currDpbIdx < MAX_DPB_SLOTS; m_currDpbIdx++) {
                if (m_DPB[m_currDpbIdx].state == DPB_EMPTY)
                    break;
            }
            if (pCurDPBEntry != &m_DPB[m_currDpbIdx]) {
                ReleaseFrame(m_DPB[m_currDpbIdx].dpbImageView);
                m_DPB[m_currDpbIdx] = *pCurDPBEntry;
            }
            pCurDPBEntry = &m_DPB[m_currDpbIdx];
        }

        if (!pPicInfo->field_pic_flag || !pPicInfo->bottom_field_flag) {
            pCurDPBEntry->state |= DPB_TOP;
            pCurDPBEntry->top_needed_for_output = true;
        }
        if (!pPicInfo->field_pic_flag || pPicInfo->bottom_field_flag) {
            pCurDPBEntry->state |= DPB_BOTTOM;
            pCurDPBEntry->bottom_needed_for_output = true;
        }
    } else {
        // C.4.5.2
        if (pCurDPBEntry->state != DPB_EMPTY) {
            if (m_currDpbIdx >= MAX_DPB_SLOTS) {
                // output immediately
                OutputPicture(m_currDpbIdx, true);
                m_DPB[m_currDpbIdx].top_needed_for_output = 0;
                m_DPB[m_currDpbIdx].bottom_needed_for_output = 0;
                pCurDPBEntry->state = DPB_EMPTY;
            } else {
                // second field of a complementary non-reference field pair
                pCurDPBEntry->state = DPB_FRAME;
                pCurDPBEntry->top_needed_for_output = true;
                pCurDPBEntry->bottom_needed_for_output = true;
            }
        } else {
            while (1) {
                if (IsDpbFull()) {
                    int32_t i = 0;
                    // does current have the lowest value of PicOrderCnt?
                    for (; i < MAX_DPB_SLOTS; i++) {
                        // If we decide to support MVC, the following check must
                        // be performed only if the view_id of the current DPB
                        // entry matches the view_id in m_DPB[i].

                        assert(m_DPB[i].topFOC >= 0);
                        assert(m_DPB[i].bottomFOC >= 0);

                        if (((m_DPB[i].state & DPB_TOP) && m_DPB[i].top_needed_for_output &&
                                ((int32_t)m_DPB[i].topFOC) <= pCurDPBEntry->picInfo.PicOrderCnt) ||
                                ((m_DPB[i].state & DPB_BOTTOM) && m_DPB[i].bottom_needed_for_output &&
                                 ((int32_t)m_DPB[i].bottomFOC) <= pCurDPBEntry->picInfo.PicOrderCnt))
                            break;
                    }
                    if (i < MAX_DPB_SLOTS) {
                        DpbBumping(false);
                    } else {
                        // DPB is full, current has lowest value of PicOrderCnt
                        if (!pPicInfo->field_pic_flag) {
                            // frame: output current picture immediately
                            OutputPicture(m_currDpbIdx, true);
                        } else {
                            // field: wait for second field
                            if (!pPicInfo->bottom_field_flag) {
                                pCurDPBEntry->state |= DPB_TOP;
                                pCurDPBEntry->top_needed_for_output = true;
                            } else {
                                pCurDPBEntry->state |= DPB_BOTTOM;
                                pCurDPBEntry->bottom_needed_for_output = true;
                            }
                        }

                        break;  // exit while (1)
                    }
                } else {
                    for (m_currDpbIdx = 0; m_currDpbIdx < MAX_DPB_SLOTS; m_currDpbIdx++) {
                        if (m_DPB[m_currDpbIdx].state == DPB_EMPTY) break;
                    }
                    if (pCurDPBEntry != &m_DPB[m_currDpbIdx]) {
                        ReleaseFrame(m_DPB[m_currDpbIdx].dpbImageView);
                        m_DPB[m_currDpbIdx] = *pCurDPBEntry;
                    }
                    pCurDPBEntry = &m_DPB[m_currDpbIdx];
                    // store current picture
                    if (!pPicInfo->field_pic_flag || !pPicInfo->bottom_field_flag) {
                        pCurDPBEntry->state |= DPB_TOP;
                        pCurDPBEntry->top_needed_for_output = true;
                    }
                    if (!pPicInfo->field_pic_flag || pPicInfo->bottom_field_flag) {
                        pCurDPBEntry->state |= DPB_BOTTOM;
                        pCurDPBEntry->bottom_needed_for_output = true;
                    }

                    break;  // exit while (1)
                }
            }
        }
    }

    assert(pCurDPBEntry);
    if (pCurDPBEntry) {
        pCurDPBEntry->dpbImageView = dpbImageView;
    }

    return m_currDpbIdx;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.5.2
void VkEncDpbH264::FillFrameNumGaps(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps)
{
    int32_t maxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);

    // 7.4.3
    if (pPicInfo->flags.IdrPicFlag)  // IDR picture
        m_PrevRefFrameNum = 0;

    if (pPicInfo->frame_num != m_PrevRefFrameNum) {
        PicInfoH264 picSave = *pPicInfo;

        // (7-10)
        uint32_t unusedShortTermFrameNum = (m_PrevRefFrameNum + 1) % maxFrameNum;
        while (unusedShortTermFrameNum != picSave.frame_num) {
            VK_DPB_DBG_PRINT(("gaps_in_frame_num: %d ", unusedShortTermFrameNum));

            if (!sps->flags.gaps_in_frame_num_value_allowed_flag) {
                VK_DPB_DBG_PRINT(("%s (error)::gap in frame_num not allowed\n", __FUNCTION__));
                break;
            }
            picSave.frame_num = unusedShortTermFrameNum;
            picSave.field_pic_flag = 0;
            picSave.bottom_field_flag = 0;
            picSave.flags.is_reference = 1;
            picSave.flags.IdrPicFlag = 0;
            picSave.flags.adaptive_ref_pic_marking_mode_flag = 0;

            // TODO: what else
            // DPB handling (C.4.2)
            while (IsDpbFull()) DpbBumping(true);
            for (m_currDpbIdx = 0; m_currDpbIdx < MAX_DPB_SLOTS; m_currDpbIdx++) {
                if (m_DPB[m_currDpbIdx].state == DPB_EMPTY) {
                    break;
                }
            }
            if (m_currDpbIdx >= MAX_DPB_SLOTS) VK_DPB_DBG_PRINT(("%s (error)::could not allocate a frame buffer\n", __FUNCTION__));
            // initialize DPB frame buffer
            DpbEntryH264 *pCurDPBEntry = &m_DPB[m_currDpbIdx];
            pCurDPBEntry->picInfo.frame_num = pPicInfo->frame_num;
            pCurDPBEntry->complementary_field_pair = false;
            if (sps->pic_order_cnt_type != STD_VIDEO_H264_POC_TYPE_0) CalculatePOC(&picSave, sps);
            CalculatePicNum(&picSave,sps);

            SlidingWindowMemoryManagememt(&picSave, sps);

            pCurDPBEntry->top_field_marking = pCurDPBEntry->bottom_field_marking = MARKING_SHORT;
            pCurDPBEntry->reference_picture = true;
            pCurDPBEntry->top_decoded_first = false;
            pCurDPBEntry->not_existing = true;
            // C.4.2
            pCurDPBEntry->top_needed_for_output = pCurDPBEntry->bottom_needed_for_output = false;
            pCurDPBEntry->state = DPB_FRAME;  // frame
            // this differs from the standard
            // empty frame buffers marked as "not needed for output" and "unused for reference"
            for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
                if ((!(m_DPB[i].state & DPB_TOP) ||
                        (!m_DPB[i].top_needed_for_output && m_DPB[i].top_field_marking == MARKING_UNUSED)) &&
                        (!(m_DPB[i].state & DPB_BOTTOM) ||
                         (!m_DPB[i].bottom_needed_for_output && m_DPB[i].bottom_field_marking == MARKING_UNUSED))) {
                    m_DPB[i].state = DPB_EMPTY;  // empty
                    ReleaseFrame(m_DPB[i].dpbImageView);
                }
            }

            // 7.4.3
            m_PrevRefFrameNum = pPicInfo->frame_num;  // TODO: only if previous picture was a reference picture?
            unusedShortTermFrameNum = (unusedShortTermFrameNum + 1) % maxFrameNum;
        }
    }

    // 7.4.3
    if (pPicInfo->flags.is_reference)  // reference picture
        m_PrevRefFrameNum = pPicInfo->frame_num;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// DPB
bool VkEncDpbH264::IsDpbFull()
{
    int32_t dpb_fullness, i;

    dpb_fullness = 0;
    for (i = 0; i < MAX_DPB_SLOTS; i++) {
        if (m_DPB[i].state != DPB_EMPTY) dpb_fullness++;
    }

    return dpb_fullness >= m_max_dpb_size;
}

bool VkEncDpbH264::IsDpbEmpty()
{
    int32_t dpb_fullness, i;

    dpb_fullness = 0;
    for (i = 0; i < MAX_DPB_SLOTS; i++) {
        if (m_DPB[i].state != DPB_EMPTY) dpb_fullness++;
    }

    return dpb_fullness == 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// C.4.5.3
void VkEncDpbH264::DpbBumping(bool alwaysbump)
{
    // If we decide to implement MVC, we'll need to loop over all the views
    // configured for this session and perform each check in the for loop
    // immediately below only if the current DPB entry's view_id matches
    // that of m_DPB[i].

    // select the frame buffer that contains the picture having the smallest value
    // of PicOrderCnt of all pictures in the DPB marked as "needed for output"
    int32_t pocMin = INF_MAX;
    int32_t minFoc = -1;
    int32_t prevOutputIdx = -1;
    for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
        if ((m_DPB[i].state & DPB_TOP) && m_DPB[i].top_needed_for_output && (m_DPB[i].topFOC < pocMin)) {
            pocMin = m_DPB[i].topFOC;
            minFoc = i;
        }
        if ((m_DPB[i].state & DPB_BOTTOM) && m_DPB[i].bottom_needed_for_output && (m_DPB[i].bottomFOC < pocMin)) {
            pocMin = m_DPB[i].bottomFOC;
            minFoc = i;
        }
    }

    if (minFoc >= 0) {
        OutputPicture(minFoc, false);
        m_DPB[minFoc].top_needed_for_output = 0;
        m_DPB[minFoc].bottom_needed_for_output = 0;
        prevOutputIdx = minFoc;

        // empty frame buffer
        if ((!(m_DPB[minFoc].state & DPB_TOP) ||
                (!m_DPB[minFoc].top_needed_for_output && m_DPB[minFoc].top_field_marking == MARKING_UNUSED)) &&
                (!(m_DPB[minFoc].state & DPB_BOTTOM) ||
                 (!m_DPB[minFoc].bottom_needed_for_output && m_DPB[minFoc].bottom_field_marking == MARKING_UNUSED))) {
            m_DPB[minFoc].state = 0;
            ReleaseFrame(m_DPB[minFoc].dpbImageView);
        }
    }

    // Special case to avoid deadlocks
    if ((prevOutputIdx < 0) && (alwaysbump)) {
        for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
            if ((m_DPB[i].state & DPB_TOP) && (m_DPB[i].topFOC <= pocMin)) {
                pocMin = m_DPB[i].topFOC;
                minFoc = i;
            }
            if ((m_DPB[i].state & DPB_BOTTOM) && (m_DPB[i].bottomFOC <= pocMin)) {
                pocMin = m_DPB[i].bottomFOC;
                minFoc = i;
            }
        }
        m_DPB[minFoc].state = DPB_EMPTY;
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.5, 8.2.5.1
void VkEncDpbH264::DecodedRefPicMarking(const PicInfoH264 *pPicInfo,
                                        const StdVideoH264SequenceParameterSet *sps,
                                        const StdVideoEncodeH264SliceHeader *slh,
                                        const StdVideoEncodeH264ReferenceListsInfo *ref,
                                        uint32_t maxMemMgmntCtrlOpsCommands)
{
    DpbEntryH264 *pCurDPBEntry = &m_DPB[m_currDpbIdx];
    if (pPicInfo->flags.IdrPicFlag) { // IDR picture
        // All reference pictures shall be marked as "unused for reference"
        for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
            m_DPB[i].top_field_marking = MARKING_UNUSED;
            m_DPB[i].bottom_field_marking = MARKING_UNUSED;
        }
        if (!pPicInfo->flags.long_term_reference_flag) {
            // the IDR picture shall be marked as "used for short-term reference"
            if (!pPicInfo->field_pic_flag || !pPicInfo->bottom_field_flag)
                pCurDPBEntry->top_field_marking = MARKING_SHORT;
            if (!pPicInfo->field_pic_flag || pPicInfo->bottom_field_flag)
                pCurDPBEntry->bottom_field_marking = MARKING_SHORT;
            // MaxLongTermFrameIdx shall be set equal to "no long-term frame indices".
            m_maxLongTermFrameIdx = -1;
        } else { // (slh->long_term_reference_flag == 1)
            // the IDR picture shall be marked as "used for long-term reference"
            if (!pPicInfo->field_pic_flag || !pPicInfo->bottom_field_flag)
                pCurDPBEntry->top_field_marking = MARKING_LONG;
            if (!pPicInfo->field_pic_flag || pPicInfo->bottom_field_flag)
                pCurDPBEntry->bottom_field_marking = MARKING_LONG;
            // the LongTermFrameIdx for the IDR picture shall be set equal to 0
            pCurDPBEntry->longTermFrameIdx = 0;
            // MaxLongTermFrameIdx shall be set equal to 0.
            m_maxLongTermFrameIdx = 0;
        }
    } else {
        if (!pPicInfo->flags.adaptive_ref_pic_marking_mode_flag)
            SlidingWindowMemoryManagememt(pPicInfo, sps);
        else  // (slh->adaptive_ref_pic_marking_mode_flag == 1)
            AdaptiveMemoryManagement(pPicInfo, ref, maxMemMgmntCtrlOpsCommands);

        // mark current as short-term if not marked as long-term (8.2.5.1)
        if ((!pPicInfo->field_pic_flag || !pPicInfo->bottom_field_flag) &&
                pCurDPBEntry->top_field_marking == MARKING_UNUSED)
            pCurDPBEntry->top_field_marking = MARKING_SHORT;
        if ((!pPicInfo->field_pic_flag || pPicInfo->bottom_field_flag) &&
                pCurDPBEntry->bottom_field_marking == MARKING_UNUSED)
            pCurDPBEntry->bottom_field_marking = MARKING_SHORT;
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.5.3
void VkEncDpbH264::SlidingWindowMemoryManagememt(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps)
{
    // If the current picture is a coded field that is the second field in decoding order
    // of a complementary reference field pair, and the first field has been marked as
    // "used for short-term reference", the current picture is also marked as
    // "used for short-term reference".
    // note: I think this could be simplified as
    // if (m_pCurDPBEntry->top_field_marking == MARKING_SHORT || m_pCurDPBEntry->bottom_field_marking == MARKING_SHORT)
    DpbEntryH264 *pCurDPBEntry = &m_DPB[m_currDpbIdx];
    if (pPicInfo->field_pic_flag &&
            ((!pPicInfo->bottom_field_flag && pCurDPBEntry->bottom_field_marking == MARKING_SHORT) ||
             (pPicInfo->bottom_field_flag && pCurDPBEntry->top_field_marking == MARKING_SHORT))) {
        if (!pPicInfo->bottom_field_flag)
            pCurDPBEntry->top_field_marking = MARKING_SHORT;
        else
            pCurDPBEntry->bottom_field_marking = MARKING_SHORT;
    } else {
        int32_t imin = MAX_DPB_SLOTS;
        int32_t minFrameNumWrap = 65536;
        int32_t numShortTerm = 0;
        int32_t numLongTerm = 0;
        for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
            // If we decide to implement MVC, the checks in this loop must only be
            // performed if the view_id from the current DPB entry matches that of
            // m_DPB[i].

            if ((m_DPB[i].top_field_marking == MARKING_SHORT || m_DPB[i].bottom_field_marking == MARKING_SHORT)) {
                numShortTerm++;
                if (m_DPB[i].frameNumWrap < minFrameNumWrap) {
                    imin = i;
                    minFrameNumWrap = m_DPB[i].frameNumWrap;
                }
            }

            if (m_DPB[i].top_field_marking == MARKING_LONG || m_DPB[i].bottom_field_marking == MARKING_LONG) {
                numLongTerm++;
            }
        }
        if ((numShortTerm + numLongTerm) >= sps->max_num_ref_frames) {
            if (numShortTerm > 0 && imin < MAX_DPB_SLOTS) {
                m_DPB[imin].top_field_marking = MARKING_UNUSED;
                m_DPB[imin].bottom_field_marking = MARKING_UNUSED;
            } else {
                VK_DPB_DBG_PRINT(("Detected DPB violation (%d+%d/%d)!\n", numShortTerm, numLongTerm, sps->max_num_ref_frames));
            }
        }
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.5.4
void VkEncDpbH264::AdaptiveMemoryManagement(const PicInfoH264 *pPicInfo, const StdVideoEncodeH264ReferenceListsInfo *ref,
                                            uint32_t maxMemMgmntCtrlOpsCommands)
{
    const StdVideoEncodeH264RefPicMarkingEntry *mmco = ref->pRefPicMarkingOperations;

    int32_t currPicNum = (!pPicInfo->field_pic_flag) ? pPicInfo->frame_num : 2 * pPicInfo->frame_num + 1;
    int32_t picNumX = 0;
    for (uint32_t k = 0; ((k < maxMemMgmntCtrlOpsCommands) && (mmco[k].memory_management_control_operation != STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_END)); k++) {
        switch (mmco[k].memory_management_control_operation) {
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_UNMARK_SHORT_TERM:
            // 8.2.5.4.1 Marking process of a short-term picture as "unused for reference"
            VK_DPB_DBG_PRINT(("%d ", mmco[k].difference_of_pic_nums_minus1));

            picNumX = currPicNum - (mmco[k].difference_of_pic_nums_minus1 + 1);  // (8-40)
            for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
                // If we decide to implement MVC, the checks in this loop must only be
                // performed if the view_id from the current DPB entry matches that of
                // m_DPB[i].

                if (m_DPB[i].top_field_marking == MARKING_SHORT && m_DPB[i].topPicNum == picNumX)
                    m_DPB[i].top_field_marking = MARKING_UNUSED;
                if (m_DPB[i].bottom_field_marking == MARKING_SHORT && m_DPB[i].bottomPicNum == picNumX)
                    m_DPB[i].bottom_field_marking = MARKING_UNUSED;
            }
            break;
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_UNMARK_LONG_TERM:
            // 8.2.5.4.2 Marking process of a long-term picture as "unused for reference"
            for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
                if (m_DPB[i].top_field_marking == MARKING_LONG && m_DPB[i].topLongTermPicNum == (int32_t)mmco[k].long_term_pic_num)
                    m_DPB[i].top_field_marking = MARKING_UNUSED;
                if (m_DPB[i].bottom_field_marking == MARKING_LONG &&
                        m_DPB[i].bottomLongTermPicNum == (int32_t)mmco[k].long_term_pic_num)
                    m_DPB[i].bottom_field_marking = MARKING_UNUSED;
            }
            break;
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_MARK_LONG_TERM:
            picNumX = currPicNum - (mmco[k].difference_of_pic_nums_minus1 + 1);  // (8-40)
            // 8.2.5.4.3 Assignment process of a LongTermFrameIdx to a short-term reference picture
            for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
                if (m_DPB[i].top_field_marking == MARKING_LONG &&
                        m_DPB[i].longTermFrameIdx == (int32_t)mmco[k].long_term_frame_idx &&
                        !(m_DPB[i].bottom_field_marking == MARKING_SHORT && m_DPB[i].bottomPicNum == picNumX))
                    m_DPB[i].top_field_marking = MARKING_UNUSED;
                if (m_DPB[i].bottom_field_marking == MARKING_LONG &&
                        m_DPB[i].longTermFrameIdx == (int32_t)mmco[k].long_term_frame_idx &&
                        !(m_DPB[i].top_field_marking == MARKING_SHORT && m_DPB[i].topPicNum == picNumX))
                    m_DPB[i].bottom_field_marking = MARKING_UNUSED;
                if (m_DPB[i].top_field_marking == MARKING_SHORT && m_DPB[i].topPicNum == picNumX) {
                    m_DPB[i].top_field_marking = MARKING_LONG;
                    m_DPB[i].longTermFrameIdx = mmco[k].long_term_frame_idx;
                    // update topLongTermPicNum, bottomLongTermPicNum for subsequent mmco 2
                    if (!pPicInfo->field_pic_flag) {
                        // frame
                        m_DPB[i].topLongTermPicNum = m_DPB[i].bottomLongTermPicNum = m_DPB[i].longTermFrameIdx;  // (8-30)
                    } else if (!pPicInfo->bottom_field_flag) {
                        // top field
                        m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx + 1;  // same parity (8-33)
                        m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx;   // opposite parity (8-34)
                    } else {
                        // bottom field
                        m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx;         // opposite parity (8-34)
                        m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx + 1;  // same parity (8-33)
                    }
                }
                if (m_DPB[i].bottom_field_marking == MARKING_SHORT && m_DPB[i].bottomPicNum == picNumX) {
                    m_DPB[i].bottom_field_marking = MARKING_LONG;
                    m_DPB[i].longTermFrameIdx = mmco[k].long_term_frame_idx;
                    // update topLongTermPicNum, bottomLongTermPicNum for subsequent mmco 2
                    if (!pPicInfo->field_pic_flag) {
                        // frame
                        m_DPB[i].topLongTermPicNum = m_DPB[i].bottomLongTermPicNum = m_DPB[i].longTermFrameIdx;  // (8-30)
                    } else if (!pPicInfo->bottom_field_flag) {
                        // top field
                        m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx + 1;  // same parity (8-33)
                        m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx;   // opposite parity (8-34)
                    } else {
                        // bottom field
                        m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx;         // opposite parity (8-34)
                        m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx + 1;  // same parity (8-33)
                    }
                }
            }
            break;
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_SET_MAX_LONG_TERM_INDEX:
            // 8.2.5.4.4 Decoding process for MaxLongTermFrameIdx
            m_maxLongTermFrameIdx = mmco[k].max_long_term_frame_idx_plus1 - 1;
            for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
                if (m_DPB[i].top_field_marking == MARKING_LONG && m_DPB[i].longTermFrameIdx > m_maxLongTermFrameIdx)
                    m_DPB[i].top_field_marking = MARKING_UNUSED;
                if (m_DPB[i].bottom_field_marking == MARKING_LONG && m_DPB[i].longTermFrameIdx > m_maxLongTermFrameIdx)
                    m_DPB[i].bottom_field_marking = MARKING_UNUSED;
            }
            break;
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_UNMARK_ALL:
        {
            // 8.2.5.4.5 Marking process of all reference pictures as "unused for reference" and setting MaxLongTermFrameIdx to
            // "no long-term frame indices"
            for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
                m_DPB[i].top_field_marking = MARKING_UNUSED;
                m_DPB[i].bottom_field_marking = MARKING_UNUSED;
            }
            m_maxLongTermFrameIdx = -1;
            DpbEntryH264 *pCurDPBEntry = &m_DPB[m_currDpbIdx];
            pCurDPBEntry->picInfo.frame_num = 0;  // 7.4.3
            // 8.2.1
            pCurDPBEntry->topFOC -= pCurDPBEntry->picInfo.PicOrderCnt;
            pCurDPBEntry->bottomFOC -= pCurDPBEntry->picInfo.PicOrderCnt;
            pCurDPBEntry->picInfo.PicOrderCnt = 0;
            break;
        }
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_MARK_CURRENT_AS_LONG_TERM:
        {
            // 8.2.5.4.6 Process for assigning a long-term frame index to the current picture
            DpbEntryH264 *pCurDPBEntry = &m_DPB[m_currDpbIdx];
            VK_DPB_DBG_PRINT(("%d ", mmco[k].long_term_frame_idx));
            for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
                if (i != m_currDpbIdx && m_DPB[i].top_field_marking == MARKING_LONG &&
                        m_DPB[i].longTermFrameIdx == (int32_t)mmco[k].long_term_frame_idx)
                    m_DPB[i].top_field_marking = MARKING_UNUSED;
                if (i != m_currDpbIdx && m_DPB[i].bottom_field_marking == MARKING_LONG &&
                        m_DPB[i].longTermFrameIdx == (int32_t)mmco[k].long_term_frame_idx)
                    m_DPB[i].bottom_field_marking = MARKING_UNUSED;
            }

            if (!pPicInfo->field_pic_flag || !pPicInfo->bottom_field_flag)
                pCurDPBEntry->top_field_marking = MARKING_LONG;
            if (!pPicInfo->field_pic_flag || pPicInfo->bottom_field_flag)
                pCurDPBEntry->bottom_field_marking = MARKING_LONG;

            pCurDPBEntry->longTermFrameIdx = mmco[k].long_term_frame_idx;
            // update topLongTermPicNum, bottomLongTermPicNum
            // (subsequent mmco 2 is not allowed to reference it, but to avoid accidental matches they have to be updated)
            if (!pPicInfo->field_pic_flag) {
                // frame
                pCurDPBEntry->topLongTermPicNum = pCurDPBEntry->bottomLongTermPicNum =
                                                        pCurDPBEntry->longTermFrameIdx;  // (8-30)
            } else if (!pPicInfo->bottom_field_flag) {
                // top field
                pCurDPBEntry->topLongTermPicNum = 2 * pCurDPBEntry->longTermFrameIdx + 1;  // same parity (8-33)
                pCurDPBEntry->bottomLongTermPicNum = 2 * pCurDPBEntry->longTermFrameIdx;   // opposite parity (8-34)
            } else {
                // bottom field
                pCurDPBEntry->topLongTermPicNum = 2 * pCurDPBEntry->longTermFrameIdx;         // opposite parity (8-34)
                pCurDPBEntry->bottomLongTermPicNum = 2 * pCurDPBEntry->longTermFrameIdx + 1;  // same parity (8-33)
            }

            break;
        }
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_END:
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_INVALID:
        default:
            assert(!"Invalid case");
            break;
        }
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.1
void VkEncDpbH264::CalculatePOC(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps)
{
    if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0) {
        CalculatePOCType0(pPicInfo, sps);
    } else {
        CalculatePOCType2(pPicInfo, sps);
    }
    // (8-1)
    DpbEntryH264 *pCurDPBEntry = &m_DPB[m_currDpbIdx];
    if (!pPicInfo->field_pic_flag || pCurDPBEntry->complementary_field_pair)  // not second field of a CFP
        pCurDPBEntry->picInfo.PicOrderCnt = std::min(pCurDPBEntry->topFOC, pCurDPBEntry->bottomFOC);
    else if (!pPicInfo->bottom_field_flag)
        pCurDPBEntry->picInfo.PicOrderCnt = pCurDPBEntry->topFOC;
    else
        pCurDPBEntry->picInfo.PicOrderCnt = pCurDPBEntry->bottomFOC;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.1.1
void VkEncDpbH264::CalculatePOCType0(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps)
{
    if (pPicInfo->flags.IdrPicFlag) { // IDR picture
        m_prevPicOrderCntMsb = 0;
        m_prevPicOrderCntLsb = 0;
    }

    int32_t picOrderCntMsb = 0;
    int32_t maxPicOrderCntLsb = 1 << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4);  // (7-2)

    // (8-3)
    if ((pPicInfo->PicOrderCnt < m_prevPicOrderCntLsb) &&
            ((m_prevPicOrderCntLsb - pPicInfo->PicOrderCnt) >= (maxPicOrderCntLsb / 2))) {
        picOrderCntMsb = m_prevPicOrderCntMsb + maxPicOrderCntLsb;
    } else if ((pPicInfo->PicOrderCnt > m_prevPicOrderCntLsb) &&
             ((pPicInfo->PicOrderCnt - m_prevPicOrderCntLsb) > (maxPicOrderCntLsb / 2))) {
        picOrderCntMsb = m_prevPicOrderCntMsb - maxPicOrderCntLsb;
    } else {
        picOrderCntMsb = m_prevPicOrderCntMsb;
    }

    DpbEntryH264 *pCurDPBEntry = &m_DPB[m_currDpbIdx];

    // (8-4)
    if (!pPicInfo->field_pic_flag || !pPicInfo->bottom_field_flag)
        pCurDPBEntry->topFOC = picOrderCntMsb + pPicInfo->PicOrderCnt;

    // (8-5)
    if (!pPicInfo->field_pic_flag || pPicInfo->bottom_field_flag)
        pCurDPBEntry->bottomFOC = picOrderCntMsb + pPicInfo->PicOrderCnt;

    if (pPicInfo->flags.is_reference) { // reference picture
        m_prevPicOrderCntMsb = picOrderCntMsb;
        m_prevPicOrderCntLsb = pPicInfo->PicOrderCnt;
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.1.2 - Unimplemented because we're not going to handle POC type 1.

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.1.3
void VkEncDpbH264::CalculatePOCType2(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps)
{
    int32_t frameNumOffset, tempPicOrderCnt;
    int32_t maxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);  // (7-1)

    // FrameNumOffset (8-12)
    if (pPicInfo->flags.IdrPicFlag)
        frameNumOffset = 0;
    else if (m_prevFrameNum > pPicInfo->frame_num)
        frameNumOffset = m_prevFrameNumOffset + maxFrameNum;
    else
        frameNumOffset = m_prevFrameNumOffset;

    // tempPicOrderCnt (8-13)
    if (pPicInfo->flags.IdrPicFlag) {
        tempPicOrderCnt = 0;
    } else if (!pPicInfo->flags.is_reference) {
        tempPicOrderCnt = 2 * (frameNumOffset + pPicInfo->frame_num) - 1;
    } else {
        tempPicOrderCnt = 2 * (frameNumOffset + pPicInfo->frame_num);
    }

    DpbEntryH264 *pCurDPBEntry = &m_DPB[m_currDpbIdx];
    // topFOC, bottomFOC (8-14)
    if (!pPicInfo->field_pic_flag) {
        pCurDPBEntry->topFOC = tempPicOrderCnt;
        pCurDPBEntry->bottomFOC = tempPicOrderCnt;
    } else if (pPicInfo->bottom_field_flag)
        pCurDPBEntry->bottomFOC = tempPicOrderCnt;
    else
        pCurDPBEntry->topFOC = tempPicOrderCnt;

    m_prevFrameNumOffset = frameNumOffset;
    m_prevFrameNum = pPicInfo->frame_num;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.4.1  Derivation of picture numbers
void VkEncDpbH264::CalculatePicNum(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps)
{
    int32_t maxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);  // (7-1)

    assert(pPicInfo->frame_num != (uint32_t)(-1));

    for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
        // (8-28)
        if (m_DPB[i].picInfo.frame_num > ((uint32_t)pPicInfo->frame_num))
            m_DPB[i].frameNumWrap = m_DPB[i].picInfo.frame_num - maxFrameNum;
        else
            m_DPB[i].frameNumWrap = m_DPB[i].picInfo.frame_num;

        if (!pPicInfo->field_pic_flag) {
            // frame
            m_DPB[i].topPicNum = m_DPB[i].bottomPicNum = m_DPB[i].frameNumWrap;                      // (8-29)
            m_DPB[i].topLongTermPicNum = m_DPB[i].bottomLongTermPicNum = m_DPB[i].longTermFrameIdx;  // (8-30)
        } else if (!pPicInfo->bottom_field_flag) {
            // top field
            m_DPB[i].topPicNum = 2 * m_DPB[i].frameNumWrap + 1;              // same parity (8-31)
            m_DPB[i].bottomPicNum = 2 * m_DPB[i].frameNumWrap;               // opposite parity (8-32)
            m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx + 1;  // same parity (8-33)
            m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx;   // opposite parity (8-34)
        } else {
            // bottom field
            m_DPB[i].topPicNum = 2 * m_DPB[i].frameNumWrap;                     // opposite parity (8-32)
            m_DPB[i].bottomPicNum = 2 * m_DPB[i].frameNumWrap + 1;              // same parity (8-31)
            m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx;         // opposite parity (8-34)
            m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].longTermFrameIdx + 1;  // same parity (8-33)
        }
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VkEncDpbH264::OutputPicture(int32_t dpb_index, bool release)
{
    if (release) {
        ReleaseFrame(m_DPB[dpb_index].dpbImageView);
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VkEncDpbH264::FlushDpb()
{
    // mark all reference pictures as "unused for reference"
    for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
        m_DPB[i].top_field_marking = MARKING_UNUSED;
        m_DPB[i].bottom_field_marking = MARKING_UNUSED;
    }
    // empty frame buffers marked as "not needed for output" and "unused for reference"
    for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
        if ((!(m_DPB[i].state & DPB_TOP) || (!m_DPB[i].top_needed_for_output && m_DPB[i].top_field_marking == MARKING_UNUSED)) &&
                (!(m_DPB[i].state & DPB_BOTTOM) ||
                 (!m_DPB[i].bottom_needed_for_output && m_DPB[i].bottom_field_marking == MARKING_UNUSED))) {
            m_DPB[i].state = DPB_EMPTY;  // empty
            ReleaseFrame(m_DPB[i].dpbImageView);
        }
    }
    while (!IsDpbEmpty()) DpbBumping(true);
}

bool VkEncDpbH264::GetRefPicture(int8_t dpbIdx, VkSharedBaseObj<VulkanVideoImagePoolNode>& dpbImageView)
{
    if ((dpbIdx >= 0) && (dpbIdx <= MAX_DPB_SLOTS)) {
        dpbImageView = m_DPB[dpbIdx].dpbImageView;
        return (dpbImageView != nullptr) ? true : false;
    } else {
        VK_DPB_DBG_PRINT(("Error : getFrameType : Wrong picture index %d\n", dpbIdx));
    }
    return false;
}

int32_t VkEncDpbH264::GetPicturePOC(int32_t picIndexField)
{
    int32_t dpb_idx = picIndexField >> 1;

    if ((dpb_idx >= 0) && (dpb_idx <= MAX_DPB_SLOTS) && (m_DPB[dpb_idx].state != DPB_EMPTY)) {
        if (((m_DPB[dpb_idx].state & DPB_BOTTOM) == DPB_BOTTOM) && (picIndexField & 1)) {
            return (m_DPB[dpb_idx].bottomFOC);
        } else {
            return (m_DPB[dpb_idx].topFOC);
        }
    }

#if 0
    if ( (dpb_idx >= 0) && (dpb_idx < MAX_DPB_SLOTS) && (m_DPB[dpb_idx].state != DPB_EMPTY) )
        return m_DPB[dpb_idx].picInfo.PicOrderCnt;
#endif

    VK_DPB_DBG_PRINT(("Error : GetPicturePOC : Wrong picture index %d\n", picIndexField));
    return -1;
}

void VkEncDpbH264::GetRefPicList(const PicInfoH264 *pPicInfo,
                                 NvVideoEncodeH264DpbSlotInfoLists<STD_VIDEO_H264_MAX_NUM_LIST_REF>* pDpbSlotInfoLists,
                                 const StdVideoH264SequenceParameterSet *sps,
                                 const StdVideoH264PictureParameterSet *pps, const StdVideoEncodeH264SliceHeader *slh,
                                 const StdVideoEncodeH264ReferenceListsInfo *ref, bool bSkipCorruptFrames)
{
    int32_t num_list[2] = {0, 0};
    RefPicListEntry stRefPicList[2][MAX_DPB_SLOTS + 1];  // one additional entry is used in sorting

    m_max_num_list[0] = 0;
    m_max_num_list[1] = 0;
    RefPicListInitialization(pPicInfo, stRefPicList[0], stRefPicList[1], sps, bSkipCorruptFrames);

    if (!bSkipCorruptFrames) {
        RefPicListReordering(pPicInfo, stRefPicList[0], stRefPicList[1], sps, slh, ref);
    }

    if (slh->flags.num_ref_idx_active_override_flag) {
        m_max_num_list[0] = ref->num_ref_idx_l0_active_minus1 + 1;
        m_max_num_list[1] = ref->num_ref_idx_l1_active_minus1 + 1;
    } else {
        m_max_num_list[0] = std::min(DeriveL0RefCount(stRefPicList[0]), pps->num_ref_idx_l0_default_active_minus1 + 1);
        m_max_num_list[1] = std::min(DeriveL1RefCount(stRefPicList[1]), pps->num_ref_idx_l1_default_active_minus1 + 1);
    }

    for (uint32_t listNum = 0; listNum < 2; listNum++) {

        for (int32_t i = 0; i < m_max_num_list[listNum]; i++) {

            int32_t dpbIndex = stRefPicList[listNum][i].dpbIndex;
            if (dpbIndex == -1) {
                break;
            }

            pDpbSlotInfoLists->refPicList[listNum][i] = (uint8_t)dpbIndex;

            pDpbSlotInfoLists->dpbSlotsUseMask |= (1 << dpbIndex);

            (num_list[listNum])++;
        }
    }

#if 0
    VK_DPB_DBG_PRINT(( "\n----> GET_REF_PIC_LIST"));
    VK_DPB_DBG_PRINT(( "\n      PicType %s", (pPicInfo->primary_pic_type == 0) ? "P": (pPicInfo->primary_pic_type == 1) ? "B": "I"));
    VK_DPB_DBG_PRINT(("\n      PicOrderCnt %d (isBottom = %d)", pPicInfo->PicOrderCnt, pPicInfo->bottom_field_flag));

    for (int32_t lx = 0; lx < 2; lx++) {
        VK_DPB_DBG_PRINT(( "\n      RefPicList[%d]:", lx ));
        for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
            VK_DPB_DBG_PRINT((" %3d ", stRefPicList[lx][i].dpbIndex));
        }
        VK_DPB_DBG_PRINT(("\n"));
        VK_DPB_DBG_PRINT(("\n----- MaxNumList[%d] = %d", lx, m_max_num_list[lx]));
        VK_DPB_DBG_PRINT(("\n"));
    }

#endif

    pDpbSlotInfoLists->refPicListCount[0] = num_list[0];
    pDpbSlotInfoLists->refPicListCount[1] = num_list[1];
}

// 8.2.4.2
void VkEncDpbH264::RefPicListInitialization(const PicInfoH264 *pPicInfo,
                                            RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                                            const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames)
{
    // TODO: how to handle not-existing pictures?
    for (int32_t k = 0; k < (MAX_DPB_SLOTS + 1); k++) {
        RefPicList0[k].dpbIndex = -1;  // "no reference picture"
        RefPicList1[k].dpbIndex = -1;  // "no reference picture"
    }

    if (pPicInfo->primary_pic_type == STD_VIDEO_H264_PICTURE_TYPE_P) {
        if (!pPicInfo->field_pic_flag) {
            RefPicListInitializationPFrame(RefPicList0, sps, bSkipCorruptFrames);
        } else {
            RefPicListInitializationPField(RefPicList0, sps, pPicInfo->bottom_field_flag, bSkipCorruptFrames);
        }
    } else if (pPicInfo->primary_pic_type == STD_VIDEO_H264_PICTURE_TYPE_B) {
        if (!pPicInfo->field_pic_flag) {
            RefPicListInitializationBFrame(RefPicList0, RefPicList1, sps, bSkipCorruptFrames);
        } else {
            RefPicListInitializationBField(pPicInfo, RefPicList0, RefPicList1, sps, bSkipCorruptFrames);
        }
    }
}

// 8.2.4.2.1
void VkEncDpbH264::RefPicListInitializationPFrame(RefPicListEntry *RefPicList0, const StdVideoH264SequenceParameterSet *sps,
        bool bSkipCorruptFrames)
{
    // short-term frames sorted by descending PicNum
    int32_t k = SortListDescending(RefPicList0, sps, 0, INF_MAX, sort_check_short_term_P_frame, bSkipCorruptFrames);
    // long-term frames sorted by ascending LongTermPicNum
    k = SortListAscending(RefPicList0, sps, k, INF_MIN, sort_check_long_term_frame, bSkipCorruptFrames);

    m_max_num_list[0] = k;
}

// 8.2.4.2.2
void VkEncDpbH264::RefPicListInitializationPField(RefPicListEntry *RefPicList0, const StdVideoH264SequenceParameterSet *sps,
                                                  bool bottomField, bool bSkipCorruptFrames)
{
    RefPicListEntry refFrameList0ShortTerm[MAX_DPB_SLOTS], refFrameListLongTerm[MAX_DPB_SLOTS];

    int32_t ksmax = SortListDescending(refFrameList0ShortTerm, sps, 0, INF_MAX, sort_check_short_term_P_field, bSkipCorruptFrames);
    int32_t klmax = SortListAscending(refFrameListLongTerm, sps, 0, INF_MIN, sort_check_long_term_field, bSkipCorruptFrames);

    int32_t k = RefPicListInitializationField(refFrameList0ShortTerm, refFrameListLongTerm, ksmax, klmax, RefPicList0, bottomField, bSkipCorruptFrames);

    m_max_num_list[0] = k;
}

// 8.2.4.2.3
void VkEncDpbH264::RefPicListInitializationBFrame(RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
        const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames)
{
    // list 0
    int32_t k0 = RefPicListInitializationBFrameListX(RefPicList0, sps, false, bSkipCorruptFrames);

    // list 1
    int32_t k1 = RefPicListInitializationBFrameListX(RefPicList1, sps, true, bSkipCorruptFrames);

    if (k1 > 1 && k0 == k1) {
        // note: it may be sufficient to only check if the first entry is identical
        // (this should imply that the entire list is identical)
        int32_t k = 0;
        for ( ; k < k1; k++) {
            if (RefPicList0[k].dpbIndex != RefPicList1[k].dpbIndex) break;
        }
        if (k == k1) { // lists are identical
            // swap first two entries
            int32_t idx = RefPicList1[0].dpbIndex;
            RefPicList1[0].dpbIndex = RefPicList1[1].dpbIndex;
            RefPicList1[1].dpbIndex = idx;
        }
    }
    m_max_num_list[0] = k0;
    m_max_num_list[1] = k1;
}

// 8.2.4.2.4
void VkEncDpbH264::RefPicListInitializationBField(const PicInfoH264 *pPicInfo, RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
        const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames)
{
    RefPicListEntry refFrameList0ShortTerm[MAX_DPB_SLOTS], refFrameList1ShortTerm[MAX_DPB_SLOTS], refFrameListLongTerm[MAX_DPB_SLOTS];
    int32_t currPOC;

    currPOC = !pPicInfo->bottom_field_flag ? m_DPB[m_currDpbIdx].topFOC : m_DPB[m_currDpbIdx].bottomFOC;

    int32_t k0 = SortListDescending(refFrameList0ShortTerm, sps, 0, currPOC, sort_check_short_term_B_field, bSkipCorruptFrames);
    k0 = SortListAscending(refFrameList0ShortTerm, sps, k0, currPOC, sort_check_short_term_B_field, bSkipCorruptFrames);

    int32_t k1 = SortListAscending(refFrameList1ShortTerm, sps, 0, currPOC, sort_check_short_term_B_field, bSkipCorruptFrames);
    k1 = SortListDescending(refFrameList1ShortTerm, sps, k1, currPOC, sort_check_short_term_B_field, bSkipCorruptFrames);

    int32_t kl = SortListAscending(refFrameListLongTerm, sps, 0, INF_MIN, sort_check_long_term_field, bSkipCorruptFrames);

    k0 = RefPicListInitializationField(refFrameList0ShortTerm, refFrameListLongTerm, k0, kl, RefPicList0, pPicInfo->bottom_field_flag, bSkipCorruptFrames);
    k1 = RefPicListInitializationField(refFrameList1ShortTerm, refFrameListLongTerm, k1, kl, RefPicList1, pPicInfo->bottom_field_flag, bSkipCorruptFrames);

    if ((k1 > 1) && (k0 == k1)) {
        int32_t k = 0;
        // note: it may be sufficient to only check if the first entry is identical
        // (this should imply that the entire list is identical)
        for (k = 0; k < k1; k++) {
            if (RefPicList0[k].dpbIndex != RefPicList1[k].dpbIndex) break;
        }
        int32_t idx = 0;
        if (k == k1) { // lists are identical
            // swap first two entries
            idx = RefPicList1[0].dpbIndex;
            RefPicList1[0].dpbIndex = RefPicList1[1].dpbIndex;
            RefPicList1[1].dpbIndex = idx;
        }
    }
    m_max_num_list[0] = k0;
    m_max_num_list[1] = k1;
}

// 8.2.4.2.5
int32_t VkEncDpbH264::RefPicListInitializationField(RefPicListEntry *refFrameListXShortTerm, RefPicListEntry *refFrameListLongTerm,
        int32_t ksmax, int32_t klmax, RefPicListEntry *RefPicListX, bool bottomField, bool bSkipCorruptFrames)
{
    int32_t k = RefPicListInitializationFieldListX(refFrameListXShortTerm, ksmax, 0, RefPicListX, bottomField, bSkipCorruptFrames);
    k =         RefPicListInitializationFieldListX(refFrameListLongTerm,   klmax, k, RefPicListX, bottomField, bSkipCorruptFrames);

    return k;
}

int32_t VkEncDpbH264::RefPicListInitializationFieldListX(RefPicListEntry *refFrameListX, int32_t kfmax, int32_t kmin,
        RefPicListEntry *RefPicListX, bool bottomField, bool bSkipCorruptFrames)
{
    int32_t bottom = bottomField;
    int32_t k = kmin;
    int32_t ktop = 0;
    int32_t kbot = 0;
    while ((ktop < kfmax || kbot < kfmax) && k < MAX_DPB_SLOTS) {
        if (!bottom) {
            while (ktop < kfmax && m_DPB[refFrameListX[ktop].dpbIndex].top_field_marking == MARKING_UNUSED) ktop++;
            if (ktop < kfmax) {
                RefPicListX[k].dpbIndex = refFrameListX[ktop].dpbIndex;
                k++;
                ktop++;
            }
        } else {
            while (kbot < kfmax && m_DPB[refFrameListX[kbot].dpbIndex].bottom_field_marking == MARKING_UNUSED) kbot++;
            if (kbot < kfmax) {
                RefPicListX[k].dpbIndex = refFrameListX[kbot].dpbIndex;
                k++;
                kbot++;
            }
        }
        bottom = !bottom;
    }
    return k;
}

int32_t VkEncDpbH264::RefPicListInitializationBFrameListX(RefPicListEntry *RefPicListX,
        const StdVideoH264SequenceParameterSet *sps, bool list1,
        bool bSkipCorruptFrames)
{
    int32_t k;
    if (!list1) {
        // short-term frames sorted by descending PicOrderCnt less than current
        k = SortListDescending(RefPicListX, sps, 0, m_DPB[m_currDpbIdx].picInfo.PicOrderCnt, sort_check_short_term_B_frame,
                               bSkipCorruptFrames);
        // short-term frames sorted by ascending PicOrderCnt above current
        k = SortListAscending(RefPicListX, sps, k, m_DPB[m_currDpbIdx].picInfo.PicOrderCnt, sort_check_short_term_B_frame,
                              bSkipCorruptFrames);
    } else {
        // short-term frames sorted by ascending PicOrderCnt above current
        k = SortListAscending(RefPicListX, sps, 0, m_DPB[m_currDpbIdx].picInfo.PicOrderCnt, sort_check_short_term_B_frame,
                              bSkipCorruptFrames);
        // short-term frames sorted by descending PicOrderCnt less than current
        k = SortListDescending(RefPicListX, sps, k, m_DPB[m_currDpbIdx].picInfo.PicOrderCnt, sort_check_short_term_B_frame,
                               bSkipCorruptFrames);
    }
    // long-term frames sorted by ascending LongTermPicNum
    k = SortListAscending(RefPicListX, sps, k, INF_MIN, sort_check_long_term_frame, bSkipCorruptFrames);

    return k;
}

int32_t VkEncDpbH264::SortListDescending(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps, int32_t kmin,
        int32_t n, ptrFuncDpbSort sort_check, bool bSkipCorruptFrames)
{
    int32_t k = kmin;
    for ( ; k < MAX_DPB_SLOTS; k++) {
        int32_t m = INF_MIN;
        int32_t i1 = -1;
        int32_t v = -1;
        // find largest entry less than or equal to n
        for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
            if (m_DPB[i].view_id != m_DPB[m_currDpbIdx].view_id) {
                continue;
            }

            if ((m_DPB[i].frame_is_corrupted == true) && (bSkipCorruptFrames == true)) {
                continue;
            }

            if (sort_check(&m_DPB[i], sps->pic_order_cnt_type, &v) && (v >= m) && (v <= n)) {
                i1 = i;
                m = v;
            }
        }
        if (i1 < 0) break;  // no more entries
        RefPicListX[k].dpbIndex = i1;
        if (m == INF_MIN) { // smallest possible entry, exit early to avoid underflow
            k++;
            break;
        }
        n = m - 1;
    }
    return k;
}

int32_t VkEncDpbH264::SortListAscending(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps, int32_t kmin,
                                        int32_t n, ptrFuncDpbSort sort_check, bool bSkipCorruptFrames)
{
    int32_t m, k, i, i1, v;

    for (k = kmin; k < MAX_DPB_SLOTS; k++) {
        m = INF_MAX;
        i1 = -1;
        // find smallest entry greater than n
        for (i = 0; i < MAX_DPB_SLOTS; i++) {
            if (m_DPB[i].view_id != m_DPB[m_currDpbIdx].view_id) {
                continue;
            }

            if ((m_DPB[i].frame_is_corrupted == true) && (bSkipCorruptFrames == true)) {
                continue;
            }

            if (sort_check(&m_DPB[i], sps->pic_order_cnt_type, &v) && v <= m && v > n) {
                i1 = i;
                m = v;
            }
        }
        if (i1 < 0) break;  // no more entries
        RefPicListX[k].dpbIndex = i1;
        n = m;
    }
    return k;
}

// 8.2.4.3
void VkEncDpbH264::RefPicListReordering(const PicInfoH264 *pPicInfo,
                                        RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                                        const StdVideoH264SequenceParameterSet *sps,
                                        const StdVideoEncodeH264SliceHeader *slh,
                                        const StdVideoEncodeH264ReferenceListsInfo *ref)
{
    int32_t num_ref_idx_lX_active_minus1;

    // scan through commands if there is refpic reorder cmds
    if (ref->flags.ref_pic_list_modification_flag_l0) {
        num_ref_idx_lX_active_minus1 =
            slh->flags.num_ref_idx_active_override_flag ? ref->num_ref_idx_l0_active_minus1 : m_max_num_list[0];
        RefPicListReorderingLX(pPicInfo, RefPicList0, sps, num_ref_idx_lX_active_minus1, ref->pRefList0ModOperations, 0);
    }

    if (ref->flags.ref_pic_list_modification_flag_l1) {
        num_ref_idx_lX_active_minus1 =
            slh->flags.num_ref_idx_active_override_flag ? ref->num_ref_idx_l1_active_minus1 : m_max_num_list[1];
        RefPicListReorderingLX(pPicInfo, RefPicList1, sps, num_ref_idx_lX_active_minus1, ref->pRefList1ModOperations, 1);
    }
}

void VkEncDpbH264::RefPicListReorderingLX(const PicInfoH264 *pPicInfo,
                                          RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps,
                                          int32_t num_ref_idx_lX_active_minus1,
                                          const StdVideoEncodeH264RefListModEntry *ref_pic_list_reordering_lX,
                                          int32_t listX)
{
    int32_t MaxFrameNum, MaxPicNum, CurrPicNum, picNumLXPred, refIdxLX, k, picNumLXNoWrap, picNumLX, LongTermPicNum;

    MaxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);  // (7-1)

    if (!pPicInfo->field_pic_flag) {
        MaxPicNum = MaxFrameNum;
        CurrPicNum = pPicInfo->frame_num;
    } else {
        MaxPicNum = 2 * MaxFrameNum;
        CurrPicNum = 2 * pPicInfo->frame_num + 1;
    }

    picNumLXPred = CurrPicNum;
    refIdxLX = 0;
    k = 0;

    do {
        switch (ref_pic_list_reordering_lX[k].modification_of_pic_nums_idc) {
        case STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_SHORT_TERM_SUBTRACT:
        case STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_SHORT_TERM_ADD:
            if (ref_pic_list_reordering_lX[k].modification_of_pic_nums_idc ==
                    STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_SHORT_TERM_SUBTRACT) {
                // (8-35)
                if (picNumLXPred - ((int32_t)ref_pic_list_reordering_lX[k].abs_diff_pic_num_minus1 + 1) < 0)
                    picNumLXNoWrap = picNumLXPred - (ref_pic_list_reordering_lX[k].abs_diff_pic_num_minus1 + 1) + MaxPicNum;
                else
                    picNumLXNoWrap = picNumLXPred - (ref_pic_list_reordering_lX[k].abs_diff_pic_num_minus1 + 1);
            } else {
                // (8-36)
                if (picNumLXPred + ((int32_t)ref_pic_list_reordering_lX[k].abs_diff_pic_num_minus1 + 1) >= MaxPicNum)
                    picNumLXNoWrap = picNumLXPred + (ref_pic_list_reordering_lX[k].abs_diff_pic_num_minus1 + 1) - MaxPicNum;
                else
                    picNumLXNoWrap = picNumLXPred + (ref_pic_list_reordering_lX[k].abs_diff_pic_num_minus1 + 1);
            }
            picNumLXPred = picNumLXNoWrap;
            // (8-37)
            if (picNumLXNoWrap > CurrPicNum)
                picNumLX = picNumLXNoWrap - MaxPicNum;
            else
                picNumLX = picNumLXNoWrap;
            RefPicListReorderingShortTerm(RefPicListX, refIdxLX, num_ref_idx_lX_active_minus1, picNumLX);
            break;
        case STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_LONG_TERM:
            LongTermPicNum = ref_pic_list_reordering_lX[k].long_term_pic_num;
            RefPicListReorderingLongTerm(RefPicListX, refIdxLX, num_ref_idx_lX_active_minus1, LongTermPicNum);
            break;
        case STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_END:
            break;
        case STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_INVALID:
        default:
            assert(!"Invalid case");
            break;
        }
        refIdxLX++;
    } while (ref_pic_list_reordering_lX[k++].modification_of_pic_nums_idc != STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_END);
}

// 8.2.4.3.1
void VkEncDpbH264::RefPicListReorderingShortTerm(RefPicListEntry *RefPicListX, int32_t refIdxLX, int32_t num_ref_idx_lX_active_minus1,
        int32_t picNumLX)
{
    int32_t idx, cIdx, nIdx = 0;
    //int32_t bottomField = 0;

    // find short-term reference picture picNumLX
    for (idx = 0; idx < MAX_DPB_SLOTS; idx++) {
        if (m_DPB[idx].view_id != m_DPB[m_currDpbIdx].view_id) {
            continue;
        }
        if (m_DPB[idx].top_field_marking == MARKING_SHORT && m_DPB[idx].topPicNum == picNumLX) {
            //bottomField = 0;
            break;
        }
        if (m_DPB[idx].bottom_field_marking == MARKING_SHORT && m_DPB[idx].bottomPicNum == picNumLX) {
            //bottomField = 1;
            break;
        }
    }
    if (idx >= MAX_DPB_SLOTS) VK_DPB_DBG_PRINT(("short-term picture picNumLX does not exist\n"));
    // (8-38)
    for (cIdx = num_ref_idx_lX_active_minus1 + 1; cIdx > refIdxLX; cIdx--) RefPicListX[cIdx] = RefPicListX[cIdx - 1];
    RefPicListX[refIdxLX].dpbIndex = idx;
    refIdxLX++;
    nIdx = refIdxLX;
    for (cIdx = refIdxLX; cIdx <= num_ref_idx_lX_active_minus1 + 1; cIdx++)
        if (!(RefPicListX[cIdx].dpbIndex == idx)) RefPicListX[nIdx++] = RefPicListX[cIdx];
}

// 8.2.4.3.2
void VkEncDpbH264::RefPicListReorderingLongTerm(RefPicListEntry *RefPicListX, int32_t refIdxLX, int32_t num_ref_idx_lX_active_minus1,
        int32_t LongTermPicNum)
{
    int32_t idx, cIdx, nIdx = 0;
    //int32_t bottomField = 0;

    // find long-term reference picture LongTermPicNum
    for (idx = 0; idx < MAX_DPB_SLOTS; idx++) {
        if (m_DPB[idx].view_id != m_DPB[m_currDpbIdx].view_id) {
            continue;
        }
        if (m_DPB[idx].top_field_marking == MARKING_LONG && m_DPB[idx].topLongTermPicNum == LongTermPicNum) {
            // bottomField = 0;
            break;
        }
        if (m_DPB[idx].bottom_field_marking == MARKING_LONG && m_DPB[idx].bottomLongTermPicNum == LongTermPicNum) {
            // bottomField = 1;
            break;
        }
    }
    if (idx >= MAX_DPB_SLOTS) VK_DPB_DBG_PRINT(("long-term picture LongTermPicNum does not exist\n"));
    // (8-39)
    for (cIdx = num_ref_idx_lX_active_minus1 + 1; cIdx > refIdxLX; cIdx--) RefPicListX[cIdx] = RefPicListX[cIdx - 1];
    RefPicListX[refIdxLX].dpbIndex = idx;
    refIdxLX++;
    nIdx = refIdxLX;
    for (cIdx = refIdxLX; cIdx <= num_ref_idx_lX_active_minus1 + 1; cIdx++)
        if (!(RefPicListX[cIdx].dpbIndex == idx)) RefPicListX[nIdx++] = RefPicListX[cIdx];
}

int32_t VkEncDpbH264::DeriveL0RefCount(RefPicListEntry *RefPicList)
{
    return m_max_num_list[0];
}

int32_t VkEncDpbH264::DeriveL1RefCount(RefPicListEntry *RefPicList)
{
    return m_max_num_list[1];
}

int32_t VkEncDpbH264::GetNumRefFramesInDPB(uint32_t viewid, int32_t *numShortTermRefs, int32_t *numLongTermRefs)
{
    int32_t numShortTerm = 0, numLongTerm = 0;
    for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
        if (m_DPB[i].view_id == viewid) {
            if (((m_DPB[i].top_field_marking == MARKING_SHORT) || (m_DPB[i].bottom_field_marking == MARKING_SHORT)) &&
                    (m_DPB[i].frame_is_corrupted == false)) {
                numShortTerm++;
            }
            if (((m_DPB[i].top_field_marking == MARKING_LONG) || (m_DPB[i].bottom_field_marking == MARKING_LONG)) &&
                    (m_DPB[i].frame_is_corrupted == false)) {
                numLongTerm++;
            }
        }
    }
    if (numShortTermRefs) (*numShortTermRefs) = numShortTerm;
    if (numLongTermRefs) (*numLongTermRefs) = numLongTerm;

    return (numShortTerm + numLongTerm);
}

int32_t VkEncDpbH264::GetPicNumXWithMinPOC(uint32_t view_id, int32_t field_pic_flag, int32_t bottom_field)
{
    int32_t pocMin = INF_MAX;
    int32_t min = -1;
    int32_t i = 0;
    for (i = 0; i < MAX_DPB_SLOTS; i++) {
        if ((m_DPB[i].state & DPB_TOP) && (m_DPB[i].top_field_marking == MARKING_SHORT) &&
                (m_DPB[i].topFOC < pocMin) && (m_DPB[i].view_id == view_id)) {
            pocMin = m_DPB[i].topFOC;
            min = i;
        }
        if ((m_DPB[i].state & DPB_BOTTOM) && (m_DPB[i].top_field_marking == MARKING_SHORT) &&
                (m_DPB[i].bottomFOC < pocMin) && (m_DPB[i].view_id == view_id)) {
            pocMin = m_DPB[i].bottomFOC;
            min = i;
        }
    }

    if (min >= 0) {
        if (field_pic_flag && bottom_field) {
            return m_DPB[min].bottomPicNum;
        } else {
            return m_DPB[min].topPicNum;
        }
    }
    return -1;
}

int32_t VkEncDpbH264::GetPicNumXWithMinFrameNumWrap(uint32_t view_id, int32_t field_pic_flag, int32_t bottom_field)
{
    int32_t minFrameNumWrap = 65536;
    int32_t minFrameNum = -1;
    int32_t i = 0;

    for (i = 0; i < MAX_DPB_SLOTS; i++) {
        if (m_DPB[i].view_id == view_id) {
            if (((m_DPB[i].top_field_marking == MARKING_SHORT) || (m_DPB[i].bottom_field_marking == MARKING_SHORT))) {
                if (m_DPB[i].frameNumWrap < minFrameNumWrap) {
                    minFrameNum = i;
                    minFrameNumWrap = m_DPB[i].frameNumWrap;
                }
            }
        }
    }

    if (minFrameNum >= 0) {
        if (field_pic_flag && bottom_field) {
            return m_DPB[minFrameNum].bottomPicNum;
        } else {
            return m_DPB[minFrameNum].topPicNum;
        }
    }
    return -1;
}

int32_t VkEncDpbH264::GetPicNum(int32_t dpb_idx, bool bottomField)
{
    if ((dpb_idx >= 0) && (dpb_idx < MAX_DPB_SLOTS) && (m_DPB[dpb_idx].state != DPB_EMPTY)) {
        return bottomField ? m_DPB[dpb_idx].bottomPicNum : m_DPB[dpb_idx].topPicNum;
    }

    VK_DPB_DBG_PRINT(("%s: Invalid index or state for decoded picture buffer \n", __FUNCTION__));
    return -1;
}

// Currently we support it only for IPPP gop pattern
bool VkEncDpbH264::InvalidateReferenceFrames(uint64_t timeStamp)
{
    bool isValidReqest = true;

    for (uint32_t i = 0; i < MAX_DPB_SLOTS; i++) {
        if ((m_DPB[i].state != DPB_EMPTY) && (timeStamp == m_DPB[i].timeStamp)) {
            if (m_DPB[i].frame_is_corrupted == true) {
                isValidReqest = false;
            }
            break;
        }
    }

    if ((timeStamp >= m_lastIDRTimeStamp) && isValidReqest) {
        for (uint32_t i = 0; i < MAX_DPB_SLOTS; i++) {
            if ((m_DPB[i].state != DPB_EMPTY) && ((timeStamp <= m_DPB[i].refFrameTimeStamp) || (timeStamp == m_DPB[i].timeStamp))) {
                if ((m_DPB[i].top_field_marking == MARKING_SHORT) || (m_DPB[i].bottom_field_marking == MARKING_SHORT)) {
                    m_DPB[i].frame_is_corrupted = true;
                }

                if ((m_DPB[i].top_field_marking == MARKING_LONG) || (m_DPB[i].bottom_field_marking == MARKING_LONG)) {
                    m_DPB[i].frame_is_corrupted = true;
                }
            }
        }
    }

    return true;
}

bool VkEncDpbH264::IsRefFramesCorrupted()
{
    int32_t i = 0;
    for (i = 0; i < MAX_DPB_SLOTS; i++) {
        if (((m_DPB[i].top_field_marking == MARKING_SHORT) || (m_DPB[i].bottom_field_marking == MARKING_SHORT)) &&
                (m_DPB[i].frame_is_corrupted == true)) {
            return true;
        }

        if (((m_DPB[i].top_field_marking == MARKING_LONG) || (m_DPB[i].bottom_field_marking == MARKING_LONG)) &&
                (m_DPB[i].frame_is_corrupted == true)) {
            return true;
        }
    }
    return false;
}

bool VkEncDpbH264::IsRefPicCorrupted(int32_t dpb_idx)
{
    if ((dpb_idx >= 0) && (dpb_idx < MAX_DPB_SLOTS) && (m_DPB[dpb_idx].state != DPB_EMPTY)) {
        return (m_DPB[dpb_idx].frame_is_corrupted == true);
    }
    return false;
}

int32_t VkEncDpbH264::GetPicNumFromDpbIdx(int32_t dpbIdx, bool *shortterm, bool *longterm)
{
    if ((dpbIdx >= 0) && (dpbIdx <= MAX_DPB_SLOTS) && (m_DPB[dpbIdx].state != DPB_EMPTY)) {
        // field pictures not supported/tested
        assert(m_DPB[dpbIdx].state == DPB_FRAME);

        if (m_DPB[dpbIdx].top_field_marking == MARKING_SHORT) {
            *shortterm = true;
            return m_DPB[dpbIdx].topPicNum;
        } else if (m_DPB[dpbIdx].bottom_field_marking == MARKING_SHORT) {
            *shortterm = true;
            return m_DPB[dpbIdx].bottomPicNum;
        } else if (m_DPB[dpbIdx].top_field_marking == MARKING_LONG || m_DPB[dpbIdx].bottom_field_marking == MARKING_LONG) {
            *longterm = true;
            return m_DPB[dpbIdx].longTermFrameIdx;
        }
    }

    *shortterm = false;
    *longterm = false;
    VK_DPB_DBG_PRINT(("%s : Invalid index or state for decoded picture buffer\n", __FUNCTION__));
    return -1;
}

uint64_t VkEncDpbH264::GetPictureTimestamp(int32_t dpb_idx)
{
    if ((dpb_idx >= 0) && (dpb_idx < MAX_DPB_SLOTS) && (m_DPB[dpb_idx].state != DPB_EMPTY)) {
        return (m_DPB[dpb_idx].timeStamp);
    }
    return 0;
}

void VkEncDpbH264::SetCurRefFrameTimeStamp(uint64_t refFrameTimeStamp)
{
    m_DPB[m_currDpbIdx].refFrameTimeStamp = refFrameTimeStamp;
}

// Returns a "view" of the DPB in terms of the entries holding valid reference
// pictures.
int32_t VkEncDpbH264::GetValidEntries(DpbEntryH264 entries[MAX_DPB_SLOTS])
{
    int32_t numEntries = 0;

    for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
        if ((m_DPB[i].top_field_marking != 0) || (m_DPB[i].bottom_field_marking != 0)) {
            entries[numEntries++] = m_DPB[i];
        }
    }

    return numEntries;
}

uint32_t VkEncDpbH264::GetUsedFbSlotsMask()
{
    uint32_t usedFbSlotsMask = 0;
    for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
        if ((m_DPB[i].top_field_marking != 0) || (m_DPB[i].bottom_field_marking != 0)) {

            int32_t fbIdx = m_DPB[i].dpbImageView->GetImageIndex();
            assert(fbIdx >= 0);
            usedFbSlotsMask |= (1 << fbIdx);
        }
    }

    return usedFbSlotsMask;
}

// Returns a flag specifying if the buffer need to be reordered.
bool VkEncDpbH264::NeedToReorder()
{
    for (int32_t i = 0; i < MAX_DPB_SLOTS; i++) {
        if ((m_DPB[i].top_field_marking != 0) || (m_DPB[i].bottom_field_marking != 0)) {
            if (m_DPB[i].frame_is_corrupted) {
                return true;
            }
        }
    }

    return false;
}
void VkEncDpbH264::FillStdReferenceInfo(uint8_t dpbIdx, StdVideoEncodeH264ReferenceInfo* pStdReferenceInfo)
{
    assert(dpbIdx < MAX_DPB_SLOTS);
    const DpbEntryH264* pDpbEntry = &m_DPB[dpbIdx];

    bool isLongTerm = (pDpbEntry->top_field_marking == MARKING_LONG);

    pStdReferenceInfo->PicOrderCnt = pDpbEntry->picInfo.PicOrderCnt;
    pStdReferenceInfo->flags.used_for_long_term_reference = isLongTerm;
    pStdReferenceInfo->long_term_frame_idx = isLongTerm ? (uint16_t)pDpbEntry->longTermFrameIdx : (uint16_t)-1;
}
