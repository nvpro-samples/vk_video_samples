#!/bin/bash
#
# Run DRM format modifier tests with the patched validation layer
#
# This script uses a custom-built validation layer that includes the fix for
# the NULL pointer crash in UpdateBindImageMemoryState() when handling
# disjoint images without VkBindImagePlaneMemoryInfo.
#
# Usage:
#   ./run_tests_with_patched_layer.sh [options]
#
# Options:
#   --gdb                     Run under gdb -tui for debugging
#   --no-env                  Skip sourcing the driver dev environment
#   --ssh-remote <user@host>  Run tests on remote server via SSH
#   --quiet                   Suppress verbose test output
#
# All other options are passed directly to drm_format_mod_test, e.g.:
#   ./run_tests_with_patched_layer.sh --all
#   ./run_tests_with_patched_layer.sh --ycbcr-only
#   ./run_tests_with_patched_layer.sh --report
#   ./run_tests_with_patched_layer.sh --gdb --ycbcr-only
#   ./run_tests_with_patched_layer.sh --ssh-remote tzlatinski@192.168.122.216 --ycbcr-only
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"

# Parse script-specific options
USE_GDB=0
SKIP_DEV_ENV=0
SSH_REMOTE=""
QUIET_MODE=0
TEST_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --gdb)
            USE_GDB=1
            shift
            ;;
        --no-env)
            SKIP_DEV_ENV=1
            shift
            ;;
        --quiet)
            QUIET_MODE=1
            shift
            ;;
        --ssh-remote)
            if [[ -z "$2" || "$2" == --* ]]; then
                echo "ERROR: --ssh-remote requires a user@host argument"
                echo "Example: --ssh-remote tzlatinski@192.168.122.216"
                exit 1
            fi
            SSH_REMOTE="$2"
            shift 2
            ;;
        *)
            TEST_ARGS+=("$1")
            shift
            ;;
    esac
done

# If SSH remote is specified, run the script on the remote server
if [[ -n "$SSH_REMOTE" ]]; then
    echo "========================================"
    echo " Running on remote: $SSH_REMOTE"
    echo "========================================"
    
    # Build the remote command - re-invoke this script without --ssh-remote
    REMOTE_ARGS=""
    [[ "$USE_GDB" -eq 1 ]] && REMOTE_ARGS="$REMOTE_ARGS --gdb"
    [[ "$SKIP_DEV_ENV" -eq 1 ]] && REMOTE_ARGS="$REMOTE_ARGS --no-env"
    [[ "$QUIET_MODE" -eq 1 ]] && REMOTE_ARGS="$REMOTE_ARGS --quiet"
    for arg in "${TEST_ARGS[@]}"; do
        REMOTE_ARGS="$REMOTE_ARGS \"$arg\""
    done
    
    # Execute on remote
    # Note: GDB TUI requires a proper TTY, so we use -t for interactive sessions
    if [[ "$USE_GDB" -eq 1 ]]; then
        exec ssh -t "$SSH_REMOTE" "cd '$SCRIPT_DIR' && ./run_tests_with_patched_layer.sh $REMOTE_ARGS"
    else
        exec ssh "$SSH_REMOTE" "cd '$SCRIPT_DIR' && ./run_tests_with_patched_layer.sh $REMOTE_ARGS"
    fi
fi

# Source the driver development environment (sets up driver build paths)
# Redirect output to /dev/null to avoid cluttering test output
DEV_ENV_SCRIPT="/data/nvidia-linux/dev_a/set_dev_env-dev.sh"
if [[ "$SKIP_DEV_ENV" -eq 0 ]] && [[ -f "$DEV_ENV_SCRIPT" ]]; then
    source "$DEV_ENV_SCRIPT" > /dev/null 2>&1
fi

# Path to the patched validation layer build
PATCHED_LAYER_PATH="/data/nvidia/vulkan/validation-layers-build/Vulkan-ValidationLayers/build-vm/layers"

# Path to the test binary (check both possible locations)
if [[ -x "$REPO_ROOT/build/bin/drm_format_mod_test" ]]; then
    TEST_BIN="$REPO_ROOT/build/bin/drm_format_mod_test"
elif [[ -x "$REPO_ROOT/build/common/libs/tests/drm_format_mod/drm_format_mod_test" ]]; then
    TEST_BIN="$REPO_ROOT/build/common/libs/tests/drm_format_mod/drm_format_mod_test"
else
    echo "ERROR: drm_format_mod_test not found. Please build it first:"
    echo "  cd $REPO_ROOT/build && make drm_format_mod_test"
    exit 1
fi

# Check if patched layer exists (silently fall back if not)
if [[ ! -f "$PATCHED_LAYER_PATH/libVkLayer_khronos_validation.so" ]]; then
    PATCHED_LAYER_PATH=""
fi

echo "========================================"
echo " DRM Format Modifier Test"
echo "========================================"
echo "GPU:         $(lspci 2>/dev/null | grep -i nvidia | head -1 | sed 's/.*: //' || echo 'N/A')"
echo "Layer:       $([ -n "$PATCHED_LAYER_PATH" ] && echo 'patched' || echo 'system')"
echo "GDB:         $([ $USE_GDB -eq 1 ] && echo 'enabled' || echo 'disabled')"
echo "Arguments:   ${TEST_ARGS[*]}"
echo "========================================"
echo ""

# Set environment for validation layer
if [[ -n "$PATCHED_LAYER_PATH" ]]; then
    export VK_LAYER_PATH="$PATCHED_LAYER_PATH"
fi
export VK_LOADER_LAYERS_ENABLE='*validation'

# Suppress loader debug output for cleaner test results
unset VK_LOADER_DEBUG

# Build final arguments - add --verbose by default unless --quiet
FINAL_ARGS=("--validation")
if [[ "$QUIET_MODE" -eq 0 ]]; then
    FINAL_ARGS+=("--verbose")
fi
FINAL_ARGS+=("${TEST_ARGS[@]}")

# Run the test
if [[ "$USE_GDB" -eq 1 ]]; then
    echo "[INFO] Starting GDB TUI mode..."
    echo "[INFO] Suggested breakpoints:"
    echo "       break DrmFormatModTest::runAllTests"
    echo "       break vkCreateImage"
    echo "       break vkBindImageMemory"
    echo ""
    exec gdb -tui --args "$TEST_BIN" "${FINAL_ARGS[@]}"
else
    # Filter out noisy loader/init messages, keep test output
    "$TEST_BIN" "${FINAL_ARGS[@]}" 2>&1 | grep -v -E "(linux_read_sorted|Copying old device|Removing driver|Failed to find vkGetDeviceProcAddr|vkCreateDevice layer callstack|\s+\|\||\s+<Application>|\s+<Loader>|\s+<Device>|Type: Explicit|Enabled By:|Manifest:|Library:|Using \".*\" with driver|Added required|Enumerating instance|Looking for instance|###### List of|^\s+VK_KHR|^\s+VK_EXT|^\s+VK_NV|^\s+VK_LUNARG|^\s+VK_MESA|^\s+VK_LAYER|\[0\] llvmpipe|\[1\] NVIDIA|\[0\] NVIDIA|\[1\] llvmpipe|Original order:|Sorted order:|Inserted device layer|VK_LAYER_KHRONOS)"
fi
