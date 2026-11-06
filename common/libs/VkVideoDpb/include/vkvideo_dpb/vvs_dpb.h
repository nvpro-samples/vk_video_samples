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
 * vvs_dpb.h: vkvideo-dpb, the decoded picture buffer of a Vulkan Video encoder.
 *
 * The library manages the reference pictures of an encoded stream: which pictures stay references,
 * which pictures each picture references, the codec syntax that says so, and the DPB slots and
 * picture resources the video commands bind. A context serves one stream of one codec operation.
 * Its lifecycle is common to every codec (create, destroy, reset, checkpoints and rewind); the calls
 * that process a picture are specialized to each codec's API.
 *
 * Per picture, the client may give the library what it knows about the picture: its type, its place
 * in the key-frame period, the GOP and the run of bipredictive pictures, its encode and display order
 * and its temporal layer (VvsDpbPictureInfo). Per sequence it may give the GOP and its reference
 * patterns (VvsDpbGopInfo).
 *
 * The interface is written in Vulkan's types, so that nothing is converted on either side:
 * - Inputs are the Std structures an encoder builds for its video commands anyway.
 * - Outputs are written once, in place, into the structures that vkCmdBeginVideoCodingKHR and
 *   vkCmdEncodeVideoKHR take, in storage the client owns: VkVideoReferenceSlotInfoKHR arrays chained
 *   to the codec's DPB slot information and Std reference information, the Std reference lists, the
 *   H.265 RPS, the H.264 marking operations and the AV1 reference fields.
 * - Results are VkResult codes, host memory comes from VkAllocationCallbacks, the codec comes from
 *   VkVideoProfileInfoKHR and the Std header version from VkExtensionProperties, with the meaning
 *   each has in vkCreateVideoSessionKHR.
 *
 * The library is written in C99; this header compiles as C99 and as C++. It makes no Vulkan call and
 * creates no Vulkan object. The picture resources it binds are the client's, carried by pointer and
 * never dereferenced: from a pool the client gives the context, or from the client's callbacks.
 *
 * Rules every call follows:
 * - One allocation, in vvsDpbCreate. Every array has a fixed capacity.
 * - No static or global variables; the only static data are constant tables. Contexts share
 *   nothing, so contexts on different threads need no locks. One context is externally
 *   synchronized.
 * - The library keeps no pointer into its own state in the client's storage, and no pointer into the
 *   client's storage but the resource pool, the resources pfnAcquireResource returns and the
 *   callbacks' pUserData.
 * - A call that fails changes nothing.
 * - The same inputs give the same slots, syntax and releases.
 * - Slot indices stay below the context's maxDpbSlots and the sequence's requirement.
 *
 * This version plans H.264, H.265 and AV1 encode.
 */
#ifndef VVS_DPB_H_
#define VVS_DPB_H_ 1

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VVS_DPB_API
#define VVS_DPB_API
#endif

#define VVS_DPB_MAKE_API_VERSION(major, minor, patch) \
    ((((uint32_t)(major)) << 22U) | (((uint32_t)(minor)) << 12U) | ((uint32_t)(patch)))
#define VVS_DPB_API_VERSION_0_2 VVS_DPB_MAKE_API_VERSION(0, 2, 0)
#define VVS_DPB_API_VERSION VVS_DPB_API_VERSION_0_2

/* Capacities. */
/* The most DPB slots a context uses: 16 references and the picture being encoded. */
#define VVS_DPB_MAX_SLOTS 17U
/* DPB slots one picture references: H.264 and H.265 16, AV1 7. */
#define VVS_DPB_MAX_ACTIVE_REFERENCES 16U
/* The references and the setup picture. */
#define VVS_DPB_MAX_BEGIN_CODING_SLOTS (VVS_DPB_MAX_ACTIVE_REFERENCES + 1U)
/* Picture resources in a VVS_DPB_RESOURCE_MODE_POOL pool. */
#define VVS_DPB_MAX_RESOURCES 64U
/* memory_management_control_operation entries the library writes for one picture: one per
 * short-term and per long-term reference, three long-term operations and the end of the list. */
#define VVS_DPB_H264_MAX_MARKING_OPERATIONS 36U
/* Bytes of a checkpoint. */
#define VVS_DPB_CHECKPOINT_SIZE 2048U
/* The deepest rewind history a context keeps. */
#define VVS_DPB_MAX_REWIND_DEPTH 16U

/* A resource index that names no resource. */
#define VVS_DPB_NO_RESOURCE 0xFFFFFFFFU

/* The dirtyIntraRefreshRegions of a picture encoded without intra refresh: its references chain no
 * VkVideoReferenceIntraRefreshInfoKHR, and later pictures see it with no dirty region. */
#define VVS_DPB_NO_INTRA_REFRESH 0xFFFFFFFFU

/* A DPB context. */
VK_DEFINE_HANDLE(VvsDpb)

/* ------------------------------------------------------------------------------------------------
 * What the client knows about a picture and its GOP
 * ---------------------------------------------------------------------------------------------- */

