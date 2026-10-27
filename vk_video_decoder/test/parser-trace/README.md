# Parser trace test

`vk-video-parser-trace` runs NvVideoParser over an H.264, H.265, AV1 or VP9 stream with no Vulkan
device, and writes a canonical text trace of every callback the parser makes:
- sequence info and parameter sets;
- every picture, with its Std picture information, its DPB and reference information, and a CRC32
  of its bytes;
- the display order.

The CTests `ParserTraceAv1`, `ParserTraceH264`, `ParserTraceH265` and `ParserTraceVp9` (label
`device-free`) run `scripts/parser_trace_suite.py` over `suites/<codec>.json`, with these checks:

- **Golden:** a committed stream's trace equals `golden/<stream>.trace`. Any change in what the
  parser hands a decoder fails until the golden is re-recorded with that change explained.
- **md5:** a stream outside the tree, from the decode samples or a conformance corpus, is held to
  the md5 of its trace in `golden/md5/<set>.md5`, so its traces need not be committed. A stream a
  list names but the root does not hold is reported and does not fail.
- **Model, AV1:** `scripts/av1_reference_model.py` replays, from the stream's syntax elements, the
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
| `src/ParserTrace.cpp` | the tool: a recording client, a host-memory bitstream buffer, a picture pool, and the stream readers |
| `scripts/av1_reference_model.py` | the AV1 model and the comparison |
| `scripts/parser_trace_suite.py` | the suite runner CTest calls |
| `suites/<codec>.json` | the streams of each codec, their goldens, md5 lists, AV1 header dumps and known categories, and the AV1 defects table |
| `streams/` | twenty committed streams of 2 to 16 KB |
| `streams/*.headers.txt` | the AV1 streams' compact header dumps, so the model check needs no ffmpeg |
| `golden/*.trace` | the committed streams' recorded traces |
| `golden/md5/*.md5` | the trace md5 of every stream of an outside set, one `<md5>  <path>` line each |

The parser is linked from its static archive, so the tool needs no Vulkan loader and no device.

## Running

```bash
ctest -R ParserTrace --output-on-failure                 # from the build directory
python3 scripts/parser_trace_suite.py --tool <build>/bin/vk-video-parser-trace --suite suites/h265.json -v
<build>/bin/vk-video-parser-trace --codec h264 --format bytestream -i stream.h264 -o stream.trace
<build>/bin/vk-video-parser-trace --codec av1 --format ivf -i stream.ivf -o stream.trace
python3 scripts/av1_reference_model.py --trace stream.trace --headers stream.headers.txt -v
```

How the tool gives each format to the parser:

| `--format` | Codecs | Packets |
|---|---|---|
| `bytestream` | H.264, H.265 | An Annex B byte stream, given as the decoder sample gives an elementary stream: the rest of the stream on each call, with partial parsing, advancing by the bytes the parser consumed |
| `ivf` | AV1, VP9 | One packet per IVF frame |
| `obu` | AV1 | The low-overhead OBU format, one packet per temporal unit |
| `annexb` | AV1 | AV1 Annex B, one packet per temporal unit |

Environment variables add streams that are not in the repository. Each set runs only when its
variable is set:
- `VVS_DECODE_SAMPLES_ROOT`: the Khronos framework's `tests/resources` directory. Every sample of
  `tests/decode_samples.json` found there joins its codec's suite, checked by md5.
- `VVS_ITU_H264_ROOT`: the ITU-T H.264.1 conformance streams, laid out as
  `AVCv1/streams/<name>/<stream>` and `FRExt/streams/<name>/<stream>`, with `MR4/MR4_TANDBERG_C.264`.
  Checked by md5.
- `VVS_AV1_SEGMENTATION_STREAMS`: a directory holding three 60-frame 1920x1080 AV1 streams
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
`name|name`, or `-` when no flag is set. Lines that start with two spaces belong to the line
above them.

| Tag | Content |
|---|---|
| `trace` | the tool's settings |
| `pkt` | a packet handed to `ParseByteStream`: one temporal unit or IVF frame, before the callbacks it causes; for a byte stream, after the callbacks of the call, its offset and the bytes the parser consumed |
| `vps`, `sps`, `pps` | `UpdatePictureParameters`, with every field of the Std parameter set; under them `ptl`, `dpbm`, `scaling` (a CRC32 of the lists), `strps`, `ltrps`, `vui`, `hrd`, `hrd-nal`, `hrd-vcl` and `palette`. The AV1 sequence header prints as `sps` with `color` and `timing` |
| `seq` | `BeginSequence` |
| `pic` | `DecodePicture`: `VkParserPictureData` and the CRC32 of the picture's bytes |
| `h264`, `dpb` | `VkParserH264PictureData` and its non-empty DPB entries, with the picture index each holds |
| `hevc`, `rps`, `mv-hevc` | `VkParserHevcPictureData`: the picture, its reference picture sets with the picture index of each reference, and the MV-HEVC fields of a layered picture |
| `av1`, `std`, `tiles`, `quant`, `seg`, `lf`, `cdef`, `lr`, `gm`, `fg` | `VkParserAv1PictureData` |
| `setup`, `slot` | AV1: `setupSlotInfo`, the picture's own reference information, and `dpbSlotInfos[i]` for the eight reference buffers, with the picture index each holds |
| `vp9`, `std`, `color`, `lf`, `seg` | `VkParserVp9PictureData` |
| `disp` | `DisplayPicture` |
| `unhandled` | `UnhandledNALU`, with the size and CRC32 of the data |
| `eos` | the end of the stream, with the picture and display counts |

