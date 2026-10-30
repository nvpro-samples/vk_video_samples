#!/bin/bash
# Copyright (c) 2017 Google Inc.

# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Checks the clang-format formatting of the C and C++ lines a change adds or edits. Run it from the
# repository root:
#
#   scripts/check_code_format.sh [base]
#
# base is the ref the change is compared with (by default origin/main, github/main or main, the
# first that resolves). The check covers every change since the merge base of base and HEAD,
# committed or not. A file that clang-format leaves unchanged at the merge base, or a new file, is
# checked; a file that is not clang-format clean there keeps its own style, which its changed lines
# follow, and is listed as skipped. Files under include/vulkan are not checked. CLANG_FORMAT names
# the clang-format to run (clang-format by default).
#
# Exit status: 0 formatted, or nothing to check; 1 formatting needed; 2 the base does not resolve,
# or clang-format or git clang-format is missing.

set -u

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

CLANG_FORMAT=${CLANG_FORMAT:-clang-format}
if ! command -v "${CLANG_FORMAT}" > /dev/null || ! git clang-format -h > /dev/null 2>&1; then
    echo "clang-format and git clang-format are needed (CLANG_FORMAT=${CLANG_FORMAT})" >&2
    exit 2
fi

BASE=${1:-}
if [ -z "${BASE}" ]; then
    for ref in origin/main github/main main; do
        if git rev-parse --verify --quiet "${ref}^{commit}" > /dev/null; then
            BASE=${ref}
            break
        fi
    done
fi
if [ -z "${BASE}" ] || ! git rev-parse --verify --quiet "${BASE}^{commit}" > /dev/null; then
    echo "The base ${BASE:-(origin/main, github/main, main)} does not resolve: $0 <ref>" >&2
    exit 2
fi
if ! MERGE_BASE=$(git merge-base "${BASE}" HEAD); then
    echo "${BASE} and HEAD have no merge base" >&2
    exit 2
fi

CHECKED=()
SKIPPED=()
while IFS= read -r file; do
    if [ ! -f "${file}" ]; then
        continue
    fi
    if git cat-file -e "${MERGE_BASE}:${file}" 2> /dev/null &&
            ! git show "${MERGE_BASE}:${file}" |
                "${CLANG_FORMAT}" --style=file --assume-filename="${file}" |
                cmp -s - <(git show "${MERGE_BASE}:${file}"); then
        SKIPPED+=("${file}")
        continue
    fi
    CHECKED+=("${file}")
done < <(git diff --name-only "${MERGE_BASE}" -- | grep -v -E "^include/vulkan" | grep -E "\.(cpp|cc|c\+\+|cxx|c|h|hpp)$")

for file in "${SKIPPED[@]}"; do
    echo "skipped, not clang-format clean at ${MERGE_BASE:0:12}: ${file}"
done
if [ ${#CHECKED[@]} -eq 0 ]; then
    echo -e "${GREEN}No source code to check for formatting.${NC}"
    exit 0
fi

FORMAT_DIFF=$(git clang-format --binary "${CLANG_FORMAT}" --style file --diff "${MERGE_BASE}" -- "${CHECKED[@]}" 2>&1)
case "${FORMAT_DIFF}" in
    "" | "no modified files to format" | "clang-format did not modify any files")
        echo -e "${GREEN}All source code in PR properly formatted.${NC}"
        exit 0
        ;;
esac
echo -e "${RED}Found formatting errors!${NC}"
echo "${FORMAT_DIFF}"
exit 1
