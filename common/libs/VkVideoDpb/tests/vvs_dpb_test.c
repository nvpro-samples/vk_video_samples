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
 * vkvideo-dpb-test: the library through its C interface, with no device. The lifecycle and its
 * errors, both resource modes, rewind and checkpoints, that a failed call changes nothing, that the
 * same inputs give the same results, the H.264 planner's slots, lists, frame_num and order counts,
 * the H.265 planner's slots, RPS, SPS sets and lists, and the AV1 planner's names, buffers, refreshes,
 * primary reference frames and prediction modes, on short sequences worked out by hand. Exit status 0
 * when every check passes.
 */
#include <stdio.h>
#include <string.h>

#include "vkvideo_dpb/vvs_dpb.h"

static int g_failures = 0;

static uint32_t MinU32(uint32_t a, uint32_t b) { return (a < b) ? a : b; }

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            g_failures++;                                                                 \
        }                                                                                 \
    } while (0)

/* ------------------------------------------------------------------------------------------------
 * A client: an H.264 stream, its pictures and its resources
 * ---------------------------------------------------------------------------------------------- */

typedef struct Stream {
    VkVideoEncodeH264ProfileInfoKHR h264Profile;
    VkVideoProfileInfoKHR profile;
    StdVideoH264SequenceParameterSet sps;
    StdVideoH264PictureParameterSet pps;
    VkVideoPictureResourceInfoKHR pool[VVS_DPB_MAX_RESOURCES];
} Stream;

typedef struct Picture {
    StdVideoEncodeH264PictureInfo std;
    VvsDpbH264EncodeSyntax syntax;
    VvsDpbPictureResult result;
} Picture;

/* The client's own resources for CALLBACK mode, handed out from a free list, released ones last. */
typedef struct Callbacks {
    VkVideoPictureResourceInfoKHR resources[32];
    uint32_t free[32];
    uint32_t freeCount;
    uint32_t held[32];
    uint32_t acquires;
    uint32_t releases;
    uint32_t retains;
    uint32_t failNextAcquire;
} Callbacks;

static VkResult VKAPI_CALL Acquire(void* pUserData, const VvsDpbPictureInfo* pPictureInfo, uint32_t* pResourceIndex,
                                   const VkVideoPictureResourceInfoKHR** ppResource) {
    Callbacks* pCallbacks = (Callbacks*)pUserData;
    (void)pPictureInfo;
    if (pCallbacks->failNextAcquire) {
        pCallbacks->failNextAcquire = 0;
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    if (pCallbacks->freeCount == 0) {
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    const uint32_t index = pCallbacks->free[0];
    pCallbacks->freeCount--;
    memmove(&pCallbacks->free[0], &pCallbacks->free[1], pCallbacks->freeCount * sizeof(pCallbacks->free[0]));
    pCallbacks->held[index]++;
    pCallbacks->acquires++;
    *pResourceIndex = index;
    *ppResource = &pCallbacks->resources[index];
    return VK_SUCCESS;
}

static void VKAPI_CALL Retain(void* pUserData, uint32_t resourceIndex) {
    Callbacks* pCallbacks = (Callbacks*)pUserData;
    for (uint32_t i = 0; i < pCallbacks->freeCount; i++) {
        if (pCallbacks->free[i] == resourceIndex) {
            pCallbacks->freeCount--;
            memmove(&pCallbacks->free[i], &pCallbacks->free[i + 1], (pCallbacks->freeCount - i) * sizeof(pCallbacks->free[0]));
            break;
        }
    }
    pCallbacks->held[resourceIndex]++;
    pCallbacks->retains++;
}

static void VKAPI_CALL Release(void* pUserData, uint32_t resourceIndex) {
    Callbacks* pCallbacks = (Callbacks*)pUserData;
    CHECK(pCallbacks->held[resourceIndex] == 1);
    pCallbacks->held[resourceIndex]--;
    pCallbacks->free[pCallbacks->freeCount++] = resourceIndex;
    pCallbacks->releases++;
}

static void InitCallbacks(Callbacks* pCallbacks) {
    memset(pCallbacks, 0, sizeof(*pCallbacks));
    for (uint32_t i = 0; i < 32; i++) {
        pCallbacks->resources[i].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        pCallbacks->resources[i].baseArrayLayer = i;
        pCallbacks->free[i] = i;
    }
    pCallbacks->freeCount = 32;
}

static void InitStream(Stream* pStream, uint8_t maxNumRefFrames, StdVideoH264PocType pocType, uint8_t list0, uint8_t list1) {
    memset(pStream, 0, sizeof(*pStream));
    pStream->h264Profile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
    pStream->h264Profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
    pStream->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    pStream->profile.pNext = &pStream->h264Profile;
    pStream->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    pStream->profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    pStream->profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    pStream->profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    pStream->sps.flags.frame_mbs_only_flag = 1;
    pStream->sps.max_num_ref_frames = maxNumRefFrames;
    pStream->sps.pic_order_cnt_type = pocType;
    pStream->sps.log2_max_frame_num_minus4 = 0;         /* MaxFrameNum 16: wraps in the tests */
    pStream->sps.log2_max_pic_order_cnt_lsb_minus4 = 0; /* MaxPicOrderCntLsb 16 */
    pStream->pps.num_ref_idx_l0_default_active_minus1 = (uint8_t)(list0 - 1U);
    pStream->pps.num_ref_idx_l1_default_active_minus1 = (uint8_t)(list1 - 1U);
    for (uint32_t i = 0; i < VVS_DPB_MAX_RESOURCES; i++) {
        pStream->pool[i].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        pStream->pool[i].baseArrayLayer = i;
    }
}

static VvsDpbCreateInfo PoolCreateInfo(const Stream* pStream, uint32_t resourceCount) {
    VvsDpbCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.apiVersion = VVS_DPB_API_VERSION;
    createInfo.flags = VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT;
    createInfo.pVideoProfile = &pStream->profile;
    createInfo.slotPolicy = VVS_DPB_SLOT_POLICY_LOWEST_FREE;
    createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_POOL;
    createInfo.resources.resourceCount = resourceCount;
    createInfo.resources.pResources = pStream->pool;
    return createInfo;
}

static VvsDpbCreateInfo CallbackCreateInfo(const Stream* pStream, Callbacks* pCallbacks) {
    VvsDpbCreateInfo createInfo = PoolCreateInfo(pStream, 0);
    createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_CALLBACK;
    createInfo.resources.pResources = NULL;
    createInfo.resources.callbacks.pUserData = pCallbacks;
    createInfo.resources.callbacks.pfnAcquireResource = Acquire;
    createInfo.resources.callbacks.pfnRetainResource = Retain;
    createInfo.resources.callbacks.pfnReleaseResource = Release;
    return createInfo;
}

static VkResult Sequence(VvsDpb dpb, const Stream* pStream, VvsDpbSequenceRequirements* pRequirements) {
    VvsDpbH264SequenceInfo sequenceInfo;
    memset(&sequenceInfo, 0, sizeof(sequenceInfo));
    sequenceInfo.pStdSps = &pStream->sps;
    sequenceInfo.pStdPps = &pStream->pps;
    return vvsDpbH264Sequence(dpb, &sequenceInfo, pRequirements);
}

/* Encodes one picture: type is the primary picture type; an IDR picture is a reference. */
static VkResult Encode(VvsDpb dpb, Picture* pPicture, StdVideoH264PictureType type, int reference, int32_t picOrderCnt) {
    memset(pPicture, 0, sizeof(*pPicture));
    pPicture->std.flags.IdrPicFlag = (type == STD_VIDEO_H264_PICTURE_TYPE_IDR) ? 1U : 0U;
    pPicture->std.flags.is_reference = reference ? 1U : 0U;
    pPicture->std.primary_pic_type = type;
    pPicture->std.PicOrderCnt = picOrderCnt;
    VvsDpbH264EncodePictureInfo pictureInfo;
    memset(&pictureInfo, 0, sizeof(pictureInfo));
    pictureInfo.pStdPictureInfo = &pPicture->std;
    pictureInfo.pSyntax = &pPicture->syntax;
    return vvsDpbEncodeH264Picture(dpb, &pictureInfo, &pPicture->result);
}

static int32_t SetupSlot(const Picture* pPicture) {
    return (pPicture->result.slots.pSetupReferenceSlot != NULL) ? pPicture->result.slots.pSetupReferenceSlot->slotIndex : -1;
}

/* The checks every result passes: the chains, the begin-coding entry and the Std pointers. */
static void CheckResultShape(const Picture* pPicture) {
    const VvsDpbPictureSlots* pSlots = &pPicture->result.slots;
    for (uint32_t i = 0; i < pSlots->beginCodingSlotCount; i++) {
        CHECK(pSlots->referenceSlots[i].sType == VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR);
        CHECK(pSlots->referenceSlots[i].pNext == &pSlots->dpbSlotInfos[i]);
        CHECK(pSlots->dpbSlotInfos[i].h264.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR);
        CHECK(pSlots->dpbSlotInfos[i].h264.pStdReferenceInfo == &pSlots->stdReferenceInfos[i].h264);
        for (uint32_t j = 0; j < i; j++) {
            CHECK(pSlots->referenceSlots[j].slotIndex != pSlots->referenceSlots[i].slotIndex);
        }
    }
    if (pSlots->pSetupReferenceSlot != NULL) {
        CHECK(pSlots->beginCodingSlotCount == pSlots->referenceSlotCount + 1U);
        CHECK(pSlots->referenceSlots[pSlots->referenceSlotCount].slotIndex == -1);
        CHECK(pSlots->pSetupReferenceSlot->pPictureResource == pSlots->referenceSlots[pSlots->referenceSlotCount].pPictureResource);
        for (uint32_t i = 0; i < pSlots->referenceSlotCount; i++) {
            CHECK(pSlots->referenceSlots[i].slotIndex != pSlots->pSetupReferenceSlot->slotIndex);
            CHECK(pSlots->referenceSlots[i].pPictureResource != pSlots->pSetupReferenceSlot->pPictureResource);
        }
    }
    CHECK(pPicture->std.pRefLists == &pPicture->syntax.refLists);
}

/* ------------------------------------------------------------------------------------------------
 * The tests
 * ---------------------------------------------------------------------------------------------- */

static void TestCreate(void) {
    Stream stream;
    InitStream(&stream, 1, STD_VIDEO_H264_POC_TYPE_2, 1, 1);
    VvsDpb dpb = VK_NULL_HANDLE;
    VvsDpbCreateInfo createInfo = PoolCreateInfo(&stream, 2);

    stream.profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR);
    stream.profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;

    createInfo.apiVersion = VVS_DPB_MAKE_API_VERSION(1, 0, 0);
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_ERROR_INCOMPATIBLE_DRIVER);
    createInfo.apiVersion = VVS_DPB_API_VERSION;

    VkExtensionProperties stdHeader;
    memset(&stdHeader, 0, sizeof(stdHeader));
    snprintf(stdHeader.extensionName, sizeof(stdHeader.extensionName), "%s", VK_STD_VULKAN_VIDEO_CODEC_H265_ENCODE_EXTENSION_NAME);
    stdHeader.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION;
    createInfo.pStdHeaderVersion = &stdHeader;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR);
    snprintf(stdHeader.extensionName, sizeof(stdHeader.extensionName), "%s", VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME);
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    vvsDpbDestroy(dpb, NULL);
    createInfo.pStdHeaderVersion = NULL;

    createInfo.rewindDepth = VVS_DPB_MAX_REWIND_DEPTH + 1U;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_ERROR_VALIDATION_FAILED);
    createInfo.rewindDepth = 0;

    createInfo.resources.resourceCount = VVS_DPB_MAX_RESOURCES + 1U;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_ERROR_VALIDATION_FAILED);
    createInfo.resources.resourceCount = 2;

    Callbacks callbacks;
    InitCallbacks(&callbacks);
    VvsDpbCreateInfo callbackInfo = CallbackCreateInfo(&stream, &callbacks);
    callbackInfo.resources.callbacks.pfnRetainResource = NULL;
    CHECK(vvsDpbCreate(&callbackInfo, NULL, &dpb) == VK_ERROR_VALIDATION_FAILED);

    /* No picture before a sequence, and the first picture is an IDR picture. */
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    Picture picture;
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_IDR, 1, 0) == VK_ERROR_VALIDATION_FAILED);
    VvsDpbSequenceRequirements requirements;
    CHECK(Sequence(dpb, &stream, &requirements) == VK_SUCCESS);
    CHECK(requirements.maxDpbSlots == 2);
    CHECK(requirements.maxReferencePictures == 1);
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_P, 1, 2) == VK_ERROR_VALIDATION_FAILED);
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_IDR, 1, 0) == VK_SUCCESS);
    vvsDpbDestroy(dpb, NULL);
    vvsDpbDestroy(VK_NULL_HANDLE, NULL);

    /* A sequence beyond the bound, and POC type 1. */
    createInfo.maxDpbSlots = 2;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    stream.sps.max_num_ref_frames = 2;
    CHECK(Sequence(dpb, &stream, &requirements) == VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR);
    stream.sps.max_num_ref_frames = 1;
    stream.sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_1;
    CHECK(Sequence(dpb, &stream, &requirements) == VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR);
    stream.sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2;
    VvsDpbGopInfo gop;
    memset(&gop, 0, sizeof(gop));
    gop.gopStructure = VVS_DPB_GOP_STRUCTURE_REGULAR;
    gop.predictiveReferencePattern = VVS_DPB_REFERENCE_PATTERN_SEQUENTIAL;
    VvsDpbH264SequenceInfo sequenceInfo = {&stream.sps, &stream.pps, &gop};
    CHECK(vvsDpbH264Sequence(dpb, &sequenceInfo, &requirements) == VK_ERROR_FEATURE_NOT_PRESENT);
    gop.predictiveReferencePattern = VVS_DPB_REFERENCE_PATTERN_UNSPECIFIED;
    CHECK(vvsDpbH264Sequence(dpb, &sequenceInfo, &requirements) == VK_SUCCESS);
    vvsDpbDestroy(dpb, NULL);

    CHECK(sizeof(VvsDpbCheckpoint) == VVS_DPB_CHECKPOINT_SIZE);
}

