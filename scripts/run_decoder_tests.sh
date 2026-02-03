#!/bin/bash
#
# Vulkan Video Decoder Test Runner
# Tests decoder functionality for all supported codecs (H.264, H.265, AV1, VP9)
#
# Usage:
#   ./run_decoder_tests.sh --video-dir <path> [--validate] [--verbose] [--codec h264|h265|av1|vp9]
#
# The tests run on a remote host via SSH (default: localhost).
# Directories are shared via NFS, so local changes are reflected on the remote.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
VIDEO_DIR=""  # Must be specified
OUTPUT_DIR="${OUTPUT_DIR:-/tmp/vulkan_decoder_tests}"
MAX_FRAMES="${MAX_FRAMES:-30}"

# Remote host configuration (default: loopback/localhost)
REMOTE_HOST="${REMOTE_HOST:-127.0.0.1}"
REMOTE_USER="${REMOTE_USER:-$USER}"
SSH_TARGET="${REMOTE_USER}@${REMOTE_HOST}"

# Decoder executable
DECODER="$BUILD_DIR/vk_video_decoder/demos/vk-video-dec-test"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# Counters
PASSED=0
FAILED=0
SKIPPED=0
TOTAL=0

# Options
VALIDATE=""
VERBOSE=""
FILTER_CODEC=""
RUN_LOCAL=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --validate|-v)
            VALIDATE="-v"
            shift
            ;;
        --verbose)
            VERBOSE="1"
            shift
            ;;
        --codec)
            FILTER_CODEC="$2"
            shift 2
            ;;
        --local)
            RUN_LOCAL="1"
            shift
            ;;
        --video-dir)
            VIDEO_DIR="$2"
            shift 2
            ;;
        --remote)
            REMOTE_HOST="$2"
            SSH_TARGET="${REMOTE_USER}@${REMOTE_HOST}"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 --video-dir <path> [--validate] [--verbose] [--codec h264|h265|av1|vp9] [--local]"
            echo ""
            echo "Options:"
            echo "  --video-dir PATH  Directory containing test video files (REQUIRED)"
            echo "  --validate, -v    Enable Vulkan validation layers"
            echo "  --verbose         Show detailed output"
            echo "  --codec CODEC     Only test specific codec (h264, h265, av1, vp9)"
            echo "  --local           Run locally instead of on remote"
            echo "  --remote HOST     Remote hostname/IP (default: 127.0.0.1)"
            echo ""
            echo "Environment:"
            echo "  REMOTE_HOST       Remote hostname/IP (default: 127.0.0.1)"
            echo "  REMOTE_USER       Remote username (default: \$USER)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Helper function to run command (locally or on remote)
run_cmd() {
    if [ -n "$RUN_LOCAL" ]; then
        eval "$@"
    else
        ssh "$SSH_TARGET" "$@"
    fi
}

# Validate required arguments
if [ -z "$VIDEO_DIR" ]; then
    echo -e "${RED}Error: --video-dir is required${NC}"
    echo "Usage: $0 --video-dir <path> [OPTIONS]"
    exit 1
fi

# Check if video directory exists (locally first, then on remote if needed)
check_video_dir() {
    if [ -n "$RUN_LOCAL" ]; then
        test -d "$VIDEO_DIR"
    else
        ssh -o ConnectTimeout=5 "$SSH_TARGET" "test -d '$VIDEO_DIR'" 2>/dev/null
    fi
}

if ! check_video_dir; then
    echo -e "${RED}Error: Video directory does not exist: $VIDEO_DIR${NC}"
    if [ -z "$RUN_LOCAL" ]; then
        echo "Note: Checking on remote host $SSH_TARGET"
    fi
    exit 1
fi

# Create output directory
run_cmd "mkdir -p '$OUTPUT_DIR'"

