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
 * The AV1 encode planner. Section numbers are those of the AV1 bitstream specification.
 *
 * A picture is coded for one reference name, which says what it is: a key or intra frame INTRA_FRAME,
 * the last pictures LAST_FRAME, a shown P picture of a GOP with B pictures GOLDEN_FRAME, a hidden picture
 * (shown later with show_existing_frame) ALTREF_FRAME. The name decides which names' buffers the picture
 * refreshes, and the names are then moved between the buffers: the names LAST_FRAME to the last of the
 * last pictures' names hold the last pictures, newest first; showing a hidden picture swaps it into
 * GOLDEN_FRAME. A name whose picture is too old for its order hint to tell it from a newer one takes the
 * picture too.
 */
#include "vvs_dpb_internal.h"

#define VVS_DPB_AV1_NAME_INVALID ((int32_t)STD_VIDEO_AV1_REFERENCE_NAME_INVALID)

/* How a picture changes the names' buffers. */
typedef enum UpdateType {
    UPDATE_LAST,
    UPDATE_GOLDEN,
    UPDATE_ALTREF,
    UPDATE_OVERLAY,
    UPDATE_INTERNAL_ALTREF,
    UPDATE_INTERNAL_OVERLAY,
    UPDATE_BWDREF,
    UPDATE_NONE
} UpdateType;

static uint32_t NameBit(int32_t name) { return 1U << (uint32_t)name; }

static UpdateType GetUpdateType(int32_t name, bool overlay) {
    switch (name) {
        case STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME:
            return overlay ? UPDATE_INTERNAL_OVERLAY : UPDATE_INTERNAL_ALTREF;
        case STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME:
            return overlay ? UPDATE_INTERNAL_OVERLAY : UPDATE_BWDREF;
        case STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME:
            return overlay ? UPDATE_OVERLAY : UPDATE_ALTREF;
        case STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME:
            return UPDATE_GOLDEN;
        case VVS_DPB_AV1_NAME_INVALID:
            return UPDATE_NONE;
        default:
            return UPDATE_LAST;
    }
}

/* The names whose buffers a picture refreshes. */
static uint32_t UpdateNames(const VvsDpbAV1Sequence* pSequence, UpdateType type) {
    const uint32_t last = NameBit((int32_t)pSequence->lastLastName);
    switch (type) {
        case UPDATE_LAST:
        case UPDATE_OVERLAY:
        case UPDATE_INTERNAL_OVERLAY:
            return last;
        case UPDATE_GOLDEN:
            return last | NameBit(STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME);
        case UPDATE_ALTREF:
            return NameBit(STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME);
        case UPDATE_INTERNAL_ALTREF:
            return NameBit(STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME);
        case UPDATE_BWDREF:
            return NameBit(STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME);
        default:
            return 0;
    }
}

/* The names' buffers after a picture: showing a hidden picture swaps its buffer in, and a picture coded
 * or shown as one of the last pictures moves the last pictures' names one buffer on. */
static void MoveNames(const VvsDpbAV1Sequence* pSequence, VvsDpbAV1State* pAV1, UpdateType type, int32_t name, bool showExisting) {
    if (type == UPDATE_NONE) {
        return;
    }
    const uint32_t lastLast = pSequence->lastLastName;
    uint8_t names[STD_VIDEO_AV1_NUM_REF_FRAMES];
    memcpy(names, pAV1->nameBuffers, sizeof(names));
    if (type == UPDATE_OVERLAY) {
        names[lastLast] = pAV1->nameBuffers[STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME];
        names[STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME] = pAV1->nameBuffers[STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME];
        names[STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME] = pAV1->nameBuffers[lastLast];
    } else if ((type == UPDATE_INTERNAL_OVERLAY) && showExisting) {
        names[lastLast] = pAV1->nameBuffers[name];
        names[name] = pAV1->nameBuffers[lastLast];
    }
    if (((type == UPDATE_LAST) || (type == UPDATE_GOLDEN) || (type == UPDATE_INTERNAL_OVERLAY) || (type == UPDATE_OVERLAY)) &&
        (lastLast > STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME)) {
        names[STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME] = names[lastLast];
        for (uint32_t n = STD_VIDEO_AV1_REFERENCE_NAME_LAST2_FRAME; n <= lastLast; n++) {
            names[n] = pAV1->nameBuffers[n - 1U];
        }
    }
    memcpy(pAV1->nameBuffers, names, sizeof(names));
}

