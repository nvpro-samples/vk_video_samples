# Parser trace test, AV1

`vk-video-parser-trace` runs NvVideoParser over a stream with no Vulkan device, and writes a
canonical text trace of every callback the parser makes:
- sequence info and parameter sets;
- every picture, with its Std picture information, its setup and DPB reference information, and a
  CRC32 of its bytes;
- the display order.

The CTest `ParserTraceAv1` (label `device-free`) runs `scripts/parser_trace_suite.py` over
`suites/av1.json` and applies two checks to every stream:

- **Golden:** the trace equals `golden/<stream>.trace`. Any change in what the parser hands a
  decoder fails until the golden is re-recorded with that change explained.
- **Model:** `scripts/av1_reference_model.py` replays, from the stream's syntax elements, the
  state the AV1 specification defines (5.9.2, 5.9.14, 7.8, 7.20, 7.21), and compares each picture
  of the trace with it. Mismatches are grouped in categories. Each stream lists the categories of
  its known defects in `known`, and the suite's `defects` table names them. A category that appears
  unlisted fails; a listed one that no longer appears fails too. A fix therefore removes its
  category from `known`, and a golden diff shows exactly the fields it changed.

Nothing is decoded. A green run says what the parser hands a client, not that a driver decodes it
correctly.

## Files

| File | What it is |
|---|---|
| `src/ParserTrace.cpp` | the tool: a recording client, a host-memory bitstream buffer, a picture pool, and IVF, low-overhead OBU and Annex B readers |
| `scripts/av1_reference_model.py` | the AV1 model and the comparison |
| `scripts/parser_trace_suite.py` | the suite runner CTest calls |
| `suites/av1.json` | the streams, their goldens, header dumps and known categories, and the defects table |
| `streams/*.ivf` | three 64x64 streams, about 4 KB each |
| `streams/*.headers.txt` | their compact header dumps, so the model check needs no ffmpeg |
| `golden/*.trace` | their recorded traces |

The parser is linked from its static archive, so the tool needs no Vulkan loader and no device.

## Running

```bash
ctest -R ParserTraceAv1 --output-on-failure            # from the build directory
python3 scripts/parser_trace_suite.py --tool <build>/bin/vk-video-parser-trace --suite suites/av1.json -v
<build>/bin/vk-video-parser-trace --codec av1 --format ivf -i stream.ivf -o stream.trace
python3 scripts/av1_reference_model.py --trace stream.trace --headers stream.headers.txt -v
```

Two environment variables add streams that are not in the repository:
- `VVS_AV1_SEGMENTATION_STREAMS`: a directory holding three 60-frame 1920x1080 streams
  (`suites/av1.json` gives the paths). They are checked like the committed ones, with the header
  dump made by ffmpeg at run time.
  - `rav1e/basketball_1080p_rav1e_60f.ivf`: BasketballDrive, rav1e `-speed 8 -qp 100`.
  - `basketball_1080p_aq1_60f.ivf`: BasketballDrive, libaom `-cpu-used 4 -aq-mode 1 -lag-in-frames 19 -b:v 3M`.
  - `cyclic/still_noise_1080p_cyclic_60f.ivf`: one BasketballDrive frame with temporal noise,
    libaom `-usage realtime -cpu-used 8 -aq-mode 3 -lag-in-frames 0 -g 60 -b:v 2M`.
- `VVS_ARGON_AV1_ROOT`: the `argon_coveragetool_av1_base_and_extended_profiles_v2.1` directory of
  AOM's Argon conformance streams
  (`https://aom-cwg-av1-argon-streams-public.s3.us-east-1.amazonaws.com/argon_coveragetool_av1_base_and_extended_profiles_v2.1.1.zip`).
  Its four Profile 0 sets are reported category by category and never fail the run.

## The trace

One line per callback, as `tag key=value ...`. Arrays print as `[a,b]`, and flag sets as
`name|name`, or `-` when no flag is set. Lines that start with two spaces belong to the `pic` line
above them.

