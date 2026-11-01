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
 * The calls common to every codec: the context's lifecycle, the slots, the picture resources, the
 * rewind history, checkpoints and the per-picture results.
 */
#include <stdlib.h>

#include "vvs_dpb_internal.h"

#define VVS_DPB_CHECKPOINT_MAGIC 0x56445043U

/* What a checkpoint holds, in VvsDpbCheckpoint::opaque. */
typedef struct VvsDpbCheckpointData {
    uint32_t magic;
    uint32_t size;
    uint64_t tag;
    uint32_t generation;
    uint32_t reserved;
    /* For each slot: its write count, and in POOL mode its resource's, when the checkpoint was
     * saved. */
    uint32_t slotWriteCounts[VVS_DPB_MAX_SLOTS];
    uint32_t resourceWriteCounts[VVS_DPB_MAX_SLOTS];
    VvsDpbState state;
} VvsDpbCheckpointData;

/* A checkpoint fits in VvsDpbCheckpoint. */
typedef char VvsDpbCheckpointDataFits[(sizeof(VvsDpbCheckpointData) <= VVS_DPB_CHECKPOINT_SIZE) ? 1 : -1];

static void* AllocateContext(size_t size, const VkAllocationCallbacks* pAllocator) {
    if (pAllocator != NULL) {
        return pAllocator->pfnAllocation(pAllocator->pUserData, size, 16, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    }
    return malloc(size);
}

static void FreeContext(void* pMemory, const VkAllocationCallbacks* pAllocator) {
    if (pAllocator != NULL) {
        pAllocator->pfnFree(pAllocator->pUserData, pMemory);
    } else {
        free(pMemory);
    }
}

static bool ValidResources(const VvsDpbResources* pResources) {
    switch (pResources->mode) {
        case VVS_DPB_RESOURCE_MODE_POOL:
            return pResources->resourceCount <= VVS_DPB_MAX_RESOURCES;
        case VVS_DPB_RESOURCE_MODE_CALLBACK:
            return (pResources->callbacks.pfnAcquireResource != NULL) && (pResources->callbacks.pfnRetainResource != NULL) &&
                   (pResources->callbacks.pfnReleaseResource != NULL);
        default:
            return false;
    }
}

/* POOL: every resource free, in index order. */
static void ResetFreeResources(VvsDpb dpb) {
    dpb->state.freeCount = 0;
    if (dpb->resources.mode == VVS_DPB_RESOURCE_MODE_POOL) {
        for (uint32_t i = 0; i < dpb->resources.resourceCount; i++) {
            dpb->state.freeResources[i] = (uint8_t)i;
        }
        dpb->state.freeCount = dpb->resources.resourceCount;
    }
}

/* A state with no picture, waiting for a key picture. */
static void ResetState(VvsDpb dpb) {
    memset(&dpb->state, 0, sizeof(dpb->state));
    dpb->state.lastSetupSlot = VVS_DPB_NO_SLOT;
    dpb->state.keyPictureNeeded = 1;
    if (dpb->operation == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) {
        dpb->state.codec.h264.maxLongTermFrameIdx = -1;
    } else if (dpb->operation == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) {
        VvsDpbAV1State* pAV1 = &dpb->state.codec.av1;
        for (uint32_t i = 0; i < STD_VIDEO_AV1_NUM_REF_FRAMES; i++) {
            pAV1->bufferSlots[i] = VVS_DPB_NO_SLOT;
            pAV1->nameBuffers[i] = (uint8_t)i;
        }
        memset(pAV1->primaryBuffers, VVS_DPB_AV1_NO_BUFFER, sizeof(pAV1->primaryBuffers));
    }
    ResetFreeResources(dpb);
}

static void DropHistory(VvsDpb dpb) {
    dpb->historyCount = 0;
    dpb->historyNext = 0;
}

static bool StdHeaderSupported(VkVideoCodecOperationFlagBitsKHR operation, const VkExtensionProperties* pStdHeaderVersion) {
    if (pStdHeaderVersion == NULL) {
        return true;
    }
    switch (operation) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            return (strncmp(pStdHeaderVersion->extensionName, VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME,
                            VK_MAX_EXTENSION_NAME_SIZE) == 0) &&
                   (pStdHeaderVersion->specVersion <= VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION);
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            return (strncmp(pStdHeaderVersion->extensionName, VK_STD_VULKAN_VIDEO_CODEC_H265_ENCODE_EXTENSION_NAME,
                            VK_MAX_EXTENSION_NAME_SIZE) == 0) &&
                   (pStdHeaderVersion->specVersion <= VK_STD_VULKAN_VIDEO_CODEC_H265_ENCODE_SPEC_VERSION);
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            return (strncmp(pStdHeaderVersion->extensionName, VK_STD_VULKAN_VIDEO_CODEC_AV1_ENCODE_EXTENSION_NAME,
                            VK_MAX_EXTENSION_NAME_SIZE) == 0) &&
                   (pStdHeaderVersion->specVersion <= VK_STD_VULKAN_VIDEO_CODEC_AV1_ENCODE_SPEC_VERSION);
        default:
            return false;
    }
}