typedef enum VvsDpbPictureType {
    /* The codec's Std picture information alone says what the picture is. */
    VVS_DPB_PICTURE_TYPE_UNSPECIFIED = 0,
    /* Intra prediction only. */
    VVS_DPB_PICTURE_TYPE_INTRA = 1,
    /* References pictures displayed before it. */
    VVS_DPB_PICTURE_TYPE_PREDICTIVE = 2,
    /* References at least one picture displayed after it. */
    VVS_DPB_PICTURE_TYPE_BIPREDICTIVE = 3,
    /* Intra prediction only, and no later picture in display order references a picture displayed
     * before it: an H.264 or H.265 IDR picture, an AV1 key frame. */
    VVS_DPB_PICTURE_TYPE_KEY = 4,
    VVS_DPB_PICTURE_TYPE_MAX_ENUM = 0x7FFFFFFF
} VvsDpbPictureType;

typedef struct VvsDpbPictureInfo {
    VvsDpbPictureType pictureType;
    /* Zero-based, in display order: within the key-frame period, within the GOP, and within the run
     * of consecutive bipredictive pictures (0 for other pictures). */
    uint32_t keyFramePeriodIndex;
    uint32_t gopIndex;
    uint32_t bipredictiveFrameIndex;
    /* Relative indices in the stream; they may restart at a key picture. A picture is named by its
     * displayOrderIndex, unique within its key-frame period. */
    uint64_t encodeOrderIndex;
    uint64_t displayOrderIndex;
    uint32_t temporalLayerIndex;
} VvsDpbPictureInfo;

typedef enum VvsDpbGopStructure {
    /* No GOP information: the codec's Std picture information decides. */
    VVS_DPB_GOP_STRUCTURE_UNSPECIFIED = 0,
    /* The client may depart from the counts and patterns below. */
    VVS_DPB_GOP_STRUCTURE_IRREGULAR = 1,
    /* Every GOP follows the counts and patterns below. */
    VVS_DPB_GOP_STRUCTURE_REGULAR = 2,
    VVS_DPB_GOP_STRUCTURE_MAX_ENUM = 0x7FFFFFFF
} VvsDpbGopStructure;

/* Which pictures a predictive or bipredictive picture references, in display order. */
typedef enum VvsDpbReferencePattern {
    VVS_DPB_REFERENCE_PATTERN_UNSPECIFIED = 0,
    /* P: the previous intra or key picture. B: the previous and next intra, key or P pictures. */
    VVS_DPB_REFERENCE_PATTERN_FLAT = 1,
    /* The picture in the middle of the run references its ends; the pictures on either side
     * reference the middle one and the nearer end. */
    VVS_DPB_REFERENCE_PATTERN_MIDDLE = 2,
    /* P: the previous picture that is not bipredictive. B: the previous picture and the next intra,
     * key or P picture. */
    VVS_DPB_REFERENCE_PATTERN_SEQUENTIAL = 3,
    /* Alternates between FLAT and SEQUENTIAL from one picture of the run to the next. */
    VVS_DPB_REFERENCE_PATTERN_ALTERNATING = 4,
    /* MIDDLE, applied again to each half: hierarchical P or B pictures. */
    VVS_DPB_REFERENCE_PATTERN_DYADIC = 5,
    VVS_DPB_REFERENCE_PATTERN_MAX_ENUM = 0x7FFFFFFF
} VvsDpbReferencePattern;

typedef enum VvsDpbTemporalLayerPattern {
    VVS_DPB_TEMPORAL_LAYER_PATTERN_UNSPECIFIED = 0,
    VVS_DPB_TEMPORAL_LAYER_PATTERN_DYADIC = 1,
    VVS_DPB_TEMPORAL_LAYER_PATTERN_MAX_ENUM = 0x7FFFFFFF
} VvsDpbTemporalLayerPattern;

typedef struct VvsDpbGopInfo {
    VvsDpbGopStructure gopStructure;
    /* In pictures; 0 assumes nothing, UINT32_MAX is infinite. */
    uint32_t keyFramePeriod;
    uint32_t gopFrameCount;
    uint32_t consecutiveBipredictiveFrameCount;
    VvsDpbReferencePattern predictiveReferencePattern;
    VvsDpbReferencePattern bipredictiveReferencePattern;
    uint32_t temporalLayerCount;
    VvsDpbTemporalLayerPattern temporalLayerPattern;
} VvsDpbGopInfo;

/* ------------------------------------------------------------------------------------------------
 * Lifecycle, common to every codec
 * ---------------------------------------------------------------------------------------------- */

typedef enum VvsDpbCreateFlagBits {
    /* Set up a DPB slot for every picture, not only for references. The command of a non-reference
     * picture deactivates its slot, and the library frees the slot at once. */
    VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT = 0x00000001,
    /* For a picture encoded with intra refresh, chain a VkVideoReferenceIntraRefreshInfoKHR to every
     * reference slot, with the dirty regions the client gave for that reference picture: for sessions
     * that use VK_KHR_video_encode_intra_refresh. */
    VVS_DPB_CREATE_INTRA_REFRESH_BIT = 0x00000002,
    VVS_DPB_CREATE_FLAG_BITS_MAX_ENUM = 0x7FFFFFFF
} VvsDpbCreateFlagBits;
typedef VkFlags VvsDpbCreateFlags;

