#!/usr/bin/env bash
#
# Build the encoder's DPB library, vkvideo-dpb (common/libs/VkVideoDpb), and its test as C only, and
# run the test.
#
# The library is C99 so that a client in C, or in any language with a C interface, can use it. The
# CMake build compiles it with the rest of the tree, where a C++-only construct in a header or a
# compiler extension in a source would not be seen. This compiles every source and the test on their
# own, as ISO C99 with warnings as errors, against the Vulkan headers the build pins, with no CMake.
#
#   check_dpb_library_c.sh [out dir]
#
#   CC                   the compiler (cc); gcc and clang take the same flags
#   SANITIZE=1           also -fsanitize=address,undefined
#   VULKAN_HEADERS_DIR   a Vulkan-Headers checkout to use instead of cloning the pinned tag
#
# Exit status: 0 when every source compiles without a warning and the test passes.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/BUILD_DPB_C}
cc=${CC:-cc}
lib=$root/common/libs/VkVideoDpb
mkdir -p "$out"

# The Vulkan-Headers tag the build fetches: one pin for both.
tag=$(sed -n 's/^ *GIT_TAG \(v[0-9.]*\)$/\1/p' "$root/cmake/FindVulkanAndDependencies.cmake" | head -1)
if [ -z "${VULKAN_HEADERS_DIR:-}" ]; then
    [ -n "$tag" ] || { echo "no Vulkan-Headers tag in cmake/FindVulkanAndDependencies.cmake" >&2; exit 1; }
    VULKAN_HEADERS_DIR=$out/Vulkan-Headers-$tag
    if [ ! -d "$VULKAN_HEADERS_DIR" ]; then
        git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" https://github.com/KhronosGroup/Vulkan-Headers.git "$VULKAN_HEADERS_DIR"
    fi
fi

flags=(-std=c99 -pedantic -Werror -Wall -Wextra -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes
       -I"$lib/include" -I"$VULKAN_HEADERS_DIR/include")
if [ "${SANITIZE:-0}" = 1 ]; then
    flags+=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all)
else
    flags+=(-O2)
fi

echo "$("$cc" --version | head -1); Vulkan-Headers ${tag:-at $VULKAN_HEADERS_DIR}"
objects=()
for source in "$lib"/src/*.c "$lib"/tests/vvs_dpb_test.c; do
    object=$out/$(basename "$source" .c).o
    "$cc" "${flags[@]}" -c "$source" -o "$object"
    objects+=("$object")
done
"$cc" "${flags[@]}" "${objects[@]}" -o "$out/vkvideo-dpb-test-c99"
"$out/vkvideo-dpb-test-c99"
