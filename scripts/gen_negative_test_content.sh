#!/usr/bin/env bash
# Generate the inputs for the NEGATIVE test cells -- the ones whose pass condition is a
# clean capability rejection, not a successful decode/encode.
#
# A negative cell only proves something if the input is genuinely the profile it claims to
# be. If the bitstream were quietly 4:2:0, the "correct rejection" would be an accident and
# the cell would keep passing after the thing it guards regressed. Every artifact here is
# therefore checked with ffprobe after it is written, and the script fails if the profile,
# pixel format or chroma subsampling is not what the cell needs.
#
# Inputs are public: the same YUVs the encode suite already downloads.
set -euo pipefail

OUT=${OUT:-/data/MediaContent/media/compressed/negative_cells}
WORK=${WORK:-$(mktemp -d)}
BUCKET=https://storage.googleapis.com/vulkan-video-samples/yuv
FRAMES=${FRAMES:-15}
W=352
H=288

mkdir -p "$OUT" "$WORK"

fetch() { # fetch <name>
    [ -f "$WORK/$1" ] || curl -fL -o "$WORK/$1" "$BUCKET/$1"
}

# ---------------------------------------------------------------------------
# 1. H.264 High 4:4:4 Predictive elementary stream.
#
#    NVDEC has no H.264 4:4:4 decode at any bit depth -- there is no NVDEC caps entry for
#    it and nvcuvid rejects it the same way -- so the Vulkan decode capability query must
#    return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR. The ENCODE side does support
#    4:4:4, which is exactly why this needs a negative cell: the asymmetry is easy to
#    "fix" in the wrong direction.
# ---------------------------------------------------------------------------
fetch 352x288_15_i444.yuv
ffmpeg -v error -y -f rawvideo -pix_fmt yuv444p -s ${W}x${H} -i "$WORK/352x288_15_i444.yuv" \
       -frames:v "$FRAMES" -c:v libx264 -profile:v high444 -pix_fmt yuv444p \
       -bsf:v h264_mp4toannexb -f h264 "$OUT/h264_high444_${W}x${H}.264"

# ---------------------------------------------------------------------------
# 2. 12-bit 4:2:0 raw input for the encode side.
#
#    The driver advertises 12-bit under __VK_VIDEO_DECODE_USAGE_MASK only: NVENC has no
#    12-bit encode on any shipping chip. Built from the 10-bit input the encode suite
#    already uses, shifted left by 2 so the samples are MSB-aligned in the 16-bit word the
#    same way real 12-bit content is -- not a 10-bit file relabelled.
# ---------------------------------------------------------------------------
fetch 352x288_420_10le.yuv
ffmpeg -v error -y -f rawvideo -pix_fmt yuv420p10le -s ${W}x${H} -i "$WORK/352x288_420_10le.yuv" \
       -frames:v "$FRAMES" -pix_fmt yuv420p12le -f rawvideo \
       "$OUT/${W}x${H}_${FRAMES}_i420_12le.yuv"

# ---------------------------------------------------------------------------
# 3. VP9 Profile 3 (4:2:2 12-bit) is produced by the memory-compression repo's
#    scripts/gen_12bit_decode_clips.sh, alongside the positive 12-bit clips, so that all
#    five VP9/HEVC 12-bit artifacts come from one master and differ only by format.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Gates. Each asserts the property the cell depends on; a mismatch is fatal.
# ---------------------------------------------------------------------------
probe() { ffprobe -v error -select_streams v:0 -show_entries "stream=$2" -of csv=p=0:nk=1 "$1"; }

h264="$OUT/h264_high444_${W}x${H}.264"
got_profile=$(probe "$h264" profile)
got_pixfmt=$(probe "$h264" pix_fmt)
[ "$got_pixfmt" = "yuv444p" ] || { echo "FAIL: $h264 is $got_pixfmt, expected yuv444p" >&2; exit 1; }
# ffprobe spells it "High 4:4:4 Predictive" -- colons included, so match that, not "444".
case "$got_profile" in
    *4:4:4*) ;;
    *) echo "FAIL: $h264 reports profile '$got_profile', expected a 4:4:4 profile" >&2; exit 1;;
esac

yuv12="$OUT/${W}x${H}_${FRAMES}_i420_12le.yuv"
expected_bytes=$(( W * H * 3 / 2 * 2 * FRAMES ))
actual_bytes=$(stat -c %s "$yuv12")
[ "$actual_bytes" = "$expected_bytes" ] || {
    echo "FAIL: $yuv12 is $actual_bytes bytes, expected $expected_bytes" >&2; exit 1; }
# The 12-bit gate that actually bites: samples must exceed the 10-bit range, or this is a
# 10-bit file with a 12-bit name and the encoder would be rejected for the wrong reason.
python3 - "$yuv12" <<'PY'
import sys, array
a = array.array('H')
with open(sys.argv[1], 'rb') as f:
    a.frombytes(f.read(2 * 1024 * 1024))
if sys.byteorder != 'little':
    a.byteswap()
peak = max(a)
if peak <= 1023:
    sys.exit("FAIL: peak sample %d fits in 10 bits -- not 12-bit content" % peak)
if peak > 4095:
    sys.exit("FAIL: peak sample %d exceeds 12 bits" % peak)
print("  12-bit gate: peak sample %d (10-bit max 1023, 12-bit max 4095)" % peak)
PY

echo
printf '%-34s %-10s %s\n' ARTIFACT BYTES SHA256
for f in "$h264" "$yuv12"; do
    printf '%-34s %-10s %s\n' "$(basename "$f")" "$(stat -c %s "$f")" "$(sha256sum "$f" | cut -d' ' -f1)"
done
echo
if [ "$(cd "$OUT" && pwd)" = "$(cd "$(dirname "$0")/../tests/resources/video/negative" 2>/dev/null && pwd)" ]; then
    echo "Written straight into tests/resources/ -- the suite will pick these up."
else
    echo "Symlink or copy these into tests/resources/video/negative/ (the cells that"
    echo "use them carry \"expected_result\": \"unsupported\")."
fi