/* IPPP with one reference and POC type 2: the slots alternate, list 0 is the previous picture,
 * frame_num counts the reference pictures and wraps at 16, and PicOrderCnt is 2 * (FrameNumOffset +
 * frame_num). */
static void TestIppp(void) {
    Stream stream;
    InitStream(&stream, 1, STD_VIDEO_H264_POC_TYPE_2, 1, 1);
    VvsDpb dpb = VK_NULL_HANDLE;
    VvsDpbCreateInfo createInfo = PoolCreateInfo(&stream, 2);
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    CHECK(Sequence(dpb, &stream, NULL) == VK_SUCCESS);

    Picture picture;
    int32_t previousSlot = -1;
    for (uint32_t i = 0; i < 40; i++) {
        const StdVideoH264PictureType type = (i == 0) ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        CHECK(Encode(dpb, &picture, type, 1, (int32_t)(2U * i)) == VK_SUCCESS);
        CheckResultShape(&picture);
        CHECK(picture.std.frame_num == (i % 16U));
        CHECK(picture.std.PicOrderCnt == (int32_t)(2U * i));
        CHECK(SetupSlot(&picture) == (int32_t)(i % 2U));
        /* Resource i is slot i's. */
        CHECK(picture.result.slots.pSetupReferenceSlot->pPictureResource == &stream.pool[i % 2U]);
        if (i == 0) {
            CHECK(picture.result.slots.referenceSlotCount == 0);
            CHECK(picture.result.releases.count == 0);
        } else {
            CHECK(picture.result.slots.referenceSlotCount == 1);
            CHECK(picture.result.slots.referenceSlots[0].slotIndex == previousSlot);
            CHECK(picture.syntax.refLists.RefPicList0[0] == (uint8_t)previousSlot);
            CHECK(picture.syntax.refLists.RefPicList0[1] == STD_VIDEO_H264_NO_REFERENCE_PICTURE);
            CHECK(picture.syntax.refLists.num_ref_idx_l0_active_minus1 == 0);
            CHECK(picture.result.slots.stdReferenceInfos[0].h264.FrameNum == ((i - 1U) % 16U));
            CHECK(picture.result.slots.stdReferenceInfos[0].h264.PicOrderCnt == (int32_t)(2U * (i - 1U)));
            /* The previous picture leaves the DPB with this one. */
            CHECK(picture.result.releases.count == 1);
            CHECK(picture.result.releases.resourceIndices[0] == (uint32_t)previousSlot);
        }
        previousSlot = SetupSlot(&picture);
    }
    vvsDpbDestroy(dpb, NULL);
}

/* I B B P with two references, the B pictures not references, POC type 0 with MaxPicOrderCntLsb 16,
 * past the wrap: the B pictures reference the I or P picture before and the P picture after, take the
 * lowest free slot and give it back at once. */
static void TestBFrames(void) {
    Stream stream;
    InitStream(&stream, 2, STD_VIDEO_H264_POC_TYPE_0, 1, 1);
    VvsDpb dpb = VK_NULL_HANDLE;
    VvsDpbCreateInfo createInfo = PoolCreateInfo(&stream, 3);
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    VvsDpbSequenceRequirements requirements;
    CHECK(Sequence(dpb, &stream, &requirements) == VK_SUCCESS);
    CHECK(requirements.maxDpbSlots == 3);
    CHECK(requirements.maxActiveReferencePictures == 2);

    Picture picture;
    int32_t anchorSlot[64];
    /* The anchors (I or P) are display 0, 3, 6, ...; each is encoded before the two B pictures
     * displayed before it. The POC is 2 * display, low 4 bits only, so it wraps every 8 pictures. */
    for (uint32_t anchor = 0; anchor <= 30; anchor += 3) {
        const StdVideoH264PictureType type = (anchor == 0) ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        CHECK(Encode(dpb, &picture, type, 1, (int32_t)((2U * anchor) & 15U)) == VK_SUCCESS);
        CheckResultShape(&picture);
        CHECK(picture.std.PicOrderCnt == (int32_t)(2U * anchor));
        anchorSlot[anchor] = SetupSlot(&picture);
        if (anchor == 0) {
            continue;
        }
        /* P: list 0 is the previous anchor (one entry: the PPS's default count). */
        CHECK(picture.result.slots.referenceSlotCount == 1);
        CHECK(picture.syntax.refLists.RefPicList0[0] == (uint8_t)anchorSlot[anchor - 3U]);
        for (uint32_t b = anchor - 2U; b < anchor; b++) {
            CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_B, 0, (int32_t)((2U * b) & 15U)) == VK_SUCCESS);
            CheckResultShape(&picture);
            CHECK(picture.std.PicOrderCnt == (int32_t)(2U * b));
            CHECK(picture.syntax.refLists.RefPicList0[0] == (uint8_t)anchorSlot[anchor - 3U]);
            CHECK(picture.syntax.refLists.RefPicList1[0] == (uint8_t)anchorSlot[anchor]);
            CHECK(picture.result.slots.referenceSlotCount == 2);
            /* The free slot, given back at once. */
            CHECK(SetupSlot(&picture) == 3 - anchorSlot[anchor] - anchorSlot[anchor - 3U]);
            CHECK(picture.result.releases.count == 1);
            CHECK(picture.result.slots.stdReferenceInfos[picture.result.slots.referenceSlotCount].h264.FrameNum ==
                  picture.std.frame_num);
        }
    }
    vvsDpbDestroy(dpb, NULL);
}

