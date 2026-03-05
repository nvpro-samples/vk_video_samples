# Vulkan Validation Layer Bug Report: Incorrect DPB Slot Activation Tracking

## Summary

`VK_LAYER_KHRONOS_validation` fires false-positive VUID-vkCmdBeginVideoCodingKHR-pPictureResource-07265 errors during normal H.264 video decode DPB slot management. The validation layer does not properly track DPB slot activations performed by `vkCmdDecodeVideoKHR`'s `pSetupReferenceSlot`.

## Environment

- Vulkan SDK: 1.4.x (Windows)
- GPU: NVIDIA RTX 5000 Ada Generation Laptop GPU
- Driver: 572.x
- Codec: H.264 Baseline/Main/High, 1280x720, YCbCr 420

## Reproduction

```bash
vk-video-dec-test.exe -i input.mov -v -vv
```

Any H.264 stream with more than `maxDpbSlots` frames triggers the error when DPB slots are reused.

## Observed Behavior

The validation layer fires VUID-07265 in two scenarios:

### Scenario 1: First-time slot activation (false positive)

```
Frame 0 (IDR):
  CmdBeginVideoCodingKHR: pReferenceSlots = [{ slotIndex=0, image=A }]
  CmdDecodeVideoKHR: pSetupReferenceSlot = { slotIndex=0, image=A }

  → VUID-07265: "DPB slot index 0 is not currently associated with
    the specified video picture resource: VkImage A"
```

Slot 0 has **never been activated** — there is no prior association to mismatch. The validation layer incorrectly requires a pre-existing association for a slot being activated for the first time.

### Scenario 2: Slot reuse with different image

```
Frame 0: setup slot=0 → Image_A   (slot 0 activated with Image_A)
Frame 1: setup slot=1 → Image_B   (slot 1 activated with Image_B)
...
Frame N: setup slot=0 → Image_C   (parser evicted old slot 0, reuses it)
  CmdBeginVideoCodingKHR: pReferenceSlots includes { slotIndex=0, image=C }
  
  → VUID-07265: "slot 0 is currently associated with Image_A, but
    you specified Image_C"
```

The slot reassociation is performed by `CmdDecodeVideoKHR`'s `pSetupReferenceSlot` within the same video coding scope. Per the spec, this is valid.

## Expected Behavior (Per Spec)

### §40.1 — Video Coding Scope

> "The set of bound reference picture resources is immutable within a video coding
> scope, however, **the DPB slot index associated with any of the bound reference
> picture resources can change during the video coding scope** in response to video
> coding operations."

### §40.1 — BeginVideoCoding Slot Interpretation

> "If `slotIndex` is non-negative and `pPictureResource` is not NULL, then the video
> picture resource [...] is **added to the set of bound reference picture resources
> and is associated with the DPB slot** index specified in `slotIndex`."

This text describes activation/re-association as part of `CmdBeginVideoCodingKHR`,
yet VUID-07265 prohibits it when the slot has a prior (different) association.

### §40.1 — DPB Slot Activation

> "A DPB slot can be activated with a new frame even if it is already active. In
> this case all previous associations of the DPB slots with reference pictures are
> replaced with an association with the reconstructed picture used to activate it."

## Attempted Workaround: slotIndex=-1

Per spec §40.1:

> "If `slotIndex` is negative and `pPictureResource` is not NULL, then the video
> picture resource [...] is added to the set of bound reference picture resources
> **without an associated DPB slot**. Such a picture resource can be subsequently
> used as a **reconstructed picture to associate it with a DPB slot**."

We attempted to use `slotIndex=-1` for setup slots in `CmdBeginVideoCodingKHR`,
keeping the real slot index only in `CmdDecodeVideoKHR`'s `pSetupReferenceSlot`.
This is spec-valid: the image is bound (satisfying VUID-07149), VUID-07265 is
skipped (only checks non-negative slotIndex), and the slot activation happens
during decode.

**Result:** The validation layer does NOT track the slot activation from
`CmdDecodeVideoKHR`'s `pSetupReferenceSlot` when the slot was listed with
`slotIndex=-1` in `BeginVideoCoding`. Subsequent frames that reference that slot
as a DPB reference also fail VUID-07265 because the layer never recorded the
activation.

This confirms the validation layer only tracks slot state from
`CmdBeginVideoCodingKHR`'s `pReferenceSlots`, not from `CmdDecodeVideoKHR`'s
`pSetupReferenceSlot` — contradicting the spec's §40.1 description of slot
lifecycle.

## Root Cause in Validation Layer

The validation layer appears to:

1. Track DPB slot → image associations only from `vkCmdBeginVideoCodingKHR`'s
   `pReferenceSlots` entries with non-negative `slotIndex`
2. NOT track slot activations/re-associations performed by `vkCmdDecodeVideoKHR`'s
   `pSetupReferenceSlot`
3. NOT handle first-time slot activation (no prior association exists)

## Proposed Fix for Validation Layer

1. When `vkCmdDecodeVideoKHR` is called with a non-NULL `pSetupReferenceSlot`,
   update the internal DPB slot tracking to associate `slotIndex` with the
   specified `pPictureResource`. This is the primary slot activation mechanism
   per the spec.

2. For VUID-07265, do not fire when the specified `slotIndex` has no prior
   association (first-time activation).

3. Alternatively, relax VUID-07265 to allow `CmdBeginVideoCodingKHR` to perform
   re-association (consistent with the spec narrative in §40.1).

## Impact

The errors are cosmetic — NVIDIA's driver correctly handles DPB slot
re-association regardless. The decoded video output is correct. No corruption
or crashes observed.