# Check remote connectivity (if not local)
if [ -z "$RUN_LOCAL" ]; then
    if ! ssh -o ConnectTimeout=5 "$SSH_TARGET" "echo 'Remote connection OK'" &>/dev/null; then
        echo -e "${RED}Error: Cannot connect to remote at $SSH_TARGET${NC}"
        echo "Use --local to run locally, or check remote connectivity"
        exit 1
    fi
fi

print_header() {
    echo ""
    echo -e "${BOLD}=============================================${NC}"
    echo -e "${BOLD}$1${NC}"
    echo -e "${BOLD}=============================================${NC}"
}

run_test() {
    local NAME="$1"
    local CODEC="$2"
    local INPUT="$3"
    local DESCRIPTION="$4"
    
    # Filter by codec if specified
    if [ -n "$FILTER_CODEC" ] && [ "$FILTER_CODEC" != "$CODEC" ]; then
        return
    fi
    
    ((TOTAL++)) || true
    
    # Check if file exists (on remote or locally)
    local FILE_EXISTS
    if [ -n "$RUN_LOCAL" ]; then
        test -f "$INPUT" && FILE_EXISTS=1 || FILE_EXISTS=0
    else
        ssh "$SSH_TARGET" "test -f '$INPUT'" && FILE_EXISTS=1 || FILE_EXISTS=0
    fi
    
    if [ "$FILE_EXISTS" -eq 0 ]; then
        echo -e "  ${YELLOW}○${NC} $NAME - File not found"
        ((SKIPPED++)) || true
        return
    fi
    
    local OUTPUT_FILE="$OUTPUT_DIR/${NAME}.yuv"
    
    # Build the remote command with validation environment if needed
    local ENV_VARS=""
    if [ -n "$VALIDATE" ]; then
        ENV_VARS="VK_LOADER_LAYERS_ENABLE='*validation' VK_VALIDATION_VALIDATE_SYNC=true"
    fi
    
    local CMD="$ENV_VARS $DECODER -i '$INPUT' --noPresent --codec $CODEC -c $MAX_FRAMES -o '$OUTPUT_FILE' $VALIDATE"
    
    if [ -n "$VERBOSE" ]; then
        echo -e "\n  ${CYAN}Command: $CMD${NC}"
    fi
    
    local START_TIME=$(date +%s.%N)
    
    if OUTPUT=$(run_cmd "$CMD" 2>&1); then
        local END_TIME=$(date +%s.%N)
        local DURATION=$(echo "$END_TIME - $START_TIME" | bc)
        
        # Check for validation errors
        if echo "$OUTPUT" | grep -qi "validation error"; then
            echo -e "  ${RED}✗${NC} $NAME - Validation errors (${DURATION}s)"
            ((FAILED++)) || true
            if [ -n "$VERBOSE" ]; then
                echo "$OUTPUT" | grep -i "validation" | head -5
            fi
        else
            echo -e "  ${GREEN}✓${NC} $NAME (${DURATION}s)"
            ((PASSED++)) || true
        fi
    else
        local END_TIME=$(date +%s.%N)
        local DURATION=$(echo "$END_TIME - $START_TIME" | bc)
        echo -e "  ${RED}✗${NC} $NAME - Failed (${DURATION}s)"
        ((FAILED++)) || true
        if [ -n "$VERBOSE" ]; then
            echo "$OUTPUT" | tail -10
        fi
    fi
}

# Verify decoder exists (on remote or locally)
if ! run_cmd "test -x '$DECODER'" 2>/dev/null; then
    echo -e "${RED}Error: Decoder not found at $DECODER${NC}"
    echo "Please build the project first: cd build && cmake .. && make"
    exit 1
fi

print_header "Vulkan Video Decoder Tests"
if [ -z "$RUN_LOCAL" ]; then
    echo -e "Target Remote: ${CYAN}$SSH_TARGET${NC}"
