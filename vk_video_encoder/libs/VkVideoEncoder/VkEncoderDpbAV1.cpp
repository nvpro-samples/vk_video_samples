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

#include "VkEncoderDpbAV1.h"
#include "VkEncoderConfigAV1.h"

#define VK_DPB_DBG_PRINT(expr) printf expr

#define SWAP(in, out, tmp) {\
    temp = in;\
    in = out;\
    out = temp;\
}

// Number of inter (non-intra) reference types.
#define REFRESH_LAST_FRAME_FLAG     (1 << STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME)
#define REFRESH_LAST2_FRAME_FLAG    (1 << STD_VIDEO_AV1_REFERENCE_NAME_LAST2_FRAME)
#define REFRESH_LAST3_FRAME_FLAG    (1 << STD_VIDEO_AV1_REFERENCE_NAME_LAST3_FRAME)
#define REFRESH_GOLDEN_FRAME_FLAG   (1 << STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME)
#define REFRESH_BWD_FRAME_FLAG      (1 << STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME)
#define REFRESH_ALT2_FRAME_FLAG     (1 << STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME)
#define REFRESH_ALT_FRAME_FLAG      (1 << STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME)

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

static StdVideoAV1ReferenceName refNameFullList[] =
{
    STD_VIDEO_AV1_REFERENCE_NAME_INTRA_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_LAST2_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_LAST3_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME,
    STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME
};

VkEncDpbAV1::VkEncDpbAV1()
    : m_maxDpbSize(0)
    , m_mapRefDirToSingleRefType(true)
{

}

VkEncDpbAV1::~VkEncDpbAV1()
{

}

VkEncDpbAV1* VkEncDpbAV1::CreateInstance()
{
    VkEncDpbAV1* pDpb = new VkEncDpbAV1();
    if (pDpb) {
        pDpb->DpbInit();
    }

    return pDpb;
}

void VkEncDpbAV1::DpbInit()
{
    DpbDeinit();
}

void VkEncDpbAV1::DpbDeinit()
{
    for (uint32_t i = 0; i < ARRAYSIZE(m_DPB); i++) {
        m_DPB[i] = DpbEntryAV1();
    }
    m_maxDpbSize = 0;
    m_maxRefFramesL0 = 0; // TODO: Update the values based on qualityLevel and tuningMode
    m_maxRefFramesL1 = 0;
    m_numRefFramesInGroup1 = 0;
    m_numRefFramesInGroup2 = 0;
    memset(m_refBufIdMap, 0xff, sizeof(m_refBufIdMap));
    memset(m_refFrameDpbIdMap, 0xff, sizeof(m_refFrameDpbIdMap));
    memset(m_primaryRefBufIdMap, 0xff, sizeof(m_primaryRefBufIdMap));
    m_primaryRefDpbIdx = -1;
    m_refBufUpdateFlag = 0;
    m_lastLastRefNameInUse = STD_VIDEO_AV1_REFERENCE_NAME_INVALID;

    m_lastKeyFrameTimeStamp = 0;
}

void VkEncDpbAV1::DpbDestroy()
{
    FlushDpb();
    DpbDeinit();

    delete this;
}