/* Which earlier picture's CDFs a picture of this name starts from. */
static uint32_t PrimaryType(const VvsDpbAV1Sequence* pSequence, int32_t name, bool errorResilient, bool overlay) {
    if ((name == STD_VIDEO_AV1_REFERENCE_NAME_INTRA_FRAME) || errorResilient) {
        return (pSequence->maxFutureReferences > 0U) ? VVS_DPB_AV1_PRIMARY_BWD : VVS_DPB_AV1_PRIMARY_REGULAR;
    }
    if (overlay) {
        return VVS_DPB_AV1_PRIMARY_OVERLAY;
    }
    switch (name) {
        case STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME:
            return VVS_DPB_AV1_PRIMARY_ARF;
        case STD_VIDEO_AV1_REFERENCE_NAME_ALTREF2_FRAME:
            return VVS_DPB_AV1_PRIMARY_INTERNAL_ARF;
        case STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME:
            return VVS_DPB_AV1_PRIMARY_GOLDEN;
        case STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME:
            return VVS_DPB_AV1_PRIMARY_BWD;
        default:
            return (pSequence->maxFutureReferences > 0U) ? VVS_DPB_AV1_PRIMARY_INTERNAL_ARF : VVS_DPB_AV1_PRIMARY_REGULAR;
    }
}

static uint32_t BufferSlot(const VvsDpbAV1State* pAV1, uint32_t buffer) {
    return (buffer < STD_VIDEO_AV1_NUM_REF_FRAMES) ? pAV1->bufferSlots[buffer] : VVS_DPB_NO_SLOT;
}

static uint32_t NameSlot(const VvsDpbAV1State* pAV1, uint32_t name) { return BufferSlot(pAV1, pAV1->nameBuffers[name]); }

static uint64_t SlotOrder(const VvsDpbState* pState, uint32_t slot) { return pState->slots[slot].pictureInfo.displayOrderIndex; }

/* A slot holds a reference while a buffer holds it. */
static void MarkReferences(VvsDpbState* pState) {
    uint32_t held = 0;
    for (uint32_t b = 0; b < STD_VIDEO_AV1_NUM_REF_FRAMES; b++) {
        if (pState->codec.av1.bufferSlots[b] < VVS_DPB_MAX_SLOTS) {
            held |= 1U << pState->codec.av1.bufferSlots[b];
        }
    }
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        if ((held & (1U << i)) != 0U) {
            pState->slots[i].flags |= VVS_DPB_SLOT_REFERENCE;
        } else {
            pState->slots[i].flags &= ~VVS_DPB_SLOT_REFERENCE;
        }
    }
}

