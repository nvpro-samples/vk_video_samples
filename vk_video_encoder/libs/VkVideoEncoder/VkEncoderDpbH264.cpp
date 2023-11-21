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

//#define LOG_INVALID_FRAMES 1
static FILE *fpInvalidFrameLog = nullptr;

#ifndef min
static inline int32_t min(int32_t a, int32_t b)
{
    return a < b ? a : b;
}
#endif
#ifndef max
static inline int32_t max(int32_t a, int32_t b)
{
    return a > b ? a : b;
}
#endif

// helper functions for refpic list intialization and reoirdering
static bool sort_check_short_term_P_frame(const VkEncDpbEntry *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->topPicNum;
    return (pDBP->top_field_marking == MARKING_SHORT && pDBP->bottom_field_marking == MARKING_SHORT);
}

static bool sort_check_short_term_P_field(const VkEncDpbEntry *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->frameNumWrap;
    return pDBP->top_field_marking == MARKING_SHORT || pDBP->bottom_field_marking == MARKING_SHORT;
}

static bool sort_check_short_term_B_frame(const VkEncDpbEntry *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->picInfo.PicOrderCnt;
    return !(pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0 && pDBP->not_existing) && pDBP->top_field_marking == MARKING_SHORT &&
           pDBP->bottom_field_marking == MARKING_SHORT;
}

static bool sort_check_short_term_B_field(const VkEncDpbEntry *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->picInfo.PicOrderCnt;
    return !(pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0 && pDBP->not_existing) &&
           (pDBP->top_field_marking == MARKING_SHORT || pDBP->bottom_field_marking == MARKING_SHORT);
}

static bool sort_check_long_term_frame(const VkEncDpbEntry *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->topLongTermPicNum;
    return pDBP->top_field_marking == MARKING_LONG && pDBP->bottom_field_marking == MARKING_LONG;
}

