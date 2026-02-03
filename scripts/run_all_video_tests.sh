#!/bin/bash
#
# Vulkan Video Samples - Complete Test Suite Runner
# Runs decoder and encoder tests on remote host with validation layer support
#
# Usage:
#   ./run_all_video_tests.sh --video-dir <path> [OPTIONS]
#
# Options:
#   --video-dir     Directory containing test video files (REQUIRED)
#   --decoder       Run decoder tests only
#   --encoder       Run encoder tests only
#   --validate, -v  Enable Vulkan validation layers
#   --aq            Include AQ (Adaptive Quantization) tests
#   --verbose       Show detailed output
#   --codec CODEC   Only test specific codec (h264, h265, av1, vp9)
#   --local         Run locally instead of on remote
#   --remote HOST   Remote hostname/IP (default: 127.0.0.1)
#   --quick         Quick test mode (fewer frames, subset of tests)
#
# The tests run on a remote host via SSH (default: localhost).
# Directories are shared via NFS, so local changes are reflected on the remote.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Remote host configuration (default: loopback/localhost)
REMOTE_HOST="${REMOTE_HOST:-127.0.0.1}"
REMOTE_USER="${REMOTE_USER:-$USER}"
SSH_TARGET="${REMOTE_USER}@${REMOTE_HOST}"
VIDEO_DIR=""  # Must be specified

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# Default options
RUN_DECODER="1"
RUN_ENCODER="1"
DECODER_ARGS=""
ENCODER_ARGS=""
QUICK_MODE=""
RUN_LOCAL=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --decoder)
            RUN_DECODER="1"
            RUN_ENCODER=""
            shift
            ;;
        --encoder)
            RUN_DECODER=""
            RUN_ENCODER="1"
            shift
            ;;
        --validate|-v)
            DECODER_ARGS="$DECODER_ARGS --validate"
            ENCODER_ARGS="$ENCODER_ARGS --validate"
            shift
            ;;
        --aq)
            ENCODER_ARGS="$ENCODER_ARGS --aq"
            shift
            ;;
        --verbose)
            DECODER_ARGS="$DECODER_ARGS --verbose"
            ENCODER_ARGS="$ENCODER_ARGS --verbose"
            shift
            ;;
        --codec)
            DECODER_ARGS="$DECODER_ARGS --codec $2"
            ENCODER_ARGS="$ENCODER_ARGS --codec $2"
            shift 2
            ;;
        --local)
            RUN_LOCAL="1"
            DECODER_ARGS="$DECODER_ARGS --local"
            ENCODER_ARGS="$ENCODER_ARGS --local"
            shift
            ;;
        --video-dir)
            VIDEO_DIR="$2"
            DECODER_ARGS="$DECODER_ARGS --video-dir $2"
            ENCODER_ARGS="$ENCODER_ARGS --video-dir $2"
            shift 2
            ;;
        --remote)
            REMOTE_HOST="$2"
            SSH_TARGET="${REMOTE_USER}@${REMOTE_HOST}"
            DECODER_ARGS="$DECODER_ARGS --remote $2"
            ENCODER_ARGS="$ENCODER_ARGS --remote $2"
            shift 2
            ;;
        --quick)
            QUICK_MODE="1"
            export MAX_FRAMES=5
            shift
            ;;
        --help|-h)
            echo "Vulkan Video Samples - Complete Test Suite Runner"
            echo ""
            echo "Usage: $0 --video-dir <path> [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --video-dir PATH  Directory containing test video files (REQUIRED)"
            echo "  --decoder         Run decoder tests only"
            echo "  --encoder         Run encoder tests only"
            echo "  --validate, -v    Enable Vulkan validation layers"
            echo "  --aq              Include AQ (Adaptive Quantization) tests"
            echo "  --verbose         Show detailed output"
            echo "  --codec CODEC     Only test specific codec (h264, h265, av1, vp9)"
            echo "  --local           Run locally instead of on remote"
            echo "  --remote HOST     Remote hostname/IP (default: 127.0.0.1)"
            echo "  --quick           Quick test mode (fewer frames)"
            echo ""
            echo "Environment:"
            echo "  REMOTE_HOST       Remote hostname/IP (default: 127.0.0.1)"
            echo "  REMOTE_USER       Remote username (default: \$USER)"
            echo "  MAX_FRAMES        Max frames per test (default: 30)"
            echo ""
            echo "Examples:"
            echo "  $0 --video-dir /data/videos                    # Run all tests"
            echo "  $0 --video-dir /data/videos --validate         # With validation"
            echo "  $0 --video-dir /data/videos --encoder --aq     # Encoder with AQ"
            echo "  $0 --video-dir /data/videos --remote 192.168.122.216  # On remote"
            echo "  $0 --video-dir /data/videos --quick --validate # Quick validation"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Validate required arguments
