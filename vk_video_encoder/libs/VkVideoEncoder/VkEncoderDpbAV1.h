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

#ifndef VK_ENCODER_DPB_AV1_H
#define VK_ENCODER_DPB_AV1_H

#include "VkVideoEncoderDef.h"
#include "VkCodecUtils/VulkanVideoImagePool.h"
#include "VkVideoEncoder/VkVideoGopStructure.h"

enum VkVideoEncoderAV1PrimaryRefType {
    REGULAR_FRAME   = 0,        // regular inter frame
    ARF_FRAME       = 1,        // alternate reference frame
    OVERLAY_FRAME   = 2,        // overlay frame
    GLD_FRAME       = 3,        // golden frame
    BRF_FRAME       = 4,        // backward reference frame
    INT_ARF_FRAME   = 5,        // internal alternate reference frame
    MAX_PRI_REF_TYPES
};

enum VkVideoEncoderAV1FrameUpdateType {
    KF_UPDATE       = 0,        // Key Frame
    LF_UPDATE       = 1,        // Last Frame
    GF_UPDATE       = 2,        // Golden Frame
    ARF_UPDATE      = 3,        // Alternate Reference Frame
    OVERLAY_UPDATE  = 4,        // Overlay Frame
    INTNL_OVERLAY_UPDATE = 5,   // Internal Overlay Frame
    INTNL_ARF_UPDATE    = 6,    // Internal Altref Frame
    BWD_UPDATE      = 7,        // backward Frame
    NO_UPDATE       = 8,        // No update to reference frame management
};

struct DpbEntryAV1 {
    enum { MAX_TILE_COLS = 16, MAX_TILE_ROWS = 16 };

    uint32_t refCount;
    uint32_t frameId;
    uint32_t picOrderCntVal; // display order relative to the last key frame
    StdVideoAV1FrameType frameType;
    StdVideoAV1ReferenceName refName;

    // The YCbCr dpb image resource
    VkSharedBaseObj<VulkanVideoImagePoolNode>  dpbImageView;
};

struct PicInfoAV1 : public StdVideoEncodeAV1PictureInfo {
    uint32_t show_existing_frame : 1;
    uint32_t frame_to_show_map_idx : 3;
    uint32_t overlay_frame : 1;
    uint32_t reference : 1;

    StdVideoAV1Quantization quantInfo;

    uint64_t timeStamp;

    bool FrameIsKey() {
        return frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY;
    }
    bool FrameIsIntraOnly() {
        return frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY;
    }
    bool FrameIsIntra() {
        return FrameIsKey() || FrameIsIntraOnly();
    }
    bool FrameIsInter() {
        return frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTER;
    }
    bool FrameIsSwitch() {
        return frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH;
    }
};


template < uint32_t MAX_PIC_REFS >
struct NvVideoEncodeAV1DpbSlotInfoLists {

    NvVideoEncodeAV1DpbSlotInfoLists()
        : refPicListCount{}
        , dpbSlotsUseMask(0)
        , refPicList{}
    { }

    uint32_t refPicListCount[2];
    uint32_t dpbSlotsUseMask;
    uint8_t refPicList[2][MAX_PIC_REFS];
};

class VkEncDpbAV1
{
public:

    enum { BUFFER_POOL_MAX_SIZE = 10 };
    enum { INVALID_IDX = -1 };

    VkEncDpbAV1(void);
    ~VkEncDpbAV1();

public:
    // 1. Init instance
    static VkEncDpbAV1 *CreateInstance(void);
    // 2. Init encode session
    int32_t DpbSequenceStart(const VkVideoEncodeAV1CapabilitiesKHR& capabilities, uint32_t userDpbSize, int32_t numBFrames,
                             VkVideoEncodeTuningModeKHR tuningMode, uint32_t qualityLevel);
    // 3. Start Picture - returns the allocated DPB index for this frame
    int8_t DpbPictureStart(StdVideoAV1FrameType frameType,
                           StdVideoAV1ReferenceName refName,
                           uint32_t picOrderCntVal,uint32_t frameId,
                           bool bShowExistingFrame, int32_t frameToShowMapId);
    // 3. End Picture
    int8_t DpbPictureEnd(int8_t dpbIndx,
                         VkSharedBaseObj<VulkanVideoImagePoolNode>&  dpbImageView,
                         const StdVideoAV1SequenceHeader *seqHdr,
                         bool bShowExistingFrame, bool bShownKeyFrameOrSwitch,
                         bool bErrorResilientMode, bool bOverlayFrame,
                         StdVideoAV1ReferenceName refName,
                         VkVideoEncoderAV1FrameUpdateType frameUpdateType);
    void DpbDestroy();
    int32_t GetMaxDPBSize()
    {
        return m_maxDpbSize;
    }
    uint64_t GetPictureTimestamp(int32_t picIdx);
    void SetCurRefFrameTimeStamp(uint64_t timeStamp);

