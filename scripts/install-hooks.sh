#!/bin/bash
#
# Install git hooks for this repository.
# Run once after cloning:
#   bash scripts/install-hooks.sh
#
# This points core.hooksPath to scripts/git-hooks/ so all developers
# share the same hooks without manually copying files.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$REPO_ROOT" || exit 1

git config core.hooksPath scripts/git-hooks
echo "Git hooks installed (core.hooksPath = scripts/git-hooks)"

# Verify clang-format is available
if command -v clang-format &>/dev/null; then
    echo "clang-format found: $(clang-format --version)"
else
    echo "warning: clang-format not found — install it for pre-commit format checking"
    echo "  Windows: included with Visual Studio or LLVM installer"
    echo "  Linux:   sudo apt install clang-format"
    echo "  macOS:   brew install clang-format"
fi