VVS_DPB_API VkResult vvsDpbCreate(const VvsDpbCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VvsDpb* pDpb) {
    if ((pCreateInfo == NULL) || (pDpb == NULL) || (pCreateInfo->pVideoProfile == NULL)) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    const uint32_t major = pCreateInfo->apiVersion >> 22U;
    const uint32_t minor = (pCreateInfo->apiVersion >> 12U) & 0x3FFU;
    if ((major != (VVS_DPB_API_VERSION >> 22U)) || (minor > ((VVS_DPB_API_VERSION >> 12U) & 0x3FFU))) {
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    const VkVideoCodecOperationFlagBitsKHR operation = pCreateInfo->pVideoProfile->videoCodecOperation;
    if ((operation != VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) &&
        (operation != VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR) && (operation != VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR)) {
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }
    if (!StdHeaderSupported(operation, pCreateInfo->pStdHeaderVersion)) {
        return VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR;
    }
    if (((pCreateInfo->slotPolicy != VVS_DPB_SLOT_POLICY_LOWEST_FREE) && (pCreateInfo->slotPolicy != VVS_DPB_SLOT_POLICY_ROTATE)) ||
        !ValidResources(&pCreateInfo->resources) || (pCreateInfo->rewindDepth > VVS_DPB_MAX_REWIND_DEPTH)) {
        return VK_ERROR_VALIDATION_FAILED;
    }

    const size_t size = sizeof(struct VvsDpb_T) + (size_t)pCreateInfo->rewindDepth * sizeof(VvsDpbState);
    VvsDpb dpb = (VvsDpb)AllocateContext(size, pAllocator);
    if (dpb == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(dpb, 0, size);
    dpb->tag = (uint64_t)(uintptr_t)dpb ^ ((uint64_t)operation << 32U);
    dpb->operation = operation;
    dpb->flags = pCreateInfo->flags;
    dpb->slotPolicy = pCreateInfo->slotPolicy;
    dpb->resources = pCreateInfo->resources;
    dpb->boundSlots = pCreateInfo->maxDpbSlots;
    dpb->boundActiveReferences = pCreateInfo->maxActiveReferencePictures;
    dpb->rewindDepth = pCreateInfo->rewindDepth;
    ResetState(dpb);
    *pDpb = dpb;
    return VK_SUCCESS;
}

VVS_DPB_API void vvsDpbDestroy(VvsDpb dpb, const VkAllocationCallbacks* pAllocator) {
    if (dpb != VK_NULL_HANDLE) {
        FreeContext(dpb, pAllocator);
    }
}

uint32_t vvsDpbReferenceMask(const VvsDpbState* pState) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        if ((pState->slots[i].flags & VVS_DPB_SLOT_REFERENCE) != 0) {
            mask |= 1U << i;
        }
    }
    return mask;
}

/* Sorts slots by key, ascending or descending; the keys of references are distinct. */
void vvsDpbSortSlots(uint32_t* pSlots, int32_t* pKeys, uint32_t count, bool descending) {
    for (uint32_t i = 1; i < count; i++) {
        const uint32_t slot = pSlots[i];
        const int32_t key = pKeys[i];
        uint32_t j = i;
        while ((j > 0) && (descending ? (pKeys[j - 1U] < key) : (pKeys[j - 1U] > key))) {
            pSlots[j] = pSlots[j - 1U];
            pKeys[j] = pKeys[j - 1U];
            j--;
        }
        pSlots[j] = slot;
        pKeys[j] = key;
    }
}