    void FillStdReferenceInfo(uint8_t dpbIdx, StdVideoEncodeAV1ReferenceInfo* pStdReferenceInfo);

    StdVideoAV1ReferenceName AssignReferenceFrameType(VkVideoGopStructure::FrameType pictureType,
                                                      uint32_t refNameFlags, bool bRefPicFlag);
    VkVideoEncoderAV1FrameUpdateType GetFrameUpdateType(StdVideoAV1ReferenceName refName,
                                                        bool bOverlayFrame);
    void ConfigureRefBufUpdate(bool bShownKeyFrameOrSwitch, bool bShowExistingFrame,
                               VkVideoEncoderAV1FrameUpdateType frameUpdateType);
    int32_t GetRefreshFrameFlags(bool bShownKeyFrameOrSwitch, bool bShowExistingFrame);

    int32_t GetRefFrameDpbId(StdVideoAV1ReferenceName refName);
    int32_t GetRefBufId(StdVideoAV1ReferenceName refName);
    int8_t  GetRefBufDpbId(int32_t refBufId);
    int32_t GetOverlayRefBufId(int32_t picOrderCntVal);

    VkVideoEncoderAV1PrimaryRefType GetPrimaryRefType(StdVideoAV1ReferenceName refName,
                                                      bool bErrorResilientMode, bool bOverlayFrame);

    int32_t GetPrimaryRefBufId(VkVideoEncoderAV1PrimaryRefType primaryRefFrameType);
    int32_t GetPrimaryRefFrame(StdVideoAV1FrameType frameType, StdVideoAV1ReferenceName refName,
                               bool bErrorResilientMode, bool bOverlayFrame);

    void UpdateRefFrameDpbIdMap(int8_t dpbIndx);
    void UpdatePrimaryRefBufIdMap(StdVideoAV1ReferenceName refName,
                                  bool bShowExistingFrame, bool bErrorResilientMode,
                                  bool bOverlayFrame);
    void UpdateRefBufIdMap(bool bShownKeyFrameOrSwitch, bool bShowExistingFrame,
                           StdVideoAV1ReferenceName refName,
                           VkVideoEncoderAV1FrameUpdateType frameUpdateType);

    void SetupReferenceFrameGroups(VkVideoGopStructure::FrameType pictureType,
                                   StdVideoAV1FrameType frameType,
                                   uint32_t curPicOrderCntVal);
    int32_t GetDpbIdx(int32_t refNameMinus1) { return m_refName2DpbIdx[refNameMinus1]; }
    int32_t GetDpbIdx(int32_t groupId, int32_t i) {
        int32_t refNameMinus1 = (groupId == 0) ? m_refNamesInGroup1[i] : m_refNamesInGroup2[i];
        return GetDpbIdx(refNameMinus1);
    }

