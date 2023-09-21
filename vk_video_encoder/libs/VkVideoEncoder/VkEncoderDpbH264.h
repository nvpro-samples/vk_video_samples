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

#ifndef VK_ENCODER_DPB_264_H
#define VK_ENCODER_DPB_264_H

#include "VkVideoEncoderDef.h"

enum dpb_state_e { DPB_EMPTY = 0, DPB_TOP, DPB_BOTTOM, DPB_FRAME };

struct RefPicListEntry {
    int32_t dpbIndex;
};

typedef struct VkEncDpbEntry {
    // DPB attributes
    StdVideoEncodeH264PictureInfo picInfo;

    int32_t state;
    bool top_needed_for_output;
    bool bottom_needed_for_output;
    bool top_decoded_first;
    bool reference_picture;
    bool complementary_field_pair;
    bool not_existing;

    // reference_frame_attributes
    int32_t top_field_marking;     // unused, short term, longterm
    int32_t bottom_field_marking;  // unused, short term, longterm
    int32_t LongTermFrameIdx;

    int32_t topFOC;
    int32_t bottomFOC;

    int32_t frameNumWrap;
    int32_t topPicNum;
    int32_t bottomPicNum;
    int32_t topLongTermPicNum;
    int32_t bottomLongTermPicNum;

    int32_t fb_index;  // index for yuv buffer, col and tempmv buffers..

    // MVC
    uint32_t view_id;

    uint64_t timeStamp;
    bool bFrameCorrupted;
    uint64_t refFrameTimeStamp;

} VkEncDpbEntry;

struct DpbPicInfo {
    uint32_t frameNum;
    int32_t PicOrderCnt;
    StdVideoH26XPictureType pictureType;

    bool isLongTerm;
    bool field_pic_flag;
    bool bottom_field_flag;
    bool isRef;
    bool isIDR;
    bool no_output_of_prior_pics_flag;
    bool adaptive_ref_pic_marking_mode_flag;

    uint64_t timeStamp;
    uint32_t pictureIdx;
    uint64_t refFrameTimeStamp;
};

typedef bool (*PFNSORTCHECK)(const VkEncDpbEntry *, StdVideoH264PocType, int32_t *);

template < uint32_t MAX_PIC_REFS >
struct NvVideoEncodeH264DpbSlotInfoLists {

    NvVideoEncodeH264DpbSlotInfoLists() : refPicList0Count(0), refPicList1Count(0), dpbSlotsUseMask(0)
    {
        memset(refPicList0, 0, sizeof(refPicList0));
        memset(refPicList1, 0, sizeof(refPicList1));
    }

    uint32_t refPicList0Count;
    uint32_t refPicList1Count;
    uint32_t dpbSlotsUseMask;
    uint8_t refPicList0[MAX_REFS];
    uint8_t refPicList1[MAX_REFS];
};

class VkEncDpbH264
{
public:
    VkEncDpbH264(void);
    ~VkEncDpbH264();

public:
    static VkEncDpbH264 *CreateInstance(void);
    int32_t DpbSequenceStart(int32_t userDpbSize = 0);
    int32_t DpbPictureStart(const DpbPicInfo *pDPBPicInfo, const StdVideoH264SequenceParameterSet *sps);
    int32_t DpbPictureEnd(const StdVideoH264SequenceParameterSet *sps, const StdVideoEncodeH264SliceHeader *slh,
                          const StdVideoEncodeH264ReferenceListsInfo *ref);
    int32_t GetPicturePOC(int32_t picIndexField);
    void DpbDestroy();
    void GetRefPicList(NvVideoEncodeH264DpbSlotInfoLists<2 * MAX_REFS>* pDpbSlotInfoLists,
                       const StdVideoH264SequenceParameterSet *sps, const StdVideoH264PictureParameterSet *pps,
                       const StdVideoEncodeH264SliceHeader *slh, const StdVideoEncodeH264ReferenceListsInfo *ref,
                       bool bSkipCorruptFrames = false);
    int32_t GetRefPicIdx(int32_t dpbIdx);
    int32_t GetMaxDPBSize()
    {
        return m_max_dpb_size;
    }
    int32_t GetNumRefFramesInDPB(uint32_t viewid, int32_t *numShortTermRefs = NULL, int32_t *numLongTermRefs = NULL);
    int32_t GetPicNumXWithMinPOC(uint32_t view_id, int32_t field_pic_flag, int32_t bottom_field);
    int32_t GetPicNumXWithMinFrameNumWrap(uint32_t view_id, int32_t field_pic_flag, int32_t bottom_field);
    int32_t GetPicNum(int32_t picIndex, bool bottomField = false);
    int32_t GetColocReadIdx(int32_t picType);
    bool InvalidateReferenceFrames(uint64_t timeStamp);
    bool IsRefFramesCorrupted();
    bool IsRefPicCorrupted(int32_t picIndex);
    int32_t GetPicNumFromDpbIdx(int32_t dpbIdx, bool *shortterm, bool *longterm);
    uint64_t GetPictureTimestamp(int32_t picIdx);
    void SetCurRefFrameTimeStamp(uint64_t timeStamp);