fi
echo -e "Decoder: $DECODER"
echo -e "Video Dir: $VIDEO_DIR"
echo -e "Output Dir: $OUTPUT_DIR"
echo -e "Max Frames: $MAX_FRAMES"
[ -n "$VALIDATE" ] && echo -e "Validation: ${GREEN}Enabled${NC}" || echo -e "Validation: Disabled"
echo ""

# ============================================================================
# H.264/AVC Tests
# ============================================================================

print_header "H.264/AVC Decoder Tests"

# CTS content
run_test "H264_clip_a" "h264" "$VIDEO_DIR/cts/clip-a.h264" "H.264 CTS clip-a"
run_test "H264_clip_b" "h264" "$VIDEO_DIR/cts/clip-b.h264" "H.264 CTS clip-b"
# Note: clip-c may fail due to DPB slot assertion in parser
run_test "H264_clip_c" "h264" "$VIDEO_DIR/cts/clip-c.h264" "H.264 CTS clip-c"
run_test "H264_4k_ibp" "h264" "$VIDEO_DIR/cts/4k_26_ibp_main.h264" "H.264 4K IBP"
run_test "H264_field" "h264" "$VIDEO_DIR/cts/avc-720x480-field.h264" "H.264 field coding"
run_test "H264_paff" "h264" "$VIDEO_DIR/cts/avc-1440x1080-paff.h264" "H.264 PAFF"

# Main content
run_test "H264_akiyo" "h264" "$VIDEO_DIR/akiyo_176x144_30p_1_0.264" "H.264 Akiyo"
run_test "H264_jellyfish_4k" "h264" "$VIDEO_DIR/jellyfish-250-mbps-4k-uhd-h264.h264" "H.264 Jellyfish 4K"
run_test "H264_tandberg" "h264" "$VIDEO_DIR/MR4_TANDBERG_C.264" "H.264 Tandberg"

# Container formats
run_test "H264_jellyfish_mkv" "h264" "$VIDEO_DIR/jellyfish-100-mbps-hd-h264.mkv" "H.264 Jellyfish MKV"
run_test "H264_golden_flower" "h264" "$VIDEO_DIR/golden_flower_h264_720_30p_7M.mp4" "H.264 Golden Flower MP4"
run_test "H264_fantastic4" "h264" "$VIDEO_DIR/fantastic4_h264_bp_1080_30_20M.mp4" "H.264 Fantastic4 MP4"
run_test "H264_text_in_motion" "h264" "$VIDEO_DIR/TextInMotion-VideoSample-1080p.mp4" "H.264 TextInMotion MP4"

# ============================================================================
# H.265/HEVC Tests
# ============================================================================

print_header "H.265/HEVC Decoder Tests"

# CTS content
run_test "HEVC_clip_d" "h265" "$VIDEO_DIR/cts/clip-d.h265" "H.265 CTS clip-d"
run_test "HEVC_slist_a" "h265" "$VIDEO_DIR/cts/video/hevc-itu-slist-a.h265" "H.265 scaling list A"
run_test "HEVC_slist_b" "h265" "$VIDEO_DIR/cts/video/hevc-itu-slist-b.h265" "H.265 scaling list B"
run_test "HEVC_jellyfish_gob" "h265" "$VIDEO_DIR/cts/jellyfish-250-mbps-4k-uhd-GOB-IPB13.h265" "H.265 Jellyfish GOB"

# Main content
run_test "HEVC_jellyfish_hd" "h265" "$VIDEO_DIR/jellyfish-110-mbps-hd-hevc.h265" "H.265 Jellyfish HD"
run_test "HEVC_jellyfish_4k_10bit" "h265" "$VIDEO_DIR/jellyfish-400-mbps-4k-uhd-hevc-10bit.h265" "H.265 4K 10-bit"
run_test "HEVC_4k_1440p" "h265" "$VIDEO_DIR/4K_1440p.hevc" "H.265 4K 1440p"