int32_t VkEncDpbAV1::DpbSequenceStart(const VkVideoEncodeAV1CapabilitiesKHR& capabilities, uint32_t userDpbSize, int32_t numBFrames,
                                      VkVideoEncodeTuningModeKHR tuningMode, uint32_t qualityLevel)
{
    DpbDeinit();

    assert(userDpbSize <= BUFFER_POOL_MAX_SIZE);
    assert(userDpbSize >= STD_VIDEO_AV1_NUM_REF_FRAMES); // less than 8 slots are not supported now

    m_maxSingleReferenceCount = capabilities.maxSingleReferenceCount;
    m_singleReferenceNameMask = capabilities.singleReferenceNameMask;
    m_maxUnidirectionalCompoundReferenceCount = capabilities.maxUnidirectionalCompoundReferenceCount;
    m_maxUnidirectionalCompoundGroup1ReferenceCount = capabilities.maxUnidirectionalCompoundReferenceCount;
    m_unidirectionalCompoundReferenceNameMask = capabilities.unidirectionalCompoundReferenceNameMask;
    m_maxBidirectionalCompoundReferenceCount = capabilities.maxBidirectionalCompoundReferenceCount;
    m_maxBidirectionalCompoundGroup1ReferenceCount = capabilities.maxBidirectionalCompoundGroup1ReferenceCount;
    m_maxBidirectionalCompoundGroup2ReferenceCount = capabilities.maxBidirectionalCompoundGroup2ReferenceCount;
    m_bidirectionalCompoundReferenceNameMask = capabilities.bidirectionalCompoundReferenceNameMask;

    m_maxDpbSize = (uint8_t)userDpbSize;
    // Restricts the number of frames in list0 and list1
    m_maxRefFramesL0 = 4;
    if (numBFrames > 0) {
        m_maxRefFramesL1 = 3; // 1
    } else {
        m_maxRefFramesL1 = 3; // 0
    }
    // Restricts the number of references in Group1 and Group2
    m_maxRefFramesGroup1 = 4;
    if (numBFrames > 0) {
        m_maxRefFramesGroup2 = 3;
    } else {
        m_maxRefFramesGroup2 = 3;
    }

    for (int32_t i = 0; i < STD_VIDEO_AV1_NUM_REF_FRAMES; i++) {
        m_refBufIdMap[i] = i;
    }

    m_lastLastRefNameInUse = (numBFrames == 0) ? STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME :
                                                 STD_VIDEO_AV1_REFERENCE_NAME_LAST3_FRAME;

    return 0;
}

int8_t VkEncDpbAV1::DpbPictureStart(StdVideoAV1FrameType frameType,
                                    StdVideoAV1ReferenceName refName,
                                    uint32_t picOrderCntVal, uint32_t frameId,
                                    bool bShowExistingFrame, int32_t frameToShowBufId)
{
    int8_t dpbIndx = INVALID_IDX;
    if (!bShowExistingFrame) {
        for (dpbIndx = 0; dpbIndx < m_maxDpbSize; dpbIndx++) {
            if (m_DPB[dpbIndx].refCount == 0) {
                assert(m_DPB[dpbIndx].dpbImageView == nullptr);
                break;
            }
        }
        if ((dpbIndx == INVALID_IDX) || (dpbIndx >= m_maxDpbSize)) {
            assert(!"DPB mangement error");
            return INVALID_IDX;
        }

        m_DPB[dpbIndx].frameId = frameId;
        m_DPB[dpbIndx].picOrderCntVal = picOrderCntVal;
        m_DPB[dpbIndx].frameType = frameType;
        m_DPB[dpbIndx].refName = refName;
        m_DPB[dpbIndx].refCount = 1;
    } else {
        dpbIndx = GetRefBufDpbId(frameToShowBufId);
        if (dpbIndx == INVALID_IDX) {
            return INVALID_IDX;
        }
        m_DPB[dpbIndx].refCount++;
    }

    return dpbIndx;
}

int8_t VkEncDpbAV1::DpbPictureEnd(int8_t dpbIndx, VkSharedBaseObj<VulkanVideoImagePoolNode>&  dpbImageView,
                         const StdVideoAV1SequenceHeader *seqHdr,
                         bool bShowExistingFrame, bool bShownKeyFrameOrSwitch,
                         bool bErrorResilientMode, bool bOverlayFrame,
                         StdVideoAV1ReferenceName refName,
                         VkVideoEncoderAV1FrameUpdateType frameUpdateType)
{
    if (!bShowExistingFrame) {
        m_DPB[dpbIndx].dpbImageView = dpbImageView;
    }

    UpdateRefFrameDpbIdMap(dpbIndx);
    UpdatePrimaryRefBufIdMap(refName, bShowExistingFrame,
                             bErrorResilientMode, bOverlayFrame);
    UpdateRefBufIdMap(bShownKeyFrameOrSwitch, bShowExistingFrame,
                      refName, frameUpdateType);

    // Release current image.  Only count references.
    ReleaseFrame(dpbIndx);
    return 0;
}

void VkEncDpbAV1::OutputPicture(int32_t dpb_index, bool release)
{

}

void VkEncDpbAV1::FlushDpb()
{

}

