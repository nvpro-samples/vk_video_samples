#!/bin/bash
# Test script for VulkanFilterYuvCompute standalone tests
# Runs vk_filter_test with various test suites

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The build directory: <repo>/build unless BUILD_DIR names another. The binary lands in its bin/.
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/../../../../build}"
TEST_APP="${BUILD_DIR}/bin/vk_filter_test"

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
    echo "  cmake --build . --target vk_filter_test"
    echo "or name another build directory: BUILD_DIR=<dir> $0"
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
        --smoke|--all|--regression|--production|--primaries)
            TEST_SUITE="${1#--}"
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
            echo "  --smoke          Run the smoke set (default)"
            echo "  --all            Run every set but the disabled cases"
            echo "  --regression     Run the regression set"
            echo "  --production     Run the production set"
            echo "  --primaries      Run the color primaries set"
            echo "  --test NAME      Run one case by name, a disabled one included"
            echo "  --list           List every set, and the disabled cases with their defects"
            echo "  --help, -h       Show this help"
            echo ""
            echo "The build directory is <repo>/build; set BUILD_DIR to name another."
            echo ""
            echo "Examples:"
            echo "  $0 --smoke -v              # Smoke tests with validation"
            echo "  $0 --all                   # Every set"
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

# Run the test. Under set -e a failing run would end the script here, before the summary.
RESULT=0
${CMD} || RESULT=$?

echo ""
echo "=============================================="
if [ ${RESULT} -eq 0 ]; then
    echo -e "${GREEN}All tests PASSED!${NC}"
else
    echo -e "${RED}Some tests FAILED (exit code: ${RESULT})${NC}"
fi
echo "=============================================="

exit ${RESULT}