static bool StateHoldsResource(const VvsDpbState* pState, uint32_t resourceIndex) {
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        if (((pState->slots[i].flags & VVS_DPB_SLOT_REFERENCE) != 0) && (pState->slots[i].resourceIndex == resourceIndex)) {
            return true;
        }
    }
    return false;
}

static void AddResource(VvsDpbResourceList* pList, uint32_t resourceIndex) {
    for (uint32_t i = 0; i < pList->count; i++) {
        if (pList->resourceIndices[i] == resourceIndex) {
            return;
        }
    }
    if (pList->count < VVS_DPB_MAX_SLOTS) {
        pList->resourceIndices[pList->count++] = resourceIndex;
    }
}

/* The resources one state holds and another does not, in slot order. */
static void ResourcesNotIn(const VvsDpbState* pHolding, const VvsDpbState* pOther, VvsDpbResourceList* pList) {
    pList->count = 0;
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        const VvsDpbSlotState* pSlot = &pHolding->slots[i];
        if (((pSlot->flags & VVS_DPB_SLOT_REFERENCE) != 0) && !StateHoldsResource(pOther, pSlot->resourceIndex)) {
            AddResource(pList, pSlot->resourceIndex);
        }
    }
}

static const VvsDpbState* HistoryEntry(VvsDpb dpb, uint32_t age) {
    /* age 1 is the newest entry: the state before the last picture call. */
    return &dpb->history[(dpb->historyNext + dpb->rewindDepth - age) % dpb->rewindDepth];
}

/* POOL: returns the released resources to the free list, after those released before. */
static void FreeResources(VvsDpb dpb, const VvsDpbResourceList* pList) {
    if (dpb->resources.mode != VVS_DPB_RESOURCE_MODE_POOL) {
        return;
    }
    for (uint32_t i = 0; i < pList->count; i++) {
        if (dpb->state.freeCount < VVS_DPB_MAX_RESOURCES) {
            dpb->state.freeResources[dpb->state.freeCount++] = (uint8_t)pList->resourceIndices[i];
        }
    }
}

static void CallRelease(const VvsDpbResources* pResources, const VvsDpbResourceList* pList) {
    if (pResources->mode != VVS_DPB_RESOURCE_MODE_CALLBACK) {
        return;
    }
    for (uint32_t i = 0; i < pList->count; i++) {
        pResources->callbacks.pfnReleaseResource(pResources->callbacks.pUserData, pList->resourceIndices[i]);
    }
}

VVS_DPB_API VkResult vvsDpbReset(VvsDpb dpb, const VvsDpbResetInfo* pResetInfo, VvsDpbResourceList* pReleases) {
    if (dpb == VK_NULL_HANDLE) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    uint32_t boundSlots = dpb->boundSlots;
    uint32_t boundActiveReferences = dpb->boundActiveReferences;
    VvsDpbResources resources = dpb->resources;
    if (pResetInfo != NULL) {
        if (pResetInfo->maxDpbSlots != 0) {
            boundSlots = pResetInfo->maxDpbSlots;
        }
        if (pResetInfo->maxActiveReferencePictures != 0) {
            boundActiveReferences = pResetInfo->maxActiveReferencePictures;
        }
        const VvsDpbResources* pNew = &pResetInfo->resources;
        if (pNew->resourceCount > 0) {
            if ((dpb->resources.mode != VVS_DPB_RESOURCE_MODE_POOL) || (pNew->resourceCount > VVS_DPB_MAX_RESOURCES) ||
                (dpb->sequenceActive && (pNew->resourceCount < dpb->slotCount))) {
                return VK_ERROR_VALIDATION_FAILED;
            }
            resources.resourceCount = pNew->resourceCount;
            resources.pResources = pNew->pResources;
        } else if (pNew->callbacks.pfnAcquireResource != NULL) {
            if ((dpb->resources.mode != VVS_DPB_RESOURCE_MODE_CALLBACK) || !ValidResources(pNew)) {
                return VK_ERROR_VALIDATION_FAILED;
            }
            resources.callbacks = pNew->callbacks;
        }
        if (dpb->sequenceActive &&
            (((boundSlots != 0) && (boundSlots < dpb->requirements.maxDpbSlots)) ||
             ((boundActiveReferences != 0) && (boundActiveReferences < dpb->requirements.maxActiveReferencePictures)))) {
            return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
        }
    }

    VvsDpbResourceList releases;
    releases.count = 0;
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        if ((dpb->state.slots[i].flags & VVS_DPB_SLOT_REFERENCE) != 0) {
            AddResource(&releases, dpb->state.slots[i].resourceIndex);
        }
    }
    const VvsDpbResources releasingResources = dpb->resources;

    dpb->boundSlots = boundSlots;
    dpb->boundActiveReferences = boundActiveReferences;
    dpb->resources = resources;
    ResetState(dpb);
    DropHistory(dpb);
    dpb->generation++;

    if (pReleases != NULL) {
        *pReleases = releases;
    }
    CallRelease(&releasingResources, &releases);
    return VK_SUCCESS;
}