/* Four references, a P picture's list 0 the PPS's three by descending FrameNumWrap across the
 * frame_num wrap; a B picture with a two-entry list 0 default takes one entry in each list. */
static void TestListsAndWrap(void) {
    Stream stream;
    InitStream(&stream, 4, STD_VIDEO_H264_POC_TYPE_0, 3, 2);
    VvsDpb dpb = VK_NULL_HANDLE;
    VvsDpbCreateInfo createInfo = PoolCreateInfo(&stream, 5);
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    CHECK(Sequence(dpb, &stream, NULL) == VK_SUCCESS);
    Picture picture;
    uint32_t frameNumOfSlot[VVS_DPB_MAX_SLOTS];
    memset(frameNumOfSlot, 0, sizeof(frameNumOfSlot));
    for (uint32_t i = 0; i < 37; i++) {
        const StdVideoH264PictureType type = (i == 0) ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        CHECK(Encode(dpb, &picture, type, 1, (int32_t)((2U * i) & 15U)) == VK_SUCCESS);
        CheckResultShape(&picture);
        const uint32_t expected = MinU32(i, 3U);
        CHECK(picture.result.slots.referenceSlotCount == expected);
        for (uint32_t k = 0; k < expected; k++) {
            /* The k-th most recent reference picture. */
            CHECK(frameNumOfSlot[picture.syntax.refLists.RefPicList0[k]] == ((i - 1U - k) % 16U));
        }
        frameNumOfSlot[SetupSlot(&picture)] = picture.std.frame_num;
    }
    /* A B picture between the last two: one entry in list 0, and one in list 1 as list 0's default
     * count is above one. */
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_B, 0, (int32_t)((2U * 36U - 1U) & 15U)) == VK_SUCCESS);
    CHECK(picture.syntax.refLists.num_ref_idx_l0_active_minus1 == 0);
    CHECK(picture.syntax.refLists.num_ref_idx_l1_active_minus1 == 0);
    CHECK(picture.syntax.refLists.RefPicList1[1] == STD_VIDEO_H264_NO_REFERENCE_PICTURE);
    CHECK(frameNumOfSlot[picture.syntax.refLists.RefPicList0[0]] == (35U % 16U));
    CHECK(frameNumOfSlot[picture.syntax.refLists.RefPicList1[0]] == (36U % 16U));
    vvsDpbDestroy(dpb, NULL);
}

/* CALLBACK mode: every resource acquired is released, by the pictures or the reset; a failed
 * acquire changes nothing, and the next pictures are those of a context that never saw it. */
static void TestCallbacks(void) {
    Stream stream;
    InitStream(&stream, 2, STD_VIDEO_H264_POC_TYPE_0, 1, 1);
    Callbacks callbacks;
    Callbacks referenceCallbacks;
    InitCallbacks(&callbacks);
    InitCallbacks(&referenceCallbacks);
    VvsDpb dpb = VK_NULL_HANDLE;
    VvsDpb referenceDpb = VK_NULL_HANDLE;
    VvsDpbCreateInfo createInfo = CallbackCreateInfo(&stream, &callbacks);
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    createInfo = CallbackCreateInfo(&stream, &referenceCallbacks);
    CHECK(vvsDpbCreate(&createInfo, NULL, &referenceDpb) == VK_SUCCESS);
    CHECK(Sequence(dpb, &stream, NULL) == VK_SUCCESS);
    CHECK(Sequence(referenceDpb, &stream, NULL) == VK_SUCCESS);

    Picture picture;
    Picture referencePicture;
    for (uint32_t i = 0; i < 20; i++) {
        const StdVideoH264PictureType type = (i == 0) ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        const int reference = ((i % 3U) != 2U) ? 1 : 0;
        if (i == 7) {
            callbacks.failNextAcquire = 1;
            CHECK(Encode(dpb, &picture, type, reference, (int32_t)(2U * i)) == VK_ERROR_OUT_OF_POOL_MEMORY);
        }
        CHECK(Encode(dpb, &picture, type, reference, (int32_t)(2U * i)) == VK_SUCCESS);
        CHECK(Encode(referenceDpb, &referencePicture, type, reference, (int32_t)(2U * i)) == VK_SUCCESS);
        CheckResultShape(&picture);
        CHECK(picture.std.frame_num == referencePicture.std.frame_num);
        CHECK(picture.std.PicOrderCnt == referencePicture.std.PicOrderCnt);
        CHECK(SetupSlot(&picture) == SetupSlot(&referencePicture));
        CHECK(picture.result.slots.referenceSlotCount == referencePicture.result.slots.referenceSlotCount);
        for (uint32_t k = 0; k < picture.result.slots.beginCodingSlotCount; k++) {
            CHECK(picture.result.slots.referenceSlots[k].slotIndex == referencePicture.result.slots.referenceSlots[k].slotIndex);
            /* The pointer is the one the client returned for that index. */
            CHECK(picture.result.slots.referenceSlots[k].pPictureResource ==
                  &callbacks.resources[picture.result.slots.resourceIndices[k]]);
        }
    }
    VvsDpbResourceList releases;
    CHECK(vvsDpbReset(dpb, NULL, &releases) == VK_SUCCESS);
    CHECK(releases.count == 2);
    CHECK(callbacks.acquires == 20);
    CHECK(callbacks.releases == callbacks.acquires);
    for (uint32_t i = 0; i < 32; i++) {
        CHECK(callbacks.held[i] == 0);
    }
    /* After a reset the next picture is an IDR picture. */
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_P, 1, 2) == VK_ERROR_VALIDATION_FAILED);
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_IDR, 1, 0) == VK_SUCCESS);
    vvsDpbDestroy(dpb, NULL);
    vvsDpbDestroy(referenceDpb, NULL);
}

/* A rewind of one picture brings back the state before it, and the pictures encoded after the rewind
 * are those of a context that never encoded the undone one. */
