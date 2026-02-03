#!/bin/bash
#
# Vulkan Video Encoder Test Runner
# Tests encoder functionality for all supported codecs (H.264, H.265, AV1)
# with optional Adaptive Quantization (AQ) testing
#
# Usage:
#   ./run_encoder_tests.sh --video-dir <path> [--validate] [--verbose] [--aq] [--codec h264|h265|av1]
#
# The tests run on a remote host via SSH (default: localhost).
# Directories are shared via NFS, so local changes are reflected on the remote.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
VIDEO_DIR=""  # Must be specified
OUTPUT_DIR="${OUTPUT_DIR:-/tmp/vulkan_encoder_tests}"
MAX_FRAMES="${MAX_FRAMES:-30}"

# Remote host configuration (default: loopback/localhost)
REMOTE_HOST="${REMOTE_HOST:-127.0.0.1}"
REMOTE_USER="${REMOTE_USER:-$USER}"
SSH_TARGET="${REMOTE_USER}@${REMOTE_HOST}"

# Encoder executable
ENCODER="$BUILD_DIR/vk_video_encoder/demos/vk-video-enc-test"

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
ENABLE_AQ=""
RUN_LOCAL=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --validate|-v)
            VALIDATE="1"
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
        --aq)
            ENABLE_AQ="1"
            shift
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
            echo "Usage: $0 --video-dir <path> [--validate] [--verbose] [--aq] [--codec h264|h265|av1] [--local]"
            echo ""
            echo "Options:"
            echo "  --video-dir PATH  Directory containing test video files (REQUIRED)"
            echo "  --validate, -v    Enable Vulkan validation layers"
            echo "  --verbose         Show detailed output"
            echo "  --aq              Include Adaptive Quantization (AQ) tests"
            echo "  --codec CODEC     Only test specific codec (h264, h265, av1)"
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

# Run a basic encoder test
# Args: NAME CODEC INPUT WIDTH HEIGHT BPP CHROMA [EXTRA_ARGS...]
run_encode_test() {
    local NAME="$1"
    local CODEC="$2"
    local INPUT="$3"
    local WIDTH="$4"
    local HEIGHT="$5"
    local BPP="${6:-8}"
    local CHROMA="${7:-420}"
    shift 7
    local EXTRA_ARGS="$@"
    
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
    
    # Determine output extension
    case $CODEC in
        h264|avc) EXT=".264" ;;
        h265|hevc) EXT=".265" ;;
        av1) EXT=".ivf" ;;
        *) EXT=".bin" ;;
    esac
    
    local OUTPUT_FILE="$OUTPUT_DIR/${NAME}${EXT}"
    local CODEC_ARG="$CODEC"
    [ "$CODEC" = "h265" ] && CODEC_ARG="hevc"
    
    # Build the remote command with validation environment if needed
    local ENV_VARS=""
    if [ -n "$VALIDATE" ]; then
        ENV_VARS="VK_LOADER_LAYERS_ENABLE='*validation' VK_VALIDATION_VALIDATE_SYNC=true"
    fi
    
    local CMD="$ENV_VARS $ENCODER -i '$INPUT' -c $CODEC_ARG --inputWidth $WIDTH --inputHeight $HEIGHT"
    CMD="$CMD --inputChromaSubsampling $CHROMA --numFrames $MAX_FRAMES -o '$OUTPUT_FILE'"
    
    if [ "$BPP" != "8" ]; then
        CMD="$CMD --inputBpp $BPP"
    fi
    
    if [ -n "$EXTRA_ARGS" ]; then
        CMD="$CMD $EXTRA_ARGS"
    fi
    
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
        elif echo "$OUTPUT" | grep -q "Done processing"; then
            echo -e "  ${GREEN}✓${NC} $NAME (${DURATION}s)"
            ((PASSED++)) || true
        else
            echo -e "  ${RED}✗${NC} $NAME - Unknown error (${DURATION}s)"
            ((FAILED++)) || true
            if [ -n "$VERBOSE" ]; then
                echo "$OUTPUT" | tail -10
            fi
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