VVS_DPB_API void vvsDpbSaveCheckpoint(VvsDpb dpb, VvsDpbCheckpoint* pCheckpoint) {
    if ((dpb == VK_NULL_HANDLE) || (pCheckpoint == NULL)) {
        return;
    }
    VvsDpbCheckpointData data;
    memset(&data, 0, sizeof(data));
    data.magic = VVS_DPB_CHECKPOINT_MAGIC;
    data.size = (uint32_t)sizeof(data);
    data.tag = dpb->tag;
    data.generation = dpb->generation;
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        data.slotWriteCounts[i] = dpb->slotWriteCounts[i];
        if ((dpb->resources.mode == VVS_DPB_RESOURCE_MODE_POOL) && (dpb->state.slots[i].resourceIndex < VVS_DPB_MAX_RESOURCES)) {
            data.resourceWriteCounts[i] = dpb->resourceWriteCounts[dpb->state.slots[i].resourceIndex];
        }
    }
    data.state = dpb->state;
    memset(pCheckpoint, 0, sizeof(*pCheckpoint));
    memcpy(pCheckpoint->opaque, &data, sizeof(data));
}

/* Whether the references of a checkpoint are where it left them. */
static bool CheckpointIntact(VvsDpb dpb, const VvsDpbCheckpointData* pData) {
    if ((pData->magic != VVS_DPB_CHECKPOINT_MAGIC) || (pData->size != sizeof(*pData)) || (pData->tag != dpb->tag) ||
        (pData->generation != dpb->generation)) {
        return false;
    }
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        const VvsDpbSlotState* pSlot = &pData->state.slots[i];
        if ((pSlot->flags & VVS_DPB_SLOT_REFERENCE) == 0) {
            continue;
        }
        if (dpb->slotWriteCounts[i] != pData->slotWriteCounts[i]) {
            return false;
        }
        if ((dpb->resources.mode == VVS_DPB_RESOURCE_MODE_POOL) &&
            ((pSlot->resourceIndex >= VVS_DPB_MAX_RESOURCES) ||
             (dpb->resourceWriteCounts[pSlot->resourceIndex] != pData->resourceWriteCounts[i]))) {
            return false;
        }
    }
    return true;
}