static void TestRewind(void) {
    Stream stream;
    InitStream(&stream, 2, STD_VIDEO_H264_POC_TYPE_0, 2, 1);
    Callbacks callbacks;
    InitCallbacks(&callbacks);
    VvsDpb dpb = VK_NULL_HANDLE;
    VvsDpbCreateInfo createInfo = CallbackCreateInfo(&stream, &callbacks);
    createInfo.rewindDepth = 1;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    VvsDpbSequenceRequirements requirements;
    CHECK(Sequence(dpb, &stream, &requirements) == VK_SUCCESS);
    /* H.264: a rewind of one picture takes no slot. */
    CHECK(requirements.maxDpbSlots == 3);

    Callbacks referenceCallbacks;
    InitCallbacks(&referenceCallbacks);
    VvsDpb referenceDpb = VK_NULL_HANDLE;
    createInfo = CallbackCreateInfo(&stream, &referenceCallbacks);
    CHECK(vvsDpbCreate(&createInfo, NULL, &referenceDpb) == VK_SUCCESS);
    CHECK(Sequence(referenceDpb, &stream, NULL) == VK_SUCCESS);

    Picture picture;
    Picture referencePicture;
    VvsDpbRewindInfo rewindInfo;
    memset(&rewindInfo, 0, sizeof(rewindInfo));
    rewindInfo.pictureCount = 1;
    CHECK(vvsDpbRewind(dpb, &rewindInfo, NULL) == VK_ERROR_VALIDATION_FAILED); /* nothing to undo */
    for (uint32_t i = 0; i < 12; i++) {
        const StdVideoH264PictureType type = (i == 0) ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        if ((i % 4U) == 3U) {
            /* Encode the picture as a P picture, undo it, and encode it as an I picture. */
            uint32_t countBefore = 0;
            CHECK(vvsDpbGetReferenceFrames(dpb, &countBefore, NULL) == VK_SUCCESS);
            VvsDpbReferenceFrame before[VVS_DPB_MAX_SLOTS];
            CHECK(vvsDpbGetReferenceFrames(dpb, &countBefore, before) == VK_SUCCESS);
            CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_P, 1, (int32_t)(2U * i)) == VK_SUCCESS);
            const uint32_t retainsBefore = callbacks.retains;
            VvsDpbResourceList releases;
            CHECK(vvsDpbRewind(dpb, &rewindInfo, &releases) == VK_SUCCESS);
            CHECK(releases.count == 1);                     /* the undone picture's */
            CHECK(callbacks.retains == retainsBefore + 1U); /* the picture its sliding window dropped */
            uint32_t countAfter = VVS_DPB_MAX_SLOTS;
            VvsDpbReferenceFrame after[VVS_DPB_MAX_SLOTS];
            CHECK(vvsDpbGetReferenceFrames(dpb, &countAfter, after) == VK_SUCCESS);
            CHECK(countAfter == countBefore);
            CHECK(memcmp(before, after, countAfter * sizeof(after[0])) == 0);
            CHECK(vvsDpbRewind(dpb, &rewindInfo, NULL) == VK_ERROR_VALIDATION_FAILED); /* beyond the depth */
            CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_I, 1, (int32_t)(2U * i)) == VK_SUCCESS);
            CHECK(Encode(referenceDpb, &referencePicture, STD_VIDEO_H264_PICTURE_TYPE_I, 1, (int32_t)(2U * i)) == VK_SUCCESS);
        } else {
            CHECK(Encode(dpb, &picture, type, 1, (int32_t)(2U * i)) == VK_SUCCESS);
            CHECK(Encode(referenceDpb, &referencePicture, type, 1, (int32_t)(2U * i)) == VK_SUCCESS);
        }
        CheckResultShape(&picture);
        CHECK(picture.std.frame_num == referencePicture.std.frame_num);
        CHECK(picture.std.PicOrderCnt == referencePicture.std.PicOrderCnt);
        CHECK(picture.result.slots.referenceSlotCount == referencePicture.result.slots.referenceSlotCount);
        for (uint32_t k = 0; k < picture.result.slots.referenceSlotCount; k++) {
            CHECK(picture.result.slots.stdReferenceInfos[k].h264.FrameNum ==
                  referencePicture.result.slots.stdReferenceInfos[k].h264.FrameNum);
        }
    }
    CHECK(vvsDpbReset(dpb, NULL, NULL) == VK_SUCCESS);
    CHECK(callbacks.releases == callbacks.acquires + callbacks.retains);
    vvsDpbDestroy(dpb, NULL);
    vvsDpbDestroy(referenceDpb, NULL);
}

/* A checkpoint returns the context to its state while no slot that held a reference then was set up
 * again; after that, and after a reset, it is refused and nothing changes. */
static void TestCheckpoint(void) {
    Stream stream;
    InitStream(&stream, 1, STD_VIDEO_H264_POC_TYPE_2, 1, 1);
    VvsDpb dpb = VK_NULL_HANDLE;
    VvsDpbCreateInfo createInfo = PoolCreateInfo(&stream, 4);
    createInfo.slotPolicy = VVS_DPB_SLOT_POLICY_ROTATE;
    createInfo.flags = 0;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    CHECK(Sequence(dpb, &stream, NULL) == VK_SUCCESS);

    Picture picture;
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_IDR, 1, 0) == VK_SUCCESS);
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_P, 1, 2) == VK_SUCCESS);
    const int32_t checkpointSlot = SetupSlot(&picture);
    VvsDpbCheckpoint checkpoint;
    vvsDpbSaveCheckpoint(dpb, &checkpoint);
    /* Non-reference pictures set up no slot without SETUP_EVERY_PICTURE. */
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_P, 0, 3) == VK_SUCCESS);
    CHECK(picture.result.slots.pSetupReferenceSlot == NULL);
    CHECK(picture.result.slots.beginCodingSlotCount == 1);

    VvsDpbRewindInfo rewindInfo;
    memset(&rewindInfo, 0, sizeof(rewindInfo));
    rewindInfo.pCheckpoint = &checkpoint;
    CHECK(vvsDpbRewind(dpb, &rewindInfo, NULL) == VK_SUCCESS);
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_P, 1, 4) == VK_SUCCESS);
    CHECK(picture.result.slots.referenceSlots[0].slotIndex == checkpointSlot);
    CHECK(picture.std.frame_num == 2);
    /* ROTATE: the next slot after the last set up, so the checkpoint's slot is set up again only
     * after the others. */
    CHECK(vvsDpbRewind(dpb, &rewindInfo, NULL) == VK_SUCCESS);
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_P, 1, 4) == VK_SUCCESS);
    CHECK(SetupSlot(&picture) != checkpointSlot);
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_P, 1, 6) == VK_SUCCESS);
    CHECK(SetupSlot(&picture) == checkpointSlot);
    uint32_t countBefore = VVS_DPB_MAX_SLOTS;
    VvsDpbReferenceFrame before[VVS_DPB_MAX_SLOTS];
    CHECK(vvsDpbGetReferenceFrames(dpb, &countBefore, before) == VK_SUCCESS);
    CHECK(vvsDpbRewind(dpb, &rewindInfo, NULL) == VK_ERROR_VALIDATION_FAILED);
    uint32_t countAfter = VVS_DPB_MAX_SLOTS;
    VvsDpbReferenceFrame after[VVS_DPB_MAX_SLOTS];
    CHECK(vvsDpbGetReferenceFrames(dpb, &countAfter, after) == VK_SUCCESS);
    CHECK((countAfter == countBefore) && (memcmp(before, after, countAfter * sizeof(after[0])) == 0));

    /* A reset refuses the checkpoints saved before it. */
    vvsDpbSaveCheckpoint(dpb, &checkpoint);
    CHECK(vvsDpbReset(dpb, NULL, NULL) == VK_SUCCESS);
    CHECK(vvsDpbRewind(dpb, &rewindInfo, NULL) == VK_ERROR_VALIDATION_FAILED);
    /* Both targets, or neither. */
    rewindInfo.pictureCount = 1;
    CHECK(vvsDpbRewind(dpb, &rewindInfo, NULL) == VK_ERROR_VALIDATION_FAILED);
    rewindInfo.pictureCount = 0;
    rewindInfo.pCheckpoint = NULL;
    CHECK(vvsDpbRewind(dpb, &rewindInfo, NULL) == VK_ERROR_VALIDATION_FAILED);
    vvsDpbDestroy(dpb, NULL);
}

/* POOL with more resources than slots: each setup takes the resource released longest ago. */
static void TestPoolOrder(void) {
    Stream stream;
    InitStream(&stream, 1, STD_VIDEO_H264_POC_TYPE_2, 1, 1);
    VvsDpb dpb = VK_NULL_HANDLE;
    VvsDpbCreateInfo createInfo = PoolCreateInfo(&stream, 5);
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    CHECK(Sequence(dpb, &stream, NULL) == VK_SUCCESS);
    Picture picture;
    for (uint32_t i = 0; i < 12; i++) {
        const StdVideoH264PictureType type = (i == 0) ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        CHECK(Encode(dpb, &picture, type, 1, (int32_t)(2U * i)) == VK_SUCCESS);
        /* 0..4 in order, then each in the order it was released. */
        CHECK(picture.result.slots.resourceIndices[picture.result.slots.referenceSlotCount] == (i % 5U));
    }
    /* Too small a pool for the sequence: refused by the picture calls, accepted again after a reset
     * gives enough. */
    VvsDpbResetInfo resetInfo;
    memset(&resetInfo, 0, sizeof(resetInfo));
    resetInfo.resources.mode = VVS_DPB_RESOURCE_MODE_POOL;
    resetInfo.resources.resourceCount = 1;
    resetInfo.resources.pResources = stream.pool;
    CHECK(vvsDpbReset(dpb, &resetInfo, NULL) == VK_ERROR_VALIDATION_FAILED);
    resetInfo.resources.resourceCount = 2;
    CHECK(vvsDpbReset(dpb, &resetInfo, NULL) == VK_SUCCESS);
    CHECK(Encode(dpb, &picture, STD_VIDEO_H264_PICTURE_TYPE_IDR, 1, 0) == VK_SUCCESS);
    CHECK(picture.result.slots.pSetupReferenceSlot->pPictureResource == &stream.pool[0]);
    vvsDpbDestroy(dpb, NULL);
}

/* The two-call idiom of vvsDpbGetReferenceFrames. */
static void TestReferenceFrames(void) {
    Stream stream;
    InitStream(&stream, 3, STD_VIDEO_H264_POC_TYPE_2, 3, 1);
    VvsDpb dpb = VK_NULL_HANDLE;
    VvsDpbCreateInfo createInfo = PoolCreateInfo(&stream, 4);
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    CHECK(Sequence(dpb, &stream, NULL) == VK_SUCCESS);
    Picture picture;
    for (uint32_t i = 0; i < 5; i++) {
        const StdVideoH264PictureType type = (i == 0) ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        CHECK(Encode(dpb, &picture, type, 1, (int32_t)(2U * i)) == VK_SUCCESS);
    }
    uint32_t count = 0;
    CHECK(vvsDpbGetReferenceFrames(dpb, &count, NULL) == VK_SUCCESS);
    CHECK(count == 3);
    VvsDpbReferenceFrame frames[VVS_DPB_MAX_SLOTS];
    count = 2;
    CHECK(vvsDpbGetReferenceFrames(dpb, &count, frames) == VK_INCOMPLETE);
    CHECK(count == 2);
    count = VVS_DPB_MAX_SLOTS;
    CHECK(vvsDpbGetReferenceFrames(dpb, &count, frames) == VK_SUCCESS);
    CHECK(count == 3);
    for (uint32_t i = 0; i < count; i++) {
        CHECK(frames[i].stdReferenceInfo.h264.FrameNum >= 2U);
        CHECK(frames[i].resourceIndex == (uint32_t)frames[i].slotIndex);
    }
    vvsDpbDestroy(dpb, NULL);
}