VVS_DPB_API VkResult vvsDpbAV1Sequence(VvsDpb dpb, const VvsDpbAV1SequenceInfo* pSequenceInfo,
                                       VvsDpbSequenceRequirements* pRequirements) {
    if ((dpb == VK_NULL_HANDLE) || (dpb->operation != VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) || (pSequenceInfo == NULL) ||
        (pSequenceInfo->pStdSequenceHeader == NULL) || (pSequenceInfo->pCapabilities == NULL)) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    if (vvsDpbGopHasPatterns(pSequenceInfo->pGopInfo)) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const StdVideoAV1SequenceHeader* pHeader = pSequenceInfo->pStdSequenceHeader;
    const VkVideoEncodeAV1CapabilitiesKHR* pCaps = pSequenceInfo->pCapabilities;
    const VvsDpbGopInfo* pGop = pSequenceInfo->pGopInfo;
    /* Without GOP information the GOP may have B pictures. */
    const bool bipredictive = (pGop == NULL) || (pGop->gopStructure == VVS_DPB_GOP_STRUCTURE_UNSPECIFIED) ||
                              (pGop->consecutiveBipredictiveFrameCount > 0U);
    const bool intraRefresh =
        ((dpb->flags & VVS_DPB_CREATE_INTRA_REFRESH_BIT) != 0U) && (pSequenceInfo->pIntraRefreshCapabilities != NULL);

    VvsDpbSequenceRequirements requirements;
    /* The buffers hold at most 7 pictures: BWDREF_FRAME and ALTREF2_FRAME are refreshed together, so
     * they hold one picture; the picture being encoded takes one more slot. */
    requirements.maxReferencePictures = STD_VIDEO_AV1_NUM_REF_FRAMES - 1U;
    requirements.maxDpbSlots = requirements.maxReferencePictures + 1U + ((dpb->rewindDepth > 0U) ? dpb->rewindDepth - 1U : 0U);
    /* The planner takes no more references than the session's bound. */
    requirements.maxActiveReferencePictures = (dpb->boundActiveReferences != 0U)
                                                  ? vvsDpbMin(dpb->boundActiveReferences, STD_VIDEO_AV1_REFS_PER_FRAME)
                                                  : STD_VIDEO_AV1_REFS_PER_FRAME;
    if ((requirements.maxDpbSlots > VVS_DPB_MAX_SLOTS) ||
        ((dpb->boundSlots != 0U) && (requirements.maxDpbSlots > dpb->boundSlots))) {
        return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
    }

    VvsDpbAV1Sequence* pSequence = &dpb->sequence.av1;
    memset(pSequence, 0, sizeof(*pSequence));
    pSequence->orderHintBits = pHeader->flags.enable_order_hint ? pHeader->order_hint_bits_minus_1 + 1U : 0U;
    if (pHeader->flags.frame_id_numbers_present_flag) {
        pSequence->deltaFrameIdBits = pHeader->delta_frame_id_length_minus_2 + 2U;
        pSequence->frameIdBits = pSequence->deltaFrameIdBits + pHeader->additional_frame_id_length_minus_1 + 1U;
    }
    pSequence->bipredictive = bipredictive ? 1U : 0U;
    pSequence->lastLastName = bipredictive ? STD_VIDEO_AV1_REFERENCE_NAME_LAST3_FRAME : STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME;
    pSequence->maxPastReferences = 4U;
    pSequence->maxFutureReferences = 3U;
    if (intraRefresh) {
        pSequence->maxPastReferences = vvsDpbMin(pSequence->maxPastReferences,
                                                 pSequenceInfo->pIntraRefreshCapabilities->maxIntraRefreshActiveReferencePictures);
        pSequence->maxFutureReferences = 0U;
    }
    pSequence->maxSingleReferenceCount = pCaps->maxSingleReferenceCount;
    pSequence->singleReferenceNameMask = pCaps->singleReferenceNameMask;
    pSequence->maxUnidirectionalCompoundReferenceCount = pCaps->maxUnidirectionalCompoundReferenceCount;
    pSequence->maxUnidirectionalCompoundGroup1ReferenceCount = pCaps->maxUnidirectionalCompoundGroup1ReferenceCount;
    pSequence->unidirectionalCompoundReferenceNameMask = pCaps->unidirectionalCompoundReferenceNameMask;
    pSequence->maxBidirectionalCompoundReferenceCount = pCaps->maxBidirectionalCompoundReferenceCount;
    pSequence->maxBidirectionalCompoundGroup1ReferenceCount = pCaps->maxBidirectionalCompoundGroup1ReferenceCount;
    pSequence->maxBidirectionalCompoundGroup2ReferenceCount = pCaps->maxBidirectionalCompoundGroup2ReferenceCount;
    pSequence->bidirectionalCompoundReferenceNameMask = pCaps->bidirectionalCompoundReferenceNameMask;
    pSequence->primaryReferenceCdfOnly =
        ((pCaps->flags & VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR) != 0U) ? 1U : 0U;
    pSequence->maxActiveReferences = requirements.maxActiveReferencePictures;
    vvsDpbSequenceCommit(dpb, &requirements, pSequenceInfo->pGopInfo);
    if (pRequirements != NULL) {
        *pRequirements = requirements;
    }
    return VK_SUCCESS;
}

/* The references of an inter picture, grouped and named (today's encoder): the pictures before it,
 * nearest first, take names LAST_FRAME to GOLDEN_FRAME, those after it BWDREF_FRAME to ALTREF_FRAME,
 * within the prediction mode's names and counts. */
typedef struct Groups {
    uint32_t count[2];
    int32_t names[2][STD_VIDEO_AV1_REFS_PER_FRAME]; /* reference name - 1 */
    uint32_t futureCount;                           /* the future references before naming */
} Groups;

static bool GroupsHoldSlot(const Groups* pGroups, const VvsDpbAV1State* pAV1, uint32_t slot) {
    for (uint32_t g = 0; g < 2U; g++) {
        for (uint32_t i = 0; i < pGroups->count[g]; i++) {
            if (NameSlot(pAV1, (uint32_t)pGroups->names[g][i] + 1U) == slot) {
                return true;
            }
        }
    }
    return false;
}