VVS_DPB_API VkResult vvsDpbRewind(VvsDpb dpb, const VvsDpbRewindInfo* pRewindInfo, VvsDpbResourceList* pReleases) {
    if ((dpb == VK_NULL_HANDLE) || (pRewindInfo == NULL) ||
        ((pRewindInfo->pictureCount != 0) == (pRewindInfo->pCheckpoint != NULL))) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    VvsDpbCheckpointData checkpoint;
    const VvsDpbState* pTarget = NULL;
    if (pRewindInfo->pictureCount != 0) {
        if (pRewindInfo->pictureCount > dpb->historyCount) {
            return VK_ERROR_VALIDATION_FAILED;
        }
        pTarget = HistoryEntry(dpb, pRewindInfo->pictureCount);
    } else {
        memcpy(&checkpoint, pRewindInfo->pCheckpoint->opaque, sizeof(checkpoint));
        if (!CheckpointIntact(dpb, &checkpoint)) {
            return VK_ERROR_VALIDATION_FAILED;
        }
        pTarget = &checkpoint.state;
    }

    VvsDpbResourceList releases;
    VvsDpbResourceList retains;
    ResourcesNotIn(&dpb->state, pTarget, &releases);
    ResourcesNotIn(pTarget, &dpb->state, &retains);

    dpb->state = *pTarget;
    if (pRewindInfo->pictureCount != 0) {
        dpb->historyNext = (dpb->historyNext + dpb->rewindDepth - pRewindInfo->pictureCount) % dpb->rewindDepth;
        dpb->historyCount -= pRewindInfo->pictureCount;
    } else {
        DropHistory(dpb);
        dpb->state.pictureCount = 0;
    }

    if (pReleases != NULL) {
        *pReleases = releases;
    }
    if (dpb->resources.mode == VVS_DPB_RESOURCE_MODE_CALLBACK) {
        for (uint32_t i = 0; i < retains.count; i++) {
            dpb->resources.callbacks.pfnRetainResource(dpb->resources.callbacks.pUserData, retains.resourceIndices[i]);
        }
    }
    CallRelease(&dpb->resources, &releases);
    return VK_SUCCESS;
}

VVS_DPB_API VkResult vvsDpbGetReferenceFrames(VvsDpb dpb, uint32_t* pReferenceFrameCount, VvsDpbReferenceFrame* pReferenceFrames) {
    if ((dpb == VK_NULL_HANDLE) || (pReferenceFrameCount == NULL)) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    uint32_t count = 0;
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
        const VvsDpbSlotState* pSlot = &dpb->state.slots[i];
        if ((pSlot->flags & VVS_DPB_SLOT_REFERENCE) == 0) {
            continue;
        }
        if (pReferenceFrames != NULL) {
            if (count == *pReferenceFrameCount) {
                result = VK_INCOMPLETE;
                break;
            }
            VvsDpbReferenceFrame* pFrame = &pReferenceFrames[count];
            memset(pFrame, 0, sizeof(*pFrame));
            pFrame->slotIndex = (int32_t)i;
            pFrame->resourceIndex = pSlot->resourceIndex;
            pFrame->pictureInfo = pSlot->pictureInfo;
            pFrame->stdReferenceInfo = pSlot->stdReferenceInfo;
            if (dpb->operation == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) {
                for (uint32_t b = 0; b < STD_VIDEO_AV1_NUM_REF_FRAMES; b++) {
                    if (dpb->state.codec.av1.bufferSlots[b] == i) {
                        pFrame->av1BufferMask |= 1U << b;
                    }
                }
            }
        }
        count++;
    }
    *pReferenceFrameCount = count;
    return result;
}

/* ------------------------------------------------------------------------------------------------
 * The steps of a picture call
 * ---------------------------------------------------------------------------------------------- */

VkResult vvsDpbPictureBegin(VvsDpb dpb, VkVideoCodecOperationFlagBitsKHR operation, uint32_t dirtyIntraRefreshRegions) {
    if ((dpb == VK_NULL_HANDLE) || (dpb->operation != operation) || !dpb->sequenceActive) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    if ((dpb->resources.mode == VVS_DPB_RESOURCE_MODE_POOL) && (dpb->resources.resourceCount < dpb->slotCount)) {
        return VK_ERROR_VALIDATION_FAILED;
    }
    dpb->intraRefreshChain =
        ((dpb->flags & VVS_DPB_CREATE_INTRA_REFRESH_BIT) != 0) && (dirtyIntraRefreshRegions != VVS_DPB_NO_INTRA_REFRESH);
    dpb->saved = dpb->state;
    return VK_SUCCESS;
}

void vvsDpbPictureAbort(VvsDpb dpb) { dpb->state = dpb->saved; }

/* The history a rewind can return to once the picture call in progress commits: the state before
 * the call, and the newest rewindDepth - 1 states of the history. */
