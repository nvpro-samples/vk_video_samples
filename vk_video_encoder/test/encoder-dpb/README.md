# Encoder DPB test

`vk-video-encoder-dpb-trace` drives the encoder's DPB, the DPB library `vkvideo-dpb`
(`common/libs/VkVideoDpb`), through a GOP sequence with no Vulkan device, making the calls the encoder
makes for each picture, and writes what it decides: the setup slot and its Std reference information,
the reference lists or AV1 reference names, and each reference's slot, picture and Std reference
information. The GOP structure and the AV1 configuration come from the encoder library's static
archive; the tool links it, the DPB library and simdjson, and no Vulkan loader.

The CTests `EncoderDpbSequencingH264`, `EncoderDpbSequencingH265` and `EncoderDpbSequencingAv1`
(label `device-free`) run `scripts/encoder_dpb_suite.py` over `suites/<codec>.json`. For each
configuration:

- **Golden:** the trace equals `golden/<configuration>.trace`, or for a long configuration its
  md5 equals the one the suite records.
- **Model:** `scripts/encoder_dpb_model.py` replays the reference marking and the initial
  reference lists the codec specifies, and its mismatch categories equal the configuration's
  `known` list. Each category is a defect the suite's `defects` table names; an unlisted category
  fails, and so does a listed one that no longer appears.

Nothing is encoded. A green run says what the DPB library decides when called as the encoder calls
it, not that a driver encodes it, and not what an encoder change the harness does not follow
would decide.

## Files

| File | What it holds |
|---|---|
| `src/EncoderDpbTrace.cpp` | the tool: the configuration, the frame queue and each codec's per-picture calls |
| `scripts/encoder_dpb_model.py` | the model: `--trace t [-v]` |
| `scripts/encoder_dpb_suite.py` | the suite runner CTest calls |
| `suites/h264.json`, `suites/h265.json`, `suites/av1.json` | the configurations, their goldens or md5s, known categories, and the defects table |
| `golden/*.trace` | the recorded traces |

## Running

```bash
<build>/bin/vk-video-encoder-dpb-trace --codec h264 --frames 64 --gop 16 --idr 60 --bframes 3 \
    --refs-l0 2 --refs-l1 1 [--closed-gop] [--level-dpb 16] [--max-dpb-slots 17] -o h264.trace
<build>/bin/vk-video-encoder-dpb-trace --codec av1 --frames 48 --gop 16 --idr 60 --bframes 3 \
    [--max-active-refs 7] [--av1-single 7:0x7f] [--av1-unidir 4:4:0xf] [--av1-bidir 7:4:3:0x7f] \
    [--av1-cdf-only 1] -o av1.trace
python3 scripts/encoder_dpb_model.py --trace h264.trace -v
python3 scripts/encoder_dpb_suite.py --tool <build>/bin/vk-video-encoder-dpb-trace --suite suites/h264.json [--mint] [-v]
```

`--refs-l0` and `--refs-l1` stand for the driver's preferred reference counts
(`preferredMaxL0ReferenceCount`, `preferredMaxL1ReferenceCount`), which set the H.264 and H.265
encoders' counts; `--level-dpb` is the H.264 DPB size the level allows for the picture size.
`--max-dpb-slots` is the device's `maxDpbSlots` (17). For AV1, `--max-active-refs` is the device's
`maxActiveReferencePictures` (7), and `--av1-single`, `--av1-unidir` and `--av1-bidir` its reference
capabilities, the counts (`N`, group 1 `G1`, group 2 `G2`) and the name mask of each prediction
mode. `--av1-cdf-only 0` is a device without
`VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR`. The defaults are those of a
device that allows every reference name: seven single references, four unidirectional compound,
and seven bidirectional compound (four and three), and primary reference frames for their CDFs
only. With
fewer than eight DPB slots, the AV1 encoder refuses the session, and the tool writes `refused` and
exits 1. `--mint` writes the goldens and md5s from the current library and prints each
configuration's categories.