int32_t VkEncDpbAV1::GetRefFrameDpbId(StdVideoAV1ReferenceName refName)
{
    int32_t refFrameDpbId = INVALID_IDX;

    if ((refName >= STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME) &&
        (refName <= STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME)) {

            int32_t refBufId = m_refBufIdMap[refName];

            if ((refBufId >= 0) && (refBufId < STD_VIDEO_AV1_NUM_REF_FRAMES)) {
                refFrameDpbId = m_refFrameDpbIdMap[refBufId];
            }
    }

    return refFrameDpbId;
}

int32_t VkEncDpbAV1::GetRefBufId(StdVideoAV1ReferenceName refName)
{
    int32_t refBufId = INVALID_IDX;

    if ((refName >= STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME) &&
        (refName <= STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME)) {
            refBufId = m_refBufIdMap[refName];
    }

    return refBufId;
}

int8_t VkEncDpbAV1::GetRefBufDpbId(int32_t refBufId)
{
    int8_t refBufDpbId = INVALID_IDX;

    if ((refBufId >= 0) && (refBufId < STD_VIDEO_AV1_NUM_REF_FRAMES)) {
            refBufDpbId = m_refFrameDpbIdMap[refBufId];
    }

    return refBufDpbId;
}

int32_t VkEncDpbAV1::GetOverlayRefBufId(int32_t picOrderCntVal)
{
    int32_t overlayRefBufId = INVALID_IDX;

    for (auto ref : refNameFullList) {

        int32_t refBufId = m_refBufIdMap[ref];
        if (refBufId >= STD_VIDEO_AV1_NUM_REF_FRAMES) continue;

        int32_t refBufDpbId = m_refFrameDpbIdMap[refBufId];
        if (refBufDpbId >= m_maxDpbSize) {
            continue;
        }

        if ((GetRefCount(refBufDpbId) > 0) && (m_DPB[refBufDpbId].picOrderCntVal == (uint32_t)picOrderCntVal)) {
            // found valid match
            overlayRefBufId = refBufId;
            break;
        }
    }

    return overlayRefBufId;
}

StdVideoAV1ReferenceName VkEncDpbAV1::AssignReferenceFrameType(VkVideoGopStructure::FrameType pictureType, uint32_t refNameFlags,
                                                               bool bRefPicFlag)
{
    StdVideoAV1ReferenceName refName;
    if ((pictureType == VkVideoGopStructure::FRAME_TYPE_IDR) ||
        ((refNameFlags >> STD_VIDEO_AV1_REFERENCE_NAME_INTRA_FRAME) & 1)) {
        refName = STD_VIDEO_AV1_REFERENCE_NAME_INTRA_FRAME;
    } else if ((refNameFlags >> STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME) & 1) {
        refName = STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME;
    } else if ((refNameFlags >> STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME) & 1) {
        refName = STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME;
    } else if ((refNameFlags >> STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME) & 1) {
        refName = STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME;
    } else if ((refNameFlags >> STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME) & 1) {
        refName = STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME;
    } else if (bRefPicFlag) {
        refName = STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME;
    } else {
        refName = STD_VIDEO_AV1_REFERENCE_NAME_INVALID;
    }

    return refName;
}

VkVideoEncoderAV1FrameUpdateType VkEncDpbAV1::GetFrameUpdateType(StdVideoAV1ReferenceName refName,
                                                                 bool bOverlayFrame)
{
    VkVideoEncoderAV1FrameUpdateType frameUpdateType = LF_UPDATE; // default update type

    if (refName == STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME) {
        frameUpdateType = (bOverlayFrame) ? INTNL_OVERLAY_UPDATE : INTNL_ARF_UPDATE;
    } else if (refName == STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME) {
        frameUpdateType = (bOverlayFrame) ? INTNL_OVERLAY_UPDATE : BWD_UPDATE;
    } else if (refName == STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME) {
        frameUpdateType = (bOverlayFrame) ? OVERLAY_UPDATE : ARF_UPDATE;
    } else if (refName == STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME) {
        frameUpdateType = GF_UPDATE;
    } else if (refName == STD_VIDEO_AV1_REFERENCE_NAME_INVALID) {
        frameUpdateType = NO_UPDATE;
    }

    return frameUpdateType;
}

