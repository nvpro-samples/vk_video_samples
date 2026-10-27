# Encoder DPB test

`vk-video-encoder-dpb-trace` drives the encoder's DPB classes, `VkEncDpbH264`, `VkEncDpbH265` and
`VkEncDpbAV1`, through a GOP sequence with no Vulkan device, making the calls the encoder makes for
each picture, and writes what they decide: the setup slot, the reference lists or AV1 reference
names, and each reference's slot, picture and Std reference information. The classes come from the
encoder library's static archive, as the encoder uses them; the tool links that archive and
simdjson's, and no Vulkan loader.

The CTests `EncoderDpbSequencingH264`, `EncoderDpbSequencingH265` and `EncoderDpbSequencingAv1`
(label `device-free`) run `scripts/encoder_dpb_suite.py` over `suites/<codec>.json`. For each
configuration:

- **Golden:** the trace equals `golden/<configuration>.trace`, or for a long configuration its
  md5 equals the one the suite records.
- **Model:** `scripts/encoder_dpb_model.py` replays the reference marking and the initial
  reference lists the codec specifies, and its mismatch categories equal the configuration's
  `known` list. Each category is a defect the suite's `defects` table names; an unlisted category
  fails, and so does a listed one that no longer appears.

Nothing is encoded. A green run says what the DPB classes decide when called as the encoder calls
them, not that a driver encodes it, and not what an encoder change the harness does not follow
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
    --refs-l0 2 --refs-l1 1 [--closed-gop] [--level-dpb 16] -o h264.trace