typedef enum VvsDpbSlotPolicy {
    /* The lowest inactive slot: slot N is set up only while slots 0 to N-1 are active. */
    VVS_DPB_SLOT_POLICY_LOWEST_FREE = 0,
    /* The next inactive slot after the last one set up, wrapping at the sequence's slot count: for
     * the slot-rotation tests, and to keep checkpoints usable for longer. */
    VVS_DPB_SLOT_POLICY_ROTATE = 1,
    VVS_DPB_SLOT_POLICY_MAX_ENUM = 0x7FFFFFFF
} VvsDpbSlotPolicy;

/* How the context gets the picture resource of each picture it sets up. */
typedef enum VvsDpbResourceMode {
    /* The context hands out the resources of the pool the client gives it. With as many resources as
     * the sequence has slots, resource i is always slot i's picture; with more, each setup takes the
     * resource released longest ago, so a released picture stays intact for resourceCount - slots more
     * setups. */
    VVS_DPB_RESOURCE_MODE_POOL = 0,
    /* The context asks the client for a resource each time it sets a picture up, and hands it back when
     * the picture leaves the DPB, through VvsDpbResourceCallbacks: the decode parser's model of
     * AllocPictureBuffer, AddRef and Release. */
    VVS_DPB_RESOURCE_MODE_CALLBACK = 1,
    VVS_DPB_RESOURCE_MODE_MAX_ENUM = 0x7FFFFFFF
} VvsDpbResourceMode;

/* Called by a picture call that sets the picture up, once, after the call has checked its inputs and
 * before it changes anything. Returns the client's index for the resource, any value but
 * VVS_DPB_NO_RESOURCE, and its VkVideoPictureResourceInfoKHR, which stays at its address as long as
 * the client keeps that index. An error makes the picture call return it and change nothing. */
typedef VkResult(VKAPI_PTR* PFN_vvsDpbAcquireResource)(void* pUserData, const VvsDpbPictureInfo* pPictureInfo,
                                                       uint32_t* pResourceIndex, const VkVideoPictureResourceInfoKHR** ppResource);
/* Called by vvsDpbRewind for each resource whose picture the DPB holds again after it was released. */
typedef void(VKAPI_PTR* PFN_vvsDpbRetainResource)(void* pUserData, uint32_t resourceIndex);
/* Called once for each resource whose picture leaves the DPB, after the call's changes are made. */
typedef void(VKAPI_PTR* PFN_vvsDpbReleaseResource)(void* pUserData, uint32_t resourceIndex);

/* The callbacks run on the calling thread, inside vvsDpbEncode*Picture, vvsDpbEncodeAV1ShowExistingFrame,
 * vvsDpbReset and vvsDpbRewind, and must not call into the same context. vvsDpbDestroy calls
 * none: a client that wants its resources back resets the context first. */
typedef struct VvsDpbResourceCallbacks {
    void* pUserData;
    PFN_vvsDpbAcquireResource pfnAcquireResource;
    PFN_vvsDpbRetainResource pfnRetainResource;
    PFN_vvsDpbReleaseResource pfnReleaseResource;
} VvsDpbResourceCallbacks;

typedef struct VvsDpbResources {
    VvsDpbResourceMode mode;
    /* POOL: the client's picture resources, by resource index: at least the sequence's maxDpbSlots, at
     * most VVS_DPB_MAX_RESOURCES. Each slot that holds a picture points to the picture's resource, with
     * its own coded offset and extent. The array is the client's and stays valid until a vvsDpbReset()
     * that replaces it, or vvsDpbDestroy(). NULL leaves every pPictureResource NULL; the client then
     * binds by the result's resourceIndices. */
    uint32_t resourceCount;
    const VkVideoPictureResourceInfoKHR* pResources;
    /* CALLBACK: all three callbacks. */
    VvsDpbResourceCallbacks callbacks;
} VvsDpbResources;

typedef struct VvsDpbCreateInfo {
    uint32_t apiVersion; /* VVS_DPB_API_VERSION */
    VvsDpbCreateFlags flags;
    /* The codec operation, with the codec's profile structure in the pNext chain. */
    const VkVideoProfileInfoKHR* pVideoProfile;
    /* The Std header the client builds against, as in VkVideoSessionCreateInfoKHR, or NULL. */
    const VkExtensionProperties* pStdHeaderVersion;
    /* The bound: VkVideoSessionCreateInfoKHR::maxDpbSlots and maxActiveReferencePictures of the
     * session the commands use, or 0 for the codec's maximum when the client sizes its session from
     * VvsDpbSequenceRequirements. Values above the codec's maximum are accepted; the library never
     * uses more than VVS_DPB_MAX_SLOTS. */
    uint32_t maxDpbSlots;
    uint32_t maxActiveReferencePictures;
    VvsDpbSlotPolicy slotPolicy;
    VvsDpbResources resources;
    /* The pictures vvsDpbRewind() can undo, from a history the context keeps; 0 for none. The context
     * never sets a picture up in a slot that held a reference within its last rewindDepth pictures, so
     * such a rewind always succeeds; VvsDpbSequenceRequirements counts the slots that takes. */
    uint32_t rewindDepth;
} VvsDpbCreateInfo;

/* Allocates the context once, with VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, sized for the codec and the
 * rewind depth.
 * VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR: not a codec operation the library plans.
 * VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR: pStdHeaderVersion names another codec, or a newer
 * version than the library's.
 * VK_ERROR_INCOMPATIBLE_DRIVER: an apiVersion of another major version, or a newer minor version.
 * VK_ERROR_VALIDATION_FAILED: CALLBACK mode without all three callbacks, a POOL pool of more than
 * VVS_DPB_MAX_RESOURCES resources, or a rewindDepth above VVS_DPB_MAX_REWIND_DEPTH.
 * VK_ERROR_OUT_OF_HOST_MEMORY. */