static uint32_t KeptHistoryCount(VvsDpb dpb) {
    return (dpb->rewindDepth > 0U) ? vvsDpbMin(dpb->historyCount, dpb->rewindDepth - 1U) : 0U;
}

uint32_t vvsDpbChooseSetupSlot(VvsDpb dpb, uint32_t excludedMask) {
    uint32_t busy = excludedMask | vvsDpbReferenceMask(&dpb->state);
    if (dpb->rewindDepth > 0U) {
        busy |= vvsDpbReferenceMask(&dpb->saved);
    }
    for (uint32_t age = 1; age <= KeptHistoryCount(dpb); age++) {
        busy |= vvsDpbReferenceMask(HistoryEntry(dpb, age));
    }
    const uint32_t count = dpb->slotCount;
    if (count == 0) {
        return VVS_DPB_NO_SLOT;
    }
    uint32_t first = 0;
    if ((dpb->slotPolicy == VVS_DPB_SLOT_POLICY_ROTATE) && (dpb->state.lastSetupSlot != VVS_DPB_NO_SLOT)) {
        first = (dpb->state.lastSetupSlot + 1U) % count;
    }
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t slot = (first + k) % count;
        if ((busy & (1U << slot)) == 0) {
            return slot;
        }
    }
    return VVS_DPB_NO_SLOT;
}

/* POOL: the resources the rewind history may bring back once the picture call in progress commits,
 * bit i for resource i. The resources the call itself releases are not on the free list yet. */
static uint64_t HistoryResources(VvsDpb dpb) {
    uint64_t mask = 0;
    for (uint32_t age = 1; age <= KeptHistoryCount(dpb); age++) {
        const VvsDpbState* pState = HistoryEntry(dpb, age);
        for (uint32_t i = 0; i < VVS_DPB_MAX_SLOTS; i++) {
            if (((pState->slots[i].flags & VVS_DPB_SLOT_REFERENCE) != 0) &&
                (pState->slots[i].resourceIndex < VVS_DPB_MAX_RESOURCES)) {
                mask |= (uint64_t)1U << pState->slots[i].resourceIndex;
            }
        }
    }
    return mask;
}

VkResult vvsDpbAcquireResource(VvsDpb dpb, uint32_t slotIndex, const VvsDpbPictureInfo* pPictureInfo) {
    VvsDpbSlotState* pSlot = &dpb->state.slots[slotIndex];
    if (dpb->resources.mode == VVS_DPB_RESOURCE_MODE_POOL) {
        VvsDpbState* pState = &dpb->state;
        uint32_t position = pState->freeCount;
        if ((dpb->resources.resourceCount == dpb->slotCount) && !StateHoldsResource(pState, slotIndex)) {
            /* Resource i is slot i's: free, or released by this call's own marking, which puts it on
             * the free list only when the call commits. */
            for (uint32_t i = 0; i < pState->freeCount; i++) {
                if (pState->freeResources[i] == slotIndex) {
                    position = i;
                    break;
                }
            }
            if ((position == pState->freeCount) && StateHoldsResource(&dpb->saved, slotIndex)) {
                pSlot->resourceIndex = slotIndex;
                pSlot->pResource = (dpb->resources.pResources != NULL) ? &dpb->resources.pResources[slotIndex] : NULL;
                return VK_SUCCESS;
            }
        }
        if (position == pState->freeCount) {
            const uint64_t kept = HistoryResources(dpb);
            for (uint32_t i = 0; i < pState->freeCount; i++) {
                if ((kept & ((uint64_t)1U << pState->freeResources[i])) == 0) {
                    position = i;
                    break;
                }
            }
        }
        if (position == pState->freeCount) {
            return VK_ERROR_OUT_OF_POOL_MEMORY;
        }
        const uint32_t resourceIndex = pState->freeResources[position];
        memmove(&pState->freeResources[position], &pState->freeResources[position + 1U], pState->freeCount - position - 1U);
        pState->freeCount--;
        pSlot->resourceIndex = resourceIndex;
        pSlot->pResource = (dpb->resources.pResources != NULL) ? &dpb->resources.pResources[resourceIndex] : NULL;
        return VK_SUCCESS;
    }

    VvsDpbPictureInfo noPictureInfo;
    if (pPictureInfo == NULL) {
        memset(&noPictureInfo, 0, sizeof(noPictureInfo));
        pPictureInfo = &noPictureInfo;
    }
    const VvsDpbResourceCallbacks* pCallbacks = &dpb->resources.callbacks;
    uint32_t resourceIndex = VVS_DPB_NO_RESOURCE;
    const VkVideoPictureResourceInfoKHR* pResource = NULL;
    const VkResult result = pCallbacks->pfnAcquireResource(pCallbacks->pUserData, pPictureInfo, &resourceIndex, &pResource);
    if (result < VK_SUCCESS) {
        return result;
    }
    if ((resourceIndex == VVS_DPB_NO_RESOURCE) || StateHoldsResource(&dpb->state, resourceIndex)) {
        if (resourceIndex != VVS_DPB_NO_RESOURCE) {
            pCallbacks->pfnReleaseResource(pCallbacks->pUserData, resourceIndex);
        }
        return VK_ERROR_VALIDATION_FAILED;
    }
    pSlot->resourceIndex = resourceIndex;
    pSlot->pResource = pResource;
    return VK_SUCCESS;
}

