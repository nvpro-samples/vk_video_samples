#!/usr/bin/env bash
#
# Run the device-free CTest suite of a sanitizer build the way CI runs it.
#
# The build is Debug with AddressSanitizer and UBSan, and every report is
# fatal:
#
#   -fsanitize=address,undefined -fno-sanitize-recover=all
#
# so a test passes only when neither sanitizer reports anything. UBSan's
# bounds check sees the overflows that stay inside one object, where ASan
# sees nothing.
#
# KNOWN lists the tests that a known defect fails under the sanitizers, each
# with the defect. They run too, and each must still fail: a known test that
# passes means its defect is fixed, and its entry goes with that fix. Every
# other device-free test must pass.
#
# Usage: scripts/run_ctest_sanitizers.sh [build-dir]   (default: BUILD-SAN)

set -u -o pipefail

BUILD_DIR="${1:-BUILD-SAN}"

# test name|the defect that fails it
KNOWN=(
    "ParserTraceH265|the H.265 parser dereferences an empty SPS pointer on the first slice"
    "AdapterTraceH265|the H.265 parser reads an empty SPS pointer on the first slice"
    "ParserFuzzReplayH265|the H.265 parser reads an empty SPS pointer on the first slice"
    "ParserCrafted.H265CraftedStream|the H.265 parser reads an empty SPS pointer on the first slice"
    "ParserCrafted.H265RefPicSetCurrBefore9|the H.265 parser reads an empty SPS pointer on the first slice"
    "EncoderExtInputFormatTaxonomy|VkVideoEncoderColorModel has no _MAX_ENUM entry, so the value 7 the test passes is outside the enumeration's range"
    "EncoderExtColorModelRegistrationGate|VkVideoEncoderColorModel has no _MAX_ENUM entry"
)

if [ ! -f "${BUILD_DIR}/CTestTestfile.cmake" ]; then
    echo "ERROR: no CTestTestfile.cmake in '${BUILD_DIR}'." >&2
    exit 1
fi

export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"

exact() {
    # A ctest regular expression that selects exactly the test named $1.
    local name="${1//./\\.}"
    echo "^${name}\$"
}

known_regex=""
for entry in "${KNOWN[@]}"; do
    known_regex="${known_regex:+${known_regex}|}$(exact "${entry%%|*}")"
done

echo "=============================================================="
echo " device-free suite under the sanitizers, known defects excluded"
echo "=============================================================="
ctest --test-dir "${BUILD_DIR}" --output-on-failure --no-tests=error \
      -L '^device-free$' -E "${known_regex}"
STATUS=$?
echo "ctest exit status: ${STATUS}"
echo

echo "=============================================================="
echo " tests of known defects (each must still fail)"
echo "=============================================================="
STALE=0
for entry in "${KNOWN[@]}"; do
    name="${entry%%|*}"
    defect="${entry#*|}"
    if ! ctest --test-dir "${BUILD_DIR}" -N -L '^device-free$' -R "$(exact "${name}")" |
            grep -q '^Total Tests: 1$'; then
        echo "ERROR: ${name} is not a device-free test; update KNOWN." >&2
        STALE=1
        continue
    fi
    if ctest --test-dir "${BUILD_DIR}" -L '^device-free$' -R "$(exact "${name}")" > /dev/null 2>&1; then
        echo "ERROR: ${name} passes. If its defect is fixed, remove it from KNOWN (${defect})." >&2
        STALE=1
    else
        echo "fails as known: ${name} (${defect})"
    fi
done
echo

if [ "${STATUS}" -ne 0 ] || [ "${STALE}" -ne 0 ]; then
    echo "FAILED (ctest=${STATUS} known-list=${STALE})"
    exit 1
fi
echo "PASSED"
exit 0
