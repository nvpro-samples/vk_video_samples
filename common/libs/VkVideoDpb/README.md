# vkvideo-dpb: the DPB library of a Vulkan Video encoder

vkvideo-dpb manages the reference pictures of an encoded stream. For each picture it decides:

- which pictures the picture references;
- which pictures stay references after it, and for how long;
- the codec syntax that says so: H.264 `frame_num`, `PicOrderCnt` and reference lists, the H.265
  reference picture set (RPS) and lists, the AV1 reference names, buffers and refresh flags;
- the DPB slots and picture resources the video commands bind.

It writes these decisions into the structures that `vkCmdBeginVideoCodingKHR` and
`vkCmdEncodeVideoKHR` take, in storage the client owns, so nothing is converted or copied on the
way to the commands.

The library is written in C99 behind a C header, `vvs_dpb.h`, with a header-only C++17 layer,
`vvs_dpb.hpp`. It makes no Vulkan call and creates no Vulkan object, so it runs without a device:
its tests run on any machine. The H.264, H.265 and AV1 encoders of this repository use it for their
DPB (see [§16](#16-the-encoder-in-this-repository)).

| File | Content |
|---|---|
| [include/vkvideo_dpb/vvs_dpb.h](include/vkvideo_dpb/vvs_dpb.h) | The C interface: the contract. Every structure, call, rule and error is documented there |
| [include/vkvideo_dpb/vvs_dpb.hpp](include/vkvideo_dpb/vvs_dpb.hpp) | The C++17 layer: ownership, and one class per codec, so that a call for another codec does not compile |
| [src/vvs_dpb.c](src/vvs_dpb.c) | The calls common to every codec: lifecycle, slots, picture resources, rewind, checkpoints, results |
| [src/vvs_dpb_h264.c](src/vvs_dpb_h264.c), [src/vvs_dpb_h265.c](src/vvs_dpb_h265.c), [src/vvs_dpb_av1.c](src/vvs_dpb_av1.c) | The codec planners |
| [tests/vvs_dpb_test.c](tests/vvs_dpb_test.c) | The library's test, in C99: CTest `VkVideoDpbLibrary` |

## Contents

1. [What the library decides, and what the client decides](#1-what-the-library-decides-and-what-the-client-decides)
2. [Terms](#2-terms)
3. [The calls](#3-the-calls)
4. [A stream, step by step](#4-a-stream-step-by-step)
5. [Slots](#5-slots)
6. [Picture resources](#6-picture-resources)
7. [What the client gives](#7-what-the-client-gives)
8. [The results](#8-the-results)
9. [H.264](#9-h264)
10. [H.265](#10-h265)
11. [AV1](#11-av1)
12. [Rewind and checkpoints](#12-rewind-and-checkpoints)
13. [Intra refresh](#13-intra-refresh)
14. [Ownership, memory and threads](#14-ownership-memory-and-threads)
15. [Errors](#15-errors)
16. [The encoder in this repository](#16-the-encoder-in-this-repository)
17. [The C++ layer](#17-the-c-layer)
18. [Examples](#18-examples)
19. [Building and testing](#19-building-and-testing)
20. [Versions](#20-versions)

## 1. What the library decides, and what the client decides

| The library decides | The client decides |
|---|---|
| The references of each picture, from the pictures the DPB holds | The picture type, and whether a picture is a reference (H.264 and H.265 `flags.is_reference`, the AV1 `reference` member) |
| When a reference leaves the DPB: the H.264 sliding window, the H.265 RPS, the AV1 reference frame update | The picture order count (H.264 `PicOrderCnt`, H.265 `PicOrderCntVal`) and the AV1 `order_hint` |
| H.264 `frame_num` and the `PicOrderCnt` a decoder derives | The SPS, PPS and AV1 sequence header, and so the DPB size |
| The reference lists and their active counts | The slice header, including `num_ref_idx_active_override_flag` ([§8.3](#83-what-remains-for-the-client)) |
| The H.265 RPS, or the SPS set that equals it | Rate control, QP, the bitstream headers |
| The AV1 reference names, `refresh_frame_flags`, `ref_frame_idx`, `ref_order_hint`, `primary_ref_frame`, `delta_frame_id_minus_1`, and the prediction mode, rate control group and CDF-only flag | The video session, its `maxDpbSlots` and `maxActiveReferencePictures`, and the picture resources |
| The DPB slot of each picture, and which picture resource it binds | The command buffers, barriers and submission |

The library works one picture at a time, in encode order. It does not reorder pictures and does not
look ahead: the client brings each picture with its type and order count, as its GOP structure
decides them.

## 2. Terms

- **Context** (`VvsDpb`): the state of one stream of one codec operation, H.264, H.265 or AV1
  encode. A client encoding two streams creates two contexts.
- **Slot**: a DPB slot of the video session, the `slotIndex` of a `VkVideoReferenceSlotInfoKHR`.
- **Picture resource**: the image region a slot's picture is reconstructed into, a
  `VkVideoPictureResourceInfoKHR`. The client owns the images. The library names each resource by
  a **resource index**: its index in the client's pool, or the index the client's acquire
  callback returns.
- **Reference**: a picture the DPB holds for later pictures to reference. A slot that holds no
  reference is free.
- **Setup**: the picture being encoded is reconstructed into a slot, its **setup slot**
  (`VkVideoEncodeInfoKHR::pSetupReferenceSlot`). A reference picture always has one. A picture
  that is not a reference has one only with `VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT`.
- **Key picture**: an H.264 or H.265 IDR picture, an AV1 key frame. The first picture after
  `vvsDpbCreate` or `vvsDpbReset` must be one.
- **The bound**: the `maxDpbSlots` and `maxActiveReferencePictures` of the client's video session.
  The library never goes above it.
- **The count**: the slots and references the active sequence needs, which the sequence call
  reports in `VvsDpbSequenceRequirements`.
- **Release**: a picture leaves the DPB. Its resource is listed in the picture call's
  `releases`, and in callback mode passed to the client's release callback.

## 3. The calls

| | Call | What it does |
|---|---|---|
| Lifecycle | `vvsDpbCreate` | Allocates the context once, through `VkAllocationCallbacks` |
| | `vvsDpbDestroy` | Frees it. A null handle is allowed |
| | `vvsDpbReset` | Drops every picture: the next one must be a key picture. Can take a new bound and new resources |
| | `vvsDpbSaveCheckpoint` | Copies the context's pictures into a 2,048-byte checkpoint the client owns |
| | `vvsDpbRewind` | Undoes the last picture calls from the context's own history, or returns to a checkpoint |
| | `vvsDpbGetReferenceFrames` | Copies the pictures the DPB holds into client storage, with Vulkan's two-call idiom |
| H.264 | `vvsDpbH264Sequence` | Starts a sequence from its SPS and PPS, and reports its requirements |
| | `vvsDpbEncodeH264Picture` | Plans one picture: references, `frame_num`, `PicOrderCnt`, lists, slots |
| H.265 | `vvsDpbH265Sequence` | Starts a sequence from its SPS and PPS; derives the SPS's short-term sets once |
| | `vvsDpbEncodeH265Picture` | Plans one picture: the RPS, lists, slots |
| AV1 | `vvsDpbAV1Sequence` | Starts a sequence from its sequence header and the encode capabilities |
| | `vvsDpbEncodeAV1Picture` | Plans one frame: names, buffers, frame header reference fields, prediction mode, slots |
| | `vvsDpbEncodeAV1ShowExistingFrame` | Plans a frame header that shows a frame the buffers hold |
| Helpers | `vvsDpbSetBeginCodingSlots`, `vvsDpbSetEncodeSlots` | Point the begin-coding and encode information at a picture's slots: three pointer assignments, inline |

A context serves one codec operation: a call for another codec fails with
`VK_ERROR_VALIDATION_FAILED`, and with the C++ layer does not compile.

## 4. A stream, step by step

### 4.1 Create the context

`VvsDpbCreateInfo` names the codec (`pVideoProfile`), the bound, the slot policy, the picture
resources and the rewind depth.

- **The bound** is the session's `maxDpbSlots` and `maxActiveReferencePictures`. A client that
  creates its session after the sequence call passes 0 for both, and sizes its session from
  `VvsDpbSequenceRequirements` ([§5.1](#51-how-many-slots-and-who-decides)).
- **The slot policy** is `VVS_DPB_SLOT_POLICY_LOWEST_FREE` for most clients ([§5.2](#52-which-slot-a-picture-takes)).
- **The resources** come from a pool or from three callbacks ([§6](#6-picture-resources)). A pool
  may be empty at create, and given with the reset that follows the session's creation.
- **`rewindDepth`** is the number of picture calls `vvsDpbRewind` can undo, 0 for none
  ([§12](#12-rewind-and-checkpoints)).
- **`pStdHeaderVersion`**, when not NULL, must name the codec's Std encode header, at a version no
  newer than the one the library is built with.

`vvsDpbCreate` makes the one allocation of the context's life.

### 4.2 Start a sequence

`vvsDpbH264Sequence`, `vvsDpbH265Sequence` or `vvsDpbAV1Sequence` reads the parameters the planner
needs (the "Read" lists of [§9](#9-h264) to [§11](#11-av1)) and keeps copies of them: the
pointers are not kept. It fails with `VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR` when the sequence
needs more than the bound.

Call it before the first picture, and again for a new SPS, PPS or sequence header. A sequence call
keeps the pictures the DPB holds, so it goes before the key picture that starts the new sequence.
It drops the rewind history and makes earlier checkpoints unusable.

### 4.3 Plan each picture

In encode order, for each picture:

1. Fill the Std picture information as the encoder does anyway: the picture type, the order count,
   `is_reference`, the temporal id. These are the "Read" fields of the codec's picture call.
2. Call `vvsDpbEncode<Codec>Picture` with that Std structure, the syntax storage and the result
   storage. The library writes the "Written" fields, the syntax and the result.
3. On success the picture is in the DPB. On failure nothing changed: not the context, not the
   client's storage, and the resource callbacks are balanced.

### 4.4 Record the commands

```c
vvsDpbSetBeginCodingSlots(&result.slots, &beginInfo);  /* referenceSlotCount, pReferenceSlots */
vvsDpbSetEncodeSlots(&result.slots, &encodeInfo);      /* pSetupReferenceSlot, referenceSlotCount, pReferenceSlots */
vkCmdBeginVideoCodingKHR(commandBuffer, &beginInfo);
vkCmdEncodeVideoKHR(commandBuffer, &encodeInfo);
vkCmdEndVideoCodingKHR(commandBuffer, &endInfo);
```

`result.slots.resourceIndices[0, beginCodingSlotCount)` name the resources the commands read and
write, in the order of `referenceSlots`, for the client's barriers. [§8](#8-the-results) describes
the result entry by entry.

### 4.5 Take the releases

`result.releases` lists the resources whose pictures left the DPB with this call. The commands of
this same picture may still read them: a picture can reference a picture its own marking then drops
(the H.264 sliding window runs after the picture). So a released resource is written again only
behind the commands recorded for the call that released it. [§6.3](#63-when-a-released-resource-is-written-again)
says how each resource mode meets that.

### 4.6 Reset, change the sequence, destroy

- **`vvsDpbReset`** drops every picture and releases its resources. Use it to restart at a key
  picture, after an error, or when the session or the picture resources are created again: the
  reset then takes the new bound and resources. The sequence stays.
- **A sequence call** starts a new SPS, PPS or sequence header ([§4.2](#42-start-a-sequence)).
- **`vvsDpbDestroy`** frees the context. It calls no callback: a client that wants the resources
  back from a callback context resets it first.

## 5. Slots

### 5.1 How many slots, and who decides

**The client sets the bound; the library decides the count within it.**

| When | What is set |
|---|---|
| `vvsDpbCreate` | The bound: the session's `maxDpbSlots` and `maxActiveReferencePictures`, or 0 for no bound when the session is created from the requirements |
| `vvsDpb<Codec>Sequence` | The count, for the sequence that starts: reported in `VvsDpbSequenceRequirements`. `VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR` when it exceeds the bound |
| `vvsDpbReset` | A new bound, when the session is created again. Refused below the active sequence's requirements |

The count is the references the sequence keeps, plus one slot for the picture being encoded, plus
the slots the rewind depth takes:

| Codec | `maxReferencePictures` | `maxDpbSlots` |
|---|---|---|
| H.264 | `max_num_ref_frames` (at least 1) | references + 1, + `rewindDepth` − 1 when `rewindDepth` > 0 |
| H.265 | `sps_max_dec_pic_buffering_minus1` of the highest sub-layer | references + 1, + `rewindDepth` |
| AV1 | 7: the 8 reference buffers hold at most 7 frames, since `BWDREF_FRAME` and `ALTREF2_FRAME` are refreshed together | 8, + `rewindDepth` − 1 when `rewindDepth` > 0 |

So slot 16 exists only when 16 references are kept. No context uses more than
`VVS_DPB_MAX_SLOTS` (17) slots, and slot indices always stay below both the bound and the count.

`maxActiveReferencePictures`, the most slots one picture references:

| Codec | Requirement |
|---|---|
| H.264 | The larger of a P picture's (list 0's default count) and a B picture's (one list 0 entry, and one list 1 entry, or list 1's default count when list 0's default count is one), each at most the references |
| H.265 | The larger of a P picture's (list 0's default count, at most 8) and a B picture's ([§10.2](#102-picture)), at most the references |
| AV1 | 7, or the bound's `maxActiveReferencePictures` when lower: the planner takes no more than the session allows |

`VvsDpbSequenceRequirements` for a few sequences, as the library reports them:

| Sequence | `rewindDepth` 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| H.264, `max_num_ref_frames` 2 | 3 | 3 | 4 | 5 |
| H.265, `sps_max_dec_pic_buffering_minus1` 2 | 3 | 4 | 5 | 6 |
| AV1 | 8 | 8 | 9 | 10 |

### 5.2 Which slot a picture takes

A picture is set up in a slot that holds no reference. With a rewind depth, it also avoids every
slot that holds a reference in a state a rewind can return to ([§12.2](#122-a-rewind-within-the-depth-always-succeeds)).
Among those slots:

- **`VVS_DPB_SLOT_POLICY_LOWEST_FREE`**: the lowest one.
- **`VVS_DPB_SLOT_POLICY_ROTATE`**: the first one after the slot set up last, wrapping at the
  sequence's slot count. When only one slot is free, as in a steady stream of reference pictures,
  the two policies choose the same slot. When several are free, ROTATE spreads the setups over all
  of them: use it to test slot rotation, and to keep checkpoints usable for longer
  ([§12.3](#123-a-checkpoint-is-checked-never-trusted)).

When does a picture's slot get set up while its references are in other slots?

- **H.264 and AV1** mark after the picture: the H.264 sliding window and the AV1 reference frame
  update run once the picture is encoded. So the picture never takes the slot of a reference, even
  one its own marking drops.
- **H.265** applies the picture's RPS before the picture: the pictures the RPS drops leave first, and
  the picture can take one of their slots.

### 5.3 Pictures that are not references

By default a picture that is not a reference has no setup slot: its encode writes no reconstructed
picture. With `VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT` every picture is set up: its encode writes the
reconstructed picture into a slot, and since the picture is no reference, the slot holds no
reference afterwards. The library frees the slot, and releases its resource, in the same call.

## 6. Picture resources

The library never dereferences a resource. It carries the client's `VkVideoPictureResourceInfoKHR`
pointers into each `VkVideoReferenceSlotInfoKHR::pPictureResource`, and names each resource by its
index in `resourceIndices[]`. Two modes, chosen at create:

### 6.1 Pool mode

`VVS_DPB_RESOURCE_MODE_POOL`, the default: the client gives the context an array of
`VkVideoPictureResourceInfoKHR`, indexed 0 to N−1, and the context hands them out itself.

- N is at least the sequence's `maxDpbSlots` and at most `VVS_DPB_MAX_RESOURCES` (64). A context may
  be created with no pool, and get it from a reset; a picture call fails while the pool is
  smaller than the sequence's slots.
- **With N equal to the slots**, resource i is always slot i's picture: no free list, no lookup.
- **With N above the slots**, each setup takes the free resource released longest ago, so a
  released picture stays intact for N − slots more setups: for a client that reads reconstructed
  pictures after they leave the DPB, or whose commands of consecutive pictures overlap.
- The array stays valid, at the same address, until a reset that replaces it or `vvsDpbDestroy`.
- `pResources` may be NULL: every `pPictureResource` is then NULL, and the client binds its
  resources by `resourceIndices[]`.

### 6.2 Callback mode

`VVS_DPB_RESOURCE_MODE_CALLBACK`: the context asks the client for a resource each time it sets a
picture up, and hands it back when the picture leaves the DPB. It is the model of a decoder
parser's picture buffers: allocate, add a reference, release.

| Callback | When |
|---|---|
| `pfnAcquireResource` | Once per setup, inside the picture call, after the call has checked its inputs and before it changes anything. It returns the client's index for the resource and the resource's `VkVideoPictureResourceInfoKHR`, which stays at its address while the client keeps that index. An error fails the picture call, which returns it and changes nothing |
| `pfnRetainResource` | From `vvsDpbRewind`, for each resource whose picture the DPB holds again |
| `pfnReleaseResource` | Once for each resource whose picture leaves the DPB, after the call's changes are made: from a picture call, `vvsDpbReset` and `vvsDpbRewind` |

The rules:

- **The callbacks balance.** Each acquire and each retain is matched by exactly one release, except
  for the pictures the DPB holds when it is destroyed. A non-reference picture set up with
  `VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT` is acquired and released in the same call.
- **The index must be new.** An index of `VVS_DPB_NO_RESOURCE`, or of a resource the DPB holds, fails
  the picture call with `VK_ERROR_VALIDATION_FAILED`; an index the DPB holds is first passed back to
  `pfnReleaseResource`, so the client's count stays balanced.
- **Order:** `vvsDpbRewind` calls every retain before any release. A reset that replaces the
  callbacks releases the old pictures through the old callbacks.
- **Threads:** the callbacks run on the calling thread, inside the call, and must not call into
  the same context.
- `pPictureInfo` passed to `pfnAcquireResource` is the picture's `VvsDpbPictureInfo`, or one with
  every member zero when the client gave none.

### 6.3 When a released resource is written again

A resource in `releases` may still be in use by commands already recorded: those of the earlier
pictures that reference it, and those of the picture call that released it. The client writes it
again only behind those commands, as for any image it writes after reading:

- **Pool mode** reuses a released resource at a later setup. With N equal to the slots that is the
  next setup of the same slot. The client's barriers order the new write after the earlier reads
  and writes of the same resource, which `resourceIndices[]` name. With a larger pool the reuse
  comes N − slots setups later.
- **Callback mode** leaves the reuse to the client: its pool decides when a released resource is
  handed out again. The encoder in this repository keeps each resource with the frame whose
  commands use it until those commands complete ([§16](#16-the-encoder-in-this-repository)).

## 7. What the client gives

### 7.1 Per picture: `VvsDpbPictureInfo`

| Member | Meaning |
|---|---|
| `pictureType` | `KEY` (IDR picture, AV1 key frame), `INTRA`, `PREDICTIVE`, `BIPREDICTIVE`, or `UNSPECIFIED`: the Std picture information alone says what the picture is |
| `keyFramePeriodIndex`, `gopIndex`, `bipredictiveFrameIndex` | Zero-based positions, in display order: within the key-frame period, within the GOP, within the run of consecutive B pictures (0 for other pictures) |
| `encodeOrderIndex`, `displayOrderIndex` | Relative indices in the stream; they may restart at a key picture. A picture is named by its `displayOrderIndex`, unique within its key-frame period |
| `temporalLayerIndex` | The picture's temporal layer |

- **H.264 and H.265:** optional (`pPictureInfo` NULL). The planner reads the Std picture
  information. A picture information given is kept with the picture and returned in the result's
  `pictureInfos[]` for every entry that names it, and passed to `pfnAcquireResource`.
- **AV1:** required. `pictureType` tells a B picture (`BIPREDICTIVE`) from a P picture, and
  `displayOrderIndex` orders the frames: the planner sorts references by it, as the order hint does
  before it wraps, and `vvsDpbEncodeAV1ShowExistingFrame` finds a frame by it.

### 7.2 Per sequence: `VvsDpbGopInfo`

| Member | Meaning |
|---|---|
| `gopStructure` | `UNSPECIFIED`: no GOP information. `REGULAR`: every GOP follows the counts and patterns. `IRREGULAR`: the client may depart from them |
| `keyFramePeriod`, `gopFrameCount`, `consecutiveBipredictiveFrameCount` | In pictures; 0 assumes nothing, `UINT32_MAX` is infinite |
| `predictiveReferencePattern`, `bipredictiveReferencePattern` | Which pictures a P or B picture references, in display order ([vvs_dpb.h](include/vkvideo_dpb/vvs_dpb.h): `VvsDpbReferencePattern`) |
| `temporalLayerCount`, `temporalLayerPattern` | The temporal layers and how pictures are assigned to them |

The GOP goes with the sequence call (`pGopInfo`, NULL for none). **This version plans without
reference or temporal-layer patterns**: a GOP whose structure is not `UNSPECIFIED` and that sets a
reference pattern, a temporal-layer pattern or more than one temporal layer is refused with
`VK_ERROR_FEATURE_NOT_PRESENT`. Without patterns:

- H.264 and H.265 read the client's `is_reference`, and the GOP information changes nothing.
- AV1 reads `consecutiveBipredictiveFrameCount` from a GOP whose structure is not `UNSPECIFIED`: it
  says whether the stream has B pictures, which decides the names a P picture is coded for
  ([§11.2](#112-names-buffers-and-refresh)). Without GOP information the planner allows for B pictures.

## 8. The results

### 8.1 `VvsDpbPictureSlots`, entry by entry

For a picture with *n* references and a setup slot *s*:

```
referenceSlots[i]       slotIndex   pNext chain
  [0] .. [n-1]          reference   dpbSlotInfos[i] -> pStdReferenceInfo = &stdReferenceInfos[i]
                                    (+ intraRefreshInfos[i], see §13)
  [n]  (setup)          -1          dpbSlotInfos[n] -> pStdReferenceInfo = &stdReferenceInfos[n]
setupReferenceSlot      s           the same chain as [n]

VkVideoBeginCodingInfoKHR::referenceSlotCount = beginCodingSlotCount   (n + 1; n without a setup)
VkVideoBeginCodingInfoKHR::pReferenceSlots    = referenceSlots
VkVideoEncodeInfoKHR::referenceSlotCount      = referenceSlotCount     (n)
VkVideoEncodeInfoKHR::pReferenceSlots         = referenceSlots
VkVideoEncodeInfoKHR::pSetupReferenceSlot     = pSetupReferenceSlot    (&setupReferenceSlot, or NULL)
```

- **The references** come first, each slot once: list 0's, then list 1's (H.264, H.265), or the
  reference names' slots (AV1). Each carries the Std reference information of its picture as the
  picture was set up and marked.
- **The setup entry** follows them, for begin-coding only: `slotIndex` −1 binds the picture's
  resource to the session without activating a slot. The encode activates slot *s* through
  `pSetupReferenceSlot`, whose entry carries the picture's own Std reference information.
- **`resourceIndices[i]`** names entry i's resource; **`pictureInfos[i]`** is the
  `VvsDpbPictureInfo` of entry i's picture, written when the client gave one.
- **`dpbSlotInfos[]`** is a union of the three codecs' `VkVideoEncode*DpbSlotInfoKHR`, with its
  `sType` set; **`stdReferenceInfos[]`** a union of the three `StdVideoEncode*ReferenceInfo`.
- Only the entries in use are written, and `pictureInfos[i]` only when the client gave that
  picture's information: the entry of a picture without one keeps what the storage held.

### 8.2 Lifetime of the client's storage

- **`VvsDpbPictureResult`** holds pointers into itself: it must not be copied or moved while the
  video commands that use it are being recorded. Keep it with the frame, as the C++ layer forces
  (`vvs::dpb::PictureResult` cannot be copied). It is 2,640 bytes in a 64-bit build.
- **The syntax storage** (`VvsDpbH264EncodeSyntax`, `VvsDpbH265EncodeSyntax`) holds what the Std
  picture information points to: the lists, the RPS. It too stays in place until the commands are
  recorded.
- The library keeps no pointer to either: they are the client's from the moment the call returns.
  Vulkan reads them when the commands are recorded, so they may be reused once the picture's
  `vkCmdEncodeVideoKHR` is recorded.

### 8.3 What remains for the client

- **The slice header's `num_ref_idx_active_override_flag`** (H.264 and H.265): the library writes
  the active counts in the reference lists. The client sets the override flag when a count differs
  from the PPS's default, as the encoders in this repository do.
- **The AV1 picture type.** A B picture with no reference after it is coded as a P picture: the
  library writes the `PREDICTIVE` rate control group for it, and the client takes the picture as a
  P picture from there.
- **AV1 show-existing frames** are frame headers, not encodes: `vvsDpbEncodeAV1ShowExistingFrame`
  gives the buffer to show, and the client writes the frame header ([§11.5](#115-show-an-existing-frame)).
- **Barriers and layouts** of the resources, from `resourceIndices[]`.

## 9. H.264

### 9.1 Sequence

`vvsDpbH264Sequence` reads:
- SPS: `max_num_ref_frames`, `log2_max_frame_num_minus4`, `pic_order_cnt_type`,
  `log2_max_pic_order_cnt_lsb_minus4`;
- PPS: `num_ref_idx_l0_default_active_minus1` and `num_ref_idx_l1_default_active_minus1`, the most
  references the planner puts in each list.

It refuses (`VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR`) `pic_order_cnt_type` 1, a
`log2_max_frame_num_minus4` or `log2_max_pic_order_cnt_lsb_minus4` above 12, more than 16
references, and a sequence above the bound.

### 9.2 Picture

`vvsDpbEncodeH264Picture` reads from `StdVideoEncodeH264PictureInfo`: `flags.IdrPicFlag`,
`flags.is_reference`, `flags.long_term_reference_flag`, `primary_pic_type`, `PicOrderCnt`,
`temporal_id`. It writes:

- **`frame_num`**: the number of reference pictures from the IDR picture up to this one, this one
  not counted, modulo `MaxFrameNum` (7.4.3). So a picture that is not a reference has the
  `frame_num` of the reference that follows it.
- **`PicOrderCnt`**: the order count a decoder derives for the picture (8.2.1). For
  `pic_order_cnt_type` 0, from the low `log2_max_pic_order_cnt_lsb` bits of the client's value, so
  the client may count on without wrapping; for type 2, from `frame_num`.
- **`pRefLists`** = `&pSyntax->refLists`: `num_ref_idx_l0_active_minus1`,
  `num_ref_idx_l1_active_minus1`, and `RefPicList0` and `RefPicList1` as slot indices.
- **`flags.adaptive_ref_pic_marking_mode_flag`** = 0: the sliding window does the marking, and the
  lists are the initial lists, so the planner writes no list modification and no marking
  operation.

The rules:

- An IDR picture is always a reference, and drops every reference before it. It is a long-term
  reference when `long_term_reference_flag` is set; no other picture becomes one.
- Another picture is a reference when `flags.is_reference` is set.
- **A P picture**'s list 0 is the initial list (8.2.4.2.1): the short-term references by
  descending `PicNum` (`FrameNumWrap`), then the long-term ones by ascending `LongTermPicNum`, up to
  list 0's default count.
- **A B picture**'s lists are the initial lists (8.2.4.2.3): by `PicOrderCnt`, the nearest before
  the picture first in list 0 and the nearest after it first in list 1. List 0 takes one entry;
  list 1 takes one entry, or up to list 1's default count when list 0's default count is one. When
  list 1 equals list 0 and has more than one entry, its first two entries are swapped.
- **An I picture** has no lists.
- **After a reference picture** that is not an IDR picture, the sliding window (8.2.5.3) drops the
  short-term reference with the smallest `FrameNumWrap` when the references fill
  `max_num_ref_frames`.

Worked example: IPPPP with `max_num_ref_frames` 2 and list 0's default count 2, a pool of 3
resources, `LOWEST_FREE`.

| Picture | `frame_num` | `PicOrderCnt` | `RefPicList0` (slots) | Setup slot (resource) | `releases` |
|---|---|---|---|---|---|
| I0 (IDR) | 0 | 0 | | 0 (0) | |
| P1 | 1 | 2 | 0 | 1 (1) | |
| P2 | 2 | 4 | 1, 0 | 2 (2) | 0 |
| P3 | 3 | 6 | 2, 1 | 0 (0) | 1 |
| P4 | 4 | 8 | 0, 2 | 1 (1) | 2 |

P2 references I0, and its sliding window then drops I0: P2's call releases resource 0, which P2's
own commands read ([§6.3](#63-when-a-released-resource-is-written-again)). P3 sets it up again.

Worked example: I B B P in encode order I0 P3 B1 B2 P6 B4 B5, the B pictures not references, with
`max_num_ref_frames` 2, both list defaults 1, `VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT` and a pool of
3 resources.

| Picture | `is_reference` | `frame_num` | `PicOrderCnt` | List 0 | List 1 | Setup slot (resource) | `releases` |
|---|---|---|---|---|---|---|---|
| I0 (IDR) | 1 | 0 | 0 | | | 0 (0) | |
| P3 | 1 | 1 | 6 | 0 | | 1 (1) | |
| B1 | 0 | 2 | 2 | 0 | 1 | 2 (2) | 2 |
| B2 | 0 | 2 | 4 | 0 | 1 | 2 (2) | 2 |
| P6 | 1 | 2 | 12 | 1 | | 2 (2) | 0 |
| B4 | 0 | 3 | 8 | 1 | 2 | 0 (0) | 0 |
| B5 | 0 | 3 | 10 | 1 | 2 | 0 (0) | 0 |

The B pictures are set up and released in the same call. With as many resources as slots, B1 and B2
write the same resource; a pool of 4 or more would give them different ones.

## 10. H.265

### 10.1 Sequence

`vvsDpbH265Sequence` reads:
- SPS: `sps_max_sub_layers_minus1`; `pDecPicBufMgr->max_dec_pic_buffering_minus1` of the highest
  sub-layer, which bounds the references; and the `num_short_term_ref_pic_sets` sets of
  `pShortTermRefPicSet`, which it derives once (7.4.8), predicted sets included, to match each
  picture's RPS against;
- PPS: `num_ref_idx_l0_default_active_minus1` and `num_ref_idx_l1_default_active_minus1`.

It refuses a NULL `pDecPicBufMgr` (`VK_ERROR_VALIDATION_FAILED`), and a short-term set that is not
valid, 16 or more references, or a sequence above the bound
(`VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR`). It does not read
`log2_max_pic_order_cnt_lsb_minus4` or the long-term pictures: the planner keeps short-term
references only.

### 10.2 Picture

`vvsDpbEncodeH265Picture` reads from `StdVideoEncodeH265PictureInfo`: `pic_type` (P, B, I or IDR),
`flags.IrapPicFlag`, `flags.is_reference`, `PicOrderCntVal`, `TemporalId`. It writes:

- **`pShortTermRefPicSet`** = `&pSyntax->shortTermRefPicSet`: the picture's own RPS, always; and
  `flags.short_term_ref_pic_set_sps_flag` and `short_term_ref_pic_set_idx` naming the SPS set that
  equals it, or 0 and 0 when none does. The client codes the slice header from those.
- **`pLongTermRefPics`** = `&pSyntax->longTermRefPics`, with no long-term picture.
- **`pRefLists`** = `&pSyntax->refLists` for a P or B picture, NULL for an I or IDR picture:
  `num_ref_idx_l0_active_minus1`, `num_ref_idx_l1_active_minus1`, and `RefPicList0` and
  `RefPicList1` as slot indices, with no list modification.

The rules, in the order the decoding process applies them before the picture (8.3.2):

1. **An IDR picture** drops every reference.
2. **The first picture after an I picture in output order** (`PicOrderCntVal` above the I
   picture's) drops the pictures before the I picture: the planner takes an I picture as a CRA
   picture, whose leading pictures still reference the pictures before it.
3. **When the DPB holds more references** than `sps_max_dec_pic_buffering_minus1`, the oldest
   short-term reference by `PicOrderCntVal` leaves.
4. **The RPS** holds every other short-term reference of the picture's temporal sub-layer or a lower
   one: those before the picture by descending `PicOrderCntVal`, those after it by ascending. The
   farthest before it leaves first when they do not fit. A reference the RPS does not name leaves
   the DPB.
5. **The pictures the picture uses** (`used_by_curr_pic` flags), nearest first:
   - a P picture: before it, up to list 0's default count and 8;
   - a B picture: before it, up to list 0's default count and 7; after it, up to list 1's default
     count and 8 less list 0's default count, at least one;
   - an IRAP picture (`flags.IrapPicFlag`) uses none. It keeps its RPS, so a CRA picture keeps the
     pictures its leading pictures need.

   A P or B picture with no picture to use is refused with `VK_ERROR_VALIDATION_FAILED`.
6. **The lists** are the initial lists (8.3.4), cut to their active counts: list 0 from the
   pictures used before the picture, then after it; list 1 from those after, then before. List 0's
   count is the PPS's default count, or the number of pictures used before the picture when that is
   fewer (at least one); list 1's likewise, with the pictures used after it.

Worked example: I B B P in encode order I0 P3 B1 B2 P6 B4 B5 P9, the B pictures not references,
`sps_max_dec_pic_buffering_minus1` 2, both list defaults 1. RPS entries are the delta
`PicOrderCntVal`, with * for the pictures used.

| Picture | RPS before | RPS after | List 0 | List 1 | Setup slot | `releases` |
|---|---|---|---|---|---|---|
| I0 (IDR) | | | | | 0 | |
| P3 | −3* | | 0 | | 1 | |
| B1 | −1* | +2* | 0 | 1 | | |
| B2 | −2* | +1* | 0 | 1 | | |
| P6 | −3*, −6 | | 1 | | 2 | |
| B4 | −1* | +2* | 1 | 2 | | 0 |
| B5 | −2* | +1* | 1 | 2 | | |
| P9 | −3*, −6 | | 2 | | 0 | |

After P6 the DPB holds I0, P3 and P6: as many as `sps_max_dec_pic_buffering_minus1` + 1. B4's RPS
can hold two of them, so I0 leaves the DPB before B4 is encoded, and B4's call releases it.

## 11. AV1

### 11.1 Sequence

`vvsDpbAV1Sequence` reads:
- the sequence header: `flags.enable_order_hint`, `order_hint_bits_minus_1`,
  `flags.frame_id_numbers_present_flag`, `delta_frame_id_length_minus_2`,
  `additional_frame_id_length_minus_1`;
- `pCapabilities`, the session's `VkVideoEncodeAV1CapabilitiesKHR`: the reference counts and
  reference name masks of each prediction mode, which bound the names the planner assigns, and
  `VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR`;
- `pIntraRefreshCapabilities`, with `VVS_DPB_CREATE_INTRA_REFRESH_BIT`
  ([§13](#13-intra-refresh));
- `pGopInfo`'s `consecutiveBipredictiveFrameCount` ([§7.2](#72-per-sequence-vvsdpbgopinfo)).

The requirements are 8 slots and 7 references ([§5.1](#51-how-many-slots-and-who-decides)).

### 11.2 Names, buffers and refresh

A frame is coded for one reference name, which says what it is:

| Frame | Name |
|---|---|
| A key frame, or a shown intra-only frame | `INTRA_FRAME`: refreshes every buffer when it is a shown key frame |
| A hidden frame (`show_frame` 0), shown later with `vvsDpbEncodeAV1ShowExistingFrame` | `ALTREF_FRAME` |
| A shown P frame, in a stream with B pictures | `GOLDEN_FRAME` |
| Another frame that is a reference | `LAST_FRAME` |
| A frame that is not a reference | None: refreshes no buffer |

A frame is a reference when the client sets `reference`; key, intra, hidden and `GOLDEN_FRAME`
frames are kept anyway.

- The name decides which names' buffers the frame refreshes (`refresh_frame_flags`), and how the
  names then move between the buffers.
- The names `LAST_FRAME` to `LAST3_FRAME` hold the last frames, newest first; without B pictures,
  `LAST_FRAME` to `GOLDEN_FRAME`. A frame coded or shown as one of them refreshes the buffer of the
  oldest and moves it to `LAST_FRAME`.
- Showing a hidden frame swaps its buffer into `GOLDEN_FRAME`.
- A name whose frame is too old for its order hint, or its frame id, to tell it from a newer frame
  takes the frame too (5.9.4).
- A slot holds a reference while a buffer holds its frame.

### 11.3 References and prediction mode

The references of an inter frame:
- before it in display order, nearest first, up to 4, named `LAST_FRAME` to `GOLDEN_FRAME`;
- for a B picture, after it, nearest first, up to 3, named `BWDREF_FRAME` to `ALTREF_FRAME`;
- within the names and counts the capabilities give the prediction mode, and within the session's
  `maxActiveReferencePictures`. When they do not fit, the farthest reference of the larger group
  goes first.

The planner then writes:
- **`predictionMode`**: `BIDIRECTIONAL_COMPOUND` with references on both sides;
  `UNIDIRECTIONAL_COMPOUND` with two or more `LAST_FRAME`-group references, or `BWDREF_FRAME` and
  `ALTREF_FRAME`; `SINGLE_REFERENCE` otherwise; `INTRA_ONLY` for an intra frame. A mode the
  capabilities do not support falls back to the next one.
- **`rateControlGroup`**: `INTRA` for an intra frame, `BIPREDICTIVE` for a B picture with a
  reference after it, `PREDICTIVE` otherwise ([§8.3](#83-what-remains-for-the-client)).
- **`referenceNameSlotIndices`**: the slot of each name the frame uses, −1 for the others.
- **`primary_ref_frame`**: the name of the last frame of the same kind, whose CDFs the frame starts
  from; `PRIMARY_REF_NONE` for an intra frame, with `error_resilient_mode`, or when no buffer holds
  such a frame. When the capabilities have
  `VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR` and that frame is not one of the
  references, it is added as one more reference for its CDFs only, and `primaryReferenceCdfOnly`
  is set. Without that capability it must be one of the references, through its own name or
  another that holds it, or the frame has none.
- **The frame header's fields**: `refresh_frame_flags`, `ref_frame_idx`, `ref_order_hint`,
  `delta_frame_id_minus_1` (zero for an intra frame).

### 11.4 Picture

`vvsDpbEncodeAV1Picture` reads `frame_type`, `order_hint`, `current_frame_id`,
`flags.error_resilient_mode` and `flags.show_frame` from `StdVideoEncodeAV1PictureInfo`, and
`pictureType` and `displayOrderIndex` from the required `pPictureInfo`. The first frame after create
or reset must be a shown key frame. A shown key frame and a switch frame refresh every buffer.

Worked example: IPPPPP, no B pictures (`consecutiveBipredictiveFrameCount` 0), capabilities with
one single reference, two unidirectional compound references and every name in every mask.

| Frame | `refresh_frame_flags` | References (name: slot) | `predictionMode` | `primary_ref_frame` | Setup slot | `releases` |
|---|---|---|---|---|---|---|
| K0 | 0xFF | | `INTRA_ONLY` | none | 0 | |
| P1 | 0x10 | LAST: 0 | `SINGLE_REFERENCE` | none | 1 | |
| P2 | 0x08 | LAST: 1, LAST2: 0 | `UNIDIRECTIONAL_COMPOUND` | LAST | 2 | |
| P3 | 0x04 | LAST: 2, LAST2: 1 | `UNIDIRECTIONAL_COMPOUND` | LAST | 3 | |
| P4 | 0x02 | LAST: 3, LAST2: 2 | `UNIDIRECTIONAL_COMPOUND` | LAST | 4 | |
| P5 | 0x10 | LAST: 4, LAST2: 3 | `UNIDIRECTIONAL_COMPOUND` | LAST | 5 | 1 |

Each frame refreshes the buffer of the oldest of the last four frames: P5 refreshes P1's buffer, and
P1 leaves the DPB. K0 stays in the buffers of `BWDREF_FRAME`, `ALTREF2_FRAME` and `ALTREF_FRAME`, so it
stays a reference.

Worked example: B pictures, encode order K0 P3 B1 B2, show P3, P6 B4 B5, show P6, with
`consecutiveBipredictiveFrameCount` 2. Each P frame is encoded hidden before the B pictures that
precede it in display order, and shown after them.

| Frame | `show_frame` | `refresh_frame_flags` | References (name: slot) | `predictionMode` | `rateControlGroup` | Setup slot |
|---|---|---|---|---|---|---|
| K0 | 1 | 0xFF | | `INTRA_ONLY` | `INTRA` | 0 |
| P3 | 0 | 0x80 | LAST: 0 | `SINGLE_REFERENCE` | `PREDICTIVE` | 1 |
| B1 | 1 | 0x00 | LAST: 0, ALTREF: 1 | `BIDIRECTIONAL_COMPOUND` | `BIPREDICTIVE` | |
| B2 | 1 | 0x00 | LAST: 0, ALTREF: 1 | `BIDIRECTIONAL_COMPOUND` | `BIPREDICTIVE` | |
| show P3 | | `frame_to_show_map_idx` 7 | | | | |
| P6 | 0 | 0x08 | LAST: 0, GOLDEN: 1 | `UNIDIRECTIONAL_COMPOUND` | `PREDICTIVE` | 2 |
| B4 | 1 | 0x00 | GOLDEN: 1, ALTREF: 2 | `BIDIRECTIONAL_COMPOUND` | `BIPREDICTIVE` | |
| B5 | 1 | 0x00 | GOLDEN: 1, ALTREF: 2 | `BIDIRECTIONAL_COMPOUND` | `BIPREDICTIVE` | |
| show P6 | | `frame_to_show_map_idx` 3 | | | | |

Showing P3 swaps its buffer into `GOLDEN_FRAME`, so P6 and the B pictures after it reference P3 as
`GOLDEN_FRAME`.

### 11.5 Show an existing frame

`vvsDpbEncodeAV1ShowExistingFrame` plans a frame header with `show_existing_frame` 1 for the frame
with a `displayOrderIndex`:

- It writes the buffer that holds the frame to `*pFrameToShowMapIdx`, for `frame_to_show_map_idx`.
- It moves the names as showing the frame does. Showing a key frame refreshes every buffer
  (7.21), as a decoder does.
- The frame's own fields for the header (`frame_type`, `RefFrameId`) are those
  `vvsDpbGetReferenceFrames` gives for the frame whose `av1BufferMask` holds that buffer.
- The result has no references and no setup: the client writes the frame header and records no
  encode. `releases` lists the frames that leave the buffers.
- It fails with `VK_ERROR_VALIDATION_FAILED`, and changes nothing, when no buffer holds the frame.

## 12. Rewind and checkpoints

### 12.1 Three ways back

| Back to | Call |
|---|---|
| A key picture: the start of the stream, a new IDR picture, after an error | `vvsDpbReset`: drops every picture; the sequence stays |
| One of the last pictures: encode a picture again as another type after a scene cut or a rate control miss, or encode the pictures after it differently | `vvsDpbRewind` with `pictureCount` N, at most the `rewindDepth` set at create: the context undoes its last N picture calls from a history it keeps |
| Any earlier point | `vvsDpbSaveCheckpoint` there, into a `VvsDpbCheckpoint` the client owns; later `vvsDpbRewind` with `pCheckpoint` |

`VvsDpbRewindInfo` names exactly one target. A rewind releases the resources of the pictures it
undoes, as a reset does, and in callback mode retains the resources of the pictures it brings back.

**Not a rewind:** encoding the same picture again with only another QP needs no call. The DPB's
decisions do not depend on the QP, so the first result stays valid, and the second encode writes
the same setup slot.

### 12.2 A rewind within the depth always succeeds

- A picture call never sets its picture up in a slot that holds a reference in a state a rewind
  can return to: the state before the call, and the states before the `rewindDepth` − 1 calls
  before it. So no reference a rewind brings back has been overwritten.
- That takes slots, which the sequence requirements count ([§5.1](#51-how-many-slots-and-who-decides)):
  H.265 applies the RPS before the picture, so a rewind of N pictures takes N more slots; H.264
  and AV1 mark after the picture, so it takes N − 1 more. A one-picture rewind costs H.265 one slot,
  and H.264 and AV1 none.
- In pool mode, a setup also avoids the resources of those states.
- The history is sized at create, inside the one allocation: about 1.6 KB per picture of
  `rewindDepth` in a 64-bit build. Without a rewind depth nothing is recorded.
- `pictureCount` counts picture calls, `vvsDpbEncodeAV1ShowExistingFrame` included, since the last
  reset, sequence call or rewind to a checkpoint. A larger count fails with
  `VK_ERROR_VALIDATION_FAILED`.
- In callback mode, the client keeps a released resource intact while a rewind can still bring its
  picture back: through the next `rewindDepth` − 1 picture calls after the one that released it.

Worked example: H.264 IPPP with `max_num_ref_frames` 2, `rewindDepth` 1 and a pool of 3; P3 is
encoded again as an I picture.

| Call | Setup slot (resource) | `releases` |
|---|---|---|
| I0 (IDR) | 0 (0) | |
| P1 | 1 (1) | |
| P2 | 2 (2) | 0 |
| P3 | 0 (0) | 1 |
| `vvsDpbRewind`, `pictureCount` 1 | | 0: P3's |
| I3, the same picture as an I picture | 0 (0) | 1 |
| `vvsDpbRewind`, `pictureCount` 2 | `VK_ERROR_VALIDATION_FAILED`: beyond `rewindDepth` | |

The rewind brings P1 back, which P3 had dropped; I3 drops it again.

### 12.3 A checkpoint is checked, never trusted

- A checkpoint is plain data, 2,048 bytes: copy it, keep any number of them, drop them. It holds the
  context's pictures, not the sequence or the GOP.
- The context counts the setups of each slot, and in pool mode of each resource. A checkpoint
  records the counts of the slots that hold its references.
- Returning to it fails with `VK_ERROR_VALIDATION_FAILED`, and changes nothing, when one of those
  slots or resources was set up again since: that reference is gone from the video session. It
  also fails for a checkpoint of another context, and for one saved before the last reset or
  sequence call.
- A checkpoint stays usable until a slot that held one of its references is set up again. With a
  steady stream of reference pictures that is soon: in an IPPP stream with one reference, a
  checkpoint saved after the IDR picture is refused two pictures later. A rewind depth adds slots,
  and `VVS_DPB_SLOT_POLICY_ROTATE` spreads the setups when several slots are free.
- In callback mode the context cannot see the client's reuse of a resource: a client that wants to
  return to a checkpoint keeps the resources of the checkpoint's pictures intact.
- A rewind to a checkpoint drops the rewind history.

## 13. Intra refresh

For sessions that use `VK_KHR_video_encode_intra_refresh`, create the context with
`VVS_DPB_CREATE_INTRA_REFRESH_BIT`, and give each picture call `dirtyIntraRefreshRegions`:

- the number of the picture's regions not yet refreshed once it is encoded, for a picture encoded
  with intra refresh;
- `VVS_DPB_NO_INTRA_REFRESH` for a picture encoded without it.

For a picture encoded with intra refresh, each reference entry of the result chains a
`VkVideoReferenceIntraRefreshInfoKHR` (`intraRefreshInfos[i]`) with the dirty regions of that
reference picture. The setup entry chains none. A picture encoded without intra refresh chains none
on its references, because a reference with dirty regions requires an encode with
`VK_VIDEO_ENCODE_INTRA_REFRESH_BIT_KHR` (`VUID-vkCmdEncodeVideoKHR-pNext-10842`). Later pictures
see a picture encoded without intra refresh as having no dirty region.

AV1 reads `VkVideoEncodeIntraRefreshCapabilitiesKHR` in the sequence call: under intra refresh,
`maxIntraRefreshActiveReferencePictures` bounds the references before a frame, and no frame takes a
reference after it.

## 14. Ownership, memory and threads

**The library owns** one allocation per context, made by `vvsDpbCreate` through
`VkAllocationCallbacks` with `VK_SYSTEM_ALLOCATION_SCOPE_OBJECT` and an alignment of 16: about
8 KB in a 64-bit build, plus about 1.6 KB per picture of `rewindDepth`. Every array in it has a fixed
capacity. No other call allocates.

**The client owns:**
- the results and the syntax storage ([§8.2](#82-lifetime-of-the-clients-storage));
- the Std picture information, which the library reads and writes in place;
- the checkpoints;
- the picture resources: the pool, or what its callbacks hand out.

**Pointers.** The library keeps no pointer into its own state in the client's storage, and keeps
only three kinds of pointer into the client's storage: the pool array, the resources
`pfnAcquireResource` returns, and the callbacks' `pUserData`. It keeps no pointer to a structure a
call is given: the sequence's parameters are copied.

**Static data.** The library has no static or global variable; its only static data are constant
tables.

**Threads.** Contexts share nothing, so any number of them run on any threads without locks: H.264
and AV1 streams side by side, or a lookahead context beside the main one. One context is externally
synchronized: the client never calls into it from two threads at once. The callbacks run on the
calling thread.

**Determinism.** The same calls with the same inputs give the same slots, syntax and releases.

## 15. Errors

**A call that fails changes nothing:** not the context, not the client's storage, and no callback
is left unbalanced.

| Code | When |
|---|---|
| `VK_SUCCESS` | |
| `VK_INCOMPLETE` | `vvsDpbGetReferenceFrames` with an array too small for every picture |
| `VK_ERROR_OUT_OF_HOST_MEMORY` | `vvsDpbCreate`: the allocation failed |
| `VK_ERROR_INCOMPATIBLE_DRIVER` | `vvsDpbCreate`: an `apiVersion` of another major version, or of a newer minor version |
| `VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR` | `vvsDpbCreate`: not H.264, H.265 or AV1 encode |
| `VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR` | `vvsDpbCreate`: `pStdHeaderVersion` names another codec's Std header, or a newer version than the library's |
| `VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR` | A sequence call: parameters the planner does not take, or a sequence above the bound. `vvsDpbReset`: a new bound below the active sequence's requirements |
| `VK_ERROR_FEATURE_NOT_PRESENT` | A sequence call: a GOP with reference or temporal-layer patterns ([§7.2](#72-per-sequence-vvsdpbgopinfo)) |
| `VK_ERROR_VALIDATION_FAILED` | Misuse: a NULL pointer the call needs; a call for another codec; a picture call with no active sequence, with a pool smaller than the sequence's slots, or whose first picture after create or reset is not a key picture; an H.265 P or B picture with nothing to reference; callback mode without its three callbacks; a pool above `VVS_DPB_MAX_RESOURCES`; a `rewindDepth` above `VVS_DPB_MAX_REWIND_DEPTH`; an unknown slot policy or resource mode; a reset that changes the resource mode, or whose pool is smaller than the active sequence's slots; a rewind beyond the history, or to a checkpoint that cannot be returned to; an acquired index that is `VVS_DPB_NO_RESOURCE` or already held; a show-existing frame no buffer holds |
| `VK_ERROR_TOO_MANY_OBJECTS` | A picture call: no slot within the sequence's count is free for the setup |
| `VK_ERROR_OUT_OF_POOL_MEMORY` | A picture call in pool mode: no free resource that the rewind history cannot bring back |
| The error `pfnAcquireResource` returns | A picture call in callback mode |

## 16. The encoder in this repository

The H.264, H.265 and AV1 encoders (`vk_video_encoder/libs/VkVideoEncoder/VkVideoEncoderH264.cpp`,
`VkVideoEncoderH265.cpp`, `VkVideoEncoderAV1.cpp`) each own one context, `vvs::dpb::H264Encode`,
`H265Encode` or `AV1Encode`:

- **Create**, in `InitEncoderCodec`: callback mode, `VVS_DPB_CREATE_SETUP_EVERY_PICTURE_BIT`,
  `VVS_DPB_CREATE_INTRA_REFRESH_BIT` when intra refresh is on, `LOWEST_FREE`, and the encoder's DPB
  picture count and the device's `maxActiveReferencePictures` as the bound. Then the sequence call:
  without GOP information for H.264 and H.265, and for AV1 with an `IRREGULAR` GOP that gives the
  GOP size and the B-frame count.
- **Resources:** `VkEncoderDpbPictures` (`VkEncoderDpbPictures.h`) puts the encoder's DPB image pool
  behind the three callbacks. The resource index is the image's index in the pool, and each frame
  holds the images its commands read and write until it completes, so a released image is reused
  only behind them.
- **Per frame**, in `ProcessDpb`: one picture call, with the frame's Std picture information, its
  syntax storage and its `vvs::dpb::PictureResult`; then `vvs::dpb::SetEncodeSlots`, and the
  begin-coding list from `slots.referenceSlots` and `slots.beginCodingSlotCount`. The encoder sets
  `num_ref_idx_active_override_flag` from the written list counts; the AV1 encoder takes a B picture
  whose rate control group is not `BIPREDICTIVE` as a P picture, and plans its show-existing frames
  with `ShowExistingFrame` and `GetReferenceFrames`.

The device-free CTests `EncoderDpbSequencingH264`, `EncoderDpbSequencingH265` and
`EncoderDpbSequencingAv1` (`vk_video_encoder/test/encoder-dpb/`) make the calls the encoder makes over
many GOP configurations, and compare the traces with goldens and with a model of each codec's
decoding process.

## 17. The C++ layer

`vvs_dpb.hpp`, in namespace `vvs::dpb`, is header-only and uses the C structures as they are:

| Type | What it adds |
|---|---|
| `Context` | Owns a `VvsDpb`: destroys it, moves, cannot be copied. `Reset`, `SaveCheckpoint`, `Rewind`, `RewindPictures(count)`, `RewindTo(checkpoint)`, `GetReferenceFrames` |
| `H264Encode`, `H265Encode`, `AV1Encode` | A `Context` for one codec operation: `Create` refuses a profile of another codec, and `Sequence` and `Picture` take only that codec's structures; `AV1Encode` adds `ShowExistingFrame` |
| `PictureResult`, `H264EncodeSyntax`, `H265EncodeSyntax` | The C storage, zero-initialized, that cannot be copied or moved, since it points into itself |
| `Checkpoint` | `VvsDpbCheckpoint`: plain data |
| `SetBeginCodingSlots`, `SetEncodeSlots` | The C helpers, by reference |

No exceptions, no RTTI, no static or global variables. A failed `Create` leaves the object as it
was; a successful one destroys the context it held before.

## 18. Examples

The examples below compile as shown, the first with a C++17 compiler and the second as C99, against
the headers of this directory; the client's own code is a comment or a parameter.

### 18.1 H.264 in C++, pool mode, the session sized from the requirements

```cpp
#include <vector>

#include "vkvideo_dpb/vvs_dpb.hpp"

// A Vulkan structure with its sType set and every other member zero.
template <class T>
T VkStruct(VkStructureType sType) {
    T value{};
    value.sType = sType;
    return value;
}

// What the encoder keeps per picture. The library writes the slots, the lists and the Std fields in
// place, and the video commands point into the same storage.
struct EncodeFrame {
    vvs::dpb::PictureResult dpb;        // slots, resources and releases
    vvs::dpb::H264EncodeSyntax syntax;  // reference lists
    StdVideoEncodeH264PictureInfo stdPictureInfo{};
    VkVideoEncodeH264PictureInfoKHR h264PictureInfo =
        VkStruct<VkVideoEncodeH264PictureInfoKHR>(VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR);
    VkVideoBeginCodingInfoKHR beginInfo = VkStruct<VkVideoBeginCodingInfoKHR>(VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR);
    VkVideoEncodeInfoKHR encodeInfo = VkStruct<VkVideoEncodeInfoKHR>(VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR);
};

// Once per stream: the context, then the sequence. The client creates its video session from the
// requirements, so the context starts with no bound and no pool.
VkResult StartStream(vvs::dpb::H264Encode& dpb, const VkVideoProfileInfoKHR& profile, const StdVideoH264SequenceParameterSet& sps,
                     const StdVideoH264PictureParameterSet& pps, VvsDpbSequenceRequirements& requirements) {
    VvsDpbCreateInfo createInfo{};
    createInfo.apiVersion = VVS_DPB_API_VERSION;
    createInfo.pVideoProfile = &profile;
    createInfo.slotPolicy = VVS_DPB_SLOT_POLICY_LOWEST_FREE;
    createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_POOL;
    VkResult result = dpb.Create(createInfo);
    if (result != VK_SUCCESS) {
        return result;
    }
    const VvsDpbH264SequenceInfo sequenceInfo{&sps, &pps, nullptr};
    return dpb.Sequence(sequenceInfo, &requirements);
}

// Once the session exists, with requirements.maxDpbSlots slots, and one picture resource per slot:
// the bound and the pool. With as many resources as slots, resource i is always slot i's picture.
VkResult GivePictures(vvs::dpb::H264Encode& dpb, const VkVideoSessionCreateInfoKHR& session,
                      const std::vector<VkVideoPictureResourceInfoKHR>& pictures) {
    VvsDpbResetInfo resetInfo{};
    resetInfo.maxDpbSlots = session.maxDpbSlots;
    resetInfo.maxActiveReferencePictures = session.maxActiveReferencePictures;
    resetInfo.resources.mode = VVS_DPB_RESOURCE_MODE_POOL;
    resetInfo.resources.resourceCount = static_cast<uint32_t>(pictures.size());
    resetInfo.resources.pResources = pictures.data();
    return dpb.Reset(&resetInfo);
}

// Once per picture, in encode order. The client fills the Std fields it decides; the library decides
// the references and writes frame_num, PicOrderCnt, the lists and the slots.
VkResult PrepareFrame(vvs::dpb::H264Encode& dpb, EncodeFrame& frame, StdVideoH264PictureType type, bool reference,
                      int32_t picOrderCnt) {
    frame.stdPictureInfo.flags.IdrPicFlag = (type == STD_VIDEO_H264_PICTURE_TYPE_IDR) ? 1U : 0U;
    frame.stdPictureInfo.flags.is_reference = reference ? 1U : 0U;
    frame.stdPictureInfo.primary_pic_type = type;
    frame.stdPictureInfo.PicOrderCnt = picOrderCnt;
    VvsDpbH264EncodePictureInfo pictureInfo{};
    pictureInfo.dirtyIntraRefreshRegions = VVS_DPB_NO_INTRA_REFRESH;
    pictureInfo.pStdPictureInfo = &frame.stdPictureInfo;
    pictureInfo.pSyntax = &frame.syntax;
    const VkResult result = dpb.Picture(pictureInfo, frame.dpb);
    if (result != VK_SUCCESS) {
        return result;
    }
    frame.h264PictureInfo.pStdPictureInfo = &frame.stdPictureInfo;
    frame.encodeInfo.pNext = &frame.h264PictureInfo;
    vvs::dpb::SetBeginCodingSlots(frame.dpb.slots, frame.beginInfo);
    vvs::dpb::SetEncodeSlots(frame.dpb.slots, frame.encodeInfo);
    // Then, with the rest of the encode information the client's own:
    //   vkCmdBeginVideoCodingKHR(cmd, &frame.beginInfo);
    //   vkCmdEncodeVideoKHR(cmd, &frame.encodeInfo);
    //   vkCmdEndVideoCodingKHR(cmd, &endInfo);
    // frame.dpb.slots.resourceIndices[0, beginCodingSlotCount) name the pictures the commands read and
    // write, for the client's barriers.
    return VK_SUCCESS;
}
```

### 18.2 H.265 in C, callback mode, a rewind after a scene cut

```c
#include <string.h>

#include "vkvideo_dpb/vvs_dpb.h"

/* The client's pictures behind the three callbacks: a free list that hands out the resource
 * released longest ago, so a resource a rewind can bring back is the last one handed out again. */
#define POOL_SIZE 8U
typedef struct ClientPool {
    VkVideoPictureResourceInfoKHR resources[POOL_SIZE];
    uint32_t freeIndices[POOL_SIZE];
    uint32_t freeCount;
} ClientPool;

static VkResult VKAPI_CALL AcquireResource(void* pUserData, const VvsDpbPictureInfo* pPictureInfo, uint32_t* pResourceIndex,
                                           const VkVideoPictureResourceInfoKHR** ppResource) {
    ClientPool* pPool = (ClientPool*)pUserData;
    (void)pPictureInfo;
    if (pPool->freeCount == 0U) {
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    *pResourceIndex = pPool->freeIndices[0];
    pPool->freeCount--;
    memmove(&pPool->freeIndices[0], &pPool->freeIndices[1], pPool->freeCount * sizeof(pPool->freeIndices[0]));
    *ppResource = &pPool->resources[*pResourceIndex];
    return VK_SUCCESS;
}

/* A rewind brought the resource's picture back into the DPB: it leaves the free list. */
static void VKAPI_CALL RetainResource(void* pUserData, uint32_t resourceIndex) {
    ClientPool* pPool = (ClientPool*)pUserData;
    for (uint32_t i = 0; i < pPool->freeCount; i++) {
        if (pPool->freeIndices[i] == resourceIndex) {
            pPool->freeCount--;
            memmove(&pPool->freeIndices[i], &pPool->freeIndices[i + 1U], (pPool->freeCount - i) * sizeof(pPool->freeIndices[0]));
            return;
        }
    }
}

/* The picture left the DPB. The commands recorded for the picture call that released it may still read
 * it, so the client writes it again only behind them; the back of the free list is behind them here. */
static void VKAPI_CALL ReleaseResource(void* pUserData, uint32_t resourceIndex) {
    ClientPool* pPool = (ClientPool*)pUserData;
    pPool->freeIndices[pPool->freeCount++] = resourceIndex;
}

typedef struct Frame {
    VvsDpbPictureResult dpb;
    VvsDpbH265EncodeSyntax syntax;
    StdVideoEncodeH265PictureInfo stdPictureInfo;
} Frame;

/* rewindDepth 1: the context can undo its last picture call, and never sets a picture up in a slot
 * that held a reference before that call. */
VkResult CreateRewindableDpb(const VkVideoProfileInfoKHR* pProfile, uint32_t maxDpbSlots, uint32_t maxActiveReferencePictures,
                             ClientPool* pPool, VvsDpb* pDpb) {
    VvsDpbCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.apiVersion = VVS_DPB_API_VERSION;
    createInfo.pVideoProfile = pProfile;
    createInfo.maxDpbSlots = maxDpbSlots;
    createInfo.maxActiveReferencePictures = maxActiveReferencePictures;
    createInfo.slotPolicy = VVS_DPB_SLOT_POLICY_LOWEST_FREE;
    createInfo.resources.mode = VVS_DPB_RESOURCE_MODE_CALLBACK;
    createInfo.resources.callbacks.pUserData = pPool;
    createInfo.resources.callbacks.pfnAcquireResource = AcquireResource;
    createInfo.resources.callbacks.pfnRetainResource = RetainResource;
    createInfo.resources.callbacks.pfnReleaseResource = ReleaseResource;
    createInfo.rewindDepth = 1;
    return vvsDpbCreate(&createInfo, NULL, pDpb);
}

static VkResult PlanPicture(VvsDpb dpb, Frame* pFrame) {
    VvsDpbH265EncodePictureInfo info;
    memset(&info, 0, sizeof(info));
    info.dirtyIntraRefreshRegions = VVS_DPB_NO_INTRA_REFRESH;
    info.pStdPictureInfo = &pFrame->stdPictureInfo;
    info.pSyntax = &pFrame->syntax;
    return vvsDpbEncodeH265Picture(dpb, &info, &pFrame->dpb);
}

/* Records the picture's commands, submits them and returns the size of its bitstream: the client's
 * own code. */
typedef uint32_t (*EncodeFn)(const Frame* pFrame, void* pUserData);

/* Encodes a P picture; when its bitstream says the scene changed, drops it, undoes its picture call
 * and encodes the picture again as a CRA picture. */
VkResult EncodeWithSceneCut(VvsDpb dpb, Frame* pFrame, uint32_t sceneCutBytes, EncodeFn encode, void* pUserData) {
    VkResult result = PlanPicture(dpb, pFrame);
    if (result != VK_SUCCESS) {
        return result;
    }
    if (encode(pFrame, pUserData) <= sceneCutBytes) {
        return VK_SUCCESS;
    }
    VvsDpbRewindInfo rewindInfo;
    memset(&rewindInfo, 0, sizeof(rewindInfo));
    rewindInfo.pictureCount = 1;
    result = vvsDpbRewind(dpb, &rewindInfo, NULL);
    if (result != VK_SUCCESS) {
        return result;
    }
    pFrame->stdPictureInfo.pic_type = STD_VIDEO_H265_PICTURE_TYPE_I;
    pFrame->stdPictureInfo.flags.IrapPicFlag = 1;
    result = PlanPicture(dpb, pFrame);
    if (result != VK_SUCCESS) {
        return result;
    }
    (void)encode(pFrame, pUserData);
    return VK_SUCCESS;
}
```

## 19. Building and testing

- **CMake:** `common/libs/VkVideoDpb/CMakeLists.txt` builds `vkvideo-dpb`, a static library, as C99
  with hidden symbols and position-independent code; link it and add its `include/` directory, and
  include `vkvideo_dpb/vvs_dpb.h` or `vkvideo_dpb/vvs_dpb.hpp`. The Vulkan headers it needs are the
  build's.
- **`VkVideoDpbLibrary`** (CTest, label `device-free`, `tests/vvs_dpb_test.c`): from C99, the
  lifecycle and its errors, both resource modes, rewind and checkpoints, that a failed call leaves
  the context as it was, and short sequences of each codec worked out by hand.
- **`EncoderDpbSequencingH264`, `H265`, `Av1`** ([§16](#16-the-encoder-in-this-repository)): the
  encoder's calls over GOP configurations, against goldens and a model of the decoding process.
- **`scripts/check_dpb_library_c.sh [out dir]`** compiles every source and the test on their own, as
  ISO C99 with warnings as errors, against the pinned Vulkan headers and without CMake, and runs the
  test. `CC=clang` picks the compiler; `SANITIZE=1` adds AddressSanitizer and
  UndefinedBehaviorSanitizer; `VULKAN_HEADERS_DIR` uses a local Vulkan-Headers checkout.
- **CI:** the `dpb-library-c` job of `.github/workflows/test.yml` runs the script with gcc and clang,
  with and without the sanitizers, and compiles the library and its test as C with MSVC
  (`/TC /W4 /WX`).

## 20. Versions

`VvsDpbCreateInfo::apiVersion` is the interface version the client is built against:
`VVS_DPB_API_VERSION`, made by `VVS_DPB_MAKE_API_VERSION(major, minor, patch)` and currently 0.2.0.
`vvsDpbCreate` accepts the library's major version with its minor version or an older one, and
refuses another major version or a newer minor version with `VK_ERROR_INCOMPATIBLE_DRIVER`.