/* ------------------------------------------------------------------------------------------------
 * H.265: a client's stream and pictures
 * ---------------------------------------------------------------------------------------------- */

typedef struct H265Stream {
    VkVideoEncodeH265ProfileInfoKHR h265Profile;
    VkVideoProfileInfoKHR profile;
    StdVideoH265DecPicBufMgr decPicBufMgr;
    StdVideoH265ShortTermRefPicSet sets[4];
    StdVideoH265SequenceParameterSet sps;
    StdVideoH265PictureParameterSet pps;
    VkVideoPictureResourceInfoKHR pool[VVS_DPB_MAX_RESOURCES];
} H265Stream;

typedef struct H265Picture {
    StdVideoEncodeH265PictureInfo std;
    VvsDpbH265EncodeSyntax syntax;
    VvsDpbPictureResult result;
} H265Picture;

/* The encoder's SPS: one set, the references before the picture, the nearest list 0's default count
 * of them used. */
static void InitH265Stream(H265Stream* pStream, uint8_t references, uint8_t list0, uint8_t list1) {
    memset(pStream, 0, sizeof(*pStream));
    pStream->h265Profile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR;
    pStream->h265Profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
    pStream->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    pStream->profile.pNext = &pStream->h265Profile;
    pStream->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    pStream->profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    pStream->profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    pStream->profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    pStream->decPicBufMgr.max_dec_pic_buffering_minus1[0] = references;
    pStream->sets[0].num_negative_pics = references;
    pStream->sets[0].used_by_curr_pic_s0_flag = (uint16_t)((1U << MinU32(references, list0)) - 1U);
    pStream->sps.pDecPicBufMgr = &pStream->decPicBufMgr;
    pStream->sps.log2_max_pic_order_cnt_lsb_minus4 = 4;
    pStream->sps.num_short_term_ref_pic_sets = 1;
    pStream->sps.pShortTermRefPicSet = pStream->sets;
    pStream->pps.num_ref_idx_l0_default_active_minus1 = (uint8_t)(list0 - 1U);
    pStream->pps.num_ref_idx_l1_default_active_minus1 = (uint8_t)(list1 - 1U);
    for (uint32_t i = 0; i < VVS_DPB_MAX_RESOURCES; i++) {
        pStream->pool[i].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        pStream->pool[i].baseArrayLayer = i;
    }
}

static VvsDpb CreateH265(const H265Stream* pStream, uint32_t rewindDepth, VvsDpbSequenceRequirements* pRequirements) {
    VvsDpbCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.apiVersion = VVS_DPB_API_VERSION;
    createInfo.flags = VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT;
    createInfo.pVideoProfile = &pStream->profile;
    createInfo.slotPolicy = VVS_DPB_SLOT_POLICY_LOWEST_FREE;
    createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_POOL;
    createInfo.resources.resourceCount = VVS_DPB_MAX_SLOTS;
    createInfo.resources.pResources = pStream->pool;
    createInfo.rewindDepth = rewindDepth;
    VvsDpb dpb = VK_NULL_HANDLE;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    VvsDpbH265SequenceInfo sequenceInfo;
    memset(&sequenceInfo, 0, sizeof(sequenceInfo));
    sequenceInfo.pStdSps = &pStream->sps;
    sequenceInfo.pStdPps = &pStream->pps;
    CHECK(vvsDpbH265Sequence(dpb, &sequenceInfo, pRequirements) == VK_SUCCESS);
    return dpb;
}

/* Encodes one picture: an IDR or I picture is an IRAP picture. */
static VkResult EncodeH265(VvsDpb dpb, H265Picture* pPicture, StdVideoH265PictureType type, int reference, int32_t poc) {
    memset(pPicture, 0, sizeof(*pPicture));
    pPicture->std.flags.is_reference = reference ? 1U : 0U;
    pPicture->std.flags.IrapPicFlag =
        ((type == STD_VIDEO_H265_PICTURE_TYPE_IDR) || (type == STD_VIDEO_H265_PICTURE_TYPE_I)) ? 1U : 0U;
    pPicture->std.pic_type = type;
    pPicture->std.PicOrderCntVal = poc;
    VvsDpbH265EncodePictureInfo pictureInfo;
    memset(&pictureInfo, 0, sizeof(pictureInfo));
    pictureInfo.dirtyIntraRefreshRegions = VVS_DPB_NO_INTRA_REFRESH;
    pictureInfo.pStdPictureInfo = &pPicture->std;
    pictureInfo.pSyntax = &pPicture->syntax;
    return vvsDpbEncodeH265Picture(dpb, &pictureInfo, &pPicture->result);
}

static int32_t H265SetupSlot(const H265Picture* pPicture) {
    return (pPicture->result.slots.pSetupReferenceSlot != NULL) ? pPicture->result.slots.pSetupReferenceSlot->slotIndex : -1;
}

/* The chains, the setup entry, and the Std pointers into the syntax. */
static void CheckH265Result(const H265Picture* pPicture) {
    const VvsDpbPictureSlots* pSlots = &pPicture->result.slots;
    for (uint32_t i = 0; i < pSlots->beginCodingSlotCount; i++) {
        CHECK(pSlots->referenceSlots[i].pNext == &pSlots->dpbSlotInfos[i]);
        CHECK(pSlots->dpbSlotInfos[i].h265.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR);
        CHECK(pSlots->dpbSlotInfos[i].h265.pStdReferenceInfo == &pSlots->stdReferenceInfos[i].h265);
        CHECK(pSlots->dpbSlotInfos[i].h265.pNext == NULL);
    }
    for (uint32_t i = 0; i < pSlots->referenceSlotCount; i++) {
        CHECK(pSlots->stdReferenceInfos[i].h265.flags.unused_for_reference == 0U);
    }
    if (pSlots->pSetupReferenceSlot != NULL) {
        const StdVideoEncodeH265ReferenceInfo* pSetup = &pSlots->stdReferenceInfos[pSlots->referenceSlotCount].h265;
        CHECK(pSetup->PicOrderCntVal == pPicture->std.PicOrderCntVal);
        CHECK(pSetup->pic_type == pPicture->std.pic_type);
        CHECK(pSetup->flags.unused_for_reference == (pPicture->std.flags.is_reference ? 0U : 1U));
    }
    CHECK(pPicture->std.pShortTermRefPicSet == &pPicture->syntax.shortTermRefPicSet);
    CHECK(pPicture->std.pLongTermRefPics == &pPicture->syntax.longTermRefPics);
    const int inter =
        (pPicture->std.pic_type == STD_VIDEO_H265_PICTURE_TYPE_P) || (pPicture->std.pic_type == STD_VIDEO_H265_PICTURE_TYPE_B);
    CHECK(pPicture->std.pRefLists == (inter ? &pPicture->syntax.refLists : NULL));
}

/* IPPP with two references and one used: the oldest goes when the references fill the DPB, the slot
 * it held takes the next picture, and the RPS is the SPS's set once two references are held. */
static void TestH265Ippp(void) {
    H265Stream stream;
    InitH265Stream(&stream, 2, 1, 1);
    VvsDpbSequenceRequirements requirements;
    VvsDpb dpb = CreateH265(&stream, 0, &requirements);
    CHECK(requirements.maxDpbSlots == 3);
    CHECK(requirements.maxReferencePictures == 2);
    CHECK(requirements.maxActiveReferencePictures == 2);

    static const int32_t kSetup[] = {0, 1, 2, 0, 1};
    static const int32_t kList0[] = {-1, 0, 1, 2, 0};
    static const uint32_t kSpsSet[] = {0, 0, 1, 1, 1};
    H265Picture picture;
    for (int32_t poc = 0; poc < 5; poc++) {
        const StdVideoH265PictureType type = (poc == 0) ? STD_VIDEO_H265_PICTURE_TYPE_IDR : STD_VIDEO_H265_PICTURE_TYPE_P;
        CHECK(EncodeH265(dpb, &picture, type, 1, poc) == VK_SUCCESS);
        CheckH265Result(&picture);
        CHECK(H265SetupSlot(&picture) == kSetup[poc]);
        CHECK(picture.std.flags.short_term_ref_pic_set_sps_flag == kSpsSet[poc]);
        CHECK(picture.std.short_term_ref_pic_set_idx == 0);
        if (poc > 0) {
            const StdVideoEncodeH265ReferenceListsInfo* pLists = picture.std.pRefLists;
            CHECK(pLists->num_ref_idx_l0_active_minus1 == 0);
            CHECK(pLists->RefPicList0[0] == kList0[poc]);
            CHECK(pLists->RefPicList0[1] == STD_VIDEO_H265_NO_REFERENCE_PICTURE);
            CHECK(picture.result.slots.referenceSlotCount == 1);
            CHECK(picture.result.slots.stdReferenceInfos[0].h265.PicOrderCntVal == poc - 1);
        }
    }
    /* P1 sends its own set: one picture before it, used. */
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_IDR, 1, 0) == VK_SUCCESS);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 1) == VK_SUCCESS);
    CHECK(picture.syntax.shortTermRefPicSet.num_negative_pics == 1);
    CHECK(picture.syntax.shortTermRefPicSet.num_positive_pics == 0);
    CHECK(picture.syntax.shortTermRefPicSet.delta_poc_s0_minus1[0] == 0);
    CHECK(picture.syntax.shortTermRefPicSet.used_by_curr_pic_s0_flag == 1);
    vvsDpbDestroy(dpb, NULL);
}