VVS_DPB_API VkResult vvsDpbCreate(const VvsDpbCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VvsDpb* pDpb);

/* dpb may be VK_NULL_HANDLE. */
VVS_DPB_API void vvsDpbDestroy(VvsDpb dpb, const VkAllocationCallbacks* pAllocator);

typedef struct VvsDpbResourceList {
    uint32_t count;
    uint32_t resourceIndices[VVS_DPB_MAX_SLOTS];
} VvsDpbResourceList;

/* What a reset may change: what a client changes when it creates its video session or its picture
 * resources again. */
typedef struct VvsDpbResetInfo {
    /* The new session's values, or 0 to keep the context's. */
    uint32_t maxDpbSlots;
    uint32_t maxActiveReferencePictures;
    /* New resources: a POOL pool with resourceCount above 0, or CALLBACK callbacks with
     * pfnAcquireResource set; otherwise the context's stay. The mode cannot change. */
    VvsDpbResources resources;
} VvsDpbResetInfo;

/* Drops every picture, without freeing the context: the next picture must be a key picture (an
 * H.264 or H.265 IDR picture, an AV1 key frame). The sequence and the GOP stay. For a rewind to a
 * key picture, an error or a session created again. pResetInfo may be NULL, which keeps the bound
 * and the resources. Every resource the DPB held is released: listed in pReleases, which may be
 * NULL, and in CALLBACK mode passed to pfnReleaseResource. The rewind history is dropped, and
 * checkpoints saved before the reset are refused.
 * VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR: the new bound is below the active sequence's
 * requirements. VK_ERROR_VALIDATION_FAILED: the mode changes, a POOL pool has fewer resources than
 * the sequence's slots or more than VVS_DPB_MAX_RESOURCES, or new CALLBACK callbacks are not all
 * three. Either way nothing changes. */
VVS_DPB_API VkResult vvsDpbReset(VvsDpb dpb, const VvsDpbResetInfo* pResetInfo, VvsDpbResourceList* pReleases);

/* A copy of a context's pictures and reference state, in storage the client owns: a point vvsDpbRewind()
 * can return to further back than rewindDepth. The sequence and the GOP are not in it. */
typedef struct VvsDpbCheckpoint {
    uint64_t opaque[VVS_DPB_CHECKPOINT_SIZE / 8U];
} VvsDpbCheckpoint;

VVS_DPB_API void vvsDpbSaveCheckpoint(VvsDpb dpb, VvsDpbCheckpoint* pCheckpoint);

/* Where vvsDpbRewind() returns to: exactly one of the two. */
typedef struct VvsDpbRewindInfo {
    /* Undo the last pictureCount picture calls, at most the context's rewindDepth: the state before the
     * first of them. Always succeeds within rewindDepth. */
    uint32_t pictureCount;
    /* Or return to this checkpoint's state, if no slot that held a reference at the checkpoint was set up
     * again since: the context counts writes per slot, and in POOL mode per resource. */
    const VvsDpbCheckpoint* pCheckpoint;
} VvsDpbRewindInfo;

/* Returns the context to an earlier state of its stream, for a rewind that is not to a key picture
 * (vvsDpbReset is that one): encoding a picture again as another type after a scene cut or a rate
 * control miss, or encoding the pictures after it differently. The resources of the pictures undone are
 * released, as by vvsDpbReset. CALLBACK: pfnRetainResource takes back each resource the DPB holds again,
 * so the client keeps a released resource intact while a rewind can still bring its picture back. A
 * rewind to a checkpoint drops the rewind history.
 * VK_ERROR_VALIDATION_FAILED, and nothing changes: both or neither target, a pictureCount above
 * rewindDepth or the pictures since the last reset, sequence call or rewind to a checkpoint, a
 * checkpoint of another context, a checkpoint saved before the last reset or sequence call, or a
 * checkpoint whose references were overwritten. */
VVS_DPB_API VkResult vvsDpbRewind(VvsDpb dpb, const VvsDpbRewindInfo* pRewindInfo, VvsDpbResourceList* pReleases);

/* What the active sequence needs, from the vvsDpb*Sequence() call that started it. A client that
 * creates its session after the sequence call sizes it from these. */
typedef struct VvsDpbSequenceRequirements {
    /* For VkVideoSessionCreateInfoKHR::maxDpbSlots: the references the SPS or the GOP keeps, and the
     * picture being encoded, so slot 16 only when 16 references are kept; plus, with a rewindDepth
     * above 0, rewindDepth more slots for H.265 and rewindDepth - 1 more for H.264 and AV1. */
    uint32_t maxDpbSlots;
    /* For VkVideoSessionCreateInfoKHR::maxActiveReferencePictures. */
    uint32_t maxActiveReferencePictures;
    /* References the DPB holds at once. */
    uint32_t maxReferencePictures;
} VvsDpbSequenceRequirements;

/* ------------------------------------------------------------------------------------------------
 * Per-picture results, common to every codec, written into storage the client owns
 * ---------------------------------------------------------------------------------------------- */