if [ -z "$VIDEO_DIR" ]; then
    echo -e "${RED}Error: --video-dir is required${NC}"
    echo "Usage: $0 --video-dir <path> [OPTIONS]"
    exit 1
fi

# Check if video directory exists
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

print_banner() {
    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}║        VULKAN VIDEO SAMPLES - COMPREHENSIVE TEST SUITE              ║${NC}"
    echo -e "${BOLD}╚══════════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "  Date:       $(date '+%Y-%m-%d %H:%M:%S')"
    if [ -n "$RUN_LOCAL" ]; then
        echo -e "  Target:     ${CYAN}localhost${NC}"
    else
        echo -e "  Target:     ${CYAN}$SSH_TARGET${NC}"
    fi
    echo -e "  Video Dir:  $VIDEO_DIR"
    echo -e "  Tests:      ${RUN_DECODER:+Decoder }${RUN_ENCODER:+Encoder}"
    echo -e "  Validation: ${DECODER_ARGS/*--validate*/Enabled}"
    [ -n "$QUICK_MODE" ] && echo -e "  Mode:       ${YELLOW}Quick${NC}"
    echo ""
}

# Check remote connectivity
check_remote() {
    if [ -n "$RUN_LOCAL" ]; then
        echo -e "Running locally..."
        return 0
    fi
    
    echo -n "Checking remote connectivity... "
    if ssh -o ConnectTimeout=5 "$SSH_TARGET" "echo OK" &>/dev/null; then
        echo -e "${GREEN}OK${NC}"
        return 0
    else
        echo -e "${RED}FAILED${NC}"
        echo ""
        echo -e "${RED}Cannot connect to remote at $SSH_TARGET${NC}"
        echo "Options:"
        echo "  1. Check remote host is running and accessible"
        echo "  2. Use --local to run locally"
        echo "  3. Set REMOTE_HOST and REMOTE_USER environment variables"
        exit 1
    fi
}

TOTAL_START=$(date +%s)
DECODER_RESULT=0
ENCODER_RESULT=0

print_banner
check_remote

# Run decoder tests
if [ -n "$RUN_DECODER" ]; then
    echo ""
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}                         DECODER TESTS                                ${NC}"
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    
    if "$SCRIPT_DIR/run_decoder_tests.sh" $DECODER_ARGS; then
        echo -e "\n${GREEN}Decoder tests completed successfully${NC}"
    else
        DECODER_RESULT=1
        echo -e "\n${RED}Decoder tests had failures${NC}"
    fi
fi

# Run encoder tests
if [ -n "$RUN_ENCODER" ]; then
    echo ""
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}                         ENCODER TESTS                                ${NC}"
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    
    if "$SCRIPT_DIR/run_encoder_tests.sh" $ENCODER_ARGS; then
        echo -e "\n${GREEN}Encoder tests completed successfully${NC}"
    else
        ENCODER_RESULT=1
        echo -e "\n${RED}Encoder tests had failures${NC}"
    fi
fi

TOTAL_END=$(date +%s)
TOTAL_DURATION=$((TOTAL_END - TOTAL_START))

# Final summary
echo ""
echo -e "${BOLD}╔══════════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║                         FINAL SUMMARY                                 ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "  Total Duration: ${TOTAL_DURATION}s"
echo ""

if [ -n "$RUN_DECODER" ]; then
    if [ $DECODER_RESULT -eq 0 ]; then
        echo -e "  Decoder Tests: ${GREEN}✓ PASSED${NC}"
    else
        echo -e "  Decoder Tests: ${RED}✗ FAILED${NC}"
    fi
fi

if [ -n "$RUN_ENCODER" ]; then
    if [ $ENCODER_RESULT -eq 0 ]; then
        echo -e "  Encoder Tests: ${GREEN}✓ PASSED${NC}"
    else
        echo -e "  Encoder Tests: ${RED}✗ FAILED${NC}"
    fi
fi

echo ""

# Exit with error if any tests failed
if [ $DECODER_RESULT -ne 0 ] || [ $ENCODER_RESULT -ne 0 ]; then
    echo -e "${RED}${BOLD}Some tests failed. See above for details.${NC}"
    exit 1
else
    echo -e "${GREEN}${BOLD}All tests passed!${NC}"
    exit 0
fi