/* IBBBP with four references, two in list 0 and one in list 1: the B pictures use the picture before
 * and the one after them, and take the same free slot one after the other; P8 uses both earlier P
 * pictures, and B5 three pictures. */
static void TestH265BFrames(void) {
    H265Stream stream;
    InitH265Stream(&stream, 4, 2, 1);
    VvsDpbSequenceRequirements requirements;
    VvsDpb dpb = CreateH265(&stream, 0, &requirements);
    CHECK(requirements.maxDpbSlots == 5);
    CHECK(requirements.maxActiveReferencePictures == 3);

    H265Picture picture;
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_IDR, 1, 0) == VK_SUCCESS);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 4) == VK_SUCCESS);
    CHECK(H265SetupSlot(&picture) == 1);
    CHECK(picture.std.pRefLists->num_ref_idx_l0_active_minus1 == 0);
    CHECK(picture.std.pRefLists->num_ref_idx_l1_active_minus1 == 0);
    CHECK(picture.syntax.shortTermRefPicSet.delta_poc_s0_minus1[0] == 3);
    for (int32_t poc = 1; poc < 4; poc++) {
        CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_B, 0, poc) == VK_SUCCESS);
        CheckH265Result(&picture);
        CHECK(H265SetupSlot(&picture) == 2);
        const StdVideoH265ShortTermRefPicSet* pRps = &picture.syntax.shortTermRefPicSet;
        CHECK((pRps->num_negative_pics == 1) && (pRps->num_positive_pics == 1));
        CHECK(pRps->delta_poc_s0_minus1[0] == (uint16_t)(poc - 1));
        CHECK(pRps->delta_poc_s1_minus1[0] == (uint16_t)(3 - poc));
        CHECK((pRps->used_by_curr_pic_s0_flag == 1) && (pRps->used_by_curr_pic_s1_flag == 1));
        CHECK(picture.std.pRefLists->RefPicList0[0] == 0);
        CHECK(picture.std.pRefLists->RefPicList1[0] == 1);
        CHECK(picture.result.slots.referenceSlotCount == 2);
    }
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 8) == VK_SUCCESS);
    CHECK(H265SetupSlot(&picture) == 2);
    CHECK(picture.std.pRefLists->num_ref_idx_l0_active_minus1 == 1);
    CHECK((picture.std.pRefLists->RefPicList0[0] == 1) && (picture.std.pRefLists->RefPicList0[1] == 0));
    CHECK(picture.syntax.shortTermRefPicSet.used_by_curr_pic_s0_flag == 3);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_B, 0, 5) == VK_SUCCESS);
    CHECK(H265SetupSlot(&picture) == 3);
    CHECK(picture.std.pRefLists->num_ref_idx_l0_active_minus1 == 1);
    CHECK((picture.std.pRefLists->RefPicList0[0] == 1) && (picture.std.pRefLists->RefPicList0[1] == 0));
    CHECK(picture.std.pRefLists->RefPicList1[0] == 2);
    CHECK(picture.result.slots.referenceSlotCount == 3);
    CHECK(picture.result.slots.referenceSlots[2].slotIndex == 2);
    vvsDpbDestroy(dpb, NULL);
}

/* An open GOP: the CRA picture I4 keeps the pictures before it, used by none; its leading picture B3
 * uses P2 and I4; the first trailing picture P6 drops P2. */
static void TestH265Cra(void) {
    H265Stream stream;
    InitH265Stream(&stream, 2, 1, 1);
    VvsDpbSequenceRequirements requirements;
    VvsDpb dpb = CreateH265(&stream, 0, &requirements);
    H265Picture picture;
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_IDR, 1, 0) == VK_SUCCESS);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 2) == VK_SUCCESS);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_I, 1, 4) == VK_SUCCESS);
    CheckH265Result(&picture);
    CHECK(H265SetupSlot(&picture) == 2);
    CHECK(picture.std.flags.short_term_ref_pic_set_sps_flag == 0);
    CHECK(picture.syntax.shortTermRefPicSet.num_negative_pics == 2);
    CHECK(picture.syntax.shortTermRefPicSet.used_by_curr_pic_s0_flag == 0);
    CHECK(picture.result.slots.referenceSlotCount == 0);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_B, 0, 3) == VK_SUCCESS);
    CHECK(H265SetupSlot(&picture) == 0);
    CHECK(picture.std.pRefLists->RefPicList0[0] == 1);
    CHECK(picture.std.pRefLists->RefPicList1[0] == 2);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 6) == VK_SUCCESS);
    CHECK(H265SetupSlot(&picture) == 0);
    CHECK(picture.syntax.shortTermRefPicSet.num_negative_pics == 1);
    CHECK(picture.syntax.shortTermRefPicSet.delta_poc_s0_minus1[0] == 1);
    CHECK(picture.std.pRefLists->RefPicList0[0] == 2);
    uint32_t count = 0;
    CHECK(vvsDpbGetReferenceFrames(dpb, &count, NULL) == VK_SUCCESS);
    CHECK(count == 2);
    vvsDpbDestroy(dpb, NULL);
}

/* SPS sets predicted from the one before them (7.4.8): set 1 from set 0 {-1} with deltaRps -1 is
 * {-1, -2}; set 2 from set 1 with deltaRps +1 is {-1} before and {+1} after. Each picture names the
 * set equal to its RPS. */
static void TestH265SpsSets(void) {
    H265Stream stream;
    InitH265Stream(&stream, 2, 2, 1);
    memset(stream.sets, 0, sizeof(stream.sets));
    stream.sets[0].num_negative_pics = 1;
    stream.sets[0].used_by_curr_pic_s0_flag = 1;
    stream.sets[1].flags.inter_ref_pic_set_prediction_flag = 1;
    stream.sets[1].flags.delta_rps_sign = 1;
    stream.sets[1].used_by_curr_pic_flag = 3;
    stream.sets[1].use_delta_flag = 3;
    stream.sets[2].flags.inter_ref_pic_set_prediction_flag = 1;
    stream.sets[2].used_by_curr_pic_flag = 6;
    stream.sets[2].use_delta_flag = 6;
    stream.sps.num_short_term_ref_pic_sets = 3;
    VvsDpbSequenceRequirements requirements;
    VvsDpb dpb = CreateH265(&stream, 0, &requirements);

    H265Picture picture;
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_IDR, 1, 0) == VK_SUCCESS);
    CHECK(picture.std.flags.short_term_ref_pic_set_sps_flag == 0);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 1) == VK_SUCCESS);
    CHECK((picture.std.flags.short_term_ref_pic_set_sps_flag == 1) && (picture.std.short_term_ref_pic_set_idx == 0));
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 2) == VK_SUCCESS);
    CHECK((picture.std.flags.short_term_ref_pic_set_sps_flag == 1) && (picture.std.short_term_ref_pic_set_idx == 1));
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 4) == VK_SUCCESS);
    CHECK(picture.std.flags.short_term_ref_pic_set_sps_flag == 0);
    CHECK((picture.syntax.shortTermRefPicSet.delta_poc_s0_minus1[0] == 1) &&
          (picture.syntax.shortTermRefPicSet.delta_poc_s0_minus1[1] == 0));
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_B, 0, 3) == VK_SUCCESS);
    CHECK((picture.std.flags.short_term_ref_pic_set_sps_flag == 1) && (picture.std.short_term_ref_pic_set_idx == 2));
    vvsDpbDestroy(dpb, NULL);

    /* A set of more than 16 pictures, and a DPB of 17 pictures, are not valid. */
    VvsDpbCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.apiVersion = VVS_DPB_API_VERSION;
    createInfo.pVideoProfile = &stream.profile;
    createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_POOL;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    VvsDpbH265SequenceInfo sequenceInfo = {&stream.sps, &stream.pps, NULL};
    stream.sets[0].num_negative_pics = 9;
    stream.sets[0].num_positive_pics = 8;
    CHECK(vvsDpbH265Sequence(dpb, &sequenceInfo, &requirements) == VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR);
    stream.sets[0].num_negative_pics = 1;
    stream.sets[0].num_positive_pics = 0;
    stream.decPicBufMgr.max_dec_pic_buffering_minus1[0] = 16;
    CHECK(vvsDpbH265Sequence(dpb, &sequenceInfo, &requirements) == VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR);
    stream.decPicBufMgr.max_dec_pic_buffering_minus1[0] = 2;
    VvsDpbGopInfo gop;
    memset(&gop, 0, sizeof(gop));
    gop.gopStructure = VVS_DPB_GOP_STRUCTURE_REGULAR;
    gop.bipredictiveReferencePattern = VVS_DPB_REFERENCE_PATTERN_DYADIC;
    sequenceInfo.pGopInfo = &gop;
    CHECK(vvsDpbH265Sequence(dpb, &sequenceInfo, &requirements) == VK_ERROR_FEATURE_NOT_PRESENT);
    sequenceInfo.pGopInfo = NULL;
    CHECK(vvsDpbH265Sequence(dpb, &sequenceInfo, &requirements) == VK_SUCCESS);
    vvsDpbDestroy(dpb, NULL);
}