# Container formats
run_test "HEVC_jellyfish_mkv" "h265" "$VIDEO_DIR/jellyfish-100-mbps-hd-hevc.mkv" "H.265 Jellyfish MKV"
run_test "HEVC_bt2020_10bit" "h265" "$VIDEO_DIR/hevc_bt2020_10bit.mp4" "H.265 BT.2020 10-bit"
run_test "HEVC_video_mkv" "h265" "$VIDEO_DIR/video-h265.mkv" "H.265 Video MKV"
run_test "HEVC_crab_rave" "h265" "$VIDEO_DIR/crab-rave_2140p@30_hevc_6M.mkv" "H.265 Crab Rave"

# ============================================================================
# AV1 Tests
# ============================================================================

print_header "AV1 Decoder Tests"

# CTS 8-bit content
run_test "AV1_basic_8" "av1" "$VIDEO_DIR/cts/basic-8.ivf" "AV1 Basic 8-bit"
run_test "AV1_cdef_8" "av1" "$VIDEO_DIR/cts/cdef-8.ivf" "AV1 CDEF 8-bit"
run_test "AV1_fkf_8" "av1" "$VIDEO_DIR/cts/forward-key-frame-8.ivf" "AV1 Forward key frame 8-bit"
run_test "AV1_gm_8" "av1" "$VIDEO_DIR/cts/global-motion-8.ivf" "AV1 Global motion 8-bit"
run_test "AV1_lf_8" "av1" "$VIDEO_DIR/cts/loop-filter-8.ivf" "AV1 Loop filter 8-bit"
run_test "AV1_lossless_8" "av1" "$VIDEO_DIR/cts/lossless-8.ivf" "AV1 Lossless 8-bit"
run_test "AV1_oh_8" "av1" "$VIDEO_DIR/cts/order-hint-8.ivf" "AV1 Order hint 8-bit"

# CTS 10-bit content
run_test "AV1_basic_10" "av1" "$VIDEO_DIR/cts/basic-10.ivf" "AV1 Basic 10-bit"
run_test "AV1_cdef_10" "av1" "$VIDEO_DIR/cts/cdef-10.ivf" "AV1 CDEF 10-bit"
run_test "AV1_fkf_10" "av1" "$VIDEO_DIR/cts/forward-key-frame-10.ivf" "AV1 Forward key frame 10-bit"
run_test "AV1_gm_10" "av1" "$VIDEO_DIR/cts/global-motion-10.ivf" "AV1 Global motion 10-bit"
run_test "AV1_lf_10" "av1" "$VIDEO_DIR/cts/loop-filter-10.ivf" "AV1 Loop filter 10-bit"
run_test "AV1_lossless_10" "av1" "$VIDEO_DIR/cts/lossless-10.ivf" "AV1 Lossless 10-bit"
run_test "AV1_oh_10" "av1" "$VIDEO_DIR/cts/order-hint-10.ivf" "AV1 Order hint 10-bit"

# CTS special content
run_test "AV1_allintra" "av1" "$VIDEO_DIR/cts/av1-1-b8-02-allintra.ivf" "AV1 All intra"
run_test "AV1_cdfupdate" "av1" "$VIDEO_DIR/cts/av1-1-b8-04-cdfupdate.ivf" "AV1 CDF update"
run_test "AV1_mv" "av1" "$VIDEO_DIR/cts/av1-1-b8-05-mv.ivf" "AV1 Motion vectors"
run_test "AV1_mfmv" "av1" "$VIDEO_DIR/cts/av1-1-b8-06-mfmv.ivf" "AV1 MFMV"
run_test "AV1_filmgrain" "av1" "$VIDEO_DIR/cts/av1-1-b8-23-film_grain-50.ivf" "AV1 Film grain"