## What the harness follows

Where the encoder feeds the DPB, the harness does what the encoder does, line by line:

- the configuration: `EncoderConfigH264::InitSpsPpsParameters` and `InitDpbCount`;
  `EncoderConfigH265::InitDpbCount`, `InitializeSpsRefPicSet` and the reference fields of
  `InitParamameters`; `EncoderConfigAV1::InitDpbCount` and `InitSequenceHeader`; and the DPB size
  `VkVideoEncoder::InitEncoder` gives the session, the DPB context and the parameter sets (as
  `dpbCount`), at most 16 and at most the device's `maxDpbSlots`;
- the order: each input frame takes its GOP position (`VkVideoGopStructure::GetPositionInGOP`),
  and `VkVideoEncoder::EnqueueFrame` and `InsertOrdered` defer it in GOP encode order until an
  IDR, the last frame, a reference frame or a GOP without B frames flushes the queue;
  `VkVideoEncoderAV1::InsertOrdered` adds, behind a frame inserted ahead of others, the
  show_existing_frame that shows it in display order;
- each picture: `VkVideoEncoderH264::EncodeFrame` and `ProcessDpb`, `VkVideoEncoderH265::EncodeFrame`
  and `ProcessDpb`, `VkVideoEncoderAV1::EncodeFrame`, `InitializeFrameHeader` and `ProcessDpb`,
  without the image pool, the rate control and the intra refresh. Each picture gets its own resource
  index, so a reference's `pic` is the input number of the picture the DPB set up in its slot.

A change to any of these is a change the harness has to follow. A build with assertions skips the
configurations that stop at one (`debug` in the suite); the runner asks the tool (`--asserts`).

## The trace

| Line | What it records |
|---|---|
| `encoder-dpb-trace` | the settings |
| `config` | the DPB size and the parameter set fields the DPB reads; H.265 `sps-rps`, the SPS short-term reference picture set; AV1 the device's active references (`max_active_refs`) and reference capabilities (`single`, `unidir`, `bidir`), and `cdf_only_primary=0` for a device without CDF-only primary reference frames |
| `refused` | AV1: the encoder refuses a DPB of fewer than eight slots |
| `pic` | a picture in encode order: its input number, type, POC and whether it is a reference |
| `h264`, `h265` | `frame_num` and `PicOrderCnt`; H.265 the flags and the reference picture set used (`rps`) with the slots of the pictures in each of its lists (`rps-slots`) |
| `av1` | the frame header fields the DPB decides: `frame_type`, `refresh_frame_flags`, the show flags, `order_hint`, `current_frame_id`, `primary_ref_frame`, `ref_frame_idx`, `ref_order_hint`, `referenceNameSlotIndices` (`names`), `primaryReferenceCdfOnly` (`cdf_only`), `predictionMode` (`mode`) and `rateControlGroup` (`rc_group`); for a show_existing_frame, the shown frame's `frame_type` and `current_frame_id` |
| `shown` | AV1 show_existing_frame: the slot and the reference buffer it shows |
| `lists` | `RefPicList0` and `RefPicList1` as slots, at the active counts |
| `ref` | each slot the encode binds: the picture it holds and every member of its Std reference information |
| `setup` | the setup slot and its Std reference information |

## The model