/* A rewind of one picture: the picture never takes the slot of a reference its own RPS drops, which
 * takes one more slot, and the picture encoded again gets the same results. Refusals change
 * nothing. */
static void TestH265RewindAndRefusals(void) {
    H265Stream stream;
    InitH265Stream(&stream, 2, 1, 1);
    VvsDpbSequenceRequirements requirements;
    VvsDpb dpb = CreateH265(&stream, 1, &requirements);
    CHECK(requirements.maxDpbSlots == 4);

    H265Picture picture;
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 0) == VK_ERROR_VALIDATION_FAILED);
    for (int32_t poc = 0; poc < 3; poc++) {
        CHECK(EncodeH265(dpb, &picture, (poc == 0) ? STD_VIDEO_H265_PICTURE_TYPE_IDR : STD_VIDEO_H265_PICTURE_TYPE_P, 1, poc) ==
              VK_SUCCESS);
    }
    H265Picture first;
    CHECK(EncodeH265(dpb, &first, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 3) == VK_SUCCESS);
    CHECK(H265SetupSlot(&first) == 3);
    VvsDpbRewindInfo rewindInfo = {1, NULL};
    CHECK(vvsDpbRewind(dpb, &rewindInfo, NULL) == VK_SUCCESS);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 3) == VK_SUCCESS);
    CHECK(H265SetupSlot(&picture) == 3);
    CHECK(memcmp(&picture.syntax.shortTermRefPicSet, &first.syntax.shortTermRefPicSet, sizeof(first.syntax.shortTermRefPicSet)) ==
          0);
    CHECK(picture.syntax.refLists.RefPicList0[0] == first.syntax.refLists.RefPicList0[0]);

    /* A P picture with no reference to use: after a non-reference IDR picture. */
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_IDR, 0, 0) == VK_SUCCESS);
    uint32_t count = 0;
    CHECK(vvsDpbGetReferenceFrames(dpb, &count, NULL) == VK_SUCCESS);
    CHECK(count == 0);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 1) == VK_ERROR_VALIDATION_FAILED);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_IDR, 1, 0) == VK_SUCCESS);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_P, 1, 1) == VK_SUCCESS);
    vvsDpbDestroy(dpb, NULL);

    /* A context of another codec. */
    Stream h264;
    InitStream(&h264, 1, STD_VIDEO_H264_POC_TYPE_2, 1, 1);
    VvsDpbCreateInfo createInfo = PoolCreateInfo(&h264, 2);
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    CHECK(Sequence(dpb, &h264, NULL) == VK_SUCCESS);
    CHECK(EncodeH265(dpb, &picture, STD_VIDEO_H265_PICTURE_TYPE_IDR, 1, 0) == VK_ERROR_VALIDATION_FAILED);
    vvsDpbDestroy(dpb, NULL);
}

/* ------------------------------------------------------------------------------------------------
 * AV1: a client's stream and frames
 * ---------------------------------------------------------------------------------------------- */

typedef struct AV1Stream {
    VkVideoEncodeAV1ProfileInfoKHR av1Profile;
    VkVideoProfileInfoKHR profile;
    StdVideoAV1SequenceHeader header;
    VkVideoEncodeAV1CapabilitiesKHR caps;
    VvsDpbGopInfo gop;
    VkVideoPictureResourceInfoKHR pool[VVS_DPB_MAX_RESOURCES];
} AV1Stream;

typedef struct AV1Frame {
    VvsDpbPictureInfo info;
    StdVideoEncodeAV1PictureInfo std;
    VkVideoEncodeAV1PictureInfoKHR av1;
    VvsDpbPictureResult result;
} AV1Frame;

/* A device that takes every reference name in every mode, and CDF-only primary reference frames. */
static void InitAV1Stream(AV1Stream* pStream, uint32_t bFrames) {
    memset(pStream, 0, sizeof(*pStream));
    pStream->av1Profile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR;
    pStream->av1Profile.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
    pStream->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    pStream->profile.pNext = &pStream->av1Profile;
    pStream->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    pStream->profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    pStream->profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    pStream->profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    pStream->header.flags.enable_order_hint = 1;
    pStream->header.order_hint_bits_minus_1 = 7;
    pStream->caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR;
    pStream->caps.flags = VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR;
    pStream->caps.maxSingleReferenceCount = 7;
    pStream->caps.singleReferenceNameMask = 0x7f;
    pStream->caps.maxUnidirectionalCompoundReferenceCount = 4;
    pStream->caps.maxUnidirectionalCompoundGroup1ReferenceCount = 4;
    pStream->caps.unidirectionalCompoundReferenceNameMask = 0xf;
    pStream->caps.maxBidirectionalCompoundReferenceCount = 7;
    pStream->caps.maxBidirectionalCompoundGroup1ReferenceCount = 4;
    pStream->caps.maxBidirectionalCompoundGroup2ReferenceCount = 3;
    pStream->caps.bidirectionalCompoundReferenceNameMask = 0x7f;
    pStream->gop.gopStructure = VVS_DPB_GOP_STRUCTURE_IRREGULAR;
    pStream->gop.consecutiveBipredictiveFrameCount = bFrames;
    for (uint32_t i = 0; i < VVS_DPB_MAX_RESOURCES; i++) {
        pStream->pool[i].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        pStream->pool[i].baseArrayLayer = i;
    }
}

static VkResult SequenceAV1(VvsDpb dpb, const AV1Stream* pStream, VvsDpbSequenceRequirements* pRequirements) {
    const VvsDpbAV1SequenceInfo sequenceInfo = {&pStream->header, &pStream->caps, NULL, &pStream->gop};
    return vvsDpbAV1Sequence(dpb, &sequenceInfo, pRequirements);
}

static VvsDpb CreateAV1(const AV1Stream* pStream, uint32_t maxDpbSlots, VvsDpbSequenceRequirements* pRequirements) {
    VvsDpbCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.apiVersion = VVS_DPB_API_VERSION;
    createInfo.flags = VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT;
    createInfo.pVideoProfile = &pStream->profile;
    createInfo.maxDpbSlots = maxDpbSlots;
    createInfo.maxActiveReferencePictures = 7;
    createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_POOL;
    createInfo.resources.resourceCount = VVS_DPB_MAX_SLOTS;
    createInfo.resources.pResources = pStream->pool;
    VvsDpb dpb = VK_NULL_HANDLE;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    CHECK(SequenceAV1(dpb, pStream, pRequirements) == VK_SUCCESS);
    return dpb;
}

/* Encodes one frame displayed at order: a key frame is shown and error resilient. */
static VkResult EncodeAV1(VvsDpb dpb, AV1Frame* pFrame, VvsDpbPictureType type, int shown, int reference, uint32_t order) {
    memset(pFrame, 0, sizeof(*pFrame));
    pFrame->info.pictureType = type;
    pFrame->info.displayOrderIndex = order;
    pFrame->std.frame_type = (type == VVS_DPB_PICTURE_TYPE_KEY)     ? STD_VIDEO_AV1_FRAME_TYPE_KEY
                             : (type == VVS_DPB_PICTURE_TYPE_INTRA) ? STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY
                                                                    : STD_VIDEO_AV1_FRAME_TYPE_INTER;
    pFrame->std.flags.show_frame = shown ? 1U : 0U;
    pFrame->std.flags.error_resilient_mode = (type == VVS_DPB_PICTURE_TYPE_KEY) ? 1U : 0U;
    pFrame->std.order_hint = (uint8_t)order;
    pFrame->std.current_frame_id = order;
    pFrame->av1.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PICTURE_INFO_KHR;
    pFrame->av1.pStdPictureInfo = &pFrame->std;
    VvsDpbAV1EncodePictureInfo pictureInfo;
    memset(&pictureInfo, 0, sizeof(pictureInfo));
    pictureInfo.pPictureInfo = &pFrame->info;
    pictureInfo.dirtyIntraRefreshRegions = VVS_DPB_NO_INTRA_REFRESH;
    pictureInfo.reference = reference ? VK_TRUE : VK_FALSE;
    pictureInfo.pPictureInfoAV1 = &pFrame->av1;
    pictureInfo.pStdPictureInfo = &pFrame->std;
    return vvsDpbEncodeAV1Picture(dpb, &pictureInfo, &pFrame->result);
}

static int32_t AV1SetupSlot(const AV1Frame* pFrame) {
    return (pFrame->result.slots.pSetupReferenceSlot != NULL) ? pFrame->result.slots.pSetupReferenceSlot->slotIndex : -1;
}

/* IPPP without B pictures: LAST_FRAME to GOLDEN_FRAME hold the last four pictures, so each P picture
 * refreshes GOLDEN_FRAME's buffer and the names move on. P2 takes P1 and the key frame, P1 as its
 * primary reference frame. */
