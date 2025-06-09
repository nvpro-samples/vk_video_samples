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
#include "VkCodecUtils/VulkanVideoImagePool.h"

struct RefPicListEntry {
    int32_t dpbIndex;
};

struct DpbEntryH264 {
    // DPB attributes
    StdVideoEncodeH264PictureInfo picInfo;

    uint32_t state : 2;
    uint32_t top_needed_for_output : 1;
    uint32_t bottom_needed_for_output : 1;
    uint32_t top_decoded_first : 1;
    uint32_t reference_picture : 1;
    uint32_t complementary_field_pair : 1;
    uint32_t not_existing : 1;
    uint32_t frame_is_corrupted : 1;

    // reference_frame_attributes
    uint32_t top_field_marking  : 2;     // unused, short term, longterm
    uint32_t bottom_field_marking : 2;  // unused, short term, longterm
    int32_t  longTermFrameIdx;

    int32_t topFOC;
    int32_t bottomFOC;

    int32_t frameNumWrap;
    int32_t topPicNum;
    int32_t bottomPicNum;
    int32_t topLongTermPicNum;
    int32_t bottomLongTermPicNum;

    // The YCbCr dpb image resource
    VkSharedBaseObj<VulkanVideoImagePoolNode>  dpbImageView;

    // MVC
    uint32_t view_id;

    uint64_t timeStamp;
    uint64_t refFrameTimeStamp;
};

struct PicInfoH264 : public StdVideoEncodeH264PictureInfo {
    uint32_t field_pic_flag : 1;
    uint32_t bottom_field_flag : 1;
    uint64_t timeStamp;
};

typedef bool (*ptrFuncDpbSort)(const DpbEntryH264 *, StdVideoH264PocType, int32_t *);

template < uint32_t MAX_PIC_REFS >
struct NvVideoEncodeH264DpbSlotInfoLists {

    NvVideoEncodeH264DpbSlotInfoLists()
        : refPicListCount{}
        , dpbSlotsUseMask(0)
        , refPicList{}
    { }

    uint32_t refPicListCount[2];
    uint32_t dpbSlotsUseMask;
    uint8_t refPicList[2][MAX_PIC_REFS];
};

class VkEncDpbH264
{
public:

    enum { MAX_DPB_SLOTS = 16 };

    VkEncDpbH264(void);
    ~VkEncDpbH264();

public:
    // 1. Init instance
    static VkEncDpbH264 *CreateInstance(void);
    // 2. Init encode session
    int32_t DpbSequenceStart(int32_t userDpbSize = 0);
    // 3. Start Picture - returns the allocated DPB index for this frame
    int8_t DpbPictureStart(const PicInfoH264 *pPicInfo,
                           const StdVideoH264SequenceParameterSet *sps);
    // 3. End Picture
    int8_t DpbPictureEnd(const PicInfoH264 *pPicInfo,
                         VkSharedBaseObj<VulkanVideoImagePoolNode>&  dpbImageView,
                         const StdVideoH264SequenceParameterSet *sps,
                         const StdVideoEncodeH264SliceHeader *slh,
                         const StdVideoEncodeH264ReferenceListsInfo *ref,
                         uint32_t maxMemMgmntCtrlOpsCommands);
    int32_t GetPicturePOC(int32_t picIndexField);
    void DpbDestroy();
    void GetRefPicList(const PicInfoH264 *pPicInfo,
                       NvVideoEncodeH264DpbSlotInfoLists<STD_VIDEO_H264_MAX_NUM_LIST_REF>* pDpbSlotInfoLists,
                       const StdVideoH264SequenceParameterSet *sps, const StdVideoH264PictureParameterSet *pps,
                       const StdVideoEncodeH264SliceHeader *slh, const StdVideoEncodeH264ReferenceListsInfo *ref,
                       bool bSkipCorruptFrames = false);
    bool GetRefPicture(int8_t dpbIdx, VkSharedBaseObj<VulkanVideoImagePoolNode>&  dpbImageView);
    int32_t GetMaxDPBSize()
    {
        return m_max_dpb_size;
    }
    int32_t GetNumRefFramesInDPB(uint32_t viewid, int32_t *numShortTermRefs = NULL, int32_t *numLongTermRefs = NULL);
    int32_t GetPicNumXWithMinPOC(uint32_t view_id, int32_t field_pic_flag, int32_t bottom_field);
    int32_t GetPicNumXWithMinFrameNumWrap(uint32_t view_id, int32_t field_pic_flag, int32_t bottom_field);
    int32_t GetPicNum(int32_t picIndex, bool bottomField = false);
    bool InvalidateReferenceFrames(uint64_t timeStamp);
    bool IsRefFramesCorrupted();
    bool IsRefPicCorrupted(int32_t picIndex);
    int32_t GetPicNumFromDpbIdx(int32_t dpbIdx, bool *shortterm, bool *longterm);
    uint64_t GetPictureTimestamp(int32_t picIdx);
    void SetCurRefFrameTimeStamp(uint64_t timeStamp);

    const StdVideoEncodeH264PictureInfo *GetCurrentDpbEntry(void)
    {
        assert((m_currDpbIdx < m_max_dpb_size) || (m_currDpbIdx == MAX_DPB_SLOTS));
        return &m_DPB[(int)m_currDpbIdx].picInfo;
    }