| Category | What it checks |
|---|---|
| `list0`, `list1` | each list is the start of the initial list (H.264 8.2.4.2: P by descending PicNum; B list 0 by descending POC below the picture then ascending above, list 1 the other way, its first two swapped when equal to list 0; H.265 8.3.4: StCurrBefore, StCurrAfter, LtCurr, repeated to the active count, list 1 starting with StCurrAfter); an H.264 list is no longer than the initial list. The encoder chooses the active counts and uses no list modification |
| `ref-unknown` | a reference slot holds the picture the model holds there as a reference |
| `ref-poc`, `ref-frame-num`, `ref-pic-type` | the reference's Std information is its picture's: `PicOrderCnt` or `PicOrderCntVal`, H.264 `FrameNum`, `primary_pic_type` or `pic_type` |
| `ref-unused` | an H.265 reference is not flagged `unused_for_reference` |
| `setup-info` | the setup slot's Std reference information is the picture's: H.264 `PicOrderCnt`, `FrameNum` and `primary_pic_type`; H.265 `PicOrderCntVal`, `pic_type`, and `unused_for_reference` exactly when the picture is not a reference; AV1 `frame_type`, `OrderHint` and `RefFrameId` (its `current_frame_id`) |
| `rps-missing`, `rps-slots` | H.265: every POC of the reference picture set is a reference picture, and the encoder's slots for each list are those pictures |
| `slot-reuse` | the setup slot holds no picture that stays a reference: H.264 after the sliding window (8.2.5.3) with `max_num_ref_frames`, H.265 after the reference picture set marks the others unused (8.3.2) |
| `slot-range` | no slot at or above the DPB size or the device's `maxDpbSlots` |
| AV1 `ref-name-slot`, `ref-order-hint` | each reference name's slot holds the frame in the buffer its `ref_frame_idx` names, and each `ref_order_hint` is that buffer's order hint, over the eight reference buffers `refresh_frame_flags` updates (a shown key frame refreshes all of them, and showing an existing key frame loads it into all, 7.20, 7.21) |
| AV1 `names-unlisted`, `ref-unnamed` | every reference name's slot is bound, and every bound slot is a reference name's |
| AV1 `ref-info`, `refresh`, `show-existing` | a reference's `frame_type`, `OrderHint` and `RefFrameId` are its frame's; a shown key frame refreshes every buffer; show_existing_frame shows the buffer that holds its frame |
| AV1 `ref-count` | an inter frame binds at most the session's active references, and names at least one reference and no more than the prediction mode the DPB plans for its type takes, of group 1, of group 2 and in all, all inside that mode's name mask: bidirectional compound for a B picture, else unidirectional compound from group 1, else single reference. A primary reference frame used for its CDFs only is bound, not named |
| AV1 `primary-ref` | the primary reference frame's name has a slot (`VUID-VkVideoEncodeAV1PictureInfoKHR-pStdPictureInfo-10291`), and is used for its CDFs only on a device that takes it (`VUID-VkVideoEncodeAV1PictureInfoKHR-flags-10289`) |

## What the configurations show

Over the suites and sweeps of 1344 H.264 configurations (0, 1, 3 and 7 B pictures, 1 to 16 list 0
and 0 to 2 list 1 references, open and closed GOPs, IDR periods 30, 60 and 400, devices with 17, 4
and 3 slots), 1728 H.265 configurations (also 1 to 8 list 0 and 0 to 3 list 1 references, GOPs of 8
and 16) and 912 AV1 configurations (GOPs of 8, 16 and 32, IDR periods 30, 60, 400 and none, 320
frames past the order hint wrap, nine sets of reference capabilities and active references, devices
with 17, 9 and 8 slots), the model finds no mismatch: the lists, the sliding window, the reference
picture sets and the AV1 reference buffers are the codecs', the Std reference information of every
reference and setup slot is its picture's, and every slot is below the DPB size.

The suite configurations of a device with fewer slots or references (`h264-*-4slots`,
`h264-*-3slots`, `h265-*-3slots`, `av1-ippp-8slots`, `av1-ibbbp-single`, `av1-ibbbp-counts`,
`av1-ib7p-2active`) keep every slot below the device's `maxDpbSlots`, and every AV1 picture within
the session's active references and the counts and names of its prediction mode.

A B picture of a device without bidirectional compound prediction (`av1-ib7p-unidir`) takes its
references from group 1, so its primary reference frame, a backward reference, is used for its
CDFs only. On a device without that (`av1-ib7p-unidir-no-cdf-only`, `av1-ibbbp-closed-no-cdf-only`)
the library gives the primary reference frame the name of a reference that is the same picture,
whose buffer holds the same CDFs, or none.