static void WriteEntry(VvsDpb dpb, const VvsDpbSlotState* pSlot, int32_t slotIndex, uint32_t entry, VvsDpbPictureResult* pResult) {
    VvsDpbPictureSlots* pSlots = &pResult->slots;
    VvsDpbCodecDpbSlotInfo* pDpbSlotInfo = &pSlots->dpbSlotInfos[entry];
    VkVideoReferenceSlotInfoKHR* pReferenceSlot = &pSlots->referenceSlots[entry];
    pReferenceSlot->sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    pReferenceSlot->pNext = pDpbSlotInfo;
    pReferenceSlot->slotIndex = slotIndex;
    pReferenceSlot->pPictureResource = pSlot->pResource;
    pSlots->resourceIndices[entry] = pSlot->resourceIndex;
    pSlots->stdReferenceInfos[entry] = pSlot->stdReferenceInfo;

    /* The references of a picture encoded with intra refresh, not its setup picture. */
    const void* pNext = NULL;
    if (dpb->intraRefreshChain && (slotIndex >= 0)) {
        VkVideoReferenceIntraRefreshInfoKHR* pIntraRefresh = &pSlots->intraRefreshInfos[entry];
        pIntraRefresh->sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_INTRA_REFRESH_INFO_KHR;
        pIntraRefresh->pNext = NULL;
        pIntraRefresh->dirtyIntraRefreshRegions = pSlot->dirtyIntraRefreshRegions;
        pNext = pIntraRefresh;
    }
    switch (dpb->operation) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            pDpbSlotInfo->h264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
            pDpbSlotInfo->h264.pNext = pNext;
            pDpbSlotInfo->h264.pStdReferenceInfo = &pSlots->stdReferenceInfos[entry].h264;
            break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            pDpbSlotInfo->h265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR;
            pDpbSlotInfo->h265.pNext = pNext;
            pDpbSlotInfo->h265.pStdReferenceInfo = &pSlots->stdReferenceInfos[entry].h265;
            break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            pDpbSlotInfo->av1.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR;
            pDpbSlotInfo->av1.pNext = pNext;
            pDpbSlotInfo->av1.pStdReferenceInfo = &pSlots->stdReferenceInfos[entry].av1;
            break;
        default:
            break;
    }
    if ((pSlot->flags & VVS_DPB_SLOT_PICTURE_INFO) != 0) {
        pSlots->pictureInfos[entry] = pSlot->pictureInfo;
    }
}

void vvsDpbResultSetReferences(VvsDpb dpb, const uint32_t* pSlotIndices, uint32_t count, VvsDpbPictureResult* pResult) {
    VvsDpbPictureSlots* pSlots = &pResult->slots;
    uint32_t written = 0;
    for (uint32_t i = 0; (i < count) && (written < VVS_DPB_MAX_ACTIVE_REFERENCES); i++) {
        const int32_t slotIndex = (int32_t)pSlotIndices[i];
        bool bound = false;
        for (uint32_t j = 0; j < written; j++) {
            if (pSlots->referenceSlots[j].slotIndex == slotIndex) {
                bound = true;
                break;
            }
        }
        if (!bound) {
            WriteEntry(dpb, &dpb->saved.slots[pSlotIndices[i]], slotIndex, written, pResult);
            written++;
        }
    }
    pSlots->referenceSlotCount = written;
    pSlots->beginCodingSlotCount = written;
    pSlots->pSetupReferenceSlot = NULL;
    pResult->releases.count = 0;
}

