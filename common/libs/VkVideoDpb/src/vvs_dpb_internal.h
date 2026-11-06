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
 * The context of vkvideo-dpb, shared by the common calls (vvs_dpb.c) and the codec planners
 * (vvs_dpb_<codec>.c).
 *
 * Everything a picture call changes is in VvsDpbState, one fixed-size structure: a picture call
 * copies it before it starts and copies it back if it fails, the rewind history is a ring of such
 * copies, and a checkpoint is one. What a call cannot undo, the writes the video commands made to the
 * slots and to the pool's resources, is counted outside it.
 */
#ifndef VVS_DPB_INTERNAL_H_
#define VVS_DPB_INTERNAL_H_ 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vkvideo_dpb/vvs_dpb.h"

/* A slot index that names no slot. */
#define VVS_DPB_NO_SLOT VVS_DPB_MAX_SLOTS

/* VvsDpbSlotState::flags */
/* The slot holds a reference picture. */
#define VVS_DPB_SLOT_REFERENCE 0x1U
/* H.264 and H.265: the picture is used for long-term reference. */
#define VVS_DPB_SLOT_LONG_TERM 0x2U
/* The client gave the picture's VvsDpbPictureInfo. */
#define VVS_DPB_SLOT_PICTURE_INFO 0x4U

/* A slot and the picture it holds. A slot without VVS_DPB_SLOT_REFERENCE holds nothing the DPB
 * keeps; its other members describe the last picture set up in it. */
typedef struct VvsDpbSlotState {
    VvsDpbPictureInfo pictureInfo;
    /* The picture's Std reference information as the video commands take it, kept current with its
     * marking. */
    VvsDpbStdReferenceInfo stdReferenceInfo;
    const VkVideoPictureResourceInfoKHR* pResource;
    uint32_t resourceIndex;
    uint32_t dirtyIntraRefreshRegions;
    uint32_t flags;
    /* H.264: LongTermFrameIdx, -1 for a short-term picture. AV1: the reference name the picture was
     * coded for, STD_VIDEO_AV1_REFERENCE_NAME_INVALID for none. */
    int32_t codecValue;
} VvsDpbSlotState;

typedef struct VvsDpbH264State {
    /* frame_num before the modulo: the count of reference pictures since the last IDR picture. */
    uint32_t frameNumCount;
    /* 8.2.1.1: the previous reference picture's PicOrderCntMsb and pic_order_cnt_lsb. */
    int32_t prevPicOrderCntMsb;
    int32_t prevPicOrderCntLsb;
    /* 8.2.1.3: the previous picture's FrameNumOffset and frame_num. */
    int32_t prevFrameNumOffset;
    uint32_t prevFrameNum;
    /* MaxLongTermFrameIdx, -1 for "no long-term frame indices". */
    int32_t maxLongTermFrameIdx;
} VvsDpbH264State;

typedef struct VvsDpbH265State {
    /* A CRA picture keeps the pictures before it in decoding order as references for the pictures that
     * precede it in output order; the first picture that follows it in output order drops them. */
    uint32_t craPending;
    int32_t craPicOrderCntVal;
} VvsDpbH265State;

/* The AV1 primary reference types: which earlier picture's CDFs a picture of each kind starts from. */
enum {
    VVS_DPB_AV1_PRIMARY_REGULAR = 0,
    VVS_DPB_AV1_PRIMARY_ARF = 1,
    VVS_DPB_AV1_PRIMARY_OVERLAY = 2,
    VVS_DPB_AV1_PRIMARY_GOLDEN = 3,
    VVS_DPB_AV1_PRIMARY_BWD = 4,
    VVS_DPB_AV1_PRIMARY_INTERNAL_ARF = 5,
    VVS_DPB_AV1_PRIMARY_TYPES = 6
};

/* A reference buffer that names no buffer. */
#define VVS_DPB_AV1_NO_BUFFER 0xFFU

typedef struct VvsDpbAV1State {
    /* The slot each of the eight reference buffers holds, VVS_DPB_NO_SLOT before the first key frame. */
    uint8_t bufferSlots[STD_VIDEO_AV1_NUM_REF_FRAMES];
    /* The buffer of each reference name, INTRA_FRAME to ALTREF_FRAME: a permutation of the buffers. */
    uint8_t nameBuffers[STD_VIDEO_AV1_NUM_REF_FRAMES];
    /* The buffer the last picture of each primary reference type refreshed, VVS_DPB_AV1_NO_BUFFER for
     * none. */
    uint8_t primaryBuffers[VVS_DPB_AV1_PRIMARY_TYPES];
} VvsDpbAV1State;