/* The codec's DPB slot information: pNext of each VkVideoReferenceSlotInfoKHR. */
typedef union VvsDpbCodecDpbSlotInfo {
    VkVideoEncodeH264DpbSlotInfoKHR h264;
    VkVideoEncodeH265DpbSlotInfoKHR h265;
    VkVideoEncodeAV1DpbSlotInfoKHR av1;
} VvsDpbCodecDpbSlotInfo;

/* The Std reference information each codec DPB slot information points to. */
typedef union VvsDpbStdReferenceInfo {
    StdVideoEncodeH264ReferenceInfo h264;
    StdVideoEncodeH265ReferenceInfo h265;
    StdVideoEncodeAV1ReferenceInfo av1;
} VvsDpbStdReferenceInfo;

/* The slots of one picture, ready for the video commands:
 *
 *   VkVideoBeginCodingInfoKHR::referenceSlotCount = beginCodingSlotCount
 *   VkVideoBeginCodingInfoKHR::pReferenceSlots    = referenceSlots
 *   VkVideoEncodeInfoKHR::referenceSlotCount      = referenceSlotCount
 *   VkVideoEncodeInfoKHR::pReferenceSlots         = referenceSlots
 *   VkVideoEncodeInfoKHR::pSetupReferenceSlot     = pSetupReferenceSlot
 *
 * referenceSlots[0, referenceSlotCount) are the references, each slot once. When the picture sets
 * up a slot, referenceSlots[referenceSlotCount] binds its resource for begin-coding with slotIndex
 * -1, and beginCodingSlotCount is referenceSlotCount + 1. setupReferenceSlot is the setup slot with
 * its index, for pSetupReferenceSlot.
 *
 * Entry i, for each referenceSlots entry in use, chains: referenceSlots[i].pNext = &dpbSlotInfos[i],
 * whose pStdReferenceInfo = &stdReferenceInfos[i]; with VVS_DPB_CREATE_INTRA_REFRESH_BIT, for a picture
 * encoded with intra refresh, a reference's dpbSlotInfos[i].pNext = &intraRefreshInfos[i].
 * setupReferenceSlot shares the setup entry's chain.
 * resourceIndices[i] names the resource of entry i: its index in the pool, or the index the client's
 * pfnAcquireResource returned; pPictureResource points to its VkVideoPictureResourceInfoKHR.
 * pictureInfos[i] is the VvsDpbPictureInfo of entry i's picture, written when the client gave one.
 * Only the entries in use are written.
 *
 * The pointers point into the structure itself: it must not be copied or moved while they are in
 * use. */
typedef struct VvsDpbPictureSlots {
    uint32_t referenceSlotCount;
    uint32_t beginCodingSlotCount;
    const VkVideoReferenceSlotInfoKHR* pSetupReferenceSlot; /* &setupReferenceSlot, or NULL */
    VkVideoReferenceSlotInfoKHR setupReferenceSlot;
    VkVideoReferenceSlotInfoKHR referenceSlots[VVS_DPB_MAX_BEGIN_CODING_SLOTS];
    uint32_t resourceIndices[VVS_DPB_MAX_BEGIN_CODING_SLOTS];
    VvsDpbCodecDpbSlotInfo dpbSlotInfos[VVS_DPB_MAX_BEGIN_CODING_SLOTS];
    VvsDpbStdReferenceInfo stdReferenceInfos[VVS_DPB_MAX_BEGIN_CODING_SLOTS];
    VkVideoReferenceIntraRefreshInfoKHR intraRefreshInfos[VVS_DPB_MAX_BEGIN_CODING_SLOTS];
    VvsDpbPictureInfo pictureInfos[VVS_DPB_MAX_BEGIN_CODING_SLOTS];
} VvsDpbPictureSlots;

typedef struct VvsDpbPictureResult {
    VvsDpbPictureSlots slots;
    /* Resources whose pictures leave the DPB with this call; in CALLBACK mode also passed to
     * pfnReleaseResource. A resource is reused once the commands that read it have completed. */
    VvsDpbResourceList releases;
} VvsDpbPictureResult;

/* One picture of the DPB, copied into the client's storage: for traces, tests and a client that
 * shows an existing AV1 frame. */
typedef struct VvsDpbReferenceFrame {
    int32_t slotIndex;
    uint32_t resourceIndex;
    VvsDpbPictureInfo pictureInfo;
    VvsDpbStdReferenceInfo stdReferenceInfo;
    /* AV1: the reference buffers that hold the picture, bit i for buffer i. */
    uint32_t av1BufferMask;
} VvsDpbReferenceFrame;

/* Vulkan's two-call idiom: with pReferenceFrames NULL, the count; otherwise up to
 * *pReferenceFrameCount entries, in slot order, VK_INCOMPLETE when more exist. */
VVS_DPB_API VkResult vvsDpbGetReferenceFrames(VvsDpb dpb, uint32_t* pReferenceFrameCount, VvsDpbReferenceFrame* pReferenceFrames);

/* ------------------------------------------------------------------------------------------------
 * H.264 encode
 * ---------------------------------------------------------------------------------------------- */