void vvsDpbPictureCommit(VvsDpb dpb, uint32_t setupSlot, VvsDpbPictureResult* pResult) {
    VvsDpbPictureSlots* pSlots = &pResult->slots;
    if (setupSlot != VVS_DPB_NO_SLOT) {
        const VvsDpbSlotState* pSlot = &dpb->state.slots[setupSlot];
        const uint32_t entry = pSlots->referenceSlotCount;
        WriteEntry(dpb, pSlot, -1, entry, pResult);
        pSlots->setupReferenceSlot = pSlots->referenceSlots[entry];
        pSlots->setupReferenceSlot.slotIndex = (int32_t)setupSlot;
        pSlots->pSetupReferenceSlot = &pSlots->setupReferenceSlot;
        pSlots->beginCodingSlotCount = entry + 1U;
        dpb->slotWriteCounts[setupSlot]++;
        if ((dpb->resources.mode == VVS_DPB_RESOURCE_MODE_POOL) && (pSlot->resourceIndex < VVS_DPB_MAX_RESOURCES)) {
            dpb->resourceWriteCounts[pSlot->resourceIndex]++;
        }
        dpb->state.lastSetupSlot = setupSlot;
    } else {
        pSlots->pSetupReferenceSlot = NULL;
        pSlots->beginCodingSlotCount = pSlots->referenceSlotCount;
    }

    /* The resources the DPB held before the call, and the setup picture's, that it does not hold
     * now. */
    VvsDpbResourceList* pReleases = &pResult->releases;
    ResourcesNotIn(&dpb->saved, &dpb->state, pReleases);
    if ((setupSlot != VVS_DPB_NO_SLOT) && !StateHoldsResource(&dpb->state, dpb->state.slots[setupSlot].resourceIndex)) {
        AddResource(pReleases, dpb->state.slots[setupSlot].resourceIndex);
    }
    FreeResources(dpb, pReleases);

    if (dpb->rewindDepth > 0) {
        dpb->history[dpb->historyNext] = dpb->saved;
        dpb->historyNext = (dpb->historyNext + 1U) % dpb->rewindDepth;
        if (dpb->historyCount < dpb->rewindDepth) {
            dpb->historyCount++;
        }
    }
    dpb->state.pictureCount++;
    CallRelease(&dpb->resources, pReleases);
}

VkResult vvsDpbSequenceCommit(VvsDpb dpb, const VvsDpbSequenceRequirements* pRequirements, const VvsDpbGopInfo* pGopInfo) {
    dpb->requirements = *pRequirements;
    dpb->slotCount = pRequirements->maxDpbSlots;
    if (pGopInfo != NULL) {
        dpb->gopInfo = *pGopInfo;
    } else {
        memset(&dpb->gopInfo, 0, sizeof(dpb->gopInfo));
    }
    dpb->sequenceActive = 1;
    DropHistory(dpb);
    dpb->generation++;
    dpb->state.pictureCount = 0;
    return VK_SUCCESS;
}

bool vvsDpbGopHasPatterns(const VvsDpbGopInfo* pGopInfo) {
    return (pGopInfo != NULL) && (pGopInfo->gopStructure != VVS_DPB_GOP_STRUCTURE_UNSPECIFIED) &&
           ((pGopInfo->predictiveReferencePattern != VVS_DPB_REFERENCE_PATTERN_UNSPECIFIED) ||
            (pGopInfo->bipredictiveReferencePattern != VVS_DPB_REFERENCE_PATTERN_UNSPECIFIED) ||
            (pGopInfo->temporalLayerPattern != VVS_DPB_TEMPORAL_LAYER_PATTERN_UNSPECIFIED) || (pGopInfo->temporalLayerCount > 1U));
}