typedef struct VvsDpbState {
    VvsDpbSlotState slots[VVS_DPB_MAX_SLOTS];
    /* The slot the last picture was set up in, VVS_DPB_NO_SLOT before the first: for ROTATE. */
    uint32_t lastSetupSlot;
    /* Picture calls since the last reset, sequence call or rewind to a checkpoint. */
    uint32_t pictureCount;
    /* After create and reset: the next picture must be a key picture. */
    uint32_t keyPictureNeeded;
    /* POOL: the free resources, the one released longest ago first. */
    uint32_t freeCount;
    uint8_t freeResources[VVS_DPB_MAX_RESOURCES];
    union {
        VvsDpbH264State h264;
        VvsDpbH265State h265;
        VvsDpbAV1State av1;
    } codec;
} VvsDpbState;

/* The parameters of the active sequence a planner reads. */
typedef struct VvsDpbH264Sequence {
    /* Max(max_num_ref_frames, 1): the references the sliding window keeps (8.2.5.3). */
    uint32_t maxNumRefFrames;
    uint32_t log2MaxFrameNum;
    uint32_t log2MaxPicOrderCntLsb;
    uint32_t picOrderCntType;
    /* num_ref_idx_lX_default_active_minus1 + 1. */
    uint32_t numRefIdxL0DefaultActive;
    uint32_t numRefIdxL1DefaultActive;
} VvsDpbH264Sequence;

/* A short-term reference picture set as 7.4.8 derives it. */
typedef struct VvsDpbH265ShortTermSet {
    uint8_t numNegativePics;
    uint8_t numPositivePics;
    /* Bit i: UsedByCurrPicS0[i], UsedByCurrPicS1[i]. */
    uint16_t usedS0;
    uint16_t usedS1;
    int16_t deltaPocS0[STD_VIDEO_H265_MAX_DPB_SIZE];
    int16_t deltaPocS1[STD_VIDEO_H265_MAX_DPB_SIZE];
} VvsDpbH265ShortTermSet;

typedef struct VvsDpbH265Sequence {
    /* sps_max_dec_pic_buffering_minus1 of the highest sub-layer: the references the DPB holds beside
     * the picture being decoded. */
    uint32_t maxReferences;
    /* num_ref_idx_lX_default_active_minus1 + 1. */
    uint32_t numRefIdxL0DefaultActive;
    uint32_t numRefIdxL1DefaultActive;
    /* The SPS's short-term sets, derived, to match a picture's set against. */
    uint32_t shortTermSetCount;
    VvsDpbH265ShortTermSet shortTermSets[STD_VIDEO_H265_MAX_SHORT_TERM_REF_PIC_SETS];
} VvsDpbH265Sequence;

typedef struct VvsDpbAV1Sequence {
    /* OrderHintBits, 0 without order hints; idLen and the delta_frame_id length, 0 without frame
     * ids. */
    uint32_t orderHintBits;
    uint32_t frameIdBits;
    uint32_t deltaFrameIdBits;
    /* The GOP has bipredictive pictures: a P picture's own name is GOLDEN, and the names LAST to LAST3
     * hold the last pictures; otherwise LAST to GOLDEN. */
    uint32_t bipredictive;
    /* STD_VIDEO_AV1_REFERENCE_NAME_LAST3_FRAME or GOLDEN_FRAME: the last of the names that hold the last
     * pictures. */
    uint32_t lastLastName;
    /* The references a picture takes from the past and from the future. */
    uint32_t maxPastReferences;
    uint32_t maxFutureReferences;
    /* VkVideoEncodeAV1CapabilitiesKHR. */
    uint32_t maxSingleReferenceCount;
    uint32_t singleReferenceNameMask;
    uint32_t maxUnidirectionalCompoundReferenceCount;
    uint32_t maxUnidirectionalCompoundGroup1ReferenceCount;
    uint32_t unidirectionalCompoundReferenceNameMask;
    uint32_t maxBidirectionalCompoundReferenceCount;
    uint32_t maxBidirectionalCompoundGroup1ReferenceCount;
    uint32_t maxBidirectionalCompoundGroup2ReferenceCount;
    uint32_t bidirectionalCompoundReferenceNameMask;
    uint32_t primaryReferenceCdfOnly;
    /* The session's maxActiveReferencePictures. */
    uint32_t maxActiveReferences;
} VvsDpbAV1Sequence;