void VkEncDpbAV1::ConfigureRefBufUpdate(bool bShownKeyFrameOrSwitch, bool bShowExistingFrame,
                                        VkVideoEncoderAV1FrameUpdateType frameUpdateType)
{
    if (bShownKeyFrameOrSwitch) {
        // refresh all buffers
        m_refBufUpdateFlag = 0xff;
        return;
    } else if (bShowExistingFrame || (frameUpdateType == NO_UPDATE)) {
        m_refBufUpdateFlag = 0;
        return;
    }

    int32_t refreshLastFrameFlag = 1 << m_lastLastRefNameInUse;

    m_refBufUpdateFlag = 0;

    switch (frameUpdateType) {
        case KF_UPDATE:
            m_refBufUpdateFlag = (refreshLastFrameFlag |
                                  REFRESH_GOLDEN_FRAME_FLAG |
                                  REFRESH_ALT2_FRAME_FLAG |
                                  REFRESH_ALT_FRAME_FLAG);
            break;

        case LF_UPDATE:
            m_refBufUpdateFlag = refreshLastFrameFlag;
            break;

        case GF_UPDATE:
            m_refBufUpdateFlag = refreshLastFrameFlag |
                                 REFRESH_GOLDEN_FRAME_FLAG;
            break;

        case OVERLAY_UPDATE:
            m_refBufUpdateFlag = refreshLastFrameFlag;
            break;

        case ARF_UPDATE:
            m_refBufUpdateFlag = REFRESH_ALT_FRAME_FLAG;
            break;

        case INTNL_OVERLAY_UPDATE:
            m_refBufUpdateFlag = refreshLastFrameFlag;
            break;

        case INTNL_ARF_UPDATE:
            m_refBufUpdateFlag = REFRESH_ALT2_FRAME_FLAG;
            break;

        case BWD_UPDATE:
            m_refBufUpdateFlag = REFRESH_BWD_FRAME_FLAG;
            break;

        default:
            break;
    }
}


VkVideoEncoderAV1PrimaryRefType VkEncDpbAV1::GetPrimaryRefType(StdVideoAV1ReferenceName refName,
                                                               bool bErrorResilientMode, bool bOverlayFrame)
{
    if ((refName == STD_VIDEO_AV1_REFERENCE_NAME_INTRA_FRAME) || bErrorResilientMode) {
        return (m_maxRefFramesL1 > 0) ? BRF_FRAME : REGULAR_FRAME;
    } else if (bOverlayFrame) {
        return OVERLAY_FRAME;
    } else if (refName == STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME) {
        return ARF_FRAME;
    } else if (refName == STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME) {
        return INT_ARF_FRAME;
    } else if (refName == STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME) {
        return GLD_FRAME;
    } else if (refName == STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME) {
        return BRF_FRAME;
    } else if (m_maxRefFramesL1 > 0) {
        return INT_ARF_FRAME;
    } else {
        return REGULAR_FRAME;
    }
}

int32_t VkEncDpbAV1::GetPrimaryRefBufId(VkVideoEncoderAV1PrimaryRefType primaryRefType)
{
    int32_t refBufId = VkEncDpbAV1::INVALID_IDX;

    assert((primaryRefType >= REGULAR_FRAME) && (primaryRefType < MAX_PRI_REF_TYPES));

    if ((primaryRefType >= REGULAR_FRAME) && (primaryRefType < MAX_PRI_REF_TYPES)) {
        refBufId = m_primaryRefBufIdMap[primaryRefType];
    }

    if (refBufId == VkEncDpbAV1::INVALID_IDX) {
        if (primaryRefType == INT_ARF_FRAME) {
            refBufId = m_primaryRefBufIdMap[ARF_FRAME];
        } else {
            refBufId = m_primaryRefBufIdMap[(m_maxRefFramesL1 > 0) ? BRF_FRAME : REGULAR_FRAME];
        }
    }

    return refBufId;
}

