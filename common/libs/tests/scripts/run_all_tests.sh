#!/bin/bash
# Comprehensive test runner for VulkanFilterYuvCompute
# Tests both standalone vk_filter_test and integration with ThreadedRenderingVk

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../../../build"
TEST_APP="${BUILD_DIR}/common/libs/tests/vk_filter_test"

# ThreadedRenderingVk paths
THREADED_DIR="/data/nvidia/vulkan/samples/ThreadedRenderingVk_Standalone"
THREADED_APP="${THREADED_DIR}/_bin/Release/ThreadedRenderingVk"
THREADED_SCRIPT="${THREADED_DIR}/scripts/test_dump_formats.sh"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

PASSED=0
FAILED=0
SKIPPED=0

print_header() {
    echo ""
    echo "=============================================="
    echo -e "${BOLD}$1${NC}"
    echo "=============================================="
}

run_test_suite() {
    local NAME="$1"
    local CMD="$2"
    
    echo ""
    echo -e "${CYAN}[$NAME]${NC}"
    echo "  Command: ${CMD}"
    
    if eval "${CMD}"; then
        echo -e "  ${GREEN}✓ PASSED${NC}"
        PASSED=$((PASSED + 1))
        return 0
    else
        echo -e "  ${RED}✗ FAILED${NC}"
        FAILED=$((FAILED + 1))
        return 1
    fi
}

print_header "VulkanFilterYuvCompute Comprehensive Test Suite"

echo ""
echo "Test applications:"
echo "  1. vk_filter_test    - Standalone filter class tests"
echo "  2. ThreadedRenderingVk - Integration tests (file dump)"
echo ""

# Check if vk_filter_test exists
if [ ! -f "${TEST_APP}" ]; then
    echo -e "${YELLOW}WARNING: vk_filter_test not found at ${TEST_APP}${NC}"
    echo "Build it with: cd ${BUILD_DIR} && cmake .. -DBUILD_TESTS=ON && make vk_filter_test"
    SKIP_STANDALONE=1
else
    SKIP_STANDALONE=0
fi

# Check if ThreadedRenderingVk exists
if [ ! -f "${THREADED_APP}" ]; then
    echo -e "${YELLOW}WARNING: ThreadedRenderingVk not found at ${THREADED_APP}${NC}"
    SKIP_INTEGRATION=1
else
    SKIP_INTEGRATION=0
fi

#
# Part 1: Standalone vk_filter_test
#
print_header "Part 1: Standalone Filter Tests (vk_filter_test)"

if [ "${SKIP_STANDALONE}" == "1" ]; then
    echo -e "${YELLOW}Skipping - application not found${NC}"
    SKIPPED=$((SKIPPED + 1))
else
    # Smoke tests (quick sanity check)
    run_test_suite "Smoke Tests" "${TEST_APP} --smoke" || true
    
    # RGBA to YCbCr (known to work)
    run_test_suite "RGBA→YCbCr (8-bit)" "${TEST_APP} --test TC001_RGBA_to_NV12" || true
    run_test_suite "RGBA→YCbCr (10-bit P010)" "${TEST_APP} --test TC002_RGBA_to_P010" || true
    run_test_suite "RGBA→YCbCr (4:2:2 NV16)" "${TEST_APP} --test TC005_RGBA_to_NV16" || true
    run_test_suite "RGBA→YCbCr (4:4:4)" "${TEST_APP} --test TC007_RGBA_to_YUV444" || true
    
    # YCbCr Copy (known to work)
    run_test_suite "YCbCr Copy NV12" "${TEST_APP} --test TC040_YCbCrCopy_NV12" || true
    
    # YCbCr Clear (known to work)
    run_test_suite "YCbCr Clear NV12" "${TEST_APP} --test TC050_YCbCrClear_NV12" || true
    
    # Edge cases
    run_test_suite "Small Resolution 64x64" "${TEST_APP} --test TC100_Small_Resolution_64x64" || true
fi

#
# Part 2: ThreadedRenderingVk Integration Tests
#
print_header "Part 2: Integration Tests (ThreadedRenderingVk)"

if [ "${SKIP_INTEGRATION}" == "1" ]; then
    echo -e "${YELLOW}Skipping - application not found${NC}"
    SKIPPED=$((SKIPPED + 1))
else
    if [ -f "${THREADED_SCRIPT}" ]; then
        echo "Running: ${THREADED_SCRIPT}"
        run_test_suite "ThreadedRenderingVk Dump Formats" "${THREADED_SCRIPT}" || true
    else
        # Run individual integration tests
        OUTPUT_DIR="/tmp/filter_test_output"
        mkdir -p "${OUTPUT_DIR}"
        cd "${OUTPUT_DIR}"
        
        # Test RGBA dump (no filter)
        run_test_suite "RGBA Raw Dump" \
            "timeout 30 ${THREADED_APP} --postFilter 0 --dump 1 --dumpFormat raw --dumpSource 0 --dumpPath rgba_test --dumpFrames 5 --width 640 --height 480 --endFrame 10 && test -f rgba_test.yuv" || true
        
        # Test NV12 Y4M dump (with filter)
        run_test_suite "NV12 Y4M Dump" \
            "timeout 30 ${THREADED_APP} --postFilter 1 --presentSource 1 --postFilterFormat 0 --dump 1 --dumpFormat y4m --dumpSource 2 --dumpPath nv12_test --dumpFrames 5 --width 640 --height 480 --endFrame 10 && test -f nv12_test.y4m" || true
        
        # Test I420 dump
        run_test_suite "I420 Raw Dump" \
            "timeout 30 ${THREADED_APP} --postFilter 1 --presentSource 1 --postFilterFormat 3 --dump 1 --dumpFormat raw --dumpSource 2 --dumpPath i420_test --dumpFrames 5 --width 640 --height 480 --endFrame 10 && test -f i420_test.yuv" || true
        
        # Test NV16 dump
        run_test_suite "NV16 Raw Dump" \
            "timeout 30 ${THREADED_APP} --postFilter 1 --presentSource 1 --postFilterFormat 4 --dump 1 --dumpFormat raw --dumpSource 2 --dumpPath nv16_test --dumpFrames 5 --width 640 --height 480 --endFrame 10 && test -f nv16_test.yuv" || true
        
        # Cleanup
        rm -f "${OUTPUT_DIR}"/*.yuv "${OUTPUT_DIR}"/*.y4m 2>/dev/null || true
    fi
fi

#
# Summary
#
print_header "Test Summary"

TOTAL=$((PASSED + FAILED + SKIPPED))

echo ""
echo -e "  ${GREEN}Passed:${NC}  ${PASSED}"
echo -e "  ${RED}Failed:${NC}  ${FAILED}"
echo -e "  ${YELLOW}Skipped:${NC} ${SKIPPED}"
echo "  ─────────────────"
echo "  Total:   ${TOTAL}"
echo ""

if [ ${FAILED} -eq 0 ]; then
    echo -e "${GREEN}${BOLD}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}${BOLD}Some tests failed.${NC}"
    echo ""
    echo "Known issues:"
    echo "  - YCBCR2RGBA: Shader generation bug (type mismatches)"
    echo "  - Y410: Packed format not yet supported"
    echo "  - Buffer I/O: Not yet implemented in filter"
    exit 1
fi