    bool GetDpbPictureResource(int32_t dpbIdx, VkSharedBaseObj<VulkanVideoImagePoolNode>& dpbImageView) {
        dpbImageView = m_DPB[dpbIdx].dpbImageView;
        return (dpbImageView != nullptr) ? true : false;
    }
    StdVideoAV1FrameType GetFrameType(int32_t dpbIdx) { assert(dpbIdx != INVALID_IDX); return m_DPB[dpbIdx].frameType; }
    StdVideoAV1ReferenceName GetRefName(int32_t dpbIdx) { assert(dpbIdx != INVALID_IDX); return m_DPB[dpbIdx].refName; }
    int32_t GetFrameId(int32_t dpbIdx) { assert(dpbIdx != INVALID_IDX); return m_DPB[dpbIdx].frameId; }
    int32_t GetPicOrderCntVal(int32_t dpbIdx) { assert(dpbIdx != INVALID_IDX); return m_DPB[dpbIdx].picOrderCntVal; }
    int32_t GetNumRefsInGroup(int32_t groupId) {
        assert(groupId < 2);
        return (groupId == 0) ? m_numRefFramesInGroup1 : m_numRefFramesInGroup2;
    }
    int32_t GetNumRefsInGroup1() { return m_numRefFramesInGroup1; }
    int32_t GetNumRefsInGroup2() { return m_numRefFramesInGroup2; }
    int32_t GetRefNameMinus1(int32_t groupId, int32_t i) {
        assert(groupId < 2);
        return (groupId == 0) ? m_refNamesInGroup1[i] : m_refNamesInGroup2[i];
    }
    int32_t GetNumRefsL0() { return m_numRefFramesL0; }
    int32_t GetNumRefsL1() { return m_numRefFramesL1; }

private:
    void DpbInit();
    void DpbDeinit();
    bool IsDpbFull();
    bool IsDpbEmpty();
    void OutputPicture(int32_t dpb_index, bool release);
    void FlushDpb();

    // void flush();

    void ReleaseFrame(int32_t dpbId)
    {
        assert(dpbId < m_maxDpbSize);
        assert(m_DPB[dpbId].refCount > 0);
        assert(m_DPB[dpbId].dpbImageView != nullptr);
        if (m_DPB[dpbId].refCount > 0) {
            m_DPB[dpbId].refCount--;
            // release the dpbImageView since it is not needed anymore
            if (m_DPB[dpbId].refCount == 0) {
                m_DPB[dpbId].dpbImageView = nullptr;
            }
        }
    }

    int32_t GetRefCount(int32_t dpbId) {
        assert(dpbId < m_maxDpbSize);
        return m_DPB[dpbId].refCount;
    }

private:
    DpbEntryAV1     m_DPB[BUFFER_POOL_MAX_SIZE + 1]; // 1 for the current
    uint8_t         m_maxDpbSize;

    uint32_t        m_maxSingleReferenceCount;
    uint32_t        m_singleReferenceNameMask;
    uint32_t        m_maxUnidirectionalCompoundReferenceCount;
    uint32_t        m_maxUnidirectionalCompoundGroup1ReferenceCount;
    uint32_t        m_unidirectionalCompoundReferenceNameMask;
    uint32_t        m_maxBidirectionalCompoundReferenceCount;
    uint32_t        m_maxBidirectionalCompoundGroup1ReferenceCount;
    uint32_t        m_maxBidirectionalCompoundGroup2ReferenceCount;
    uint32_t        m_bidirectionalCompoundReferenceNameMask;

    int32_t         m_maxRefFramesL0; // maximum reference frames allowed from the past
    int32_t         m_maxRefFramesL1; // maximum reference frames allowed from the future
    int32_t         m_numRefFramesL0; // final number of reference frames from the past
    int32_t         m_numRefFramesL1; // final number of reference frames from the future

    bool            m_mapRefDirToSingleRefType;
    int32_t         m_maxRefFramesGroup1; // maximum references in Group1
    int32_t         m_maxRefFramesGroup2; // maximum references in Group2
    int32_t         m_numRefFramesInGroup1; // group 1 count
    int32_t         m_numRefFramesInGroup2; // group 2 count
    int32_t         m_refNamesInGroup1[STD_VIDEO_AV1_REFS_PER_FRAME]; // Value is refFrame-1
    int32_t         m_refNamesInGroup2[STD_VIDEO_AV1_REFS_PER_FRAME];

    int32_t         m_refName2DpbIdx[STD_VIDEO_AV1_REFS_PER_FRAME]; // index is refName - 1
    int32_t         m_refBufIdMap[STD_VIDEO_AV1_NUM_REF_FRAMES]; // refType -> vbi Index
    int8_t          m_refFrameDpbIdMap[STD_VIDEO_AV1_NUM_REF_FRAMES]; // vbi Index -> dpb slot index
    int32_t         m_primaryRefBufIdMap[MAX_PRI_REF_TYPES];
    int32_t         m_primaryRefDpbIdx;
    uint32_t        m_refBufUpdateFlag;
    StdVideoAV1ReferenceName m_lastLastRefNameInUse;

    uint64_t        m_lastKeyFrameTimeStamp;

};

#endif  // VK_ENCODER_DPB_AV1_H