int32_t VkEncDpbAV1::GetPrimaryRefFrame(StdVideoAV1FrameType frameType, StdVideoAV1ReferenceName refName,
                                        bool bErrorResilientMode, bool bOverlayFrame)
{
    m_primaryRefDpbIdx = VkEncDpbAV1::INVALID_IDX;

    if ((frameType == STD_VIDEO_AV1_FRAME_TYPE_KEY) || (frameType == STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY) || bErrorResilientMode) {
       return STD_VIDEO_AV1_PRIMARY_REF_NONE;
    }

    // Find the most recent reference frame with the same reference type as the current frame
    VkVideoEncoderAV1PrimaryRefType primaryRefType = GetPrimaryRefType(refName, bErrorResilientMode, bOverlayFrame);
    int32_t primaryRefBufId = GetPrimaryRefBufId(primaryRefType);
    int32_t primaryRefDpbIdx = GetRefBufDpbId(primaryRefBufId);
    int32_t primaryRefFrame = STD_VIDEO_AV1_PRIMARY_REF_NONE;

    if ((primaryRefBufId == VkEncDpbAV1::INVALID_IDX) ||
        (primaryRefDpbIdx == VkEncDpbAV1::INVALID_IDX) ||
        (GetRefCount(primaryRefDpbIdx) == 0)) {
            return STD_VIDEO_AV1_PRIMARY_REF_NONE;
    }

    for (StdVideoAV1ReferenceName ref : refNameList) {
        if (GetRefBufId(ref) == primaryRefBufId) {
            primaryRefFrame = ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME;
            m_primaryRefDpbIdx = primaryRefDpbIdx;
            break;
        }
    }

    return primaryRefFrame;
}

int32_t VkEncDpbAV1::GetRefreshFrameFlags(bool bShownKeyFrameOrSwitch, bool bShowExistingFrame)
{
    int32_t refreshFrameFlags = 0;

    if (bShownKeyFrameOrSwitch) {
        // refresh all buffers
        refreshFrameFlags = 0xff;
    } else if (bShowExistingFrame) {
        refreshFrameFlags = 0;
    } else {
        for (int32_t i = 0; i < STD_VIDEO_AV1_NUM_REF_FRAMES; i++) {
            if (m_refBufUpdateFlag & (1 << i)) {
                int32_t refBufId = GetRefBufId((StdVideoAV1ReferenceName)i);
                if (refBufId == VkEncDpbAV1::INVALID_IDX) continue;
                refreshFrameFlags |= (1 << refBufId);
            }
        }
    }

    return refreshFrameFlags;
}

void VkEncDpbAV1::UpdateRefFrameDpbIdMap(int8_t dpbIndx)
{
    for (int i = 0; i < STD_VIDEO_AV1_NUM_REF_FRAMES; i++) {

        if (((m_refBufUpdateFlag >> i) & 1) == 1) {

            int32_t bufId = m_refBufIdMap[i];
            int32_t dpbId = m_refFrameDpbIdMap[bufId];

            if (dpbId != VkEncDpbAV1::INVALID_IDX) {
                ReleaseFrame(dpbId);
            }

            // assign new DPB entry
            m_refFrameDpbIdMap[bufId] = dpbIndx;
            // increase reference count (
            assert(m_DPB[dpbIndx].refCount <= STD_VIDEO_AV1_NUM_REF_FRAMES);
            m_DPB[dpbIndx].refCount++;
        }
    }

}

void VkEncDpbAV1::UpdatePrimaryRefBufIdMap(StdVideoAV1ReferenceName refName,
                                           bool bShowExistingFrame, bool bErrorResilientMode,
                                           bool bOverlayFrame)
{
    if (!bShowExistingFrame) {
        VkVideoEncoderAV1PrimaryRefType primaryRefType =
                    GetPrimaryRefType(refName, bErrorResilientMode, bOverlayFrame);

        for (StdVideoAV1ReferenceName ref : refNameList) {

            // If more than one frame is refreshed, it doesn't matter which one we pick.
            // So, pick the first if any.
            if (m_refBufUpdateFlag & (1 << ref)) {
                m_primaryRefBufIdMap[(int)primaryRefType] = GetRefBufId(ref);
                break;
            }
        }
    }
}