    uint32_t GetUpdatedFrameNumAndPicOrderCnt(int32_t& PicOrderCnt)
    {
        const StdVideoEncodeH264PictureInfo * pPictureInfo = GetCurrentDpbEntry();

        PicOrderCnt = pPictureInfo->PicOrderCnt;
        return pPictureInfo->frame_num;
    }

    int32_t GetValidEntries(DpbEntryH264 entries[MAX_DPB_SLOTS]);
    uint32_t GetUsedFbSlotsMask();
    bool NeedToReorder();
    void FillStdReferenceInfo(uint8_t dpbIdx, StdVideoEncodeH264ReferenceInfo* pStdReferenceInfo);

private:
    void DpbInit();
    void DpbDeinit();
    void FillFrameNumGaps(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps);
    bool IsDpbFull();
    bool IsDpbEmpty();
    void DpbBumping(bool alwaysbump);
    void DecodedRefPicMarking(const PicInfoH264 *pPicInfo,
                              const StdVideoH264SequenceParameterSet *sps,
                              const StdVideoEncodeH264SliceHeader *slh,
                              const StdVideoEncodeH264ReferenceListsInfo *ref,
                              uint32_t maxMemMgmntCtrlOpsCommands);
    void SlidingWindowMemoryManagememt(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps);
    void AdaptiveMemoryManagement(const PicInfoH264 *pPicInfo, const StdVideoEncodeH264ReferenceListsInfo *ref,
                                  uint32_t maxMemMgmntCtrlOpsCommands);
    void CalculatePOC(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps);
    void CalculatePOCType0(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps);
    void CalculatePOCType2(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps);
    void CalculatePicNum(const PicInfoH264 *pPicInfo, const StdVideoH264SequenceParameterSet *sps);
    void OutputPicture(int32_t dpb_index, bool release);
    void FlushDpb();

    // void flush();

    void RefPicListInitialization(const PicInfoH264 *pPicInfo, RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                                  const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames);
    void RefPicListInitializationPFrame(RefPicListEntry *RefPicList0, const StdVideoH264SequenceParameterSet *sps,
                                        bool bSkipCorruptFrames);
    void RefPicListInitializationPField(RefPicListEntry *RefPicList0, const StdVideoH264SequenceParameterSet *sps,
                                        bool bottomField, bool bSkipCorruptFrames);
    void RefPicListInitializationBFrame(RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                                        const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames);
    void RefPicListInitializationBField(const PicInfoH264 *pPicInfo, RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                                        const StdVideoH264SequenceParameterSet *sps, bool bSkipCorruptFrames);
    int32_t RefPicListInitializationField(RefPicListEntry *refFrameListXShortTerm, RefPicListEntry *refFrameListLongTerm, int32_t ksmax,
                                          int32_t klmax, RefPicListEntry *RefPicListX, bool bottomField, bool bSkipCorruptFrames);
    int32_t RefPicListInitializationFieldListX(RefPicListEntry *refFrameListX, int32_t kfmax, int32_t kmin, RefPicListEntry *RefPicListX,
                                               bool bottomField, bool bSkipCorruptFrames);
    int32_t RefPicListInitializationBFrameListX(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps,
            bool list1, bool bSkipCorruptFrames);
    int32_t SortListDescending(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps, int32_t kmin, int32_t n,
                               ptrFuncDpbSort sort_check, bool bSkipCorruptFrames);
    int32_t SortListAscending(RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps, int32_t kmin, int32_t n,
                              ptrFuncDpbSort sort_check, bool bSkipCorruptFrames);
    void RefPicListReordering(const PicInfoH264 *pPicInfo, RefPicListEntry *RefPicList0, RefPicListEntry *RefPicList1,
                              const StdVideoH264SequenceParameterSet *sps, const StdVideoEncodeH264SliceHeader *slh,
                              const StdVideoEncodeH264ReferenceListsInfo *ref);
    void RefPicListReorderingLX(const PicInfoH264 *pPicInfo,
                                RefPicListEntry *RefPicListX, const StdVideoH264SequenceParameterSet *sps,
                                int32_t num_ref_idx_lX_active_minus1,
                                const StdVideoEncodeH264RefListModEntry *ref_pic_list_reordering_lX, int32_t listX);
    void RefPicListReorderingShortTerm(RefPicListEntry *RefPicListX, int32_t refIdxLX, int32_t num_ref_idx_lX_active_minus1, int32_t picNumLX);
    void RefPicListReorderingLongTerm(RefPicListEntry *RefPicListX, int32_t refIdxLX, int32_t num_ref_idx_lX_active_minus1,
                                      int32_t LongTermPicNum);
    int32_t DeriveL0RefCount(RefPicListEntry *RefPicList);
    int32_t DeriveL1RefCount(RefPicListEntry *RefPicList);
    void ReleaseFrame(VkSharedBaseObj<VulkanVideoImagePoolNode>&  dpbImageView);

private:
    int32_t  m_maxLongTermFrameIdx;
    int32_t  m_max_dpb_size;
    int32_t  m_prevPicOrderCntMsb;
    int32_t  m_prevPicOrderCntLsb;
    int32_t  m_prevFrameNumOffset;
    uint32_t m_prevFrameNum;
    uint32_t m_PrevRefFrameNum;
    int32_t  m_max_num_list[2];
    int8_t   m_currDpbIdx;
    DpbEntryH264 m_DPB[MAX_DPB_SLOTS + 1]; // 1 for the current

    uint64_t m_lastIDRTimeStamp;
};

#endif  // VK_ENCODER_DPB_264_H
