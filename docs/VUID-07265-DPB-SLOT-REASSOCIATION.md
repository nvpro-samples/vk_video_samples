# Validation Layer False Positive: VUID-07265 (DPB Slot Activation)

## TL;DR

`VK_LAYER_KHRONOS_validation` fires false-positive VUID-07265 errors during
normal H.264 video decode. The sample's DPB management is **spec-correct**.
The validation layer has incomplete DPB slot state tracking — it does not
properly handle first-time slot activation or slot re-association via
`CmdDecodeVideoKHR`'s `pSetupReferenceSlot`.

The error is suppressed in `g_ignoredValidationMessageIds` (MessageID 0xa9049dc2).
A detailed bug report for VK_LAYER_KHRONOS_validation is in
`docs/VK_LAYER_VALIDATION_BUG_REPORT-DPB-SLOT-TRACKING.md`.

## What the Sample Does (Correctly)

The decoder follows the standard Vulkan Video decode pattern:

1. `CmdBeginVideoCodingKHR` declares the set of images and DPB slots for the scope
2. `CmdDecodeVideoKHR` decodes a frame, with `pSetupReferenceSlot` establishing
   the association between a DPB slot index and the decoded image
3. `CmdEndVideoCodingKHR` ends the scope

On subsequent frames, reference slots in `CmdBeginVideoCodingKHR` point to
previously-decoded images via their DPB slot index. The images and slot indices
are exactly what the codec parser specifies — the sample does not fabricate
or modify any associations.

When the H.264 parser evicts a reference from the DPB and reuses that slot
index for a new frame (normal sliding-window DPB management), the new frame's
image is associated with the slot via `pSetupReferenceSlot` during decode.
This is the **only** mechanism for slot association per the spec.

## What the Validation Layer Reports (Incorrectly)

```
VUID-vkCmdBeginVideoCodingKHR-pPictureResource-07265:
  DPB slot index N is not currently associated with the specified
  video picture resource: VkImage X
```

This fires in two cases:

1. **First-time activation** — The slot has never been used before. There is
   no prior association to mismatch. The validation layer incorrectly requires
   a pre-existing association.

2. **Slot reuse** — The parser reused a DPB slot index with a different image
   (the previous reference was evicted). The new association is established by
   `CmdDecodeVideoKHR`, but the validation layer checks `CmdBeginVideoCodingKHR`
   against stale state.

## Why the Sample Is Correct (Spec References)

### Slot activation happens during decode, not during BeginVideoCoding

Per Vulkan Spec §40.1:

> "A DPB slot can be activated with a new frame even if it is already active.
> In this case all previous associations of the DPB slots with reference
> pictures are replaced with an association with the reconstructed picture
> used to activate it."

The reconstructed picture is specified by `CmdDecodeVideoKHR`'s
`pSetupReferenceSlot`. This is the sole mechanism for establishing and
changing slot-to-image associations.

### BeginVideoCoding declares the working set, not final state

Per Vulkan Spec §40.1:

> "The set of bound reference picture resources is immutable within a video
> coding scope, however, **the DPB slot index associated with any of the
> bound reference picture resources can change** during the video coding scope
> in response to video coding operations."

The spec explicitly acknowledges that slot associations change within the
scope — `CmdBeginVideoCodingKHR` declares images, not final slot state.

### The VUID text itself

> "Each video picture resource [...] for which slotIndex is not negative must
> match one of the video picture resources currently associated with the DPB
> slot index [...] **at the time the command is executed on the device**"

For first-time activation, there IS no current association — the VUID condition
is vacuously impossible to satisfy. This is a spec gap.

## Attempted Workaround and Findings

We tested using `slotIndex = -1` for setup slots in `CmdBeginVideoCodingKHR`
(binding the image without slot association). Per spec §40.1:

> "If slotIndex is negative and pPictureResource is not NULL, then the video
> picture resource is added to the set of bound reference picture resources
> without an associated DPB slot. **Such a picture resource can be subsequently
> used as a reconstructed picture to associate it with a DPB slot.**"

This is spec-valid but **the validation layer does not track the resulting
slot activation from `CmdDecodeVideoKHR`**. Subsequent frames referencing that
slot also fail VUID-07265, confirming the layer only tracks slot state from
`CmdBeginVideoCodingKHR`, not from `CmdDecodeVideoKHR`'s `pSetupReferenceSlot`.

## Resolution

The VUID is suppressed via `g_ignoredValidationMessageIds` (0xa9049dc2) with
a detailed comment explaining why. A bug report for VK_LAYER_KHRONOS_validation
is documented in `docs/VK_LAYER_VALIDATION_BUG_REPORT-DPB-SLOT-TRACKING.md`.