typedef struct VvsDpbH264SequenceInfo {
    /* Read: max_num_ref_frames, log2_max_frame_num_minus4, pic_order_cnt_type (0 or 2),
     * log2_max_pic_order_cnt_lsb_minus4. */
    const StdVideoH264SequenceParameterSet* pStdSps;
    /* Read: num_ref_idx_l0_default_active_minus1 and num_ref_idx_l1_default_active_minus1, the most
     * references the planner puts in each list. */
    const StdVideoH264PictureParameterSet* pStdPps;
    /* NULL: no GOP information. */
    const VvsDpbGopInfo* pGopInfo;
} VvsDpbH264SequenceInfo;

/* At an IDR picture that activates an SPS, or to change the GOP from the next key or intra
 * picture. The pointers are not kept. The rewind history is dropped, and checkpoints saved before
 * the call are refused.
 * VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR: the sequence needs more slots or references than the
 * context's bound, or pic_order_cnt_type is 1.
 * VK_ERROR_FEATURE_NOT_PRESENT: a GOP with reference or temporal layer patterns, which this version
 * does not plan. */
VVS_DPB_API VkResult vvsDpbH264Sequence(VvsDpb dpb, const VvsDpbH264SequenceInfo* pSequenceInfo,
                                        VvsDpbSequenceRequirements* pRequirements);

/* Storage for the reference syntax of an encoded H.264 picture. The library writes it and points the
 * Std structures into it. */
typedef struct VvsDpbH264EncodeSyntax {
    StdVideoEncodeH264ReferenceListsInfo refLists;
    StdVideoEncodeH264RefListModEntry refList0ModOperations[STD_VIDEO_H264_MAX_NUM_LIST_REF + 1U];
    StdVideoEncodeH264RefListModEntry refList1ModOperations[STD_VIDEO_H264_MAX_NUM_LIST_REF + 1U];
    StdVideoEncodeH264RefPicMarkingEntry refPicMarkingOperations[VVS_DPB_H264_MAX_MARKING_OPERATIONS];
} VvsDpbH264EncodeSyntax;

typedef struct VvsDpbH264EncodePictureInfo {
    /* The client's picture information, or NULL for a picture type of UNSPECIFIED. */
    const VvsDpbPictureInfo* pPictureInfo;
    /* VVS_DPB_CREATE_INTRA_REFRESH_BIT: the picture's dirty intra refresh regions once encoded, or
     * VVS_DPB_NO_INTRA_REFRESH for a picture encoded without intra refresh. */
    uint32_t dirtyIntraRefreshRegions;
    /* What VkVideoEncodeH264PictureInfoKHR::pStdPictureInfo points to.
     * Read: flags.IdrPicFlag, flags.long_term_reference_flag, primary_pic_type, PicOrderCnt,
     * temporal_id; flags.is_reference when the GOP has no reference pattern.
     * Written: frame_num; PicOrderCnt, the order count the decoding process derives for the
     * picture (8.2.1): for pic_order_cnt_type 0 from the low bits of the client's, for type 2 from
     * frame_num; flags.is_reference when the GOP has a reference pattern;
     * flags.adaptive_ref_pic_marking_mode_flag; pRefLists = &pSyntax->refLists, with
     * num_ref_idx_l0_active_minus1, num_ref_idx_l1_active_minus1, RefPicList0 and RefPicList1 as slot
     * indices, list modification and marking operations. */
    StdVideoEncodeH264PictureInfo* pStdPictureInfo;
    VvsDpbH264EncodeSyntax* pSyntax;
} VvsDpbH264EncodePictureInfo;

/* Plans the picture's references, writes the syntax, and applies it through the decoding process's
 * marking (8.2.5). Without a reference pattern, a P picture's list 0 is the initial list (8.2.4.2.1)
 * up to the PPS's default count; a B picture's lists are the initial lists (8.2.4.2.3) with one
 * entry in list 0, and up to the PPS's default count in list 1 when that list 0 count is one, else
 * one; references stay until the sliding window (8.2.5.3) drops them.
 * VK_ERROR_VALIDATION_FAILED, and nothing changes: no active sequence, a context of another codec,
 * a first picture after create or reset that is not an IDR picture, or a NULL pointer. */
VVS_DPB_API VkResult vvsDpbEncodeH264Picture(VvsDpb dpb, const VvsDpbH264EncodePictureInfo* pPictureInfo,
                                             VvsDpbPictureResult* pResult);

/* ------------------------------------------------------------------------------------------------
 * H.265 encode
 * ---------------------------------------------------------------------------------------------- */

typedef struct VvsDpbH265SequenceInfo {
    /* Read: sps_max_sub_layers_minus1, pDecPicBufMgr->max_dec_pic_buffering_minus1 of that sub-layer,
     * which bounds the references, and the num_short_term_ref_pic_sets sets of pShortTermRefPicSet,
     * which it derives once (7.4.8) to match each picture's RPS against. */
    const StdVideoH265SequenceParameterSet* pStdSps;
    /* Read: num_ref_idx_l0_default_active_minus1 and num_ref_idx_l1_default_active_minus1, the most
     * references the planner puts in each list. */
    const StdVideoH265PictureParameterSet* pStdPps;
    /* NULL: no GOP information. */
    const VvsDpbGopInfo* pGopInfo;
} VvsDpbH265SequenceInfo;