static bool sort_check_long_term_field(const VkEncDpbEntry *pDBP, StdVideoH264PocType pic_order_cnt_type, int32_t *pv)
{
    *pv = pDBP->LongTermFrameIdx;
    return pDBP->top_field_marking == MARKING_LONG || pDBP->bottom_field_marking == MARKING_LONG;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
VkEncDpbH264::VkEncDpbH264()
    : m_MaxLongTermFrameIdx(0),
      m_max_dpb_size(0),
      m_prevPicOrderCntMsb(0),
      m_prevPicOrderCntLsb(0),
      m_prevFrameNumOffset(0),
      m_prevFrameNum(0),
      m_PrevRefFrameNum(0),
      m_pCurPicInfo(nullptr),
      m_pCurDPBEntry(nullptr),
      m_nCurDPBIdx(0),
      m_lastIDRTimeStamp(0)
{
    memset(m_max_num_list, 0, sizeof(m_max_num_list));
    memset(m_FrameBuffer, 0, sizeof(m_FrameBuffer));
    memset(m_DPBIdx2FBIndexMapping, 0, sizeof(m_DPBIdx2FBIndexMapping));
}

VkEncDpbH264::~VkEncDpbH264() {}
VkEncDpbH264 *VkEncDpbH264::CreateInstance(void)
{
    VkEncDpbH264 *pDpb = new VkEncDpbH264();
    if (pDpb) pDpb->DpbInit();
    return pDpb;
}

int32_t VkEncDpbH264::AllocateFrame()
{
    for (int32_t i = 0; i < m_max_dpb_size + 1; i++) {
        if (m_FrameBuffer[i] == -1) {
            m_FrameBuffer[i] = 1;  // in use
            return i;
        }
    }
    assert(!"Could not allocate a frame buffer");
    return -1;
}

void VkEncDpbH264::ReleaseFrame(int32_t fb_idx)
{
    if (fb_idx >= 0 && fb_idx <= MAX_DPB_SIZE) {
        m_FrameBuffer[fb_idx] = -1;  // not in use
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VkEncDpbH264::DpbInit()
{
    m_pCurPicInfo = nullptr;
    m_max_dpb_size = 0;
    m_max_num_list[0] = 0;
    m_max_num_list[1] = 0;
    m_pCurDPBEntry = nullptr;
    for (int32_t i = 0; i < MAX_DPB_SIZE + 1; i++) {
        m_FrameBuffer[i] = -1;
        m_DPBIdx2FBIndexMapping[i] = -1;
    }
#if defined(LOG_INVALID_FRAMES)
    fpInvalidFrameLog = fopen("InvalidateFrameEnc.txt", "wt");
#endif
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VkEncDpbH264::DpbDeinit()
{
    if (m_pCurPicInfo != nullptr) {
        delete m_pCurPicInfo;
        m_pCurPicInfo = nullptr;
    };
    m_pCurDPBEntry = nullptr;
    m_max_dpb_size = 0;
    m_nCurDPBIdx = 0;
    m_lastIDRTimeStamp = 0;
};

void VkEncDpbH264::DpbDestroy()
{
    FlushDpb();
    DpbDeinit();
    if (fpInvalidFrameLog) {
        fclose(fpInvalidFrameLog);
        fpInvalidFrameLog = nullptr;
    }

    delete this;
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
//
// The number of entries DPB_N should at least be equal to the max number of references (R) + decoded pictures that cannot
// be displayed yet + 1 (current picture to be recontructed). At the end of the reconstruction of the current picture,
// if it is not a reference picture and can be displayed, the picture will not be part of the fullness of the DPB. The number
// of entries DPB_N = dpb_size (as viewed by H264 std) + 1
// returns -1 if err
int32_t VkEncDpbH264::DpbSequenceStart(int32_t userDpbSize)
{
    int32_t i;

    DpbDeinit();

    m_max_dpb_size = userDpbSize;

    m_pCurPicInfo = new (DpbPicInfo);
    if (m_pCurPicInfo == nullptr) {
        VK_DPB_DBG_PRINT(("%s :Error in msenc_picture.cpp, init_sequence failed\n", __FUNCTION__));
        return -1;
    }

    memset(m_DPB, 0, sizeof(m_DPB));
    for (i = 0; i < MAX_DPB_SIZE + 1; i++) {
        m_DPB[i].fb_index = -1;
    }

    for (i = 0; i < MAX_DPB_SIZE + 1; i++) {
        m_FrameBuffer[i] = -1;
        m_DPBIdx2FBIndexMapping[i] = -1;
    }

    if (1)  //(!no_output_of_prior_pics_flag)
        FlushDpb();

    return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
int32_t VkEncDpbH264::DpbPictureStart(const DpbPicInfo *pDPBPicInfo, const StdVideoH264SequenceParameterSet *sps)
{
    int32_t i;

    memcpy(m_pCurPicInfo, pDPBPicInfo, sizeof(DpbPicInfo));

    FillFrameNumGaps(sps);

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
    if ((m_DPB[m_nCurDPBIdx].state == DPB_TOP || m_DPB[m_nCurDPBIdx].state == DPB_BOTTOM) &&  // contains a single field
            m_pCurPicInfo->field_pic_flag &&                                                      // current is a field
            ((m_DPB[m_nCurDPBIdx].state == DPB_TOP && m_pCurPicInfo->bottom_field_flag) ||
             (m_DPB[m_nCurDPBIdx].state == DPB_BOTTOM && !m_pCurPicInfo->bottom_field_flag)) &&  // opposite parity
            ((!m_DPB[m_nCurDPBIdx].reference_picture &&                                          // first is a non-reference picture
              !m_pCurPicInfo->isRef)                                                             // current is a non-reference picture
             || (m_DPB[m_nCurDPBIdx].reference_picture &&                                        // first is reference picture
                 m_pCurPicInfo->isRef &&                                                         // current is reference picture
                 (m_DPB[m_nCurDPBIdx].picInfo.frame_num == m_pCurPicInfo->frameNum) &&           // same frame_num
                 !m_pCurPicInfo->isIDR))) {                                                      // current is not an IDR picture
        // second field
        m_pCurDPBEntry->complementary_field_pair = true;
    } else {
        m_nCurDPBIdx = MAX_DPB_SIZE;
        m_pCurDPBEntry = &m_DPB[m_nCurDPBIdx];
        if (m_pCurDPBEntry->state != DPB_EMPTY) {
            OutputPicture(m_nCurDPBIdx, true);
        }

        // initialize DPB frame buffer
        m_pCurDPBEntry->state = DPB_EMPTY;
        m_pCurDPBEntry->top_needed_for_output = m_pCurDPBEntry->bottom_needed_for_output = false;
        m_pCurDPBEntry->top_field_marking = m_pCurDPBEntry->bottom_field_marking = MARKING_UNUSED;
        m_pCurDPBEntry->reference_picture = m_pCurPicInfo->isRef;
        m_pCurDPBEntry->top_decoded_first = !m_pCurPicInfo->bottom_field_flag;
        m_pCurDPBEntry->complementary_field_pair = false;
        m_pCurDPBEntry->not_existing = false;
        m_pCurDPBEntry->picInfo.frame_num = m_pCurPicInfo->frameNum;
        m_pCurDPBEntry->timeStamp = m_pCurPicInfo->timeStamp;
        m_pCurDPBEntry->bFrameCorrupted = false;
        if (m_pCurPicInfo->isIDR) {
            m_lastIDRTimeStamp = m_pCurPicInfo->timeStamp;
        }

        m_pCurDPBEntry->fb_index = AllocateFrame();
    }

    CalculatePOC(sps);
    CalculatePicNum(sps);

    for (i = 0; i <= MAX_DPB_SIZE; i++) {
        m_DPBIdx2FBIndexMapping[i] = m_DPB[i].fb_index;
    }

    return m_pCurDPBEntry->fb_index;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// per picture processing after decoding last slice
int32_t VkEncDpbH264::DpbPictureEnd(const StdVideoH264SequenceParameterSet *sps, const StdVideoEncodeH264SliceHeader *slh,
                                    const StdVideoEncodeH264ReferenceListsInfo *ref)
{
    int32_t i;

    if (m_pCurDPBEntry->complementary_field_pair)  // second field of a CFP
        m_pCurDPBEntry->picInfo.PicOrderCnt = min(m_pCurDPBEntry->topFOC, m_pCurDPBEntry->bottomFOC);

    if (m_pCurPicInfo->isRef)  // reference picture
        DecodedRefPicMarking(sps, slh, ref);

    // C.4.4 Removal of pictures from the DPB before possible insertion of the current picture
    if (m_pCurPicInfo->isIDR) { // IDR picture
        // TODO: infer no_output_of_prior_pics_flag if size has changed etc.
        if (m_pCurPicInfo->no_output_of_prior_pics_flag) {
            for (i = 0; i < MAX_DPB_SIZE; i++) {
                m_DPB[i].state = DPB_EMPTY;  // empty
                ReleaseFrame(m_DPB[i].fb_index);
                m_DPB[i].fb_index = -1;
            }
        }
    }

    // this differs from the standard
    // empty frame buffers marked as "not needed for output" and "unused for reference"
    for (i = 0; i < MAX_DPB_SIZE; i++) {
        if ((!(m_DPB[i].state & DPB_TOP) || (!m_DPB[i].top_needed_for_output && m_DPB[i].top_field_marking == MARKING_UNUSED)) &&
                (!(m_DPB[i].state & DPB_BOTTOM) ||
                 (!m_DPB[i].bottom_needed_for_output && m_DPB[i].bottom_field_marking == MARKING_UNUSED))) {
            m_DPB[i].state = DPB_EMPTY;  // empty
            ReleaseFrame(m_DPB[i].fb_index);
            m_DPB[i].fb_index = -1;
        }
    }

    if ((m_pCurPicInfo->isIDR && !m_pCurPicInfo->no_output_of_prior_pics_flag)) {
        while (!IsDpbEmpty()) DpbBumping(false);
    }

    // C.4.5

    if (m_pCurPicInfo->isRef) { // reference picture
        // C.4.5.1
        if (m_pCurDPBEntry->state == DPB_EMPTY) {
            while (IsDpbFull()) DpbBumping(true);

            // find an empty DPB entry, copy current to it
            for (m_nCurDPBIdx = 0; m_nCurDPBIdx < MAX_DPB_SIZE; m_nCurDPBIdx++) {
                if (m_DPB[m_nCurDPBIdx].state == DPB_EMPTY) break;
            }
            if (m_nCurDPBIdx >= MAX_DPB_SIZE) {
                VK_DPB_DBG_PRINT(("could not allocate a frame buffer\n"));
                exit(1);
            }
            if (m_pCurDPBEntry != &m_DPB[m_nCurDPBIdx]) {
                ReleaseFrame(m_DPB[m_nCurDPBIdx].fb_index);
                m_DPB[m_nCurDPBIdx].fb_index = -1;
                m_DPB[m_nCurDPBIdx] = *m_pCurDPBEntry;
            }
            m_pCurDPBEntry = &m_DPB[m_nCurDPBIdx];
        }

        if (!m_pCurPicInfo->field_pic_flag || !m_pCurPicInfo->bottom_field_flag) {
            m_pCurDPBEntry->state |= DPB_TOP;
            m_pCurDPBEntry->top_needed_for_output = true;
        }
        if (!m_pCurPicInfo->field_pic_flag || m_pCurPicInfo->bottom_field_flag) {
            m_pCurDPBEntry->state |= DPB_BOTTOM;
            m_pCurDPBEntry->bottom_needed_for_output = true;
        }
    } else {
        // C.4.5.2
        if (m_pCurDPBEntry->state != DPB_EMPTY) {
            if (m_nCurDPBIdx >= MAX_DPB_SIZE) {
                // output immediately
                OutputPicture(m_nCurDPBIdx, true);
                m_DPB[m_nCurDPBIdx].top_needed_for_output = 0;
                m_DPB[m_nCurDPBIdx].bottom_needed_for_output = 0;
                m_pCurDPBEntry->state = 0;
            } else {
                // second field of a complementary non-reference field pair
                m_pCurDPBEntry->state = DPB_FRAME;
                m_pCurDPBEntry->top_needed_for_output = true;
                m_pCurDPBEntry->bottom_needed_for_output = true;
            }
        } else {
            while (1) {
                if (IsDpbFull()) {
                    // does current have the lowest value of PicOrderCnt?
                    for (i = 0; i < MAX_DPB_SIZE; i++) {
                        // If we decide to support MVC, the following check must
                        // be performed only if the view_id of the current DPB
                        // entry matches the view_id in m_DPB[i].

                        assert(m_DPB[i].topFOC >= 0);
                        assert(m_DPB[i].bottomFOC >= 0);

                        if (((m_DPB[i].state & DPB_TOP) && m_DPB[i].top_needed_for_output &&
                                ((int32_t)m_DPB[i].topFOC) <= m_pCurDPBEntry->picInfo.PicOrderCnt) ||
                                ((m_DPB[i].state & DPB_BOTTOM) && m_DPB[i].bottom_needed_for_output &&
                                 ((int32_t)m_DPB[i].bottomFOC) <= m_pCurDPBEntry->picInfo.PicOrderCnt))
                            break;
                    }
                    if (i < MAX_DPB_SIZE) {
                        DpbBumping(false);
                    } else {
                        // DPB is full, current has lowest value of PicOrderCnt
                        if (!m_pCurPicInfo->field_pic_flag) {
                            // frame: output current picture immediately
                            OutputPicture(m_nCurDPBIdx, true);
                        } else {
                            // field: wait for second field
                            if (!m_pCurPicInfo->bottom_field_flag) {
                                m_pCurDPBEntry->state |= DPB_TOP;
                                m_pCurDPBEntry->top_needed_for_output = true;
                            } else {
                                m_pCurDPBEntry->state |= DPB_BOTTOM;
                                m_pCurDPBEntry->bottom_needed_for_output = true;
                            }
                        }

                        break;  // exit while (1)
                    }
                } else {
                    for (m_nCurDPBIdx = 0; m_nCurDPBIdx < MAX_DPB_SIZE; m_nCurDPBIdx++) {
                        if (m_DPB[m_nCurDPBIdx].state == DPB_EMPTY) break;
                    }
                    if (m_nCurDPBIdx >= MAX_DPB_SIZE) {
                        VK_DPB_DBG_PRINT(("could not allocate a frame buffer\n"));
                        exit(1);
                    }
                    if (m_pCurDPBEntry != &m_DPB[m_nCurDPBIdx]) {
                        ReleaseFrame(m_DPB[m_nCurDPBIdx].fb_index);
                        m_DPB[m_nCurDPBIdx].fb_index = -1;
                        m_DPB[m_nCurDPBIdx] = *m_pCurDPBEntry;
                    }
                    m_pCurDPBEntry = &m_DPB[m_nCurDPBIdx];
                    // store current picture
                    if (!m_pCurPicInfo->field_pic_flag || !m_pCurPicInfo->bottom_field_flag) {
                        m_pCurDPBEntry->state |= DPB_TOP;
                        m_pCurDPBEntry->top_needed_for_output = true;
                    }
                    if (!m_pCurPicInfo->field_pic_flag || m_pCurPicInfo->bottom_field_flag) {
                        m_pCurDPBEntry->state |= DPB_BOTTOM;
                        m_pCurDPBEntry->bottom_needed_for_output = true;
                    }

                    break;  // exit while (1)
                }
            }
        }
    }

    return m_nCurDPBIdx;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.5.2
void VkEncDpbH264::FillFrameNumGaps(const StdVideoH264SequenceParameterSet *sps)
{
    uint32_t UnusedShortTermFrameNum;
    int32_t i;
    DpbPicInfo picSave;
    int32_t MaxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);

    // 7.4.3
    if (m_pCurPicInfo->isIDR)  // IDR picture
        m_PrevRefFrameNum = 0;

    if (m_pCurPicInfo->frameNum != m_PrevRefFrameNum) {
        picSave = *m_pCurPicInfo;

        // (7-10)
        UnusedShortTermFrameNum = (m_PrevRefFrameNum + 1) % MaxFrameNum;
        while (UnusedShortTermFrameNum != picSave.frameNum) {
            VK_DPB_DBG_PRINT(("gaps_in_frame_num: %d ", UnusedShortTermFrameNum));

            if (!sps->flags.gaps_in_frame_num_value_allowed_flag) {
                VK_DPB_DBG_PRINT(("%s (error)::gap in frame_num not allowed\n", __FUNCTION__));
                break;
            }
            m_pCurPicInfo->frameNum = UnusedShortTermFrameNum;
            m_pCurPicInfo->field_pic_flag = 0;
            m_pCurPicInfo->bottom_field_flag = 0;
            m_pCurPicInfo->isRef = 1;
            m_pCurPicInfo->isIDR = 0;
            m_pCurPicInfo->adaptive_ref_pic_marking_mode_flag = 0;

            // TODO: what else
            // DPB handling (C.4.2)
            while (IsDpbFull()) DpbBumping(true);
            for (m_nCurDPBIdx = 0; m_nCurDPBIdx < MAX_DPB_SIZE; m_nCurDPBIdx++) {
                if (m_DPB[m_nCurDPBIdx].state == DPB_EMPTY) break;
            }
            if (m_nCurDPBIdx >= MAX_DPB_SIZE) VK_DPB_DBG_PRINT(("%s (error)::could not allocate a frame buffer\n", __FUNCTION__));
            // initialize DPB frame buffer
            m_pCurDPBEntry = &m_DPB[m_nCurDPBIdx];
            m_pCurDPBEntry->picInfo.frame_num = m_pCurPicInfo->frameNum;
            m_pCurDPBEntry->complementary_field_pair = false;
            if (sps->pic_order_cnt_type != STD_VIDEO_H264_POC_TYPE_0) CalculatePOC(sps);
            CalculatePicNum(sps);

            SlidingWindowMemoryManagememt(sps);

            m_pCurDPBEntry->top_field_marking = m_pCurDPBEntry->bottom_field_marking = MARKING_SHORT;
            m_pCurDPBEntry->reference_picture = true;
            m_pCurDPBEntry->top_decoded_first = false;
            m_pCurDPBEntry->not_existing = true;
            // C.4.2
            m_pCurDPBEntry->top_needed_for_output = m_pCurDPBEntry->bottom_needed_for_output = false;
            m_pCurDPBEntry->state = DPB_FRAME;  // frame
            // this differs from the standard
            // empty frame buffers marked as "not needed for output" and "unused for reference"
            for (i = 0; i < MAX_DPB_SIZE; i++) {
                if ((!(m_DPB[i].state & DPB_TOP) ||
                        (!m_DPB[i].top_needed_for_output && m_DPB[i].top_field_marking == MARKING_UNUSED)) &&
                        (!(m_DPB[i].state & DPB_BOTTOM) ||
                         (!m_DPB[i].bottom_needed_for_output && m_DPB[i].bottom_field_marking == MARKING_UNUSED))) {
                    m_DPB[i].state = DPB_EMPTY;  // empty
                    ReleaseFrame(m_DPB[i].fb_index);
                    m_DPB[i].fb_index = -1;
                }
            }

            // 7.4.3
            m_PrevRefFrameNum = m_pCurPicInfo->frameNum;  // TODO: only if previous picture was a reference picture?
            UnusedShortTermFrameNum = (UnusedShortTermFrameNum + 1) % MaxFrameNum;
        }
        *m_pCurPicInfo = picSave;
    }

    // 7.4.3
    if (m_pCurPicInfo->isRef)  // reference picture
        m_PrevRefFrameNum = m_pCurPicInfo->frameNum;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// DPB
bool VkEncDpbH264::IsDpbFull()
{
    int32_t dpb_fullness, i;

    dpb_fullness = 0;
    for (i = 0; i < MAX_DPB_SIZE; i++) {
        if (m_DPB[i].state != DPB_EMPTY) dpb_fullness++;
    }

    return dpb_fullness >= m_max_dpb_size;
}

bool VkEncDpbH264::IsDpbEmpty()
{
    int32_t dpb_fullness, i;

    dpb_fullness = 0;
    for (i = 0; i < MAX_DPB_SIZE; i++) {
        if (m_DPB[i].state != DPB_EMPTY) dpb_fullness++;
    }

    return dpb_fullness == 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// C.4.5.3
void VkEncDpbH264::DpbBumping(bool alwaysbump)
{
    int32_t i = 0, pocMin = INF_MAX, iMin = -1;
    int32_t prevOutputIdx = -1;

    // If we decide to implement MVC, we'll need to loop over all the views
    // configured for this session and perform each check in the for loop
    // immediately below only if the current DPB entry's view_id matches
    // that of m_DPB[i].

    // select the frame buffer that contains the picture having the smallest value
    // of PicOrderCnt of all pictures in the DPB marked as "needed for output"
    pocMin = INF_MAX;
    iMin = -1;
    for (i = 0; i < MAX_DPB_SIZE; i++) {
        if ((m_DPB[i].state & DPB_TOP) && m_DPB[i].top_needed_for_output && m_DPB[i].topFOC < pocMin) {
            pocMin = m_DPB[i].topFOC;
            iMin = i;
        }
        if ((m_DPB[i].state & DPB_BOTTOM) && m_DPB[i].bottom_needed_for_output && m_DPB[i].bottomFOC < pocMin) {
            pocMin = m_DPB[i].bottomFOC;
            iMin = i;
        }
    }

    if (iMin >= 0) {
        OutputPicture(iMin, false);
        m_DPB[iMin].top_needed_for_output = 0;
        m_DPB[iMin].bottom_needed_for_output = 0;
        prevOutputIdx = iMin;

        // empty frame buffer
        if ((!(m_DPB[iMin].state & DPB_TOP) ||
                (!m_DPB[iMin].top_needed_for_output && m_DPB[iMin].top_field_marking == MARKING_UNUSED)) &&
                (!(m_DPB[iMin].state & DPB_BOTTOM) ||
                 (!m_DPB[iMin].bottom_needed_for_output && m_DPB[iMin].bottom_field_marking == MARKING_UNUSED))) {
            m_DPB[iMin].state = 0;
            ReleaseFrame(m_DPB[iMin].fb_index);
            m_DPB[iMin].fb_index = -1;
        }
    }

    // Special case to avoid deadlocks
    if ((prevOutputIdx < 0) && (alwaysbump)) {
        for (i = 0; i < MAX_DPB_SIZE; i++) {
            if ((m_DPB[i].state & DPB_TOP) && (m_DPB[i].topFOC <= pocMin)) {
                pocMin = m_DPB[i].topFOC;
                iMin = i;
            }
            if ((m_DPB[i].state & DPB_BOTTOM) && (m_DPB[i].bottomFOC <= pocMin)) {
                pocMin = m_DPB[i].bottomFOC;
                iMin = i;
            }
        }
        m_DPB[iMin].state = DPB_EMPTY;
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.5, 8.2.5.1
void VkEncDpbH264::DecodedRefPicMarking(const StdVideoH264SequenceParameterSet *sps,
                                        const StdVideoEncodeH264SliceHeader *slh,
                                        const StdVideoEncodeH264ReferenceListsInfo *ref)
{
    int32_t i;

    if (m_pCurPicInfo->isIDR) { // IDR picture
        // All reference pictures shall be marked as "unused for reference"
        for (i = 0; i < MAX_DPB_SIZE; i++) {
            m_DPB[i].top_field_marking = MARKING_UNUSED;
            m_DPB[i].bottom_field_marking = MARKING_UNUSED;
        }
        if (!m_pCurPicInfo->isLongTerm) {
            // the IDR picture shall be marked as "used for short-term reference"
            if (!m_pCurPicInfo->field_pic_flag || !m_pCurPicInfo->bottom_field_flag)
                m_pCurDPBEntry->top_field_marking = MARKING_SHORT;
            if (!m_pCurPicInfo->field_pic_flag || m_pCurPicInfo->bottom_field_flag)
                m_pCurDPBEntry->bottom_field_marking = MARKING_SHORT;
            // MaxLongTermFrameIdx shall be set equal to "no long-term frame indices".
            m_MaxLongTermFrameIdx = -1;
        } else { // (slh->long_term_reference_flag == 1)
            // the IDR picture shall be marked as "used for long-term reference"
            if (!m_pCurPicInfo->field_pic_flag || !m_pCurPicInfo->bottom_field_flag)
                m_pCurDPBEntry->top_field_marking = MARKING_LONG;
            if (!m_pCurPicInfo->field_pic_flag || m_pCurPicInfo->bottom_field_flag)
                m_pCurDPBEntry->bottom_field_marking = MARKING_LONG;
            // the LongTermFrameIdx for the IDR picture shall be set equal to 0
            m_pCurDPBEntry->LongTermFrameIdx = 0;
            // MaxLongTermFrameIdx shall be set equal to 0.
            m_MaxLongTermFrameIdx = 0;
        }
    } else {
        if (!m_pCurPicInfo->adaptive_ref_pic_marking_mode_flag)
            SlidingWindowMemoryManagememt(sps);
        else  // (slh->adaptive_ref_pic_marking_mode_flag == 1)
            AdaptiveMemoryManagement(ref);

        // mark current as short-term if not marked as long-term (8.2.5.1)
        if ((!m_pCurPicInfo->field_pic_flag || !m_pCurPicInfo->bottom_field_flag) &&
                m_pCurDPBEntry->top_field_marking == MARKING_UNUSED)
            m_pCurDPBEntry->top_field_marking = MARKING_SHORT;
        if ((!m_pCurPicInfo->field_pic_flag || m_pCurPicInfo->bottom_field_flag) &&
                m_pCurDPBEntry->bottom_field_marking == MARKING_UNUSED)
            m_pCurDPBEntry->bottom_field_marking = MARKING_SHORT;
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.5.3
void VkEncDpbH264::SlidingWindowMemoryManagememt(const StdVideoH264SequenceParameterSet *sps)
{
    int32_t numShortTerm, numLongTerm;
    int32_t i, imin, minFrameNumWrap;

    // If the current picture is a coded field that is the second field in decoding order
    // of a complementary reference field pair, and the first field has been marked as
    // "used for short-term reference", the current picture is also marked as
    // "used for short-term reference".
    // note: I think this could be simplified as
    // if (m_pCurDPBEntry->top_field_marking == MARKING_SHORT || m_pCurDPBEntry->bottom_field_marking == MARKING_SHORT)
    if (m_pCurPicInfo->field_pic_flag &&
            ((!m_pCurPicInfo->bottom_field_flag && m_pCurDPBEntry->bottom_field_marking == MARKING_SHORT) ||
             (m_pCurPicInfo->bottom_field_flag && m_pCurDPBEntry->top_field_marking == MARKING_SHORT))) {
        if (!m_pCurPicInfo->bottom_field_flag)
            m_pCurDPBEntry->top_field_marking = MARKING_SHORT;
        else
            m_pCurDPBEntry->bottom_field_marking = MARKING_SHORT;
    } else {
        imin = MAX_DPB_SIZE;
        minFrameNumWrap = 65536;
        numShortTerm = 0;
        numLongTerm = 0;
        for (i = 0; i < MAX_DPB_SIZE; i++) {
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
        if (numShortTerm + numLongTerm >= sps->max_num_ref_frames) {
            if (numShortTerm > 0 && imin < MAX_DPB_SIZE) {
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
void VkEncDpbH264::AdaptiveMemoryManagement(const StdVideoEncodeH264ReferenceListsInfo *ref)
{
    int32_t picNumX, CurrPicNum;
    int32_t i, k;

    const StdVideoEncodeH264RefPicMarkingEntry *mmco = ref->pRefPicMarkingOperations;

    CurrPicNum = (!m_pCurPicInfo->field_pic_flag) ? m_pCurPicInfo->frameNum : 2 * m_pCurPicInfo->frameNum + 1;

    for (k = 0; ((k < MAX_MMCOS) && (mmco[k].memory_management_control_operation != STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_END)); k++) {
        switch (mmco[k].memory_management_control_operation) {
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_UNMARK_SHORT_TERM:
            // 8.2.5.4.1 Marking process of a short-term picture as "unused for reference"
            VK_DPB_DBG_PRINT(("%d ", mmco[k].difference_of_pic_nums_minus1));

            picNumX = CurrPicNum - (mmco[k].difference_of_pic_nums_minus1 + 1);  // (8-40)
            for (i = 0; i < MAX_DPB_SIZE; i++) {
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
            for (i = 0; i < MAX_DPB_SIZE; i++) {
                if (m_DPB[i].top_field_marking == MARKING_LONG && m_DPB[i].topLongTermPicNum == (int32_t)mmco[k].long_term_pic_num)
                    m_DPB[i].top_field_marking = MARKING_UNUSED;
                if (m_DPB[i].bottom_field_marking == MARKING_LONG &&
                        m_DPB[i].bottomLongTermPicNum == (int32_t)mmco[k].long_term_pic_num)
                    m_DPB[i].bottom_field_marking = MARKING_UNUSED;
            }
            break;
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_MARK_LONG_TERM:
            picNumX = CurrPicNum - (mmco[k].difference_of_pic_nums_minus1 + 1);  // (8-40)
            // 8.2.5.4.3 Assignment process of a LongTermFrameIdx to a short-term reference picture
            for (i = 0; i < MAX_DPB_SIZE; i++) {
                if (m_DPB[i].top_field_marking == MARKING_LONG &&
                        m_DPB[i].LongTermFrameIdx == (int32_t)mmco[k].long_term_frame_idx &&
                        !(m_DPB[i].bottom_field_marking == MARKING_SHORT && m_DPB[i].bottomPicNum == picNumX))
                    m_DPB[i].top_field_marking = MARKING_UNUSED;
                if (m_DPB[i].bottom_field_marking == MARKING_LONG &&
                        m_DPB[i].LongTermFrameIdx == (int32_t)mmco[k].long_term_frame_idx &&
                        !(m_DPB[i].top_field_marking == MARKING_SHORT && m_DPB[i].topPicNum == picNumX))
                    m_DPB[i].bottom_field_marking = MARKING_UNUSED;
                if (m_DPB[i].top_field_marking == MARKING_SHORT && m_DPB[i].topPicNum == picNumX) {
                    m_DPB[i].top_field_marking = MARKING_LONG;
                    m_DPB[i].LongTermFrameIdx = mmco[k].long_term_frame_idx;
                    // update topLongTermPicNum, bottomLongTermPicNum for subsequent mmco 2
                    if (!m_pCurPicInfo->field_pic_flag) {
                        // frame
                        m_DPB[i].topLongTermPicNum = m_DPB[i].bottomLongTermPicNum = m_DPB[i].LongTermFrameIdx;  // (8-30)
                    } else if (!m_pCurPicInfo->bottom_field_flag) {
                        // top field
                        m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx + 1;  // same parity (8-33)
                        m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx;   // opposite parity (8-34)
                    } else {
                        // bottom field
                        m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx;         // opposite parity (8-34)
                        m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx + 1;  // same parity (8-33)
                    }
                }
                if (m_DPB[i].bottom_field_marking == MARKING_SHORT && m_DPB[i].bottomPicNum == picNumX) {
                    m_DPB[i].bottom_field_marking = MARKING_LONG;
                    m_DPB[i].LongTermFrameIdx = mmco[k].long_term_frame_idx;
                    // update topLongTermPicNum, bottomLongTermPicNum for subsequent mmco 2
                    if (!m_pCurPicInfo->field_pic_flag) {
                        // frame
                        m_DPB[i].topLongTermPicNum = m_DPB[i].bottomLongTermPicNum = m_DPB[i].LongTermFrameIdx;  // (8-30)
                    } else if (!m_pCurPicInfo->bottom_field_flag) {
                        // top field
                        m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx + 1;  // same parity (8-33)
                        m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx;   // opposite parity (8-34)
                    } else {
                        // bottom field
                        m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx;         // opposite parity (8-34)
                        m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx + 1;  // same parity (8-33)
                    }
                }
            }
            break;
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_SET_MAX_LONG_TERM_INDEX:
            // 8.2.5.4.4 Decoding process for MaxLongTermFrameIdx
            m_MaxLongTermFrameIdx = mmco[k].max_long_term_frame_idx_plus1 - 1;
            for (i = 0; i < MAX_DPB_SIZE; i++) {
                if (m_DPB[i].top_field_marking == MARKING_LONG && m_DPB[i].LongTermFrameIdx > m_MaxLongTermFrameIdx)
                    m_DPB[i].top_field_marking = MARKING_UNUSED;
                if (m_DPB[i].bottom_field_marking == MARKING_LONG && m_DPB[i].LongTermFrameIdx > m_MaxLongTermFrameIdx)
                    m_DPB[i].bottom_field_marking = MARKING_UNUSED;
            }
            break;
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_UNMARK_ALL:
            // 8.2.5.4.5 Marking process of all reference pictures as "unused for reference" and setting MaxLongTermFrameIdx to
            // "no long-term frame indices"
            for (i = 0; i < MAX_DPB_SIZE; i++) {
                m_DPB[i].top_field_marking = MARKING_UNUSED;
                m_DPB[i].bottom_field_marking = MARKING_UNUSED;
            }
            m_MaxLongTermFrameIdx = -1;
            m_pCurDPBEntry->picInfo.frame_num = 0;  // 7.4.3
            // 8.2.1
            m_pCurDPBEntry->topFOC -= m_pCurDPBEntry->picInfo.PicOrderCnt;
            m_pCurDPBEntry->bottomFOC -= m_pCurDPBEntry->picInfo.PicOrderCnt;
            m_pCurDPBEntry->picInfo.PicOrderCnt = 0;
            break;
        case STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_MARK_CURRENT_AS_LONG_TERM:
            // 8.2.5.4.6 Process for assigning a long-term frame index to the current picture
            VK_DPB_DBG_PRINT(("%d ", mmco[k].long_term_frame_idx));
            for (i = 0; i < MAX_DPB_SIZE; i++) {
                if (i != m_nCurDPBIdx && m_DPB[i].top_field_marking == MARKING_LONG &&
                        m_DPB[i].LongTermFrameIdx == (int32_t)mmco[k].long_term_frame_idx)
                    m_DPB[i].top_field_marking = MARKING_UNUSED;
                if (i != m_nCurDPBIdx && m_DPB[i].bottom_field_marking == MARKING_LONG &&
                        m_DPB[i].LongTermFrameIdx == (int32_t)mmco[k].long_term_frame_idx)
                    m_DPB[i].bottom_field_marking = MARKING_UNUSED;
            }

            if (!m_pCurPicInfo->field_pic_flag || !m_pCurPicInfo->bottom_field_flag)
                m_pCurDPBEntry->top_field_marking = MARKING_LONG;
            if (!m_pCurPicInfo->field_pic_flag || m_pCurPicInfo->bottom_field_flag)
                m_pCurDPBEntry->bottom_field_marking = MARKING_LONG;

            m_pCurDPBEntry->LongTermFrameIdx = mmco[k].long_term_frame_idx;
            // update topLongTermPicNum, bottomLongTermPicNum
            // (subsequent mmco 2 is not allowed to reference it, but to avoid accidental matches they have to be updated)
            if (!m_pCurPicInfo->field_pic_flag) {
                // frame
                m_pCurDPBEntry->topLongTermPicNum = m_pCurDPBEntry->bottomLongTermPicNum =
                                                        m_pCurDPBEntry->LongTermFrameIdx;  // (8-30)
            } else if (!m_pCurPicInfo->bottom_field_flag) {
                // top field
                m_pCurDPBEntry->topLongTermPicNum = 2 * m_pCurDPBEntry->LongTermFrameIdx + 1;  // same parity (8-33)
                m_pCurDPBEntry->bottomLongTermPicNum = 2 * m_pCurDPBEntry->LongTermFrameIdx;   // opposite parity (8-34)
            } else {
                // bottom field
                m_pCurDPBEntry->topLongTermPicNum = 2 * m_pCurDPBEntry->LongTermFrameIdx;         // opposite parity (8-34)
                m_pCurDPBEntry->bottomLongTermPicNum = 2 * m_pCurDPBEntry->LongTermFrameIdx + 1;  // same parity (8-33)
            }

            break;
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
void VkEncDpbH264::CalculatePOC(const StdVideoH264SequenceParameterSet *sps)
{
    if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0) {
        CalculatePOCType0(sps);
    } else {
        CalculatePOCType2(sps);
    }
    // (8-1)
    if (!m_pCurPicInfo->field_pic_flag || m_pCurDPBEntry->complementary_field_pair)  // not second field of a CFP
        m_pCurDPBEntry->picInfo.PicOrderCnt = min(m_pCurDPBEntry->topFOC, m_pCurDPBEntry->bottomFOC);
    else if (!m_pCurPicInfo->bottom_field_flag)
        m_pCurDPBEntry->picInfo.PicOrderCnt = m_pCurDPBEntry->topFOC;
    else
        m_pCurDPBEntry->picInfo.PicOrderCnt = m_pCurDPBEntry->bottomFOC;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.1.1
void VkEncDpbH264::CalculatePOCType0(const StdVideoH264SequenceParameterSet *sps)
{
    int32_t MaxPicOrderCntLsb, PicOrderCntMsb;

    if (m_pCurPicInfo->isIDR) { // IDR picture
        m_prevPicOrderCntMsb = 0;
        m_prevPicOrderCntLsb = 0;
    }

    MaxPicOrderCntLsb = 1 << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4);  // (7-2)

    // (8-3)
    if ((m_pCurPicInfo->PicOrderCnt < m_prevPicOrderCntLsb) &&
            ((m_prevPicOrderCntLsb - m_pCurPicInfo->PicOrderCnt) >= (MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = m_prevPicOrderCntMsb + MaxPicOrderCntLsb;
    else if ((m_pCurPicInfo->PicOrderCnt > m_prevPicOrderCntLsb) &&
             ((m_pCurPicInfo->PicOrderCnt - m_prevPicOrderCntLsb) > (MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = m_prevPicOrderCntMsb - MaxPicOrderCntLsb;
    else
        PicOrderCntMsb = m_prevPicOrderCntMsb;

    // (8-4)
    if (!m_pCurPicInfo->field_pic_flag || !m_pCurPicInfo->bottom_field_flag)
        m_pCurDPBEntry->topFOC = PicOrderCntMsb + m_pCurPicInfo->PicOrderCnt;

    // (8-5)
    if (!m_pCurPicInfo->field_pic_flag || m_pCurPicInfo->bottom_field_flag)
        m_pCurDPBEntry->bottomFOC = PicOrderCntMsb + m_pCurPicInfo->PicOrderCnt;

    if (m_pCurPicInfo->isRef) { // reference picture
        m_prevPicOrderCntMsb = PicOrderCntMsb;
        m_prevPicOrderCntLsb = m_pCurPicInfo->PicOrderCnt;
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.1.2 - Unimplemented because we're not going to handle POC type 1.

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.1.3
void VkEncDpbH264::CalculatePOCType2(const StdVideoH264SequenceParameterSet *sps)
{
    int32_t FrameNumOffset, tempPicOrderCnt;
    int32_t MaxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);  // (7-1)

    // FrameNumOffset (8-12)
    if (m_pCurPicInfo->isIDR)
        FrameNumOffset = 0;
    else if (m_prevFrameNum > m_pCurPicInfo->frameNum)
        FrameNumOffset = m_prevFrameNumOffset + MaxFrameNum;
    else
        FrameNumOffset = m_prevFrameNumOffset;

    // tempPicOrderCnt (8-13)
    if (m_pCurPicInfo->isIDR)
        tempPicOrderCnt = 0;
    else if (!m_pCurPicInfo->isRef)
        tempPicOrderCnt = 2 * (FrameNumOffset + m_pCurPicInfo->frameNum) - 1;
    else
        tempPicOrderCnt = 2 * (FrameNumOffset + m_pCurPicInfo->frameNum);

    // topFOC, bottomFOC (8-14)
    if (!m_pCurPicInfo->field_pic_flag) {
        m_pCurDPBEntry->topFOC = tempPicOrderCnt;
        m_pCurDPBEntry->bottomFOC = tempPicOrderCnt;
    } else if (m_pCurPicInfo->bottom_field_flag)
        m_pCurDPBEntry->bottomFOC = tempPicOrderCnt;
    else
        m_pCurDPBEntry->topFOC = tempPicOrderCnt;

    m_prevFrameNumOffset = FrameNumOffset;
    m_prevFrameNum = m_pCurPicInfo->frameNum;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// 8.2.4.1  Derivation of picture numbers
void VkEncDpbH264::CalculatePicNum(const StdVideoH264SequenceParameterSet *sps)
{
    int32_t i;
    int32_t MaxFrameNum;

    MaxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);  // (7-1)

    assert(m_pCurPicInfo->frameNum != (uint32_t)(-1));

    for (i = 0; i < MAX_DPB_SIZE; i++) {
        // (8-28)
        if (m_DPB[i].picInfo.frame_num > ((uint32_t)m_pCurPicInfo->frameNum))
            m_DPB[i].frameNumWrap = m_DPB[i].picInfo.frame_num - MaxFrameNum;
        else
            m_DPB[i].frameNumWrap = m_DPB[i].picInfo.frame_num;

        if (!m_pCurPicInfo->field_pic_flag) {
            // frame
            m_DPB[i].topPicNum = m_DPB[i].bottomPicNum = m_DPB[i].frameNumWrap;                      // (8-29)
            m_DPB[i].topLongTermPicNum = m_DPB[i].bottomLongTermPicNum = m_DPB[i].LongTermFrameIdx;  // (8-30)
        } else if (!m_pCurPicInfo->bottom_field_flag) {
            // top field
            m_DPB[i].topPicNum = 2 * m_DPB[i].frameNumWrap + 1;              // same parity (8-31)
            m_DPB[i].bottomPicNum = 2 * m_DPB[i].frameNumWrap;               // opposite parity (8-32)
            m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx + 1;  // same parity (8-33)
            m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx;   // opposite parity (8-34)
        } else {
            // bottom field
            m_DPB[i].topPicNum = 2 * m_DPB[i].frameNumWrap;                     // opposite parity (8-32)
            m_DPB[i].bottomPicNum = 2 * m_DPB[i].frameNumWrap + 1;              // same parity (8-31)
            m_DPB[i].topLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx;         // opposite parity (8-34)
            m_DPB[i].bottomLongTermPicNum = 2 * m_DPB[i].LongTermFrameIdx + 1;  // same parity (8-33)
        }
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VkEncDpbH264::OutputPicture(int32_t dpb_index, bool release)
{
    if (release) {
        ReleaseFrame(m_DPB[dpb_index].fb_index);
        m_DPB[dpb_index].fb_index = -1;
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VkEncDpbH264::FlushDpb()
{
    int32_t i;
    // mark all reference pictures as "unused for reference"
    for (i = 0; i < MAX_DPB_SIZE; i++) {
        m_DPB[i].top_field_marking = MARKING_UNUSED;
        m_DPB[i].bottom_field_marking = MARKING_UNUSED;
    }
    // empty frame buffers marked as "not needed for output" and "unused for reference"
    for (i = 0; i < MAX_DPB_SIZE; i++) {
        if ((!(m_DPB[i].state & DPB_TOP) || (!m_DPB[i].top_needed_for_output && m_DPB[i].top_field_marking == MARKING_UNUSED)) &&
                (!(m_DPB[i].state & DPB_BOTTOM) ||
                 (!m_DPB[i].bottom_needed_for_output && m_DPB[i].bottom_field_marking == MARKING_UNUSED))) {
            m_DPB[i].state = DPB_EMPTY;  // empty
            ReleaseFrame(m_DPB[i].fb_index);
            m_DPB[i].fb_index = -1;
        }
    }
    while (!IsDpbEmpty()) DpbBumping(true);
}

int32_t VkEncDpbH264::GetRefPicIdx(int32_t dpbIdx)
{
    if ((dpbIdx >= 0) && (dpbIdx <= MAX_DPB_SIZE)) {
        return (m_DPB[dpbIdx].fb_index);
    } else {
        VK_DPB_DBG_PRINT(("Error : getFrameType : Wrong picture index %d\n", dpbIdx));
        return (-1);
    }
}

int32_t VkEncDpbH264::GetPicturePOC(int32_t picIndexField)
{
    int32_t dpb_idx = picIndexField >> 1;

    if ((dpb_idx >= 0) && (dpb_idx <= MAX_DPB_SIZE) && (m_DPB[dpb_idx].state != DPB_EMPTY)) {
        if (((m_DPB[dpb_idx].state & DPB_BOTTOM) == DPB_BOTTOM) && (picIndexField & 1)) {
            return (m_DPB[dpb_idx].bottomFOC);
        } else {
            return (m_DPB[dpb_idx].topFOC);
        }
    }

#if 0
    if ( (dpb_idx >= 0) && (dpb_idx < MAX_DPB_SIZE) && (m_DPB[dpb_idx].state != DPB_EMPTY) )
        return m_DPB[dpb_idx].picInfo.PicOrderCnt;
#endif

    VK_DPB_DBG_PRINT(("Error : GetPicturePOC : Wrong picture index %d\n", picIndexField));
    return -1;
}

void VkEncDpbH264::GetRefPicList(NvVideoEncodeH264DpbSlotInfoLists<2 * MAX_REFS>* pDpbSlotInfoLists,
                                 const StdVideoH264SequenceParameterSet *sps,
                                 const StdVideoH264PictureParameterSet *pps, const StdVideoEncodeH264SliceHeader *slh,
                                 const StdVideoEncodeH264ReferenceListsInfo *ref, bool bSkipCorruptFrames)
{
    int32_t num_list[2] = {0, 0};
    RefPicListEntry stRefPicList[2][MAX_REFS + 1];  // one additional entry is used in sorting

    m_max_num_list[0] = 0;
    m_max_num_list[1] = 0;
    RefPicListInitialization(stRefPicList[0], stRefPicList[1], sps, bSkipCorruptFrames);

    if (!bSkipCorruptFrames) {
        RefPicListReordering(stRefPicList[0], stRefPicList[1], sps, slh, ref);
    }

    if (slh->flags.num_ref_idx_active_override_flag) {
        m_max_num_list[0] = ref->num_ref_idx_l0_active_minus1 + 1;
        m_max_num_list[1] = ref->num_ref_idx_l1_active_minus1 + 1;
    } else {
        m_max_num_list[0] = min(DeriveL0RefCount(stRefPicList[0]), pps->num_ref_idx_l0_default_active_minus1 + 1);
        m_max_num_list[1] = min(DeriveL1RefCount(stRefPicList[1]), pps->num_ref_idx_l1_default_active_minus1 + 1);
    }

    for (int32_t i = 0; i < m_max_num_list[0]; i++) {
        int32_t dpbIndex = stRefPicList[0][i].dpbIndex;
        if (dpbIndex == -1) break;

        pDpbSlotInfoLists->refPicList0[i] = dpbIndex;

        pDpbSlotInfoLists->dpbSlotsUseMask |= (1 << dpbIndex);

        (num_list[0])++;
    }

    for (int32_t i = 0; i < m_max_num_list[1]; i++) {
        int32_t dpbIndex = stRefPicList[1][i].dpbIndex;
        if (dpbIndex == -1) break;

        pDpbSlotInfoLists->refPicList1[i] = dpbIndex;

        pDpbSlotInfoLists->dpbSlotsUseMask |= (1 << dpbIndex);

        (num_list[1])++;
    }

#if 0
    VK_DPB_DBG_PRINT(( "\n----> GET_REF_PIC_LIST"));
    VK_DPB_DBG_PRINT(( "\n      PicType %s", (m_pCurPicInfo->pictureType == 0) ? "P": (m_pCurPicInfo->pictureType == 1) ? "B": "I"));
    VK_DPB_DBG_PRINT(("\n      PicOrderCnt %d (isBottom = %d)", m_pCurPicInfo->PicOrderCnt, m_pCurPicInfo->bottom_field_flag));

    for (int32_t lx=0; lx<2; lx++) {
        VK_DPB_DBG_PRINT(( "\n      RefPicList[%d]:", lx ));
        for (int32_t i=0; i<MAX_REFS; i++) {
            VK_DPB_DBG_PRINT((" %3d ", stRefPicList[lx][i].dpbIndex));
        }
        VK_DPB_DBG_PRINT(("\n"));
        VK_DPB_DBG_PRINT(("\n----- MaxNumList[%d] = %d", lx, m_max_num_list[lx]));
        VK_DPB_DBG_PRINT(("\n"));
    }

#endif

    pDpbSlotInfoLists->refPicList0Count = num_list[0];
    pDpbSlotInfoLists->refPicList1Count = num_list[1];
}

// 8.2.4.2
void VkEncDpbH264::RefPicListInitialization(RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
        const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames)
{
    int32_t k;

    // TODO: how to handle not-existing pictures?
    for (k = 0; k < (MAX_REFS + 1); k++) {
        RefPicList0[k].dpbIndex = -1;  // "no reference picture"
        RefPicList1[k].dpbIndex = -1;  // "no reference picture"
    }

    if (m_pCurPicInfo->pictureType == STD_VIDEO_H26X_PICTURE_TYPE_P) {
        if (!m_pCurPicInfo->field_pic_flag)
            RefPicListInitializationPFrame(RefPicList0, sps, bSkipCorruptFrames);
        else
            RefPicListInitializationPField(RefPicList0, sps, bSkipCorruptFrames);
    } else if (m_pCurPicInfo->pictureType == STD_VIDEO_H26X_PICTURE_TYPE_B) {
        if (!m_pCurPicInfo->field_pic_flag)
            RefPicListInitializationBFrame(RefPicList0, RefPicList1, sps, bSkipCorruptFrames);
        else
            RefPicListInitializationBField(RefPicList0, RefPicList1, sps, bSkipCorruptFrames);
    }
}

// 8.2.4.2.1
void VkEncDpbH264::RefPicListInitializationPFrame(RefPicListEntry *RefPicList0, const StdVideoH264SequenceParameterSet *sps,
        bool bSkipCorruptFrames)
{
    int32_t k;

    // short-term frames sorted by descending PicNum
    k = SortListDescending(RefPicList0, sps, 0, INF_MAX, sort_check_short_term_P_frame, bSkipCorruptFrames);
    // long-term frames sorted by ascending LongTermPicNum
    k = SortListAscending(RefPicList0, sps, k, INF_MIN, sort_check_long_term_frame, bSkipCorruptFrames);

    m_max_num_list[0] = k;
}

// 8.2.4.2.2
void VkEncDpbH264::RefPicListInitializationPField(RefPicListEntry *RefPicList0, const StdVideoH264SequenceParameterSet *sps,
        bool bSkipCorruptFrames)
{
    RefPicListEntry refFrameList0ShortTerm[MAX_REFS], refFrameListLongTerm[MAX_REFS];
    int32_t ksmax, klmax, k;

    ksmax = SortListDescending(refFrameList0ShortTerm, sps, 0, INF_MAX, sort_check_short_term_P_field, bSkipCorruptFrames);
    klmax = SortListAscending(refFrameListLongTerm, sps, 0, INF_MIN, sort_check_long_term_field, bSkipCorruptFrames);

    k = RefPicListInitializationField(refFrameList0ShortTerm, refFrameListLongTerm, ksmax, klmax, RefPicList0, bSkipCorruptFrames);

    m_max_num_list[0] = k;
}

// 8.2.4.2.3
void VkEncDpbH264::RefPicListInitializationBFrame(RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
        const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames)
{
    int32_t k, k0, k1, idx;

    // list 0
    k0 = RefPicListInitializationBFrameListX(RefPicList0, sps, false, bSkipCorruptFrames);

    // list 1
    k1 = RefPicListInitializationBFrameListX(RefPicList1, sps, true, bSkipCorruptFrames);

    if (k1 > 1 && k0 == k1) {
        // note: it may be sufficient to only check if the first entry is identical
        // (this should imply that the entire list is identical)
        for (k = 0; k < k1; k++) {
            if (RefPicList0[k].dpbIndex != RefPicList1[k].dpbIndex) break;
        }
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

// 8.2.4.2.4
void VkEncDpbH264::RefPicListInitializationBField(RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
        const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames)
{
    RefPicListEntry refFrameList0ShortTerm[MAX_REFS], refFrameList1ShortTerm[MAX_REFS], refFrameListLongTerm[MAX_REFS];
    int32_t currPOC;

    currPOC = !m_pCurPicInfo->bottom_field_flag ? m_pCurDPBEntry->topFOC : m_pCurDPBEntry->bottomFOC;

    int32_t k0 = SortListDescending(refFrameList0ShortTerm, sps, 0, currPOC, sort_check_short_term_B_field, bSkipCorruptFrames);
    k0 = SortListAscending(refFrameList0ShortTerm, sps, k0, currPOC, sort_check_short_term_B_field, bSkipCorruptFrames);

    int32_t k1 = SortListAscending(refFrameList1ShortTerm, sps, 0, currPOC, sort_check_short_term_B_field, bSkipCorruptFrames);
    k1 = SortListDescending(refFrameList1ShortTerm, sps, k1, currPOC, sort_check_short_term_B_field, bSkipCorruptFrames);

    int32_t kl = SortListAscending(refFrameListLongTerm, sps, 0, INF_MIN, sort_check_long_term_field, bSkipCorruptFrames);

    k0 = RefPicListInitializationField(refFrameList0ShortTerm, refFrameListLongTerm, k0, kl, RefPicList0, bSkipCorruptFrames);
    k1 = RefPicListInitializationField(refFrameList1ShortTerm, refFrameListLongTerm, k1, kl, RefPicList1, bSkipCorruptFrames);

    if (k1 > 1 && k0 == k1) {
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
        int32_t ksmax, int32_t klmax, RefPicListEntry *RefPicListX, bool bSkipCorruptFrames)
{
    int32_t k = RefPicListInitializationFieldListX(refFrameListXShortTerm, ksmax, 0, RefPicListX, bSkipCorruptFrames);
    k = RefPicListInitializationFieldListX(refFrameListLongTerm, klmax, k, RefPicListX, bSkipCorruptFrames);

    return k;
}

int32_t VkEncDpbH264::RefPicListInitializationFieldListX(RefPicListEntry *refFrameListX, int32_t kfmax, int32_t kmin,
        RefPicListEntry *RefPicListX, bool bSkipCorruptFrames)
{
    int32_t bottom, k, ktop, kbot;

    bottom = m_pCurPicInfo->bottom_field_flag;
    ;
    k = kmin;
    ktop = kbot = 0;
    while ((ktop < kfmax || kbot < kfmax) && k < MAX_REFS) {
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
        k = SortListDescending(RefPicListX, sps, 0, m_pCurDPBEntry->picInfo.PicOrderCnt, sort_check_short_term_B_frame,
                               bSkipCorruptFrames);
        // short-term frames sorted by ascending PicOrderCnt above current
        k = SortListAscending(RefPicListX, sps, k, m_pCurDPBEntry->picInfo.PicOrderCnt, sort_check_short_term_B_frame,
                              bSkipCorruptFrames);
    } else {
        // short-term frames sorted by ascending PicOrderCnt above current
        k = SortListAscending(RefPicListX, sps, 0, m_pCurDPBEntry->picInfo.PicOrderCnt, sort_check_short_term_B_frame,
                              bSkipCorruptFrames);
        // short-term frames sorted by descending PicOrderCnt less than current
        k = SortListDescending(RefPicListX, sps, k, m_pCurDPBEntry->picInfo.PicOrderCnt, sort_check_short_term_B_frame,
                               bSkipCorruptFrames);
    }
    // long-term frames sorted by ascending LongTermPicNum
    k = SortListAscending(RefPicListX, sps, k, INF_MIN, sort_check_long_term_frame, bSkipCorruptFrames);

    return k;
}

int32_t VkEncDpbH264::SortListDescending(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps, int32_t kmin,
        int32_t n, PFNSORTCHECK sort_check, bool bSkipCorruptFrames)
{
    int32_t m, k, i, i1, v;

    for (k = kmin; k < MAX_REFS; k++) {
        m = INF_MIN;
        i1 = -1;
        // find largest entry less than or equal to n
        for (i = 0; i < MAX_DPB_SIZE; i++) {
            if (m_DPB[i].view_id != m_pCurDPBEntry->view_id) {
                continue;
            }

            if ((m_DPB[i].bFrameCorrupted == true) && (bSkipCorruptFrames == true)) {
                continue;
            }

            if (sort_check(&m_DPB[i], sps->pic_order_cnt_type, &v) && v >= m && v <= n) {
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
                                        int32_t n, PFNSORTCHECK sort_check, bool bSkipCorruptFrames)
{
    int32_t m, k, i, i1, v;

    for (k = kmin; k < MAX_REFS; k++) {
        m = INF_MAX;
        i1 = -1;
        // find smallest entry greater than n
        for (i = 0; i < MAX_DPB_SIZE; i++) {
            if (m_DPB[i].view_id != m_pCurDPBEntry->view_id) {
                continue;
            }

            if ((m_DPB[i].bFrameCorrupted == true) && (bSkipCorruptFrames == true)) {
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
void VkEncDpbH264::RefPicListReordering(RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                                        const StdVideoH264SequenceParameterSet *sps,
                                        const StdVideoEncodeH264SliceHeader *slh,
                                        const StdVideoEncodeH264ReferenceListsInfo *ref)
{
    int32_t num_ref_idx_lX_active_minus1;

    // scan through commands if there is refpic reorder cmds
    if (ref->flags.ref_pic_list_modification_flag_l0) {
        num_ref_idx_lX_active_minus1 =
            slh->flags.num_ref_idx_active_override_flag ? ref->num_ref_idx_l0_active_minus1 : m_max_num_list[0];
        RefPicListReorderingLX(RefPicList0, sps, num_ref_idx_lX_active_minus1, ref->pRefList0ModOperations, 0);
    }

    if (ref->flags.ref_pic_list_modification_flag_l1) {
        num_ref_idx_lX_active_minus1 =
            slh->flags.num_ref_idx_active_override_flag ? ref->num_ref_idx_l1_active_minus1 : m_max_num_list[1];
        RefPicListReorderingLX(RefPicList1, sps, num_ref_idx_lX_active_minus1, ref->pRefList1ModOperations, 1);
    }
}

void VkEncDpbH264::RefPicListReorderingLX(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps,
        int32_t num_ref_idx_lX_active_minus1,
        const StdVideoEncodeH264RefListModEntry *ref_pic_list_reordering_lX, int32_t listX)
{
    int32_t MaxFrameNum, MaxPicNum, CurrPicNum, picNumLXPred, refIdxLX, k, picNumLXNoWrap, picNumLX, LongTermPicNum;

    MaxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);  // (7-1)

    if (!m_pCurPicInfo->field_pic_flag) {
        MaxPicNum = MaxFrameNum;
        CurrPicNum = m_pCurPicInfo->frameNum;
    } else {
        MaxPicNum = 2 * MaxFrameNum;
        CurrPicNum = 2 * m_pCurPicInfo->frameNum + 1;
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
    for (idx = 0; idx < MAX_DPB_SIZE; idx++) {
        if (m_DPB[idx].view_id != m_pCurDPBEntry->view_id) {
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
    if (idx >= MAX_DPB_SIZE) VK_DPB_DBG_PRINT(("short-term picture picNumLX does not exist\n"));
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
    for (idx = 0; idx < MAX_DPB_SIZE; idx++) {
        if (m_DPB[idx].view_id != m_pCurDPBEntry->view_id) {
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
    if (idx >= MAX_DPB_SIZE) VK_DPB_DBG_PRINT(("long-term picture LongTermPicNum does not exist\n"));
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
    int32_t i = 0;
    for (i = 0; i < MAX_DPB_SIZE; i++) {
        if (m_DPB[i].view_id == viewid) {
            if ((m_DPB[i].top_field_marking == MARKING_SHORT || m_DPB[i].bottom_field_marking == MARKING_SHORT) &&
                    (m_DPB[i].bFrameCorrupted == false)) {
                numShortTerm++;
            }
            if ((m_DPB[i].top_field_marking == MARKING_LONG || m_DPB[i].bottom_field_marking == MARKING_LONG) &&
                    (m_DPB[i].bFrameCorrupted == false)) {
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
    int32_t iMin = -1;
    int32_t i = 0;
    for (i = 0; i < MAX_DPB_SIZE; i++) {
        if ((m_DPB[i].state & DPB_TOP) && m_DPB[i].top_field_marking == MARKING_SHORT && m_DPB[i].topFOC < pocMin &&
                m_DPB[i].view_id == view_id) {
            pocMin = m_DPB[i].topFOC;
            iMin = i;
        }
        if ((m_DPB[i].state & DPB_BOTTOM) && m_DPB[i].top_field_marking == MARKING_SHORT && m_DPB[i].bottomFOC < pocMin &&
                m_DPB[i].view_id == view_id) {
            pocMin = m_DPB[i].bottomFOC;
            iMin = i;
        }
    }

    if (iMin >= 0) {
        if (field_pic_flag && bottom_field) {
            return m_DPB[iMin].bottomPicNum;
        } else {
            return m_DPB[iMin].topPicNum;
        }
    }
    return -1;
}

int32_t VkEncDpbH264::GetPicNumXWithMinFrameNumWrap(uint32_t view_id, int32_t field_pic_flag, int32_t bottom_field)
{
    int32_t minFrameNumWrap = 65536;
    int32_t imin = -1;
    int32_t i = 0;

    for (i = 0; i < MAX_DPB_SIZE; i++) {
        if (m_DPB[i].view_id == view_id) {
            if ((m_DPB[i].top_field_marking == MARKING_SHORT || m_DPB[i].bottom_field_marking == MARKING_SHORT)) {
                if (m_DPB[i].frameNumWrap < minFrameNumWrap) {
                    imin = i;
                    minFrameNumWrap = m_DPB[i].frameNumWrap;
                }
            }
        }
    }

    if (imin >= 0) {
        if (field_pic_flag && bottom_field) {
            return m_DPB[imin].bottomPicNum;
        } else {
            return m_DPB[imin].topPicNum;
        }
    }
    return -1;
}

int32_t VkEncDpbH264::GetPicNum(int32_t dpb_idx, bool bottomField)
{
    if ((dpb_idx >= 0) && (dpb_idx < MAX_DPB_SIZE) && (m_DPB[dpb_idx].state != DPB_EMPTY)) {
        return bottomField ? m_DPB[dpb_idx].bottomPicNum : m_DPB[dpb_idx].topPicNum;
    }

    VK_DPB_DBG_PRINT(("%s: Invalid index or state for decoded picture buffer \n", __FUNCTION__));
    return -1;
}

int32_t VkEncDpbH264::GetColocReadIdx(int32_t dpb_idx)
{
    if (dpb_idx == UCHAR_MAX) return -1;

    return m_DPB[dpb_idx].fb_index;
}

// Currently we support it only for IPPP gop pattern
bool VkEncDpbH264::InvalidateReferenceFrames(uint64_t timeStamp)
{
    int32_t i = 0;
    bool isValidReqest = true;

    if (fpInvalidFrameLog) {
#ifdef PRIu64
        fprintf(fpInvalidFrameLog, ("InvalidateReferenceFrames: invalid timestamp = %" PRIu64 ", IDR timestamp = %" PRIu64 "\n"),
                timeStamp, m_lastIDRTimeStamp);
#endif
    }

    for (i = 0; i < MAX_DPB_SIZE; i++) {
        if ((m_DPB[i].state != DPB_EMPTY) && (timeStamp == m_DPB[i].timeStamp)) {
            if (m_DPB[i].bFrameCorrupted == true) {
                isValidReqest = false;
            }
            break;
        }
    }

    if (timeStamp >= m_lastIDRTimeStamp && isValidReqest) {
        for (i = 0; i < MAX_DPB_SIZE; i++) {
            if ((m_DPB[i].state != DPB_EMPTY) && ((timeStamp <= m_DPB[i].refFrameTimeStamp) || (timeStamp == m_DPB[i].timeStamp))) {
                if (m_DPB[i].top_field_marking == MARKING_SHORT || m_DPB[i].bottom_field_marking == MARKING_SHORT) {
                    m_DPB[i].bFrameCorrupted = true;
                }

                if (m_DPB[i].top_field_marking == MARKING_LONG || m_DPB[i].bottom_field_marking == MARKING_LONG) {
                    m_DPB[i].bFrameCorrupted = true;
                }
            }
        }
    }

#if 0
    if (fpInvalidFrameLog) {
        for (i=0; i<MAX_DPB_SIZE; i++) {
            fprintf(fpInvalidFrameLog, "InvalidateReferenceFrames : timestamp = %" PRIu64 ", frame_num = %d , pictureIdx = %d , dpb_state = %d, corrupted = %d\n",m_DPB[i].timeStamp, m_DPB[i].frame_num, m_DPB[i].pictureIdx, m_DPB[i].state, m_DPB[i].bFrameCorrupted);
        }
        fprintf(fpInvalidFrameLog, "\n\n");
    }
#endif

    return true;
}

bool VkEncDpbH264::IsRefFramesCorrupted()
{
    int32_t i = 0;
    for (i = 0; i < MAX_DPB_SIZE; i++) {
        if ((m_DPB[i].top_field_marking == MARKING_SHORT || m_DPB[i].bottom_field_marking == MARKING_SHORT) &&
                (m_DPB[i].bFrameCorrupted == true)) {
            return true;
        }

        if ((m_DPB[i].top_field_marking == MARKING_LONG || m_DPB[i].bottom_field_marking == MARKING_LONG) &&
                (m_DPB[i].bFrameCorrupted == true)) {
            return true;
        }
    }
    return false;
}

bool VkEncDpbH264::IsRefPicCorrupted(int32_t dpb_idx)
{
    if ((dpb_idx >= 0) && (dpb_idx < MAX_DPB_SIZE) && (m_DPB[dpb_idx].state != DPB_EMPTY)) {
        return (m_DPB[dpb_idx].bFrameCorrupted == true);
    }
    return false;
}

int32_t VkEncDpbH264::GetPicNumFromDpbIdx(int32_t dpbIdx, bool *shortterm, bool *longterm)
{
    if ((dpbIdx >= 0) && (dpbIdx <= MAX_DPB_SIZE) && (m_DPB[dpbIdx].state != DPB_EMPTY)) {
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
            return m_DPB[dpbIdx].LongTermFrameIdx;
        }
    }

    *shortterm = false;
    *longterm = false;
    VK_DPB_DBG_PRINT(("%s : Invalid index or state for decoded picture buffer\n", __FUNCTION__));
    return -1;
}

uint64_t VkEncDpbH264::GetPictureTimestamp(int32_t dpb_idx)
{
    if ((dpb_idx >= 0) && (dpb_idx < MAX_DPB_SIZE) && (m_DPB[dpb_idx].state != DPB_EMPTY)) {
        return (m_DPB[dpb_idx].timeStamp);
    }
    return 0;
}

void VkEncDpbH264::SetCurRefFrameTimeStamp(uint64_t refFrameTimeStamp)
{
    m_pCurDPBEntry->refFrameTimeStamp = refFrameTimeStamp;
}

// Returns a "view" of the DPB in terms of the entries holding valid reference
// pictures.
int32_t VkEncDpbH264::GetValidEntries(VkEncDpbEntry entries[MAX_DPB_SIZE])
{
    int32_t numEntries = 0;

    for (int32_t i = 0; i < MAX_DPB_SIZE; i++) {
        if (m_DPB[i].top_field_marking != 0 || m_DPB[i].bottom_field_marking != 0) {
            entries[numEntries++] = m_DPB[i];
        }
    }

    return numEntries;
}

void VkEncDpbH264::FillStdReferenceInfo(uint8_t dpbIdx, StdVideoEncodeH264ReferenceInfo* pStdReferenceInfo)
{
    assert(dpbIdx < MAX_DPB_SIZE);
    const VkEncDpbEntry* pDpbEntry = &m_DPB[dpbIdx];

    bool isLongTerm = (pDpbEntry->top_field_marking == MARKING_LONG);

    pStdReferenceInfo->PicOrderCnt = pDpbEntry->picInfo.PicOrderCnt;
    pStdReferenceInfo->flags.used_for_long_term_reference = isLongTerm;
    pStdReferenceInfo->long_term_frame_idx = isLongTerm ? pDpbEntry->LongTermFrameIdx : (uint16_t)-1;
}
