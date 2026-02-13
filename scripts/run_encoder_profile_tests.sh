#!/bin/bash
#
# Vulkan Video Encoder JSON Profile Test Runner (shell wrapper)
# Runs all NVIDIA JSON profiles via run_encoder_profile_tests.py.
#
# Usage:
#   ./run_encoder_profile_tests.sh --video-dir <path> [OPTIONS]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY_SCRIPT="$SCRIPT_DIR/run_encoder_profile_tests.py"
PYTHON_BIN="${PYTHON_BIN:-python3}"

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    echo "Error: Python interpreter not found: $PYTHON_BIN" >&2
    echo "Set PYTHON_BIN or install python3." >&2
    exit 1
fi

if [ ! -f "$PY_SCRIPT" ]; then
    echo "Error: Missing script: $PY_SCRIPT" >&2
    exit 1
fi

if [ $# -eq 0 ]; then
    cat <<'EOF'
Usage: run_encoder_profile_tests.sh --video-dir <path> [OPTIONS]

Options (forwarded to run_encoder_profile_tests.py):
  --video-dir PATH      Directory containing YUV test videos (required)
  --profile-dir PATH    JSON profile directory
  --codec CODEC         Filter: h264 | h265 | av1
  --profile-filter STR  Run only profiles matching substring
  --max-frames N        Frames to encode per profile
  --validate, -v        Enable Vulkan validation layers
  --verbose             Verbose output
  --local               Run locally instead of SSH
  --remote HOST         Remote host/IP (default: 127.0.0.1)
  --remote-user USER    Remote username
  --build-dir PATH      Build directory
  --output-dir PATH     Output directory for encoded streams
  --input-file PATH     Custom YUV input file for all profiles
  --input-width N       Required with --input-file
  --input-height N      Required with --input-file
  --input-bpp N         Input bit depth (default: 8)
  --input-chroma STR    Input chroma subsampling (default: 420)
  --max-supported-quality-preset N
                        Max qualityPreset supported by current driver (default: 4)

Examples:
  ./run_encoder_profile_tests.sh --video-dir /data/misc/VideoClips --local
  ./run_encoder_profile_tests.sh --video-dir /data/misc/VideoClips --codec av1 --validate
EOF
    exit 1
fi

exec "$PYTHON_BIN" "$PY_SCRIPT" "$@"