void VkEncDpbAV1::UpdateRefBufIdMap(bool bShownKeyFrameOrSwitch, bool bShowExistingFrame,
                                    StdVideoAV1ReferenceName refName,
                                    VkVideoEncoderAV1FrameUpdateType frameUpdateType)
{
    // For shown key frame and S-frames virtual buffer mapping does not change
    if (bShownKeyFrameOrSwitch || (frameUpdateType == NO_UPDATE) ) {
        return;
    }

    // Initialize the new referene map as a copy of the old one.
    int32_t refBufIdMap[STD_VIDEO_AV1_NUM_REF_FRAMES];
    memcpy(refBufIdMap, m_refBufIdMap, sizeof(refBufIdMap));

    // The reference management strategy is current as follows:
    // * ALTREF and GOLDEN frames are swaped as follows:
    //    ** When we code and ALTREF it refreshes ALTREF buffer.
    //    ** When we code a true OVERLAY, it refreshes GOLDEN buffer
    //       and the buffers are swapped.
    //       GOLDEN (possibly refreshed by the OVERLAY) becomes the
    //       new ALTREF and the old ARGRER (denoised version if arnr
    //       is used) becomes new GOLDEN.
    // * LAST, LAST, LAST3, if no bipredictive rate control group is
    //   used, GOLDEN work like a FIFO.  When a frame does a
    //   m_lastLastRefNameInUse update, all the virtual buffers are
    //   shifted by one slot: the old LAST frame becomes LAST2, old
    //   LAST2 becomes LAST3, etc...
    // * After encoding an INTNL_OVERLAY (ALTREF2 and BWD frame overlays),
    //   the decoded picture becomes the new last frame.  All the other
    //   frames in the last frame queue are shifted accordingly.
    if (frameUpdateType == OVERLAY_UPDATE) {
        // Swap m_lastLastRefNameInUse, GOLDEN and ALTREF virtual buffers
        refBufIdMap[m_lastLastRefNameInUse] = m_refBufIdMap[STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME];
        refBufIdMap[STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME] = m_refBufIdMap[STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME];
        refBufIdMap[STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME] = m_refBufIdMap[m_lastLastRefNameInUse];
    } else if ((frameUpdateType == INTNL_OVERLAY_UPDATE) && bShowExistingFrame) {
        // ALTREF2/BWD without any coded overlay - Move virtual buffers to m_lastLastRefNameInUse if valid to become LAST
        // frame reference next picture.
        // swap ALTREF2/BWD nad m_lastLastRefNameInUse virtual buffers
        refBufIdMap[m_lastLastRefNameInUse] = m_refBufIdMap[refName];
        refBufIdMap[refName] = m_refBufIdMap[m_lastLastRefNameInUse];
    }

    if ((frameUpdateType == LF_UPDATE) ||
        (frameUpdateType == GF_UPDATE) ||
        (frameUpdateType == INTNL_OVERLAY_UPDATE) ||
        (frameUpdateType == OVERLAY_UPDATE)) {
            // shift last frame types by one slot if needed
            if (m_lastLastRefNameInUse > STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME) {
                // use refBufIdMap instead of m_refBufIdMap since value might have been modified above
                refBufIdMap[STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] = refBufIdMap[m_lastLastRefNameInUse];
                for (int32_t lastFrameType = STD_VIDEO_AV1_REFERENCE_NAME_LAST2_FRAME; lastFrameType <= m_lastLastRefNameInUse; lastFrameType++) {
                    refBufIdMap[lastFrameType] = m_refBufIdMap[lastFrameType - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME];
                }
            }
    }

    // save new reference map
    memcpy(m_refBufIdMap, refBufIdMap, sizeof(m_refBufIdMap));

}