static void FormGroups(const VvsDpb dpb, bool bipredictive, uint64_t order, uint32_t primarySlot, Groups* pGroups) {
    const VvsDpbAV1Sequence* pSequence = &dpb->sequence.av1;
    const VvsDpbState* pState = &dpb->state;
    const VvsDpbAV1State* pAV1 = &pState->codec.av1;
    uint32_t past[VVS_DPB_MAX_SLOTS];
    uint32_t future[VVS_DPB_MAX_SLOTS];
    uint32_t pastCount = 0;
    uint32_t futureCount = 0;
    for (uint32_t i = 0; i < dpb->slotCount; i++) {
        if ((pState->slots[i].flags & VVS_DPB_SLOT_REFERENCE) == 0U) {
            continue;
        }
        /* Past by descending order, future by ascending: insertion keeps equal keys in slot order. */
        if (SlotOrder(pState, i) < order) {
            uint32_t j = pastCount++;
            while ((j > 0U) && (SlotOrder(pState, past[j - 1U]) < SlotOrder(pState, i))) {
                past[j] = past[j - 1U];
                j--;
            }
            past[j] = i;
        } else {
            uint32_t j = futureCount++;
            while ((j > 0U) && (SlotOrder(pState, future[j - 1U]) > SlotOrder(pState, i))) {
                future[j] = future[j - 1U];
                j--;
            }
            future[j] = i;
        }
    }
    uint32_t limit0 = vvsDpbMin(pastCount, pSequence->maxPastReferences);
    uint32_t limit1 = bipredictive ? vvsDpbMin(futureCount, pSequence->maxFutureReferences) : 0U;
    uint32_t mask;
    uint32_t maxReferences = pSequence->maxActiveReferences;
    if (bipredictive && (pSequence->maxBidirectionalCompoundReferenceCount > 0U)) {
        mask = pSequence->bidirectionalCompoundReferenceNameMask;
        limit0 = vvsDpbMin(limit0, pSequence->maxBidirectionalCompoundGroup1ReferenceCount);
        limit1 = vvsDpbMin(limit1, pSequence->maxBidirectionalCompoundGroup2ReferenceCount);
        maxReferences = vvsDpbMin(maxReferences, pSequence->maxBidirectionalCompoundReferenceCount);
    } else if (pSequence->maxUnidirectionalCompoundReferenceCount > 0U) {
        /* Group 1 only. */
        mask = pSequence->unidirectionalCompoundReferenceNameMask & (NameBit(STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME) - 1U);
        limit0 = vvsDpbMin(limit0, pSequence->maxUnidirectionalCompoundGroup1ReferenceCount);
        maxReferences = vvsDpbMin(maxReferences, pSequence->maxUnidirectionalCompoundReferenceCount);
    } else {
        mask = pSequence->singleReferenceNameMask;
        maxReferences = vvsDpbMin(maxReferences, pSequence->maxSingleReferenceCount);
    }
    pGroups->futureCount = limit1;

    static const uint32_t kFirst[2] = {STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME, STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME};
    static const uint32_t kEnd[2] = {STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME, STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME};
    const uint32_t* pSlots[2] = {past, future};
    const uint32_t counts[2] = {pastCount, futureCount};
    const uint32_t limits[2] = {limit0, limit1};
    for (uint32_t g = 0; g < 2U; g++) {
        pGroups->count[g] = 0;
        for (uint32_t r = 0; (r < counts[g]) && (pGroups->count[g] < limits[g]); r++) {
            for (uint32_t name = kFirst[g]; name <= kEnd[g]; name++) {
                if (((mask & (1U << (name - 1U))) != 0U) && (NameSlot(pAV1, name) == pSlots[g][r])) {
                    pGroups->names[g][pGroups->count[g]++] = (int32_t)name - 1;
                    break;
                }
            }
        }
    }
    /* Over the mode's count, or over the session's active references with the primary reference
     * frame, added as one more reference for its CDFs when its picture is none of these: the farthest
     * reference of the larger group goes first. */
    for (;;) {
        const uint32_t total = pGroups->count[0] + pGroups->count[1];
        const bool added =
            pSequence->primaryReferenceCdfOnly && (primarySlot != VVS_DPB_NO_SLOT) && !GroupsHoldSlot(pGroups, pAV1, primarySlot);
        if ((total == 0U) || ((total <= maxReferences) && ((total + (added ? 1U : 0U)) <= pSequence->maxActiveReferences))) {
            break;
        }
        if (pGroups->count[0] > pGroups->count[1]) {
            pGroups->count[0]--;
        } else {
            pGroups->count[1]--;
        }
    }
}

