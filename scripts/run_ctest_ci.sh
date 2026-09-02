#!/usr/bin/env bash
#
# Run the CTest suite the way CI runs it.
#
# A plain `ctest` is not enough on a GPU-less runner: the suite reports
# 2 passed / 4 skipped / 8 failed there, and a job that is red on arrival is a
# job people learn to ignore.
#
# So the suite is split by LABEL:
#
#   device-free  needs no Vulkan device; must be GREEN everywhere; gating.
#   gpu          needs a real GPU; exits 77 (or 2 for the dma-buf import test)
#                and is reported SKIPPED when there is no device, so this set
#                is also green on a GPU-less runner -- and really gates on a
#                runner that has one.
#
# Both sets are gating. `--no-tests=error` means a label typo or an empty set
# fails the job instead of silently passing, and the audit below means a new
# add_test() that forgets its label fails the job too. Both are there because
# the failure mode this script exists to fix is a check that cannot fail.
#
# Usage: scripts/run_ctest_ci.sh [build-dir]   (default: BUILD)

set -u -o pipefail

BUILD_DIR="${1:-BUILD}"

if [ ! -f "${BUILD_DIR}/CTestTestfile.cmake" ]; then
    echo "ERROR: no CTestTestfile.cmake in '${BUILD_DIR}'." >&2
    echo "       Configure with CTest enabled first, e.g." >&2
    echo "       cmake -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=Release" >&2
    exit 1
fi

count_tests() {
    # $@ are extra ctest args. Read the count out of `ctest -N`, and read the
    # exit status of ctest itself rather than of the pipeline tail.
    local out status
    out="$(ctest --test-dir "${BUILD_DIR}" -N "$@" 2>&1)"
    status=$?
    if [ "${status}" -ne 0 ]; then
        echo "ERROR: 'ctest -N $*' failed:" >&2
        echo "${out}" >&2
        exit 1
    fi
    echo "${out}" | sed -n 's/^Total Tests: \([0-9]*\)$/\1/p'
}

echo "=============================================================="
echo " CTest label audit"
echo "=============================================================="
TOTAL="$(count_tests)"
LABELLED="$(count_tests -L 'device-free|gpu')"

if [ -z "${TOTAL}" ] || [ -z "${LABELLED}" ]; then
    echo "ERROR: could not parse the test counts out of 'ctest -N'." >&2
    exit 1
fi

echo "registered tests: ${TOTAL}"
echo "labelled tests  : ${LABELLED}"

if [ "${TOTAL}" -eq 0 ]; then
    echo "ERROR: zero tests registered. Either the top-level enable_testing()" >&2
    echo "       was dropped or BUILD_TESTS is OFF. A ctest step over an empty" >&2
    echo "       suite is a CI step that can never fail -- refusing to pass." >&2
    exit 1
fi

if [ "${TOTAL}" -ne "${LABELLED}" ]; then
    echo "ERROR: $(( TOTAL - LABELLED )) registered test(s) carry neither the" >&2
    echo "       'device-free' nor the 'gpu' label, so they are in no CI set" >&2
    echo "       and gate nothing. Unlabelled:" >&2
    comm -23 \
        <(ctest --test-dir "${BUILD_DIR}" -N | sed -n 's/^  Test *#[0-9]*: //p' | sort) \
        <(ctest --test-dir "${BUILD_DIR}" -N -L 'device-free|gpu' | sed -n 's/^  Test *#[0-9]*: //p' | sort) \
        | sed 's/^/         /' >&2
    exit 1
fi
echo "OK: every registered test is in exactly one CI set."
echo

echo "=============================================================="
echo " device-free suite (gating; must be green on any host)"
echo "=============================================================="
ctest --test-dir "${BUILD_DIR}" --output-on-failure --no-tests=error \
      -L '^device-free$'
DEVICE_FREE_STATUS=$?
echo "device-free ctest exit status: ${DEVICE_FREE_STATUS}"
echo

echo "=============================================================="
echo " gpu suite (gating; all-SKIP when the host has no GPU)"
echo "=============================================================="
ctest --test-dir "${BUILD_DIR}" --output-on-failure --no-tests=error \
      -L '^gpu$'
GPU_STATUS=$?
echo "gpu ctest exit status: ${GPU_STATUS}"
echo

if [ "${DEVICE_FREE_STATUS}" -ne 0 ] || [ "${GPU_STATUS}" -ne 0 ]; then
    echo "FAILED (device-free=${DEVICE_FREE_STATUS} gpu=${GPU_STATUS})"
    exit 1
fi

echo "PASSED (device-free=0 gpu=0)"
exit 0