# SVC content
# Note: L2T1 and L2T2 may fail due to decoder SVC layer handling issues (segfault)
run_test "AV1_svc_l1t2" "av1" "$VIDEO_DIR/cts/av1-1-b8-22-svc-L1T2.ivf" "AV1 SVC L1T2"
run_test "AV1_svc_l2t1" "av1" "$VIDEO_DIR/cts/av1-1-b8-22-svc-L2T1.ivf" "AV1 SVC L2T1"
run_test "AV1_svc_l2t2" "av1" "$VIDEO_DIR/cts/av1-1-b8-22-svc-L2T2.ivf" "AV1 SVC L2T2"

# CTS video directory content
run_test "AV1_176x144_basic_8" "av1" "$VIDEO_DIR/cts/video/av1-176x144-main-basic-8.ivf" "AV1 176x144 basic 8-bit"
run_test "AV1_176x144_basic_10" "av1" "$VIDEO_DIR/cts/video/av1-176x144-main-basic-10.ivf" "AV1 176x144 basic 10-bit"
run_test "AV1_352x288_allintra" "av1" "$VIDEO_DIR/cts/video/av1-352x288-main-allintra-8.ivf" "AV1 352x288 all intra"
run_test "AV1_352x288_filmgrain" "av1" "$VIDEO_DIR/cts/video/av1-352x288-main-filmgrain-8.ivf" "AV1 352x288 film grain"
run_test "AV1_1080p_superres" "av1" "$VIDEO_DIR/cts/video/av1-1920x1080-main-superres-8.ivf" "AV1 1080p superres"
run_test "AV1_1080p_intrabc" "av1" "$VIDEO_DIR/cts/video/av1-1920x1080-intrabc-extreme-dv-8.ivf" "AV1 1080p intrabc extreme"
run_test "AV1_sizeup" "av1" "$VIDEO_DIR/cts/video/av1-sizeup-fluster.ivf" "AV1 sizeup fluster"

# ============================================================================
# VP9 Tests
# ============================================================================

print_header "VP9 Decoder Tests"

# Known VP9 content
run_test "VP9_big_buck_bunny_1080p" "vp9" "$VIDEO_DIR/Big_Buck_Bunny_1080_10s_30MB.webm" "VP9 Big Buck Bunny 1080p"

# VP9 content from common test directories
for VP9_FILE in "$VIDEO_DIR"/vp9*.ivf "$VIDEO_DIR"/vp9*.webm "$VIDEO_DIR"/*vp9*.webm "$VIDEO_DIR"/*vp9*.ivf; do
    if [ -f "$VP9_FILE" ]; then
        BASENAME=$(basename "$VP9_FILE" | sed 's/\.[^.]*$//' | tr '-' '_')
        run_test "VP9_${BASENAME}" "vp9" "$VP9_FILE" "VP9 $BASENAME"
    fi
done

# VP9 from cts directory (if any exist)
for VP9_FILE in "$VIDEO_DIR/cts/"*vp9*.ivf "$VIDEO_DIR/cts/"*vp9*.webm "$VIDEO_DIR/cts/video/"*vp9*.ivf; do
    if [ -f "$VP9_FILE" ]; then
        BASENAME=$(basename "$VP9_FILE" | sed 's/\.[^.]*$//' | tr '-' '_')
        run_test "VP9_cts_${BASENAME}" "vp9" "$VP9_FILE" "VP9 CTS $BASENAME"
    fi
done

# ============================================================================
# Summary
# ============================================================================

print_header "Test Summary"

echo ""
echo -e "Total Tests:    $TOTAL"
echo -e "  ${GREEN}✓ Passed:${NC}    $PASSED"
echo -e "  ${RED}✗ Failed:${NC}    $FAILED"
echo -e "  ${YELLOW}○ Skipped:${NC}   $SKIPPED"
echo ""

if [ $FAILED -eq 0 ] && [ $PASSED -gt 0 ]; then
    echo -e "${GREEN}${BOLD}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}${BOLD}Some tests failed.${NC}"
    exit 1
fi