struct VvsDpb_T {
    /* Names the context in its checkpoints. */
    uint64_t tag;
    VkVideoCodecOperationFlagBitsKHR operation;
    VvsDpbCreateFlags flags;
    VvsDpbSlotPolicy slotPolicy;
    VvsDpbResources resources;
    /* The bound, 0 for none. */
    uint32_t boundSlots;
    uint32_t boundActiveReferences;
    uint32_t rewindDepth;
    /* The rewind history: historyCount states, the newest in history[historyNext - 1]. */
    uint32_t historyCount;
    uint32_t historyNext;
    /* Counts resets and sequence calls; a checkpoint holds the one it was saved in. */
    uint32_t generation;
    uint32_t sequenceActive;
    /* The picture call in progress chains its references' intra refresh information. */
    uint32_t intraRefreshChain;
    /* The slots the active sequence uses: requirements.maxDpbSlots. */
    uint32_t slotCount;
    VvsDpbSequenceRequirements requirements;
    VvsDpbGopInfo gopInfo;
    union {
        VvsDpbH264Sequence h264;
        VvsDpbH265Sequence h265;
        VvsDpbAV1Sequence av1;
    } sequence;
    /* The setups each slot and each POOL resource has had: what a checkpoint is checked against. */
    uint32_t slotWriteCounts[VVS_DPB_MAX_SLOTS];
    uint32_t resourceWriteCounts[VVS_DPB_MAX_RESOURCES];
    VvsDpbState state;
    /* The state before the picture call in progress. */
    VvsDpbState saved;
    /* rewindDepth states. */
    VvsDpbState history[];
};

/* ------------------------------------------------------------------------------------------------
 * The steps of a picture call, in order: begin, the planner's decisions, acquire (the last step that
 * can fail), the planner's changes and outputs, commit.
 * ---------------------------------------------------------------------------------------------- */

/* Checks the context can take a picture of this codec, and saves the state. dirtyIntraRefreshRegions
 * is the picture's, VVS_DPB_NO_INTRA_REFRESH for a picture encoded without intra refresh. */
VkResult vvsDpbPictureBegin(VvsDpb dpb, VkVideoCodecOperationFlagBitsKHR operation, uint32_t dirtyIntraRefreshRegions);

/* The dirty intra refresh regions a picture keeps for the pictures that reference it. */
static inline uint32_t vvsDpbDirtyRegions(uint32_t dirtyIntraRefreshRegions) {
    return (dirtyIntraRefreshRegions == VVS_DPB_NO_INTRA_REFRESH) ? 0U : dirtyIntraRefreshRegions;
}

/* Returns the state to the one vvsDpbPictureBegin() saved. */
void vvsDpbPictureAbort(VvsDpb dpb);

/* The slot a picture is set up in: inactive, not in excludedMask, and not holding a reference in a
 * state of the rewind history. VVS_DPB_NO_SLOT when there is none. */
uint32_t vvsDpbChooseSetupSlot(VvsDpb dpb, uint32_t excludedMask);

/* Gives the slot a resource, from the pool or the client's pfnAcquireResource. */
VkResult vvsDpbAcquireResource(VvsDpb dpb, uint32_t slotIndex, const VvsDpbPictureInfo* pPictureInfo);

/* Writes the result's references, in the order given, each slot once, from the saved state: the
 * references as the picture uses them, before its marking. */
void vvsDpbResultSetReferences(VvsDpb dpb, const uint32_t* pSlotIndices, uint32_t count, VvsDpbPictureResult* pResult);

/* Ends the picture call: writes the setup entry from the state (VVS_DPB_NO_SLOT for a picture that
 * sets up no slot), the releases, the rewind history and the write counts, and calls the release
 * callbacks. */
void vvsDpbPictureCommit(VvsDpb dpb, uint32_t setupSlot, VvsDpbPictureResult* pResult);

/* Sets up the active sequence, once the codec has checked it. */
VkResult vvsDpbSequenceCommit(VvsDpb dpb, const VvsDpbSequenceRequirements* pRequirements, const VvsDpbGopInfo* pGopInfo);

/* Whether the GOP asks for planning this version does not do. */
bool vvsDpbGopHasPatterns(const VvsDpbGopInfo* pGopInfo);

/* The slots holding references in a state, bit i for slot i. */
uint32_t vvsDpbReferenceMask(const VvsDpbState* pState);

/* Sorts slots by key, ascending or descending; the keys of references are distinct. */
void vvsDpbSortSlots(uint32_t* pSlots, int32_t* pKeys, uint32_t count, bool descending);

/* H.264 and H.265: a slot that holds a short-term reference picture. */
static inline bool vvsDpbIsShortTerm(const VvsDpbSlotState* pSlot) {
    return (pSlot->flags & (VVS_DPB_SLOT_REFERENCE | VVS_DPB_SLOT_LONG_TERM)) == VVS_DPB_SLOT_REFERENCE;
}

static inline uint32_t vvsDpbMin(uint32_t a, uint32_t b) { return (a < b) ? a : b; }
static inline uint32_t vvsDpbMax(uint32_t a, uint32_t b) { return (a > b) ? a : b; }

#endif /* VVS_DPB_INTERNAL_H_ */