void VkEncDpbAV1::SetupReferenceFrameGroups(VkVideoGopStructure::FrameType pictureType, StdVideoAV1FrameType frame_type,
                                            uint32_t curPicOrderCntVal)
{
    m_numRefFramesL0 = 0;
    m_numRefFramesL1 = 0;
    m_numRefFramesInGroup1 = 0;
    m_numRefFramesInGroup2 = 0;
    memset(m_refName2DpbIdx, 0xff, sizeof(m_refName2DpbIdx));

    if ((frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY) ||
        (frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY)) {
        return;
    }

    // prepare a list of reference name to dpb idx mapping
    for (auto ref : refNameList) {
        m_refName2DpbIdx[ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] = GetRefFrameDpbId(ref);
    }

    // Divide the valid reference frames in to 2 groups.
    // One group contains pictures from past and other from future.
    int32_t numRefFramesL0 = 0, numRefFramesL1 = 0;
    int32_t refFrameDpbIdListL0[STD_VIDEO_AV1_NUM_REF_FRAMES];
    int32_t refFrameDpbIdListL1[STD_VIDEO_AV1_NUM_REF_FRAMES];
    int32_t refFramePocListL0[STD_VIDEO_AV1_NUM_REF_FRAMES];
    int32_t refFramePocListL1[STD_VIDEO_AV1_NUM_REF_FRAMES];

    for (int dpbId = 0; dpbId < m_maxDpbSize; dpbId++) {
        if (GetRefCount(dpbId) != 0) {
            if (m_DPB[dpbId].picOrderCntVal < curPicOrderCntVal) {
                refFrameDpbIdListL0[numRefFramesL0] = dpbId;
                refFramePocListL0[numRefFramesL0] = m_DPB[dpbId].picOrderCntVal;
                numRefFramesL0++;
            } else {
                refFrameDpbIdListL1[numRefFramesL1] = dpbId;
                refFramePocListL1[numRefFramesL1] = m_DPB[dpbId].picOrderCntVal;
                numRefFramesL1++;
            }
        }
    }

    // sort L0 list in decreasing order of POC value
    for (int32_t i = 0; i < numRefFramesL0; i++) {
        for (int j = 0; j < numRefFramesL0 - 1; j++) {
            if (refFramePocListL0[j] < refFramePocListL0[j + 1]) {
                int32_t temp;
                SWAP(refFramePocListL0[j], refFramePocListL0[j + 1], temp);
                SWAP(refFrameDpbIdListL0[j], refFrameDpbIdListL0[j + 1], temp);
            }
        }
    }

    // sort L1 list in increasing order of POC value
    for (int32_t i = 0; i < numRefFramesL1; i++) {
        for (int j = 0; j < numRefFramesL1 - 1; j++) {
            if (refFramePocListL1[j] > refFramePocListL1[j + 1]) {
                int32_t temp;
                SWAP(refFramePocListL1[j], refFramePocListL1[j + 1], temp);
                SWAP(refFrameDpbIdListL1[j], refFrameDpbIdListL1[j + 1], temp);
            }
        }
    }

    // Limit number of reference pictures from past and future to use for perf/quality
    if (pictureType == VkVideoGopStructure::FRAME_TYPE_P) {
        m_numRefFramesL0 = std::min(numRefFramesL0, m_maxRefFramesL0);
        m_numRefFramesL1 = 0;   // no future frames for P pictures
    } else { // B pictures
        m_numRefFramesL0 = std::min(numRefFramesL0, m_maxRefFramesL0);
        m_numRefFramesL1 = std::min(numRefFramesL1, m_maxRefFramesL1);
    }

    if (m_mapRefDirToSingleRefType) {
        // Pick a predition mode to use now:
        // - If this was intended to be a B picture, try to use bidir compound.
        //     - If bidir compound is not available, fallback to unidirectional compound
        // - If > 2 references, try to use unidirectional compound
        //     - If not available, fallback to single reference
        // - Otherwise, use single reference
        // For unidir compound, only consider group1 for now.
        uint32_t supportedReferenceMask = 0;
        if (pictureType == VkVideoGopStructure::FRAME_TYPE_B) {
            if (m_maxBidirectionalCompoundReferenceCount > 0) {
                supportedReferenceMask = m_bidirectionalCompoundReferenceNameMask;
            } else {
                // Limit only to group1 for now
                supportedReferenceMask = m_unidirectionalCompoundReferenceNameMask & ((1 << STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME) - 1);
            }
        } else {
            if (m_maxUnidirectionalCompoundReferenceCount > 0) {
                supportedReferenceMask = m_unidirectionalCompoundReferenceNameMask & ((1 << STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME) - 1);
            } else {
                supportedReferenceMask = m_singleReferenceNameMask;
            }
        }

        assert(supportedReferenceMask != 0);

        // Group 1
        int32_t numRef = 0;
        for (int32_t refId = 0; ((refId < numRefFramesL0) && (numRef < m_numRefFramesL0)); refId++) {
            int32_t ref = STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME;

            for (; ref <= STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME; ref++) { // TODO: remove restriction
                if ((supportedReferenceMask & (1 << (ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME))) == 0) {
                    continue;
                }

                if (m_refName2DpbIdx[ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] == refFrameDpbIdListL0[refId]) {
                    break; // match found
                }
            }

            if (ref > STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME) {
                continue;
            }
            m_refNamesInGroup1[numRef] = ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME;
            numRef++;
        }
        m_numRefFramesInGroup1 = numRef;

        // Group 2
        numRef = 0;
        for (int32_t refId = 0; ((refId < numRefFramesL1) && (numRef < m_numRefFramesL1)); refId++) {
            int32_t ref = STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME;

            for (; ref <= STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME; ref++) { // TODO: remove restriction
                if ((supportedReferenceMask & (1 << (ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME))) == 0) {
                    continue;
                }

                if (m_refName2DpbIdx[ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] == refFrameDpbIdListL1[refId]) {
                    break; // match found
                }
            }

            if (ref > STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME) {
                continue;
            }
            m_refNamesInGroup2[numRef] = ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME;
            numRef++;
        }
        m_numRefFramesInGroup2 = numRef;
    } else {
        // map list0 frames to Group1 or Group2
        int32_t numRef1 = 0, numRef2 = 0;
        for (int32_t refId = 0; refId < numRefFramesL0; refId++) {

            for (auto ref : refNameList) {
                if (m_refName2DpbIdx[ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] == refFrameDpbIdListL0[refId]) {

                    if ((ref < STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME) && (numRef1 < m_maxRefFramesGroup1)) {
                        // Add it to the group if not added already
                        bool present = false;
                        for (int32_t n = 0; n < numRef1; n++) {
                            if (m_refNamesInGroup1[n] == ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME) { // already present in group1
                                present = true;
                                break;
                            }
                        }
                        if (!present) {
                            m_refNamesInGroup1[numRef1] = ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME;
                            numRef1++;
                        }
                    } else if ((ref >= STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME) && (numRef2 < m_maxRefFramesGroup2)) {
                        // Add it to the group if not added already
                        bool present = false;
                        for (int32_t n = 0; n < numRef2; n++) {
                            if (m_refNamesInGroup2[n] == ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME) { // already present in group1
                                present = true;
                                break;
                            }
                        }
                        if (!present) {
                            m_refNamesInGroup2[numRef2] = ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME;
                            numRef2++;
                        }
                    }
                }
            }
        }

        // map list1 frames
        for (int32_t refId = 0; refId < numRefFramesL1; refId++) {

            for (auto ref : refNameList) {
                if (m_refName2DpbIdx[ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] == refFrameDpbIdListL1[refId]) {
                    if ((ref < STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME) && (numRef2 < m_maxRefFramesGroup1)) {
                        // Add it to the group if not added already
                        bool present = false;
                        for (int32_t n = 0; n < numRef1; n++) {
                            if (m_refNamesInGroup1[n] == ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME) { // already present in group2
                                present = true;
                                break;
                            }
                        }
                        if (!present) {
                            m_refNamesInGroup1[numRef1] = ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME;
                            numRef1++;
                        }
                    } else if ((ref >= STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME) && (numRef2 < m_maxRefFramesGroup2)) { // group2 reference
                        // Add it to the group if not added already
                        bool present = false;
                        for (int32_t n = 0; n < numRef2; n++) {
                            if (m_refNamesInGroup2[n] == ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME) { // already present in group2
                                present = true;
                                break;
                            }
                        }
                        if (!present) {
                            m_refNamesInGroup2[numRef2] = ref - STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME;
                            numRef2++;
                        }
                    }
                }
            }
        }

        m_numRefFramesInGroup1 = numRef1;
        m_numRefFramesInGroup2 = numRef2;
    }

}

void VkEncDpbAV1::FillStdReferenceInfo(uint8_t dpbIdx, StdVideoEncodeAV1ReferenceInfo* pStdReferenceInfo)
{
    assert(dpbIdx < m_maxDpbSize);
    const DpbEntryAV1* pDpbEntry = &m_DPB[dpbIdx];

    memset(pStdReferenceInfo, 0, sizeof(StdVideoEncodeAV1ReferenceInfo));
    pStdReferenceInfo->RefFrameId = 0; //GetRefBufId(); FIXME
    pStdReferenceInfo->frame_type = pDpbEntry->frameType;
    pStdReferenceInfo->OrderHint = pDpbEntry->picOrderCntVal % (1 << ORDER_HINT_BITS);
}