# Verify encoder exists (on remote or locally)
if ! run_cmd "test -x '$ENCODER'" 2>/dev/null; then
    echo -e "${RED}Error: Encoder not found at $ENCODER${NC}"
    echo "Please build the project first: cd build && cmake .. && make"
    exit 1
fi

print_header "Vulkan Video Encoder Tests"
if [ -z "$RUN_LOCAL" ]; then
    echo -e "Target Remote: ${CYAN}$SSH_TARGET${NC}"
fi
echo -e "Encoder: $ENCODER"
echo -e "Video Dir: $VIDEO_DIR"
echo -e "Output Dir: $OUTPUT_DIR"
echo -e "Max Frames: $MAX_FRAMES"
[ -n "$VALIDATE" ] && echo -e "Validation: ${GREEN}Enabled${NC}" || echo -e "Validation: Disabled"
[ -n "$ENABLE_AQ" ] && echo -e "AQ Tests: ${GREEN}Enabled${NC}" || echo -e "AQ Tests: Disabled"
echo ""

# ============================================================================
# H.264/AVC Encoder Tests
# ============================================================================

print_header "H.264/AVC Encoder Tests"

# Basic resolution tests - 8-bit
run_encode_test "H264_176x144_8bit" "h264" \
    "$VIDEO_DIR/cts/video/176x144_420_8le.yuv" 176 144 8 420

run_encode_test "H264_352x288_8bit" "h264" \
    "$VIDEO_DIR/cts/video/352x288_420_8le.yuv" 352 288 8 420

run_encode_test "H264_720x480_8bit" "h264" \
    "$VIDEO_DIR/cts/video/720x480_420_8le.yuv" 720 480 8 420

run_encode_test "H264_1920x1080_8bit" "h264" \
    "$VIDEO_DIR/cts/video/1920x1080_420_8le.yuv" 1920 1080 8 420

# Main content
run_encode_test "H264_352x288_main" "h264" \
    "$VIDEO_DIR/352x288_420_8le.yuv" 352 288 8 420

run_encode_test "H264_basketball_1080p" "h264" \
    "$VIDEO_DIR/BasketballDrive_1920x1080_50.yuv" 1920 1080 8 420

# GOP structure tests
run_encode_test "H264_gop_8" "h264" \
    "$VIDEO_DIR/cts/video/352x288_420_8le.yuv" 352 288 8 420 \
    "--gopFrameCount 8 --consecutiveBFrameCount 0"

run_encode_test "H264_gop_16_b3" "h264" \
    "$VIDEO_DIR/cts/video/352x288_420_8le.yuv" 352 288 8 420 \
    "--gopFrameCount 16 --consecutiveBFrameCount 3"

run_encode_test "H264_gop_intra_only" "h264" \
    "$VIDEO_DIR/cts/video/352x288_420_8le.yuv" 352 288 8 420 \
    "--gopFrameCount 1"

# Rate control tests
run_encode_test "H264_rc_cbr" "h264" \
    "$VIDEO_DIR/cts/video/720x480_420_8le.yuv" 720 480 8 420 \
    "--rateControlMode cbr --averageBitrate 2000000"

run_encode_test "H264_rc_vbr" "h264" \
    "$VIDEO_DIR/cts/video/720x480_420_8le.yuv" 720 480 8 420 \
    "--rateControlMode vbr --averageBitrate 2000000"

# ============================================================================
# H.265/HEVC Encoder Tests
# ============================================================================

print_header "H.265/HEVC Encoder Tests"

# Basic resolution tests - 8-bit
run_encode_test "HEVC_176x144_8bit" "h265" \
    "$VIDEO_DIR/cts/video/176x144_420_8le.yuv" 176 144 8 420

run_encode_test "HEVC_352x288_8bit" "h265" \
    "$VIDEO_DIR/cts/video/352x288_420_8le.yuv" 352 288 8 420

run_encode_test "HEVC_720x480_8bit" "h265" \
    "$VIDEO_DIR/cts/video/720x480_420_8le.yuv" 720 480 8 420