/* At an IDR picture that activates an SPS, or to change the GOP from the next key or intra
 * picture. The pointers are not kept. The rewind history is dropped, and checkpoints saved before
 * the call are refused.
 * VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR: the sequence needs more slots or references than the
 * context's bound, or a short-term set of the SPS is not valid.
 * VK_ERROR_FEATURE_NOT_PRESENT: a GOP with reference or temporal layer patterns, which this version
 * does not plan. */
VVS_DPB_API VkResult vvsDpbH265Sequence(VvsDpb dpb, const VvsDpbH265SequenceInfo* pSequenceInfo,
                                        VvsDpbSequenceRequirements* pRequirements);

/* Storage for the reference syntax of an encoded H.265 picture. */
typedef struct VvsDpbH265EncodeSyntax {
    StdVideoEncodeH265ReferenceListsInfo refLists;
    StdVideoH265ShortTermRefPicSet shortTermRefPicSet;
    StdVideoEncodeH265LongTermRefPics longTermRefPics;
} VvsDpbH265EncodeSyntax;

typedef struct VvsDpbH265EncodePictureInfo {
    /* The client's picture information, or NULL for a picture type of UNSPECIFIED. */
    const VvsDpbPictureInfo* pPictureInfo;
    /* VVS_DPB_CREATE_INTRA_REFRESH_BIT: the picture's dirty intra refresh regions once encoded, or
     * VVS_DPB_NO_INTRA_REFRESH for a picture encoded without intra refresh. */
    uint32_t dirtyIntraRefreshRegions;
    /* What VkVideoEncodeH265PictureInfoKHR::pStdPictureInfo points to.
     * Read: pic_type (P, B, I or IDR), flags.IrapPicFlag, PicOrderCntVal, TemporalId;
     * flags.is_reference when the GOP has no reference pattern.
     * Written: flags.is_reference when the GOP has a reference pattern;
     * pShortTermRefPicSet = &pSyntax->shortTermRefPicSet, the picture's RPS, and
     * flags.short_term_ref_pic_set_sps_flag and short_term_ref_pic_set_idx naming the SPS set that
     * equals it (0 and 0 when none does); pLongTermRefPics = &pSyntax->longTermRefPics, with no
     * long-term picture; pRefLists = &pSyntax->refLists for a P or B picture, NULL for an intra one,
     * with num_ref_idx_l0_active_minus1, num_ref_idx_l1_active_minus1, RefPicList0 and RefPicList1 as
     * slot indices, and no list modification. */
    StdVideoEncodeH265PictureInfo* pStdPictureInfo;
    VvsDpbH265EncodeSyntax* pSyntax;
} VvsDpbH265EncodePictureInfo;

/* Plans the picture's RPS (7.4.8) and lists (8.3.4), and applies the RPS through the decoding
 * process's marking (8.3.2). The planner keeps short-term references only. Without a reference
 * pattern:
 * - An IDR picture drops every reference; the first picture after a CRA picture in output order drops
 *   the pictures before the CRA picture; when the references fill sps_max_dec_pic_buffering_minus1,
 *   the oldest goes.
 * - The RPS holds every other reference of the picture's sub-layer or a lower one. A P picture uses
 *   the nearest ones before it, up to list 0's default count and 8; a B picture the nearest ones
 *   before it, up to list 0's default count and 7, and the nearest ones after it, up to list 1's
 *   default count and to 8 less list 0's default count, at least one. An IRAP picture uses none.
 * - The lists are the initial lists, at the PPS's default counts or at the pictures used when
 *   fewer.
 * VK_ERROR_VALIDATION_FAILED, and nothing changes: no active sequence, a context of another codec,
 * a first picture after create or reset that is not an IDR picture, a P or B picture with no
 * reference to use, or a NULL pointer. */
VVS_DPB_API VkResult vvsDpbEncodeH265Picture(VvsDpb dpb, const VvsDpbH265EncodePictureInfo* pPictureInfo,
                                             VvsDpbPictureResult* pResult);

/* ------------------------------------------------------------------------------------------------
 * AV1 encode
 * ---------------------------------------------------------------------------------------------- */

typedef struct VvsDpbAV1SequenceInfo {
    /* Read: enable_order_hint, order_hint_bits_minus_1, frame_id_numbers_present_flag,
     * delta_frame_id_length_minus_2, additional_frame_id_length_minus_1. */
    const StdVideoAV1SequenceHeader* pStdSequenceHeader;
    /* Read: the reference counts and reference name masks of each prediction mode, which bound the
     * names the planner assigns, and VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR. */
    const VkVideoEncodeAV1CapabilitiesKHR* pCapabilities;
    /* With VVS_DPB_CREATE_INTRA_REFRESH_BIT: maxIntraRefreshActiveReferencePictures bounds the references
     * a picture takes before it, and no picture takes one after it. NULL otherwise. */
    const VkVideoEncodeIntraRefreshCapabilitiesKHR* pIntraRefreshCapabilities;
    /* NULL: no GOP information, which plans for bipredictive pictures. Without patterns,
     * consecutiveBipredictiveFrameCount says whether the GOP has them. */
    const VvsDpbGopInfo* pGopInfo;
} VvsDpbAV1SequenceInfo;

