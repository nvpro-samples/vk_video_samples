#!/bin/bash
# Test script for VulkanFilterYuvCompute standalone tests
# Runs vk_filter_test with various test suites

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../../../build"
TEST_APP="${BUILD_DIR}/common/libs/tests/vk_filter_test"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo "=============================================="
echo " VulkanFilterYuvCompute Test Suite"
echo "=============================================="
echo ""

# Check if test app exists
if [ ! -f "${TEST_APP}" ]; then
    echo -e "${RED}ERROR: Test application not found at ${TEST_APP}${NC}"
    echo ""
    echo "Build it first:"
    echo "  mkdir -p ${BUILD_DIR} && cd ${BUILD_DIR}"
    echo "  cmake .. -DBUILD_TESTS=ON"
    echo "  make -j\$(nproc) vk_filter_test"
    exit 1
fi

# Parse command line arguments
VALIDATE=0
VERBOSE=0
TEST_SUITE="smoke"
SPECIFIC_TEST=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --validate|-v)
            VALIDATE=1
            shift
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        --smoke)
            TEST_SUITE="smoke"
            shift
            ;;
        --rgba2ycbcr)
            TEST_SUITE="rgba2ycbcr"
            shift
            ;;
        --ycbcr2rgba)
            TEST_SUITE="ycbcr2rgba"
            shift
            ;;
        --copy)
            TEST_SUITE="copy"
            shift
            ;;
        --transfer)
            TEST_SUITE="transfer"
            shift
            ;;
        --full)
            TEST_SUITE="full"
            shift
            ;;
        --test)
            SPECIFIC_TEST="$2"
            shift 2
            ;;
        --list)
            echo "Running: ${TEST_APP} --list"
            ${TEST_APP} --list
            exit 0
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --validate, -v   Run with Vulkan validation layers"
            echo "  --verbose        Show verbose output"
            echo "  --smoke          Run smoke tests only (default)"
            echo "  --rgba2ycbcr     Run RGBA to YCbCr conversion tests"
            echo "  --ycbcr2rgba     Run YCbCr to RGBA conversion tests (may fail - shader bug)"
            echo "  --copy           Run YCbCr copy tests"
            echo "  --transfer       Run transfer operation tests"
            echo "  --full           Run all tests"
            echo "  --test NAME      Run specific test by name"
            echo "  --list           List all available tests"
            echo "  --help, -h       Show this help"
            echo ""
            echo "Test suites:"
            echo "  smoke      - Quick sanity check (8 tests)"
            echo "  rgba2ycbcr - RGBA to YCbCr conversions (8 formats)"
            echo "  ycbcr2rgba - YCbCr to RGBA conversions (known shader bugs)"
            echo "  copy       - YCbCr format copy operations"
            echo "  transfer   - Pre/post transfer operations"
            echo "  full       - All tests"
            echo ""
            echo "Examples:"
            echo "  $0 --smoke -v              # Smoke tests with validation"
            echo "  $0 --rgba2ycbcr            # All RGBA to YCbCr tests"
            echo "  $0 --test TC001_RGBA_to_NV12  # Run specific test"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Build command line
CMD="${TEST_APP}"
if [ "${VALIDATE}" == "1" ]; then
    CMD="${CMD} --validate"
    echo "Validation layers: ENABLED"
else
    echo "Validation layers: disabled (use -v to enable)"
fi

if [ "${VERBOSE}" == "1" ]; then
    CMD="${CMD} --verbose"
fi

if [ -n "${SPECIFIC_TEST}" ]; then
    CMD="${CMD} --test ${SPECIFIC_TEST}"
    echo "Running specific test: ${SPECIFIC_TEST}"
else
    CMD="${CMD} --${TEST_SUITE}"
    echo "Test suite: ${TEST_SUITE}"
fi

echo ""
echo -e "${CYAN}Command:${NC}"
echo "  ${CMD}"
echo ""
echo "=============================================="

# Run the test
${CMD}
RESULT=$?

echo ""
echo "=============================================="
if [ ${RESULT} -eq 0 ]; then
    echo -e "${GREEN}All tests PASSED!${NC}"
else
    echo -e "${RED}Some tests FAILED (exit code: ${RESULT})${NC}"
fi
echo "=============================================="

exit ${RESULT}