static void TestAV1Ippp(void) {
    AV1Stream stream;
    InitAV1Stream(&stream, 0);
    VvsDpbSequenceRequirements requirements;
    VvsDpb dpb = CreateAV1(&stream, 9, &requirements);
    CHECK(requirements.maxDpbSlots == 8);
    CHECK(requirements.maxActiveReferencePictures == 7);

    AV1Frame frame;
    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_KEY, 1, 1, 0) == VK_SUCCESS);
    CHECK(AV1SetupSlot(&frame) == 0);
    CHECK(frame.std.refresh_frame_flags == 0xff);
    CHECK(frame.std.primary_ref_frame == STD_VIDEO_AV1_PRIMARY_REF_NONE);
    CHECK(frame.av1.predictionMode == VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_INTRA_ONLY_KHR);
    CHECK(frame.av1.rateControlGroup == VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_INTRA_KHR);
    CHECK(frame.result.slots.referenceSlotCount == 0);

    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_PREDICTIVE, 1, 1, 1) == VK_SUCCESS);
    CHECK(AV1SetupSlot(&frame) == 1);
    CHECK(frame.std.refresh_frame_flags == 0x10);
    CHECK(frame.std.primary_ref_frame == STD_VIDEO_AV1_PRIMARY_REF_NONE);
    CHECK(frame.av1.referenceNameSlotIndices[0] == 0);
    CHECK(frame.av1.referenceNameSlotIndices[1] == -1);
    CHECK(frame.av1.predictionMode == VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR);
    CHECK(frame.av1.rateControlGroup == VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_PREDICTIVE_KHR);
    for (uint32_t i = 0; i < STD_VIDEO_AV1_REFS_PER_FRAME; i++) {
        CHECK(frame.std.ref_frame_idx[i] == (int8_t)(i + 1U));
    }

    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_PREDICTIVE, 1, 1, 2) == VK_SUCCESS);
    CHECK(AV1SetupSlot(&frame) == 2);
    CHECK(frame.std.refresh_frame_flags == 0x08);
    static const int8_t kRefFrameIdx[STD_VIDEO_AV1_REFS_PER_FRAME] = {4, 1, 2, 3, 5, 6, 7};
    CHECK(memcmp(frame.std.ref_frame_idx, kRefFrameIdx, sizeof(kRefFrameIdx)) == 0);
    static const uint8_t kRefOrderHint[STD_VIDEO_AV1_NUM_REF_FRAMES] = {0, 0, 0, 0, 1, 0, 0, 0};
    CHECK(memcmp(frame.std.ref_order_hint, kRefOrderHint, sizeof(kRefOrderHint)) == 0);
    CHECK(frame.std.primary_ref_frame == 0);
    CHECK(frame.av1.primaryReferenceCdfOnly == VK_FALSE);
    CHECK((frame.av1.referenceNameSlotIndices[0] == 1) && (frame.av1.referenceNameSlotIndices[1] == 0));
    CHECK(frame.av1.predictionMode == VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_UNIDIRECTIONAL_COMPOUND_KHR);
    CHECK(frame.result.slots.referenceSlotCount == 2);
    CHECK(frame.result.slots.dpbSlotInfos[0].av1.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR);
    CHECK(frame.result.slots.stdReferenceInfos[0].av1.OrderHint == 1);
    CHECK(frame.result.slots.stdReferenceInfos[1].av1.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY);
    vvsDpbDestroy(dpb, NULL);
}

/* A GOP with B pictures: P2 is encoded first and hidden (ALTREF_FRAME), B1 takes the key frame and P2,
 * and showing P2 moves it into GOLDEN_FRAME's buffer. P3, shown, is a GOLDEN_FRAME picture. */
static void TestAV1Hidden(void) {
    AV1Stream stream;
    InitAV1Stream(&stream, 1);
    VvsDpbSequenceRequirements requirements;
    VvsDpb dpb = CreateAV1(&stream, 9, &requirements);
    CHECK(requirements.maxDpbSlots == 8);

    AV1Frame frame;
    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_KEY, 1, 1, 0) == VK_SUCCESS);
    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_PREDICTIVE, 0, 1, 2) == VK_SUCCESS);
    CHECK(AV1SetupSlot(&frame) == 1);
    CHECK(frame.std.refresh_frame_flags == 0x80);
    CHECK(frame.std.primary_ref_frame == 0);
    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_BIPREDICTIVE, 1, 0, 1) == VK_SUCCESS);
    CHECK(AV1SetupSlot(&frame) == 2);
    CHECK(frame.std.refresh_frame_flags == 0);
    CHECK((frame.av1.referenceNameSlotIndices[0] == 0) && (frame.av1.referenceNameSlotIndices[6] == 1));
    CHECK(frame.std.primary_ref_frame == 6);
    CHECK(frame.av1.predictionMode == VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_BIDIRECTIONAL_COMPOUND_KHR);
    CHECK(frame.av1.rateControlGroup == VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_BIPREDICTIVE_KHR);
    CHECK(frame.result.releases.count == 1);

    uint8_t buffer = 0xff;
    VvsDpbPictureResult result;
    CHECK(vvsDpbEncodeAV1ShowExistingFrame(dpb, 5, &buffer, &result) == VK_ERROR_VALIDATION_FAILED);
    CHECK(vvsDpbEncodeAV1ShowExistingFrame(dpb, 2, &buffer, &result) == VK_SUCCESS);
    CHECK(buffer == 7);
    CHECK((result.slots.referenceSlotCount == 0) && (result.slots.pSetupReferenceSlot == NULL));
    VvsDpbReferenceFrame frames[VVS_DPB_MAX_SLOTS];
    uint32_t count = VVS_DPB_MAX_SLOTS;
    CHECK(vvsDpbGetReferenceFrames(dpb, &count, frames) == VK_SUCCESS);
    CHECK(count == 2);
    CHECK((frames[0].slotIndex == 0) && (frames[0].av1BufferMask == 0x7f));
    CHECK((frames[1].slotIndex == 1) && (frames[1].av1BufferMask == 0x80));

    /* The names after showing P2: LAST_FRAME to LAST3_FRAME buffers 4, 1, 2, GOLDEN_FRAME 7 (P2). */
    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_PREDICTIVE, 1, 1, 3) == VK_SUCCESS);
    static const int8_t kRefFrameIdx[STD_VIDEO_AV1_REFS_PER_FRAME] = {4, 1, 2, 7, 5, 6, 3};
    CHECK(memcmp(frame.std.ref_frame_idx, kRefFrameIdx, sizeof(kRefFrameIdx)) == 0);
    CHECK(frame.std.refresh_frame_flags == 0x84);
    CHECK(AV1SetupSlot(&frame) == 2);
    vvsDpbDestroy(dpb, NULL);
}

/* The first frame is a shown key frame; a frame needs its picture information; fewer than 8 slots
 * are refused, and a B picture with no picture after it is coded as a P picture. */
static void TestAV1Refusals(void) {
    AV1Stream stream;
    InitAV1Stream(&stream, 3);
    VvsDpbSequenceRequirements requirements;
    VvsDpb dpb = CreateAV1(&stream, 0, &requirements);
    AV1Frame frame;
    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_PREDICTIVE, 1, 1, 0) == VK_ERROR_VALIDATION_FAILED);
    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_KEY, 0, 1, 0) == VK_ERROR_VALIDATION_FAILED);
    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_KEY, 1, 1, 0) == VK_SUCCESS);
    CHECK(EncodeAV1(dpb, &frame, VVS_DPB_PICTURE_TYPE_BIPREDICTIVE, 1, 0, 1) == VK_SUCCESS);
    CHECK(frame.av1.rateControlGroup == VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_PREDICTIVE_KHR);
    VvsDpbAV1EncodePictureInfo pictureInfo;
    memset(&pictureInfo, 0, sizeof(pictureInfo));
    pictureInfo.pPictureInfoAV1 = &frame.av1;
    pictureInfo.pStdPictureInfo = &frame.std;
    CHECK(vvsDpbEncodeAV1Picture(dpb, &pictureInfo, &frame.result) == VK_ERROR_VALIDATION_FAILED);
    vvsDpbDestroy(dpb, NULL);

    VvsDpbCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.apiVersion = VVS_DPB_API_VERSION;
    createInfo.pVideoProfile = &stream.profile;
    createInfo.maxDpbSlots = 7;
    createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_POOL;
    CHECK(vvsDpbCreate(&createInfo, NULL, &dpb) == VK_SUCCESS);
    CHECK(SequenceAV1(dpb, &stream, &requirements) == VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR);
    vvsDpbDestroy(dpb, NULL);
}

int main(void) {
    TestCreate();
    TestIppp();
    TestBFrames();
    TestListsAndWrap();
    TestCallbacks();
    TestRewind();
    TestCheckpoint();
    TestPoolOrder();
    TestReferenceFrames();
    TestH265Ippp();
    TestH265BFrames();
    TestH265Cra();
    TestH265SpsSets();
    TestH265RewindAndRefusals();
    TestAV1Ippp();
    TestAV1Hidden();
    TestAV1Refusals();
    if (g_failures != 0) {
        fprintf(stderr, "vkvideo-dpb-test: %d checks failed\n", g_failures);
        return 1;
    }
    printf("vkvideo-dpb-test: all checks passed\n");
    return 0;
}