Pictures come from a fixed pool, lowest free index first, so the indices depend only on the
parser's reference counting.

## The AV1 model

`av1_reference_model.py` reads the syntax elements from ffmpeg's `trace_headers` dump of the IVF
file the tool writes with `--write-ivf`. That IVF holds exactly the temporal units the parser was
given; ffmpeg's own OBU and Annex B demuxers drop temporal units of some streams, and `-copyinkf`
keeps the packets before the first one ffmpeg flags as a key frame. `--save-headers` writes the
compact dump the committed streams carry.

| Categories | Compared |
|---|---|
| `hdr-*` | `frame_type`, `OrderHint`, `primary_ref_frame`, `refresh_frame_flags`, `ref_frame_idx` (including frame_refs_short_signaling, 7.8), `OrderHints`, `error_resilient_mode`, `disable_cdf_update`, `disable_frame_end_update_cdf` |
| `hdr-quant`, `hdr-loop-filter`, `hdr-tx-mode`, `hdr-mode-flags`, `hdr-film-grain` | syntax read after the segmentation parameters, against ffmpeg's reading of it: `base_q_idx` and the `DeltaQ` values, `loop_filter_level`, `TxMode`, `reference_select`, `skip_mode_present`, `reduced_tx_set`, `allow_warped_motion` (with `is_motion_mode_switchable` and `allow_high_precision_mv` for inter frames), and `apply_grain`, `grain_seed` and `point_y_value`. A misread earlier in the header that shifts the bits shows here |
| `hdr-frame-size` | `UpscaledWidth`, `FrameWidth` and `FrameHeight`, from frame_size(), superres_params() and frame_size_with_refs(), which takes a size from a reference |
| `tile-*` | `TileCols`, `TileRows` and `tileCount`; `width_in_sbs_minus_1` and `height_in_sbs_minus_1`; `MiColStarts` and `MiRowStarts` in mode info units, as Vulkan's `pMiColStarts` and `pMiRowStarts` take them; `context_update_tile_id` and `tile_size_bytes_minus_1`; and that every tile offset and size lies inside the picture's data, after the tile before it |
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
python3 scripts/parser_trace_suite.py --tool <build>/bin/vk-video-parser-trace --suite suites/<codec>.json --mint
```

`--mint` writes the goldens, the md5 lists of the sets whose roots are set, and the header dumps of
the committed AV1 streams (this needs ffmpeg), from the current parser, and prints each AV1
stream's categories. A golden records today's parser, defects included. A change that alters a
golden or an md5 explains each changed field in its commit, and updates `known` to match.

## The committed streams

Encoded with ffmpeg 6.1.1, x264 core 164, x265 3.5, libvpx 1.14.0, rav1e 0.7.1 and libaom 3.8.2 from
`testsrc2=size=64x64:rate=30`, or the size the table gives:

| Stream | Encoder settings | Reaches |
|---|---|---|
| `h264-x264-high-bpyramid-64x64.h264` | `-frames:v 24 -c:v libx264 -preset medium -profile:v high -x264-params threads=1:scenecut=0:b-adapt=0:bframes=3:b-pyramid=normal:ref=4:keyint=12` | B pyramid with memory management control operations, POC type 0, CABAC |
| `h264-x264-baseline-64x64.h264` | `-frames:v 16 -c:v libx264 -preset medium -profile:v baseline -x264-params threads=1:scenecut=0:ref=3:keyint=8` | POC type 2, CAVLC, sliding-window references |
| `h264-x264-mbaff-64x64.h264` | `-frames:v 12 -c:v libx264 -preset medium -x264-params threads=1:scenecut=0:interlaced=1:tff=1:bframes=2:ref=3:keyint=12` | MBAFF frames (`frame_mbs_only_flag` 0) |
| `h264-x264-cqm-weightp-slices-64x64.h264` | `-frames:v 12 -c:v libx264 -preset medium -x264-params threads=1:scenecut=0:cqm=jvt:weightp=2:slices=4:bframes=2:keyint=12` | PPS scaling lists, weighted prediction, four slices per picture |
| `h265-x265-bpyramid-cra-64x64.h265` | `-frames:v 24 -c:v libx265 -x265-params pools=1:frame-threads=1:scenecut=0:bframes=3:b-pyramid=1:ref=4:keyint=12:open-gop=1` | an open GOP: CRA with RASL pictures, B pyramid |
| `h265-x265-idr-64x64.h265` | `-frames:v 16 -c:v libx265 -x265-params pools=1:frame-threads=1:scenecut=0:bframes=0:ref=3:keyint=8:open-gop=0` | IDR pictures in a closed GOP |
| `h265-x265-main10-64x64.h265` | `-frames:v 12 -pix_fmt yuv420p10le -c:v libx265 -x265-params pools=1:frame-threads=1:scenecut=0:bframes=2:keyint=12` | Main 10 |
| `h265-x265-hrd-scaling-slices-sublayers-64x64.h265` | `-frames:v 16 -c:v libx265 -x265-params pools=1:frame-threads=1:scenecut=0:ctu=16:hrd=1:vbv-maxrate=300:vbv-bufsize=300:scaling-list=default:slices=2:temporal-layers=1:bframes=3:keyint=16` | HRD parameters in the VPS and the SPS VUI, two temporal sub-layers, default scaling lists, two slices per picture |
| `vp9-libvpx-altref-64x64.ivf` | `testsrc2=size=64x64:rate=30:duration=0.8`, two passes of `-c:v libvpx-vp9 -deadline good -cpu-used 4 -threads 1 -row-mt 0 -auto-alt-ref 1 -lag-in-frames 16 -arnr-maxframes 7 -g 24 -b:v 200k` | hidden alternate reference frames in superframes |
| `vp9-libvpx-profile2-10bit-64x64.ivf` | `-frames:v 12 -pix_fmt yuv420p10le -c:v libvpx-vp9 -deadline good -cpu-used 4 -threads 1 -row-mt 0 -profile:v 2 -g 12 -b:v 200k` | profile 2, 10-bit |
| `vp9-libvpx-tiles-cyclic-aq-512x64.ivf` | 512x64, `-frames:v 12 -c:v libvpx-vp9 -deadline good -cpu-used 4 -threads 1 -row-mt 0 -tile-columns 1 -aq-mode 3 -lag-in-frames 0 -g 12 -b:v 300k` | two tile columns, segmentation from cyclic refresh |
| `av1-rav1e-segmentation-64x64.ivf` | `-frames:v 12 -c:v librav1e -speed 8 -qp 100 -g 12` | per-segment quantizer deltas inherited by later frames, references with segmentation |
| `av1-libaom-no-order-hint-64x64.ivf` | `-frames:v 12 -c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 -g 12 -aom-params enable-order-hint=0` | reference sign bias with order hints off |
| `av1-libaom-altref-aq-64x64.ivf` | `-frames:v 17 -c:v libaom-av1 -cpu-used 6 -lag-in-frames 16 -auto-alt-ref 1 -arnr-strength 5 -arnr-max-frames 7 -aq-mode 1 -g 17` | hidden alternate reference frames, show_existing_frame, references with segmentation |
| `av1-libaom-key-frames-64x64.ivf` | `-frames:v 12 -c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 -g 4 -keyint_min 4` | shown key frames after inter frames, which reset `OrderHints` |
| `av1-libaom-still-after-inter-64x64.ivf` | `-frames:v 4 -c:v libaom-av1 -cpu-used 8 -lag-in-frames 0`, followed by `-frames:v 1 -c:v libaom-av1 -cpu-used 8 -still-picture 1`, joined with `-f concat -i <list> -c copy` | a reduced still picture header after frames with order hints |
| `av1-libaom-film-grain-64x64.ivf` | `-frames:v 17 -c:v libaom-av1 -cpu-used 6 -lag-in-frames 16 -auto-alt-ref 1 -g 17 -denoise-noise-level 25` | film grain on every shown frame, and one frame with show_frame and showable_frame 0, which codes no film grain parameters |
| `av1-libaom-64-tile-rows-64x4096.ivf` | 64x4096 from `testsrc2` blurred with `gblur=sigma=20`, `-frames:v 2 -c:v libaom-av1 -cpu-used 8 -crf 63 -lag-in-frames 0 -tile-rows 6 -aom-params sb-size=64` | 64 tile rows of one superblock |
| `av1-libaom-128-tiles-1024x512.ivf` | 1024x512 from `testsrc2` blurred with `gblur=sigma=20`, `-frames:v 2 -c:v libaom-av1 -cpu-used 8 -crf 63 -lag-in-frames 0 -tile-columns 4 -tile-rows 3 -aom-params sb-size=64` | 16 by 8 tiles, 128 in all |
| `av1-libaom-still-picture-1080p.ivf` | 1920x1080, `-frames:v 1 -c:v libaom-av1 -cpu-used 8 -crf 63 -still-picture 1` | a reduced still picture header with `seq_level_idx` 8, level 4.0 |