VVS_DPB_API VkResult vvsDpbEncodeAV1Picture(VvsDpb dpb, const VvsDpbAV1EncodePictureInfo* pPictureInfo,
                                            VvsDpbPictureResult* pResult) {
    if ((pPictureInfo == NULL) || (pPictureInfo->pPictureInfo == NULL) || (pPictureInfo->pPictureInfoAV1 == NULL) ||
        (pPictureInfo->pStdPictureInfo == NULL) || (pResult == NULL)) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    VkResult result = vvsDpbPictureBegin(dpb, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, pPictureInfo->dirtyIntraRefreshRegions);
    if (result != VK_SUCCESS) {
        return result;
    }
    const VvsDpbAV1Sequence* pSequence = &dpb->sequence.av1;
    VvsDpbState* pState = &dpb->state;
    VvsDpbAV1State* pAV1 = &pState->codec.av1;
    StdVideoEncodeAV1PictureInfo* pStd = pPictureInfo->pStdPictureInfo;
    VkVideoEncodeAV1PictureInfoKHR* pAV1Info = pPictureInfo->pPictureInfoAV1;
    const VvsDpbPictureInfo* pInfo = pPictureInfo->pPictureInfo;
    const StdVideoAV1FrameType frameType = pStd->frame_type;
    const bool key = frameType == STD_VIDEO_AV1_FRAME_TYPE_KEY;
    const bool intra = key || (frameType == STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY);
    const bool shown = pStd->flags.show_frame != 0U;
    const bool errorResilient = pStd->flags.error_resilient_mode != 0U;
    const bool shownKeyOrSwitch = (key && shown) || (frameType == STD_VIDEO_AV1_FRAME_TYPE_SWITCH);
    const bool bipredictive = pInfo->pictureType == VVS_DPB_PICTURE_TYPE_BIPREDICTIVE;
    if (pState->keyPictureNeeded && !(key && shown)) {
        vvsDpbPictureAbort(dpb);
        return VK_ERROR_VALIDATION_FAILED;
    }

    /* The name the picture is coded for. */
    int32_t name;
    if (key) {
        name = STD_VIDEO_AV1_REFERENCE_NAME_INTRA_FRAME;
    } else if (!shown) {
        name = STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME;
    } else if (intra) {
        name = STD_VIDEO_AV1_REFERENCE_NAME_INTRA_FRAME;
    } else if (!bipredictive && pSequence->bipredictive) {
        name = STD_VIDEO_AV1_REFERENCE_NAME_GOLDEN_FRAME;
    } else if (pPictureInfo->reference) {
        name = STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME;
    } else {
        name = VVS_DPB_AV1_NAME_INVALID;
    }

    /* The primary reference frame: the last picture of the same kind, through a name that holds it. */
    uint32_t primaryName = STD_VIDEO_AV1_PRIMARY_REF_NONE;
    uint32_t primarySlot = VVS_DPB_NO_SLOT;
    if (!intra && !errorResilient) {
        const uint32_t type = PrimaryType(pSequence, name, false, false);
        uint32_t buffer = pAV1->primaryBuffers[type];
        if (buffer == VVS_DPB_AV1_NO_BUFFER) {
            buffer = (type == VVS_DPB_AV1_PRIMARY_INTERNAL_ARF)
                         ? pAV1->primaryBuffers[VVS_DPB_AV1_PRIMARY_ARF]
                         : pAV1->primaryBuffers[(pSequence->maxFutureReferences > 0U) ? VVS_DPB_AV1_PRIMARY_BWD
                                                                                      : VVS_DPB_AV1_PRIMARY_REGULAR];
        }
        const uint32_t slot = BufferSlot(pAV1, buffer);
        if ((slot != VVS_DPB_NO_SLOT) && ((pState->slots[slot].flags & VVS_DPB_SLOT_REFERENCE) != 0U)) {
            for (uint32_t n = STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME; n <= STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME; n++) {
                if (pAV1->nameBuffers[n] == buffer) {
                    primaryName = n - 1U;
                    primarySlot = slot;
                    break;
                }
            }
        }
    }

    Groups groups;
    memset(&groups, 0, sizeof(groups));
    if (!intra) {
        FormGroups(dpb, bipredictive, pInfo->displayOrderIndex, primarySlot, &groups);
        /* Without the CDF-only use of a primary reference frame, the primary reference frame is one of
         * the references, by its own name or another that holds the picture, or none. */
        if ((primaryName != STD_VIDEO_AV1_PRIMARY_REF_NONE) && !pSequence->primaryReferenceCdfOnly) {
            uint32_t samePicture = STD_VIDEO_AV1_PRIMARY_REF_NONE;
            bool named = false;
            for (uint32_t g = 0; (g < 2U) && !named; g++) {
                for (uint32_t i = 0; i < groups.count[g]; i++) {
                    if ((uint32_t)groups.names[g][i] == primaryName) {
                        named = true;
                        break;
                    }
                    if ((samePicture == STD_VIDEO_AV1_PRIMARY_REF_NONE) &&
                        (NameSlot(pAV1, (uint32_t)groups.names[g][i] + 1U) == primarySlot)) {
                        samePicture = (uint32_t)groups.names[g][i];
                    }
                }
            }
            if (!named) {
                primaryName = samePicture;
                if (primaryName == STD_VIDEO_AV1_PRIMARY_REF_NONE) {
                    primarySlot = VVS_DPB_NO_SLOT;
                }
            }
        }
    }
    /* A B picture with no picture after it is coded as a P picture. */
    const bool codedBipredictive = bipredictive && !intra && (groups.futureCount > 0U);

    /* The names the picture refreshes, and those whose pictures are too old for their order hint (or
     * frame id) to tell them from newer ones (5.9.4). */
    const UpdateType updateType = GetUpdateType(name, false);
    uint32_t names = shownKeyOrSwitch ? 0xFFU : UpdateNames(pSequence, updateType);
    if (!shownKeyOrSwitch && ((pSequence->orderHintBits > 0U) || (pSequence->frameIdBits > 0U))) {
        for (uint32_t n = STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME; n <= STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME; n++) {
            const uint32_t slot = NameSlot(pAV1, n);
            if (((names & NameBit((int32_t)n)) != 0U) || (slot == VVS_DPB_NO_SLOT) ||
                ((pState->slots[slot].flags & VVS_DPB_SLOT_REFERENCE) == 0U)) {
                continue;
            }
            bool stale = false;
            if (pSequence->frameIdBits > 0U) {
                const uint32_t idMask = (1U << pSequence->frameIdBits) - 1U;
                const uint32_t next = (pStd->current_frame_id + STD_VIDEO_AV1_NUM_REF_FRAMES) & idMask;
                const uint32_t delta = (next - pState->slots[slot].stdReferenceInfo.av1.RefFrameId) & idMask;
                stale = delta >= (1U << pSequence->deltaFrameIdBits);
            }
            if (pSequence->orderHintBits > 0U) {
                const uint64_t modulus = (uint64_t)1U << (pSequence->orderHintBits + 1U);
                const uint64_t next = (pInfo->displayOrderIndex + STD_VIDEO_AV1_NUM_REF_FRAMES) % modulus;
                const uint64_t delta = (next + modulus - (SlotOrder(pState, slot) % modulus)) % modulus;
                stale = stale || (delta >= ((uint64_t)1U << pSequence->orderHintBits));
            }
            if (stale) {
                names |= NameBit((int32_t)n);
            }
        }
    }
    uint8_t refresh = 0xFFU;
    if (!shownKeyOrSwitch) {
        refresh = 0;
        for (uint32_t n = 0; n < STD_VIDEO_AV1_NUM_REF_FRAMES; n++) {
            if ((names & NameBit((int32_t)n)) != 0U) {
                refresh = (uint8_t)(refresh | (1U << pAV1->nameBuffers[n]));
            }
        }
    }

    /* The setup slot, one no buffer holds. */
    const bool setup = (refresh != 0U) || ((dpb->flags & VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT) != 0U);
    uint32_t setupSlot = VVS_DPB_NO_SLOT;
    if (setup) {
        setupSlot = vvsDpbChooseSetupSlot(dpb, 0);
        if (setupSlot == VVS_DPB_NO_SLOT) {
            vvsDpbPictureAbort(dpb);
            return VK_ERROR_TOO_MANY_OBJECTS;
        }
        result = vvsDpbAcquireResource(dpb, setupSlot, pInfo);
        if (result != VK_SUCCESS) {
            vvsDpbPictureAbort(dpb);
            return result;
        }
    }

    /* Nothing fails from here on. The frame header's reference fields, from the buffers before the
     * picture. */
    memset(pStd->ref_frame_idx, 0, sizeof(pStd->ref_frame_idx));
    memset(pStd->ref_order_hint, 0, sizeof(pStd->ref_order_hint));
    memset(pStd->delta_frame_id_minus_1, 0, sizeof(pStd->delta_frame_id_minus_1));
    if (!intra) {
        for (uint32_t n = STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME; n <= STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME; n++) {
            pStd->ref_frame_idx[n - 1U] = (int8_t)pAV1->nameBuffers[n];
            const uint32_t slot = NameSlot(pAV1, n);
            if ((pSequence->frameIdBits > 0U) && (slot != VVS_DPB_NO_SLOT)) {
                const uint32_t idMask = (1U << pSequence->frameIdBits) - 1U;
                pStd->delta_frame_id_minus_1[n - 1U] =
                    ((pStd->current_frame_id - pState->slots[slot].stdReferenceInfo.av1.RefFrameId) & idMask) - 1U;
            }
        }
        for (uint32_t b = 0; b < STD_VIDEO_AV1_NUM_REF_FRAMES; b++) {
            const uint32_t slot = BufferSlot(pAV1, b);
            pStd->ref_order_hint[b] = (slot != VVS_DPB_NO_SLOT) ? pState->slots[slot].stdReferenceInfo.av1.OrderHint : 0U;
        }
    }
    pStd->primary_ref_frame = (uint8_t)primaryName;
    pStd->refresh_frame_flags = refresh;

    /* The reference names, and the slots the encode binds: group 1's, group 2's, then the primary
     * reference frame's when it is used for its CDFs only. */
    uint32_t references[STD_VIDEO_AV1_REFS_PER_FRAME + 1U];
    uint32_t referenceCount = 0;
    for (uint32_t n = 0; n < STD_VIDEO_AV1_REFS_PER_FRAME; n++) {
        pAV1Info->referenceNameSlotIndices[n] = -1;
    }
    bool cdfOnly = primaryName != STD_VIDEO_AV1_PRIMARY_REF_NONE;
    for (uint32_t g = 0; g < 2U; g++) {
        for (uint32_t i = 0; i < groups.count[g]; i++) {
            const int32_t n = groups.names[g][i];
            const uint32_t slot = NameSlot(pAV1, (uint32_t)n + 1U);
            pAV1Info->referenceNameSlotIndices[n] = (int32_t)slot;
            references[referenceCount++] = slot;
            if ((uint32_t)n == primaryName) {
                cdfOnly = false;
            }
        }
    }
    const bool lastNamed = pAV1Info->referenceNameSlotIndices[STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME - 1U] != -1;
    const bool bwdNamed = pAV1Info->referenceNameSlotIndices[STD_VIDEO_AV1_REFERENCE_NAME_BWDREF_FRAME - 1U] != -1;
    const bool altNamed = pAV1Info->referenceNameSlotIndices[STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME - 1U] != -1;
    if (cdfOnly) {
        pAV1Info->referenceNameSlotIndices[primaryName] = (int32_t)primarySlot;
        references[referenceCount++] = primarySlot;
    }
    pAV1Info->primaryReferenceCdfOnly = cdfOnly ? VK_TRUE : VK_FALSE;

    /* The prediction mode the references allow, and the rate control group. */
    VkVideoEncodeAV1PredictionModeKHR mode;
    if (intra) {
        mode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_INTRA_ONLY_KHR;
    } else if ((groups.count[0] > 0U) && (groups.count[1] > 0U)) {
        mode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_BIDIRECTIONAL_COMPOUND_KHR;
    } else if ((lastNamed && (groups.count[0] >= 2U)) || (bwdNamed && altNamed)) {
        mode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_UNIDIRECTIONAL_COMPOUND_KHR;
    } else {
        mode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR;
    }
    if ((mode == VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_BIDIRECTIONAL_COMPOUND_KHR) &&
        (pSequence->maxBidirectionalCompoundReferenceCount == 0U)) {
        mode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_UNIDIRECTIONAL_COMPOUND_KHR;
    }
    if ((mode == VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_UNIDIRECTIONAL_COMPOUND_KHR) &&
        (pSequence->maxUnidirectionalCompoundReferenceCount == 0U)) {
        mode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR;
    }
    if ((mode == VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR) && (pSequence->maxSingleReferenceCount == 0U)) {
        mode = VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_INTRA_ONLY_KHR;
    }
    pAV1Info->predictionMode = mode;
    pAV1Info->rateControlGroup = intra               ? VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_INTRA_KHR
                                 : codedBipredictive ? VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_BIPREDICTIVE_KHR
                                                     : VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_PREDICTIVE_KHR;

    vvsDpbResultSetReferences(dpb, references, referenceCount, pResult);

    /* The picture's slot, then the buffers and the names (7.20). */
    if (setup) {
        VvsDpbSlotState* pSlot = &pState->slots[setupSlot];
        pSlot->flags = VVS_DPB_SLOT_PICTURE_INFO;
        pSlot->pictureInfo = *pInfo;
        pSlot->dirtyIntraRefreshRegions = vvsDpbDirtyRegions(pPictureInfo->dirtyIntraRefreshRegions);
        pSlot->codecValue = name;
        StdVideoEncodeAV1ReferenceInfo* pReference = &pSlot->stdReferenceInfo.av1;
        memset(pReference, 0, sizeof(*pReference));
        pReference->RefFrameId = pStd->current_frame_id;
        pReference->frame_type = frameType;
        pReference->OrderHint = pStd->order_hint;
        for (uint32_t n = 0; n < STD_VIDEO_AV1_NUM_REF_FRAMES; n++) {
            if ((names & NameBit((int32_t)n)) != 0U) {
                pAV1->bufferSlots[pAV1->nameBuffers[n]] = (uint8_t)setupSlot;
            }
        }
    }
    for (uint32_t n = STD_VIDEO_AV1_REFERENCE_NAME_LAST_FRAME; n <= STD_VIDEO_AV1_REFERENCE_NAME_ALTREF_FRAME; n++) {
        if ((names & NameBit((int32_t)n)) != 0U) {
            pAV1->primaryBuffers[PrimaryType(pSequence, name, errorResilient, false)] = pAV1->nameBuffers[n];
            break;
        }
    }
    if (!shownKeyOrSwitch) {
        MoveNames(pSequence, pAV1, updateType, name, false);
    }
    MarkReferences(pState);
    if (key && shown) {
        pState->keyPictureNeeded = 0;
    }
    vvsDpbPictureCommit(dpb, setupSlot, pResult);
    return VK_SUCCESS;
}