run_encode_test "HEVC_1920x1080_8bit" "h265" \
    "$VIDEO_DIR/cts/video/1920x1080_420_8le.yuv" 1920 1080 8 420

# 10-bit tests
run_encode_test "HEVC_352x288_10bit" "h265" \
    "$VIDEO_DIR/cts/video/352x288_420_10le.yuv" 352 288 10 420

run_encode_test "HEVC_720x480_10bit" "h265" \
    "$VIDEO_DIR/cts/video/720x480_420_10le.yuv" 720 480 10 420

run_encode_test "HEVC_1920x1080_10bit" "h265" \
    "$VIDEO_DIR/cts/video/1920x1080_420_10le.yuv" 1920 1080 10 420

# Main content
run_encode_test "HEVC_basketball_1080p" "h265" \
    "$VIDEO_DIR/BasketballDrive_1920x1080_50.yuv" 1920 1080 8 420

# 10-bit test content
run_encode_test "HEVC_jellyfish_10bit_420" "h265" \
    "$VIDEO_DIR/test_content_10bit/jellyfish_1920x1080_10bit_420_100frames.yuv" 1920 1080 10 420

# GOP structure tests
run_encode_test "HEVC_gop_8" "h265" \
    "$VIDEO_DIR/cts/video/352x288_420_8le.yuv" 352 288 8 420 \
    "--gopFrameCount 8 --consecutiveBFrameCount 0"

run_encode_test "HEVC_gop_16_b3" "h265" \
    "$VIDEO_DIR/cts/video/352x288_420_8le.yuv" 352 288 8 420 \
    "--gopFrameCount 16 --consecutiveBFrameCount 3"

# Rate control tests
run_encode_test "HEVC_rc_cbr" "h265" \
    "$VIDEO_DIR/cts/video/720x480_420_8le.yuv" 720 480 8 420 \
    "--rateControlMode cbr --averageBitrate 2000000"

run_encode_test "HEVC_rc_vbr" "h265" \
    "$VIDEO_DIR/cts/video/720x480_420_8le.yuv" 720 480 8 420 \
    "--rateControlMode vbr --averageBitrate 2000000"

# ============================================================================
# AV1 Encoder Tests
# ============================================================================

print_header "AV1 Encoder Tests"

# Basic resolution tests - 8-bit
run_encode_test "AV1_176x144_8bit" "av1" \
    "$VIDEO_DIR/cts/video/176x144_420_8le.yuv" 176 144 8 420

run_encode_test "AV1_352x288_8bit" "av1" \
    "$VIDEO_DIR/cts/video/352x288_420_8le.yuv" 352 288 8 420

run_encode_test "AV1_720x480_8bit" "av1" \
    "$VIDEO_DIR/cts/video/720x480_420_8le.yuv" 720 480 8 420

run_encode_test "AV1_1920x1080_8bit" "av1" \
    "$VIDEO_DIR/cts/video/1920x1080_420_8le.yuv" 1920 1080 8 420

# 10-bit tests
run_encode_test "AV1_352x288_10bit" "av1" \
    "$VIDEO_DIR/cts/video/352x288_420_10le.yuv" 352 288 10 420

run_encode_test "AV1_720x480_10bit" "av1" \
    "$VIDEO_DIR/cts/video/720x480_420_10le.yuv" 720 480 10 420

run_encode_test "AV1_1920x1080_10bit" "av1" \
    "$VIDEO_DIR/cts/video/1920x1080_420_10le.yuv" 1920 1080 10 420

# Main content
run_encode_test "AV1_basketball_1080p" "av1" \
    "$VIDEO_DIR/BasketballDrive_1920x1080_50.yuv" 1920 1080 8 420

# GOP structure tests
run_encode_test "AV1_gop_8" "av1" \
    "$VIDEO_DIR/cts/video/352x288_420_8le.yuv" 352 288 8 420 \
    "--gopFrameCount 8 --consecutiveBFrameCount 0"

run_encode_test "AV1_gop_16_b3" "av1" \
    "$VIDEO_DIR/cts/video/352x288_420_8le.yuv" 352 288 8 420 \
    "--gopFrameCount 16 --consecutiveBFrameCount 3"