python3 scripts/encoder_dpb_model.py --trace h264.trace -v
python3 scripts/encoder_dpb_suite.py --tool <build>/bin/vk-video-encoder-dpb-trace --suite suites/h264.json [--mint] [-v]
```

`--refs-l0` and `--refs-l1` stand for the driver's preferred reference counts
(`preferredMaxL0ReferenceCount`, `preferredMaxL1ReferenceCount`), which set the H.264 and H.265
encoders' counts; `--level-dpb` is the H.264 DPB size the level allows for the picture size. AV1
uses the reference capabilities of a device that allows every reference name: seven single
references, four unidirectional compound, and seven bidirectional compound (four and three). `--mint` writes the
goldens and md5s from the current classes and prints each configuration's categories.

## What the harness follows

Where the encoder feeds the DPB, the harness does what the encoder does, line by line:

- the configuration: `EncoderConfigH264::InitSpsPpsParameters` and `InitDpbCount`;
  `EncoderConfigH265::InitDpbCount`, `InitializeSpsRefPicSet` and the reference fields of
  `InitParamameters`; `EncoderConfigAV1::InitDpbCount` and `InitSequenceHeader`; and the DPB size
  `VkVideoEncoder::InitEncoder` passes on, at most 16;
- the order: each input frame takes its GOP position (`VkVideoGopStructure::GetPositionInGOP`),
  and `VkVideoEncoder::EnqueueFrame` and `InsertOrdered` defer it in GOP encode order until an
  IDR, the last frame, a reference frame or a GOP without B frames flushes the queue;
  `VkVideoEncoderAV1::InsertOrdered` adds, behind a frame inserted ahead of others, the
  show_existing_frame that shows it in display order;
- each picture: `VkVideoEncoderH264::EncodeFrame` and `ProcessDpb`, `VkVideoEncoderH265::EncodeFrame`
  and `ProcessDpb`, `VkVideoEncoderAV1::EncodeFrame`, `StartOfVideoCodingEncodeOrder`,
  `InitializeFrameHeader` and `ProcessDpb`, without the image pool, the rate control and the intra
  refresh. Each picture gets its own image node, so a reference's `pic` is the input number of the
  picture whose image the DPB hands back; the AV1 encoder's test for a reference it already binds,
  by image index, is a test of the node here.

A change to any of these is a change the harness has to follow. A build with assertions skips the
configurations that stop at one (`debug` in the suite); the runner asks the tool (`--asserts`).

## The trace

| Line | What it records |
|---|---|
| `encoder-dpb-trace` | the settings |
| `config` | the DPB size and the parameter set fields the DPB reads; H.265 `sps-rps`, the SPS short-term reference picture set |
| `pic` | a picture in encode order: its input number, type, POC and whether it is a reference |
| `h264`, `h265` | `frame_num` and `PicOrderCnt`; H.265 the flags and the reference picture set used (`rps`) with the slots `DpbPictureStart` put in each list (`rps-slots`) |
| `av1` | the frame header fields the DPB decides: `frame_type`, the reference name the frame takes, `refresh_frame_flags`, the show flags, `order_hint`, `primary_ref_frame`, `ref_frame_idx`, `ref_order_hint`, and `referenceNameSlotIndices` (`names`) |
| `shown` | AV1 show_existing_frame: the slot and the reference buffer it shows |
| `lists` | `RefPicList0` and `RefPicList1` as slots, at the active counts |
| `ref` | each slot the encode binds: the picture it holds and `FillStdReferenceInfo`'s fields |
| `setup` | the setup slot |

## The model

| Category | What it checks |
|---|---|
| `list0`, `list1` | each list is the start of the initial list (H.264 8.2.4.2: P by descending PicNum; B list 0 by descending POC below the picture then ascending above, list 1 the other way, its first two swapped when equal to list 0; H.265 8.3.4: StCurrBefore, StCurrAfter, LtCurr, repeated to the active count, list 1 starting with StCurrAfter); an H.264 list is no longer than the initial list. The encoder chooses the active counts and uses no list modification |
| `ref-unknown` | a reference slot holds the picture the model holds there as a reference |
| `ref-poc`, `ref-frame-num`, `ref-pic-type` | the reference's Std information is its picture's: `PicOrderCnt` or `PicOrderCntVal`, H.264 `FrameNum`, `primary_pic_type` or `pic_type` |
| `ref-unused` | an H.265 reference is not flagged `unused_for_reference` |
| `rps-missing`, `rps-slots` | H.265: every POC of the reference picture set is a reference picture, and the encoder's slots for each list are those pictures |
| `slot-reuse` | the setup slot holds no picture that stays a reference: H.264 after the sliding window (8.2.5.3) with `max_num_ref_frames`, H.265 after the reference picture set marks the others unused (8.3.2) |
| `slot-range` | no slot at or above the DPB size |
| AV1 `ref-name-slot`, `ref-order-hint` | each reference name's slot holds the frame in the buffer its `ref_frame_idx` names, and each `ref_order_hint` is that buffer's order hint, over the eight reference buffers `refresh_frame_flags` updates (a shown key frame refreshes all of them, and showing an existing key frame loads it into all, 7.20, 7.21) |
| AV1 `names-unlisted`, `ref-unnamed` | every reference name's slot is bound, and every bound slot is a reference name's |
| AV1 `ref-info`, `refresh`, `show-existing` | a reference's `frame_type` and `OrderHint` are its frame's; a shown key frame refreshes every buffer; show_existing_frame shows the buffer that holds its frame |

## What the configurations show

Over the suites and a sweep of 720 configurations (0, 1, 3 and 7 B frames; 1 to 16 L0 and 0 to 2
L1 references; open and closed GOPs; IDR periods 30, 60 and 400):

- the lists, the sliding window and the reference picture sets match the model everywhere;
- every H.264 reference has `FrameNum` and `primary_pic_type` 0, and every H.265 reference
  `pic_type` 0 (`ref-frame-num`, `ref-pic-type`);
- with 16 L0 references and B frames, the 16-slot DPB has no slot for a non-reference
  picture: `DpbPictureEnd` asserts in a build with assertions and returns slot 16 in a release
  build (`slot-range`);
- with 16 L0 and 1 L1 references, `max_num_ref_frames` is 17, above H.264's limit of 16,
  and the 16-slot DPB overwrites a picture the sliding window still holds (`slot-reuse`).

Over the AV1 suite and a sweep of 60 configurations (0, 1, 2, 3, 5 and 7 B frames, open and closed
GOPs, IDR periods 30, 60, 64 and 400, past the order hint wrap) the reference names, the order
hints, the references and the setup slots match the reference buffers everywhere.

The H.265 long-term "Foll" loop, is not reachable: the encoder sets no long-term references.
Its test drives the class with long-term references and lands with its fix.