    const StdVideoEncodeH264PictureInfo *GetCurrentDpbEntry(void)
    {
        assert(m_pCurDPBEntry);
        return &m_pCurDPBEntry->picInfo;
    }

    int32_t GetValidEntries(VkEncDpbEntry entries[MAX_DPB_SIZE]);
    void FillStdReferenceInfo(uint8_t dpbIdx, StdVideoEncodeH264ReferenceInfo* pStdReferenceInfo);

private:
    void DpbInit();
    void DpbDeinit();
    void FillFrameNumGaps(const StdVideoH264SequenceParameterSet *sps);
    bool IsDpbFull();
    bool IsDpbEmpty();
    void DpbBumping(bool alwaysbump);
    void DecodedRefPicMarking(const StdVideoH264SequenceParameterSet *sps, const StdVideoEncodeH264SliceHeader *slh,
                              const StdVideoEncodeH264ReferenceListsInfo *ref);
    void SlidingWindowMemoryManagememt(const StdVideoH264SequenceParameterSet *sps);
    void AdaptiveMemoryManagement(const StdVideoEncodeH264ReferenceListsInfo *ref);
    void CalculatePOC(const StdVideoH264SequenceParameterSet *sps);
    void CalculatePOCType0(const StdVideoH264SequenceParameterSet *sps);
    void CalculatePOCType2(const StdVideoH264SequenceParameterSet *sps);
    void CalculatePicNum(const StdVideoH264SequenceParameterSet *sps);
    void OutputPicture(int32_t dpb_index, bool release);
    void FlushDpb();

    // void flush();

    void RefPicListInitialization(RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                                  const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames);
    void RefPicListInitializationPFrame(RefPicListEntry *RefPicList0, const StdVideoH264SequenceParameterSet *sps,
                                        bool bSkipCorruptFrames);
    void RefPicListInitializationPField(RefPicListEntry *RefPicList0, const StdVideoH264SequenceParameterSet *sps,
                                        bool bSkipCorruptFrames);
    void RefPicListInitializationBFrame(RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                                        const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames);
    void RefPicListInitializationBField(RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                                        const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames);
    int32_t RefPicListInitializationField(RefPicListEntry *refFrameListXShortTerm, RefPicListEntry *refFrameListLongTerm, int32_t ksmax,
                                          int32_t klmax, RefPicListEntry *RefPicListX, bool bSkipCorruptFrames);
    int32_t RefPicListInitializationFieldListX(RefPicListEntry *refFrameListX, int32_t kfmax, int32_t kmin, RefPicListEntry *RefPicListX,
            bool bSkipCorruptFrames);
    int32_t RefPicListInitializationBFrameListX(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps,
            bool list1, bool bSkipCorruptFrames);
    int32_t SortListDescending(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps, int32_t kmin, int32_t n,
                               PFNSORTCHECK sort_check, bool bSkipCorruptFrames);
    int32_t SortListAscending(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps, int32_t kmin, int32_t n,
                              PFNSORTCHECK sort_check, bool bSkipCorruptFrames);
    void RefPicListReordering(RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                              const StdVideoH264SequenceParameterSet *sps, const StdVideoEncodeH264SliceHeader *slh,
                              const StdVideoEncodeH264ReferenceListsInfo *ref);
    void RefPicListReorderingLX(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps,
                                int32_t num_ref_idx_lX_active_minus1,
                                const StdVideoEncodeH264RefListModEntry *ref_pic_list_reordering_lX, int32_t listX);
    void RefPicListReorderingShortTerm(RefPicListEntry *RefPicListX, int32_t refIdxLX, int32_t num_ref_idx_lX_active_minus1, int32_t picNumLX);
    void RefPicListReorderingLongTerm(RefPicListEntry *RefPicListX, int32_t refIdxLX, int32_t num_ref_idx_lX_active_minus1,
                                      int32_t LongTermPicNum);
    int32_t DeriveL0RefCount(RefPicListEntry *RefPicList);
    int32_t DeriveL1RefCount(RefPicListEntry *RefPicList);
    int32_t AllocateFrame();
    void ReleaseFrame(int32_t fb_idx);

private:
    int32_t m_MaxLongTermFrameIdx;
    int32_t m_max_dpb_size;
    int32_t m_prevPicOrderCntMsb;
    int32_t m_prevPicOrderCntLsb;
    int32_t m_prevFrameNumOffset;
    uint32_t m_prevFrameNum;
    uint32_t m_PrevRefFrameNum;
    int32_t m_max_num_list[2];
    VkEncDpbEntry m_DPB[MAX_DPB_SIZE + 1];
    DpbPicInfo *m_pCurPicInfo;
    VkEncDpbEntry *m_pCurDPBEntry;
    int32_t m_nCurDPBIdx;
    int32_t m_FrameBuffer[MAX_DPB_SIZE + 1];

    // This is a map from m_DPB indices to m_FrameBuffer indices and is
    // actually not read from anywhere in the code, but is useful for
    // understanding which framebuffer indices are in use.
    int32_t m_DPBIdx2FBIndexMapping[MAX_DPB_SIZE + 1];

    uint64_t m_lastIDRTimeStamp;
};

#endif  // VK_ENCODER_DPB_264_H