# Rate control tests
run_encode_test "AV1_rc_cbr" "av1" \
    "$VIDEO_DIR/cts/video/720x480_420_8le.yuv" 720 480 8 420 \
    "--rateControlMode cbr --averageBitrate 2000000"

run_encode_test "AV1_rc_vbr" "av1" \
    "$VIDEO_DIR/cts/video/720x480_420_8le.yuv" 720 480 8 420 \
    "--rateControlMode vbr --averageBitrate 2000000"

# ============================================================================
# AQ (Adaptive Quantization) Tests
# ============================================================================

if [ -n "$ENABLE_AQ" ]; then
    print_header "Adaptive Quantization (AQ) Tests"
    
    AQ_INPUT="$VIDEO_DIR/cts/video/720x480_420_8le.yuv"
    if [ ! -f "$AQ_INPUT" ]; then
        AQ_INPUT="$VIDEO_DIR/cts/video/352x288_420_8le.yuv"
        AQ_WIDTH=352
        AQ_HEIGHT=288
    else
        AQ_WIDTH=720
        AQ_HEIGHT=480
    fi
    
    for CODEC in h264 h265 av1; do
        # Skip if codec filter set
        if [ -n "$FILTER_CODEC" ] && [ "$FILTER_CODEC" != "$CODEC" ]; then
            continue
        fi
        
        echo -e "\n  ${CYAN}$CODEC AQ Tests:${NC}"
        
        # Spatial AQ only
        run_encode_test "AQ_${CODEC}_spatial_default" "$CODEC" \
            "$AQ_INPUT" $AQ_WIDTH $AQ_HEIGHT 8 420 \
            "--spatialAQStrength 0.0 --temporalAQStrength -2.0"
        
        run_encode_test "AQ_${CODEC}_spatial_0.5" "$CODEC" \
            "$AQ_INPUT" $AQ_WIDTH $AQ_HEIGHT 8 420 \
            "--spatialAQStrength 0.5 --temporalAQStrength -2.0"
        
        run_encode_test "AQ_${CODEC}_spatial_max" "$CODEC" \
            "$AQ_INPUT" $AQ_WIDTH $AQ_HEIGHT 8 420 \
            "--spatialAQStrength 1.0 --temporalAQStrength -2.0"
        
        # Temporal AQ only
        run_encode_test "AQ_${CODEC}_temporal_default" "$CODEC" \
            "$AQ_INPUT" $AQ_WIDTH $AQ_HEIGHT 8 420 \
            "--spatialAQStrength -2.0 --temporalAQStrength 0.0"
        
        run_encode_test "AQ_${CODEC}_temporal_0.5" "$CODEC" \
            "$AQ_INPUT" $AQ_WIDTH $AQ_HEIGHT 8 420 \
            "--spatialAQStrength -2.0 --temporalAQStrength 0.5"
        
        run_encode_test "AQ_${CODEC}_temporal_max" "$CODEC" \
            "$AQ_INPUT" $AQ_WIDTH $AQ_HEIGHT 8 420 \
            "--spatialAQStrength -2.0 --temporalAQStrength 1.0"
        
        # Combined AQ
        run_encode_test "AQ_${CODEC}_combined_default" "$CODEC" \
            "$AQ_INPUT" $AQ_WIDTH $AQ_HEIGHT 8 420 \
            "--spatialAQStrength 0.0 --temporalAQStrength 0.0"
        
        run_encode_test "AQ_${CODEC}_combined_medium" "$CODEC" \
            "$AQ_INPUT" $AQ_WIDTH $AQ_HEIGHT 8 420 \
            "--spatialAQStrength 0.5 --temporalAQStrength 0.5"
        
        run_encode_test "AQ_${CODEC}_combined_max" "$CODEC" \
            "$AQ_INPUT" $AQ_WIDTH $AQ_HEIGHT 8 420 \
            "--spatialAQStrength 1.0 --temporalAQStrength 1.0"
    done
fi

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