/* At a key frame with a new sequence header, or to change the GOP from the next key or intra frame.
 * The pointers are not kept. The requirements count 8 slots: the buffers hold at most 7 pictures,
 * BWDREF_FRAME and ALTREF2_FRAME one. maxActiveReferencePictures is the context's bound, the most the
 * planner uses.
 * VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR: the sequence needs more slots than the context's bound.
 * VK_ERROR_FEATURE_NOT_PRESENT: a GOP with reference or temporal layer patterns. */
VVS_DPB_API VkResult vvsDpbAV1Sequence(VvsDpb dpb, const VvsDpbAV1SequenceInfo* pSequenceInfo,
                                       VvsDpbSequenceRequirements* pRequirements);

typedef struct VvsDpbAV1EncodePictureInfo {
    /* Required: pictureType tells a B picture from a P picture, and displayOrderIndex orders the
     * pictures, as the order hint does before it wraps. */
    const VvsDpbPictureInfo* pPictureInfo;
    /* VVS_DPB_CREATE_INTRA_REFRESH_BIT: the picture's dirty intra refresh regions once encoded, or
     * VVS_DPB_NO_INTRA_REFRESH for a picture encoded without intra refresh. */
    uint32_t dirtyIntraRefreshRegions;
    /* Without a reference pattern: VK_TRUE to keep the picture for reference, which the Std picture
     * information has no flag for. Key, intra, hidden and GOLDEN_FRAME pictures are kept anyway. */
    VkBool32 reference;
    /* Written in place: predictionMode, the mode the references allow (INTRA_ONLY for an intra
     * frame); rateControlGroup, BIPREDICTIVE for a B picture that has a reference after it, else
     * PREDICTIVE or INTRA; referenceNameSlotIndices, -1 for a name the picture does not use; and
     * primaryReferenceCdfOnly. */
    VkVideoEncodeAV1PictureInfoKHR* pPictureInfoAV1;
    /* What pPictureInfoAV1->pStdPictureInfo points to.
     * Read: frame_type, order_hint, current_frame_id, flags.error_resilient_mode, flags.show_frame.
     * Written: refresh_frame_flags, ref_frame_idx, ref_order_hint, primary_ref_frame,
     * delta_frame_id_minus_1 (zero for an intra frame). */
    StdVideoEncodeAV1PictureInfo* pStdPictureInfo;
} VvsDpbAV1EncodePictureInfo;

/* Maps the references to reference names and buffers, and applies the reference frame update
 * (7.20). Without a reference pattern, a picture is coded for one name: a key or shown intra frame
 * INTRA_FRAME, a hidden frame (show_frame 0, shown later with vvsDpbEncodeAV1ShowExistingFrame)
 * ALTREF_FRAME, a shown P picture of a GOP with B pictures GOLDEN_FRAME, another reference
 * LAST_FRAME. The names LAST_FRAME to LAST3_FRAME (to GOLDEN_FRAME without B pictures) hold the last
 * pictures, newest first; showing a hidden picture swaps it into GOLDEN_FRAME; a name whose picture is
 * too old for its order hint takes the picture too. The references are the pictures before it, nearest
 * first, up to 4, named LAST_FRAME to GOLDEN_FRAME, and for a B picture those after it, up to 3, named
 * BWDREF_FRAME to ALTREF_FRAME, within the prediction mode's names and counts and the session's active
 * references.
 * VK_ERROR_VALIDATION_FAILED, and nothing changes: no active sequence, a context of another codec,
 * a first picture after create or reset that is not a shown key frame, or a NULL pointer. */
VVS_DPB_API VkResult vvsDpbEncodeAV1Picture(VvsDpb dpb, const VvsDpbAV1EncodePictureInfo* pPictureInfo,
                                            VvsDpbPictureResult* pResult);

/* A frame header with show_existing_frame, for the picture with this displayOrderIndex: writes the
 * buffer to name in frame_to_show_map_idx, and moves the names as showing the picture does. A key
 * frame refreshes every buffer, as a decoder does. The picture's own fields (frame_type, RefFrameId)
 * are those vvsDpbGetReferenceFrames gives for the frame whose av1BufferMask holds the buffer.
 * VK_ERROR_VALIDATION_FAILED, and nothing changes: no buffer holds the picture. */
VVS_DPB_API VkResult vvsDpbEncodeAV1ShowExistingFrame(VvsDpb dpb, uint64_t displayOrderIndex, uint8_t* pFrameToShowMapIdx,
                                                      VvsDpbPictureResult* pResult);

/* ------------------------------------------------------------------------------------------------
 * Helpers: the results into the video commands, without copying
 * ---------------------------------------------------------------------------------------------- */

static inline void vvsDpbSetBeginCodingSlots(const VvsDpbPictureSlots* pSlots, VkVideoBeginCodingInfoKHR* pBeginInfo) {
    pBeginInfo->referenceSlotCount = pSlots->beginCodingSlotCount;
    pBeginInfo->pReferenceSlots = pSlots->referenceSlots;
}

static inline void vvsDpbSetEncodeSlots(const VvsDpbPictureSlots* pSlots, VkVideoEncodeInfoKHR* pEncodeInfo) {
    pEncodeInfo->pSetupReferenceSlot = pSlots->pSetupReferenceSlot;
    pEncodeInfo->referenceSlotCount = pSlots->referenceSlotCount;
    pEncodeInfo->pReferenceSlots = pSlots->referenceSlots;
}

#ifdef __cplusplus
}
#endif

#endif /* VVS_DPB_H_ */