VVS_DPB_API VkResult vvsDpbEncodeAV1ShowExistingFrame(VvsDpb dpb, uint64_t displayOrderIndex, uint8_t* pFrameToShowMapIdx,
                                                      VvsDpbPictureResult* pResult) {
    if ((pFrameToShowMapIdx == NULL) || (pResult == NULL)) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    VkResult result = vvsDpbPictureBegin(dpb, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, VVS_DPB_NO_INTRA_REFRESH);
    if (result != VK_SUCCESS) {
        return result;
    }
    const VvsDpbAV1Sequence* pSequence = &dpb->sequence.av1;
    VvsDpbState* pState = &dpb->state;
    VvsDpbAV1State* pAV1 = &pState->codec.av1;
    /* The buffer of the first name, INTRA_FRAME to ALTREF_FRAME, that holds the picture. */
    uint32_t buffer = VVS_DPB_AV1_NO_BUFFER;
    uint32_t slot = VVS_DPB_NO_SLOT;
    for (uint32_t n = 0; n < STD_VIDEO_AV1_NUM_REF_FRAMES; n++) {
        const uint32_t s = NameSlot(pAV1, n);
        if ((s != VVS_DPB_NO_SLOT) && ((pState->slots[s].flags & VVS_DPB_SLOT_REFERENCE) != 0U) &&
            (SlotOrder(pState, s) == displayOrderIndex)) {
            buffer = pAV1->nameBuffers[n];
            slot = s;
            break;
        }
    }
    if (slot == VVS_DPB_NO_SLOT) {
        vvsDpbPictureAbort(dpb);
        return VK_ERROR_VALIDATION_FAILED;
    }
    *pFrameToShowMapIdx = (uint8_t)buffer;
    const VvsDpbSlotState* pSlot = &pState->slots[slot];
    if ((pSlot->stdReferenceInfo.av1.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY) ||
        (pSlot->stdReferenceInfo.av1.frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH)) {
        /* A shown key frame refreshes every buffer (7.21). */
        for (uint32_t b = 0; b < STD_VIDEO_AV1_NUM_REF_FRAMES; b++) {
            pAV1->bufferSlots[b] = (uint8_t)slot;
        }
    } else {
        MoveNames(pSequence, pAV1, GetUpdateType(pSlot->codecValue, true), pSlot->codecValue, true);
    }
    MarkReferences(pState);
    vvsDpbResultSetReferences(dpb, NULL, 0, pResult);
    vvsDpbPictureCommit(dpb, VVS_DPB_NO_SLOT, pResult);
    return VK_SUCCESS;
}