| Tag | Content |
|---|---|
| `trace` | the tool's settings |
| `pkt` | a packet handed to `ParseByteStream`: one temporal unit |
| `sps`, `color`, `timing` | `UpdatePictureParameters` with an AV1 sequence header |
| `seq` | `BeginSequence` |
| `pic` | `DecodePicture`: `VkParserPictureData` and the CRC32 of the picture's bytes |
| `av1`, `std`, `tiles`, `quant`, `seg`, `lf`, `cdef`, `lr`, `gm`, `fg` | `VkParserAv1PictureData` |
| `setup` | `setupSlotInfo`, the picture's own reference information |
| `slot` | `dpbSlotInfos[i]` for the eight reference buffers, with the picture index each holds |
| `disp` | `DisplayPicture` |
| `eos` | the end of the stream, with the picture and display counts |

Pictures come from a fixed pool, lowest free index first, so the indices depend only on the
parser's reference counting.

## The model

`av1_reference_model.py` reads the syntax elements from ffmpeg's `trace_headers` dump of the IVF
file the tool writes with `--write-ivf`. That IVF holds exactly the temporal units the parser was
given; ffmpeg's own OBU and Annex B demuxers drop temporal units of some streams, and `-copyinkf`
keeps the packets before the first one ffmpeg flags as a key frame. `--save-headers` writes the
compact dump the committed streams carry.

| Categories | Compared |
|---|---|
| `hdr-*` | `frame_type`, `OrderHint`, `primary_ref_frame`, `refresh_frame_flags`, `ref_frame_idx` (including frame_refs_short_signaling, 7.8), `OrderHints`, `error_resilient_mode`, `disable_cdf_update`, `disable_frame_end_update_cdf` |
| `seg-*` | the segmentation flags, and the effective `FeatureEnabled` and `FeatureData` after load_previous() and segmentation_params() |
| `setup-*` | the picture's own `StdVideoDecodeAV1ReferenceInfo` |
| `ref-*` | the `StdVideoDecodeAV1ReferenceInfo` of every buffer the picture references, which Vulkan defines as the information of the frame stored there |
| `picture-count` | a picture present in the trace or in the model but not in both, per packet |

Not compared, because the specification leaves them undefined: `RefFrameSignBias` of intra frames,
element 0 of `SavedOrderHints`, and the segmentation update flags of a frame without segmentation.
Buffers whose content the headers do not determine, after an error-resilient `ref_order_hint`
mismatch, are skipped. Scalable streams are not compared: the parser decodes one operating point
and the dump holds them all.

## Recording

```bash
python3 scripts/parser_trace_suite.py --tool <build>/bin/vk-video-parser-trace --suite suites/av1.json --mint
```

`--mint` writes the goldens, and the header dumps of the committed streams (this needs ffmpeg),
from the current parser, and prints each stream's categories. A golden records today's parser,
defects included. A change that alters a golden explains each changed field in its commit, and
updates `known` to match.

## The committed streams

Encoded with ffmpeg 6.1.1, rav1e 0.7.1 and libaom 3.8.2 from `testsrc2=size=64x64:rate=30`:

| Stream | Encoder settings | Reaches |
|---|---|---|
| `av1-rav1e-segmentation-64x64.ivf` | `-frames:v 12 -c:v librav1e -speed 8 -qp 100 -g 12` | per-segment quantizer deltas inherited by later frames, references with segmentation |
| `av1-libaom-no-order-hint-64x64.ivf` | `-frames:v 12 -c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 -g 12 -aom-params enable-order-hint=0` | reference sign bias with order hints off |
| `av1-libaom-altref-aq-64x64.ivf` | `-frames:v 17 -c:v libaom-av1 -cpu-used 6 -lag-in-frames 16 -auto-alt-ref 1 -arnr-strength 5 -arnr-max-frames 7 -aq-mode 1 -g 17` | hidden alternate reference frames, show_existing_frame, references with segmentation |
