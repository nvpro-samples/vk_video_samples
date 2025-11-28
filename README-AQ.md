# Adaptive Quantization (AQ) Library Integration Guide

This document describes how to fetch, build, and use the Vulkan Video Samples with Adaptive Quantization (AQ) library support.

## Overview

The Adaptive Quantization (AQ) library provides spatial and temporal adaptive quantization capabilities for Vulkan Video encoding. The library improves video quality by adjusting quantization parameters based on spatial complexity and temporal motion characteristics.

**Key Features:**
- **Spatial AQ**: Adjusts quantization based on spatial complexity within each frame
- **Temporal AQ**: Adjusts quantization based on temporal motion between frames
- **Combined Mode**: Uses both spatial and temporal analysis with configurable mixing ratios

## Prerequisites

- NVIDIA GPU with Vulkan Video support
- NVIDIA Driver with Vulkan Video VK_KHR_video_encode_quantization_map extension support is required
- CMake 3.20 or later
- C++17 compatible compiler
- Vulkan SDK installed
- Git access to NVIDIA internal repositories

## CMake Variables Setup

Before building, you'll need to configure CMake with the following variables:

### Required CMake Variables

- `NV_AQ_GPU_LIB` - Path to the AQ library (should point to the `aq-vulkan` directory)
  - **Linux Example**: `/path/to/vulkan-video-verification/aq-vulkan`
  - **Windows Example**: `C:/path/to/vulkan-video-verification/aq-vulkan`

### Optional CMake Variables

- `CMAKE_BUILD_TYPE` - Build type: `Debug` or `Release` (default: `Debug`)
- `CMAKE_CUDA_ARCHITECTURES` - CUDA architectures to compile for (default: `"86;89;90"`)
- `CMAKE_INSTALL_PREFIX` - Installation prefix (default: platform-specific)

**Note:** All paths should use forward slashes (`/`) even on Windows when passed to CMake.

## Fetching the AQ Library

The AQ library is located at:
```
https://gitlab-master.nvidia.com/vulkan-video/vulkan-video-verification
```

**Note:** If you're using the automated build script (see [Building with AQ Support](#building-with-aq-support)), the script will handle fetching both repositories automatically. You can skip this section if using the script.

### Manual Fetching

#### Linux

```bash
git clone https://gitlab-master.nvidia.com/vulkan-video/vulkan-video-verification.git
cd vulkan-video-verification
# Note the absolute path to aq-vulkan directory for use in CMake configuration
```

#### Windows

```cmd
git clone https://gitlab-master.nvidia.com/vulkan-video/vulkan-video-verification.git
cd vulkan-video-verification
REM Note the absolute path to aq-vulkan directory for use in CMake configuration
```

The AQ library structure should include:
- `aq-vulkan/` - Vulkan-specific AQ implementation
- `aq_common/` - Common AQ interfaces and utilities

**Verify the AQ library path:**

**Linux:**
```bash
ls -la vulkan-video-verification/aq-vulkan/CMakeLists.txt
```

**Windows:**
```cmd
dir vulkan-video-verification\aq-vulkan\CMakeLists.txt
```

### Recommended Directory Structure

When using the automated build script, the recommended structure is:

```
target_dir/
    vulkan-video-samples/    (main project - nvpro sample/encoder)
        build/               (Debug build output)
        build-release/       (Release build output)
    algo/                    (AQ library repository)
        aq-vulkan/           (AQ Vulkan implementation)
        aq_common/           (Common AQ interfaces)
```

The build script will create this structure automatically when cloning repositories.

## Building with AQ Support

There are two ways to build the project with AQ support:

1. **Automated Build Script** (Recommended) - Uses a Python script to fetch repositories and build automatically
2. **Manual Build Steps** - Step-by-step manual commands for more control

### Option 1: Automated Build Script

A Python script is provided to automate fetching both repositories and building the project. This is the recommended approach for first-time setup.

#### Script Location

The script is located at:
```
scripts/build_aq_project.py
```

#### Required Parameters

- `--target-dir <path>`: Target directory where repositories will be cloned and builds will occur (required)
  - Structure created: `target_dir/vulkan-video-samples` and `target_dir/algo`
- `--build-type <type>`: Build type - `Debug` or `Release` (required)

#### Repository Options

You can either clone repositories automatically or use existing ones:

**To clone repositories automatically:**
- `--vulkan-samples-repo-url <url>`: Git URL for vulkan-video-samples repository
- `--aq-repo-url <url>`: Git URL for AQ library repository (algo repository)
- `--vulkan-samples-branch <branch>`: Branch for vulkan-video-samples (default: `vulkan-aq-lib-integration`)
- `--aq-branch <branch>`: Branch for AQ repository (default: `main`)

**To use existing repositories:**
- `--project-root <path>`: Path to existing vulkan-video-samples directory
- `--aq-lib-path <path>`: Path to existing aq-vulkan directory

#### Examples

**Linux - Clone both repositories and build Debug:**

```bash
python3 scripts/build_aq_project.py \
    --target-dir /path/to/target \
    --vulkan-samples-repo-url https://github.com/nvpro-samples/vk_video_samples \
    --aq-repo-url https://gitlab-master.nvidia.com/vulkan-video/vulkan-video-verification \
    --build-type Debug
```

**Linux - Clone both repositories and build Release:**

```bash
python3 scripts/build_aq_project.py \
    --target-dir /path/to/target \
    --vulkan-samples-repo-url https://github.com/nvpro-samples/vk_video_samples \
    --aq-repo-url https://gitlab-master.nvidia.com/vulkan-video/vulkan-video-verification \
    --build-type Release
```

**Linux - Use existing repositories in target_dir:**

```bash
python3 scripts/build_aq_project.py \
    --target-dir /path/to/target \
    --build-type Debug
```

**Windows - Clone both repositories and build Debug:**

```cmd
python scripts\build_aq_project.py ^
    --target-dir C:\path\to\target ^
    --vulkan-samples-repo-url https://github.com/nvpro-samples/vk_video_samples ^
    --aq-repo-url https://gitlab-master.nvidia.com/vulkan-video/vulkan-video-verification ^
    --build-type Debug
```

**Windows - Use existing repositories:**

```cmd
python scripts\build_aq_project.py ^
    --target-dir C:\path\to\target ^
    --build-type Release
```

#### Script Features

- **Automatic repository cloning**: Fetches both vulkan-video-samples and AQ library repositories
- **Repository updates**: Updates existing repositories if they're already cloned
- **Cross-platform**: Works on both Windows and Linux
- **Build verification**: Verifies that key files were built successfully
- **Dry-run mode**: Use `--dry-run` to see what would be executed without running it

#### Additional Options

- `--cuda-architectures <archs>`: CUDA architectures (default: `"86;89;90"`)
- `--num-jobs <count>`: Number of parallel build jobs (default: auto-detect)
- `--skip-install`: Skip installation step
- `--skip-verify`: Skip build verification
- `--verbose`: Print verbose output

#### Getting Help

To see all available options:

```bash
python3 scripts/build_aq_project.py --help
```

Or on Windows:

```cmd
python scripts\build_aq_project.py --help
```

### Option 2: Manual Build Steps

### Linux Build Instructions

#### Step 1: Navigate to the Project Directory

```bash
cd /path/to/vulkan-video-samples
```

#### Step 2: Create Build Directory

```bash
mkdir -p build
cd build
```

#### Step 3: Configure CMake (Debug Build)

Configure CMake with the `NV_AQ_GPU_LIB` variable pointing to the AQ library:

```bash
cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CUDA_ARCHITECTURES="86;89;90" \
    -DNV_AQ_GPU_LIB="/path/to/vulkan-video-verification/aq-vulkan"
```

#### Step 4: Build the Project (Debug)

```bash
cmake --build . --config Debug -j$(nproc)
```

#### Step 5: Install the Project (Debug)

```bash
cmake --install . --config Debug
```

#### Step 6: Configure CMake (Release Build)

For Release builds, create a separate build directory or reconfigure:

```bash
cd ..
mkdir -p build-release
cd build-release

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CUDA_ARCHITECTURES="86;89;90" \
    -DNV_AQ_GPU_LIB="/path/to/vulkan-video-verification/aq-vulkan"
```

#### Step 7: Build and Install (Release)

```bash
cmake --build . --config Release -j$(nproc)
cmake --install . --config Release
```

#### Step 8: Verify Build

After successful build, verify the AQ library was built:

```bash
# Debug build
ls -la build/lib/libnvenc_aq_vulkan.so*

# Release build
ls -la build-release/lib/libnvenc_aq_vulkan.so*
```

The encoder test executable will be located at:
```bash
# Debug build
build/vk_video_encoder/test/vulkan-video-enc-test

# Release build
build-release/vk_video_encoder/test/vulkan-video-enc-test
```

### Windows Build Instructions

#### Step 1: Navigate to the Project Directory

```cmd
cd C:\path\to\vulkan-video-samples
```

#### Step 2: Create Build Directory

```cmd
mkdir build
cd build
```

#### Step 3: Configure CMake (Debug Build)

Configure CMake with the `NV_AQ_GPU_LIB` variable pointing to the AQ library:

```cmd
cmake .. ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_CUDA_ARCHITECTURES="86;89;90" ^
    -DNV_AQ_GPU_LIB="C:/path/to/vulkan-video-verification/aq-vulkan"
```

**Note:** Use forward slashes (`/`) in paths even on Windows when passing to CMake.

#### Step 4: Build the Project (Debug)

```cmd
cmake --build . --config Debug -j %NUMBER_OF_PROCESSORS%
```

#### Step 5: Install the Project (Debug)

```cmd
cmake --install . --config Debug
```

#### Step 6: Configure CMake (Release Build)

For Release builds, create a separate build directory or reconfigure:

```cmd
cd ..
mkdir build-release
cd build-release

cmake .. ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_CUDA_ARCHITECTURES="86;89;90" ^
    -DNV_AQ_GPU_LIB="C:/path/to/vulkan-video-verification/aq-vulkan"
```

#### Step 7: Build and Install (Release)

```cmd
cmake --build . --config Release -j %NUMBER_OF_PROCESSORS%
cmake --install . --config Release
```

#### Step 8: Verify Build

After successful build, verify the AQ library was built:

```cmd
REM Debug build
dir build\lib\nvenc_aq_vulkan.dll

REM Release build
dir build-release\lib\nvenc_aq_vulkan.dll
```

The encoder test executable will be located at:
```cmd
REM Debug build
build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe

REM Release build
build-release\vk_video_encoder\test\Release\vulkan-video-enc-test.exe
```

**Note:** The `NV_AQ_GPU_LIB` path will automatically resolve the required include directories:
- `aq_common/interface` (for `EncodeAqAnalyzes.h`)
- `aq_common/inc` (for `AqProcessor.h` and other headers)
- `aq-vulkan/inc` (for `VulkanGpuTemporalAQ.h` and Vulkan-specific headers)

## Running the Encoder with AQ

### Setting Up Library Path

Before running the encoder, ensure the AQ library is in the library search path:

**Linux (Debug Build):**
```bash
export LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib:$LD_LIBRARY_PATH
```

**Linux (Release Build):**
```bash
export LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build-release/lib:$LD_LIBRARY_PATH
```

**Windows:**
The DLL search path should include the build directory. You can either:
1. Copy the DLLs to the executable directory, or
2. Add the build directory to your `PATH` environment variable:
```cmd
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
```

### AQ Command-Line Parameters

The encoder supports the following AQ-related parameters:

- `--spatialAQStrength <float>`: Spatial AQ strength in range [-1.0, 1.0]
  - `< -1.0` = disabled (default: -2.0)
  - `0.0` = default/neutral strength
  - `-1.0` = minimum strength, `1.0` = maximum strength
  - If `>= -1.0`, spatial AQ is enabled
  - In combined mode, ratio determines mix (larger value = more influence)

- `--temporalAQStrength <float>`: Temporal AQ strength in range [-1.0, 1.0]
  - `< -1.0` = disabled (default: -2.0)
  - `0.0` = default/neutral strength
  - `-1.0` = minimum strength, `1.0` = maximum strength
  - If `>= -1.0`, temporal AQ is enabled
  - In combined mode, ratio determines mix (larger value = more influence)

- `--aqDumpDir <string>`: Directory for AQ dump files (default: `./aqDump`)

**AQ Mode Behavior:**
- **Spatial Only**: Set `--spatialAQStrength` to a value in [-1.0, 1.0] and leave `--temporalAQStrength` as default (< -1.0)
- **Temporal Only**: Set `--temporalAQStrength` to a value in [-1.0, 1.0] and leave `--spatialAQStrength` as default (< -1.0)
- **Combined Mode**: Set both `--spatialAQStrength` and `--temporalAQStrength` to values in [-1.0, 1.0]

## Example Commands

> **Note on Bitrate Units**: The `--averageBitrate` parameter is in **bits per second (bps)**.
> For example, `5000000` = 5,000,000 bps = **5 Mbps**.
>
> Recommended bitrate values:
> | Resolution | Low Quality | Medium Quality | High Quality |
> |------------|-------------|----------------|--------------|
> | **1080p** | 2-3 Mbps | 5-8 Mbps | 10-15 Mbps |
> | **4K** | 8-12 Mbps | 15-25 Mbps | 30-50 Mbps |

### Quick Reference: --averageBitrate Values

| Resolution | Value | Actual Bitrate | Quality Level |
|------------|-------|----------------|---------------|
| 1080p | `2000000` | 2 Mbps | Low |
| 1080p | `5000000` | 5 Mbps | Medium |
| 1080p | `8000000` | 8 Mbps | High |
| 4K | `10000000` | 10 Mbps | Low |
| 4K | `15000000` | 15 Mbps | Medium |
| 4K | `25000000` | 25 Mbps | High |

### H.264 (AVC) Encoding Examples

#### Spatial AQ Only (H.264)

**Linux:**
```bash
rm -rf /tmp/vulkan_nvpro_test/* && \
LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib \
/path/to/vulkan-video-samples/build/vk_video_encoder/test/vulkan-video-enc-test \
    -i /path/to/test/video.yuv \
    -o /tmp/vulkan_nvpro_test/encoded_h264_spatial.264 \
    -c avc \
    --inputWidth 1920 \
    --inputHeight 1080 \
    --encodeWidth 1920 \
    --encodeHeight 1080 \
    --numFrames 32 \
    --startFrame 0 \
    --rateControlMode vbr \
    --averageBitrate 5000000 \
    --gopFrameCount 16 \
    --idrPeriod 4294967295 \
    --consecutiveBFrameCount 3 \
    --inputChromaSubsampling 420 \
    --spatialAQStrength 0.0 \
    --temporalAQStrength -2.0 \
    --qualityLevel 4 \
    --usageHints transcoding \
    --contentHints default \
    --tuningMode default \
    --aqDumpDir /tmp/vulkan_nvpro_test
```

**Windows:**
```cmd
if exist C:\tmp\vulkan_nvpro_test rmdir /s /q C:\tmp\vulkan_nvpro_test
mkdir C:\tmp\vulkan_nvpro_test
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
C:\path\to\vulkan-video-samples\build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe ^
    -i C:\path\to\test\video.yuv ^
    -o C:\tmp\vulkan_nvpro_test\encoded_h264_spatial.264 ^
    -c avc ^
    --inputWidth 1920 ^
    --inputHeight 1080 ^
    --encodeWidth 1920 ^
    --encodeHeight 1080 ^
    --numFrames 32 ^
    --startFrame 0 ^
    --rateControlMode vbr ^
    --averageBitrate 5000000 ^
    --gopFrameCount 16 ^
    --idrPeriod 4294967295 ^
    --consecutiveBFrameCount 3 ^
    --inputChromaSubsampling 420 ^
    --spatialAQStrength 0.0 ^
    --temporalAQStrength -2.0 ^
    --qualityLevel 4 ^
    --usageHints transcoding ^
    --contentHints default ^
    --tuningMode default ^
    --aqDumpDir C:\tmp\vulkan_nvpro_test
```

#### Temporal AQ Only (H.264)

**Linux:**
```bash
rm -rf /tmp/vulkan_nvpro_test/* && \
LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib \
/path/to/vulkan-video-samples/build/vk_video_encoder/test/vulkan-video-enc-test \
    -i /path/to/test/video.yuv \
    -o /tmp/vulkan_nvpro_test/encoded_h264_temporal.264 \
    -c avc \
    --inputWidth 1920 \
    --inputHeight 1080 \
    --encodeWidth 1920 \
    --encodeHeight 1080 \
    --numFrames 32 \
    --startFrame 0 \
    --rateControlMode vbr \
    --averageBitrate 5000000 \
    --gopFrameCount 16 \
    --idrPeriod 4294967295 \
    --consecutiveBFrameCount 3 \
    --inputChromaSubsampling 420 \
    --spatialAQStrength -2.0 \
    --temporalAQStrength 0.0 \
    --qualityLevel 4 \
    --usageHints transcoding \
    --contentHints default \
    --tuningMode default \
    --aqDumpDir /tmp/vulkan_nvpro_test
```

**Windows:**
```cmd
if exist C:\tmp\vulkan_nvpro_test rmdir /s /q C:\tmp\vulkan_nvpro_test
mkdir C:\tmp\vulkan_nvpro_test
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
C:\path\to\vulkan-video-samples\build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe ^
    -i C:\path\to\test\video.yuv ^
    -o C:\tmp\vulkan_nvpro_test\encoded_h264_spatial.264 ^
    -c avc ^
    --inputWidth 1920 ^
    --inputHeight 1080 ^
    --encodeWidth 1920 ^
    --encodeHeight 1080 ^
    --numFrames 32 ^
    --startFrame 0 ^
    --rateControlMode vbr ^
    --averageBitrate 5000000 ^
    --gopFrameCount 16 ^
    --idrPeriod 4294967295 ^
    --consecutiveBFrameCount 3 ^
    --inputChromaSubsampling 420 ^
    --spatialAQStrength 0.0 ^
    --temporalAQStrength -2.0 ^
    --qualityLevel 4 ^
    --usageHints transcoding ^
    --contentHints default ^
    --tuningMode default ^
    --aqDumpDir C:\tmp\vulkan_nvpro_test
```

#### Combined AQ (H.264)

**Linux:**
```bash
rm -rf /tmp/vulkan_nvpro_test/* && \
LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib \
/path/to/vulkan-video-samples/build/vk_video_encoder/test/vulkan-video-enc-test \
    -i /path/to/test/video.yuv \
    -o /tmp/vulkan_nvpro_test/encoded_h264_combined.264 \
    -c avc \
    --inputWidth 1920 \
    --inputHeight 1080 \
    --encodeWidth 1920 \
    --encodeHeight 1080 \
    --numFrames 32 \
    --startFrame 0 \
    --rateControlMode vbr \
    --averageBitrate 5000000 \
    --gopFrameCount 16 \
    --idrPeriod 4294967295 \
    --consecutiveBFrameCount 3 \
    --inputChromaSubsampling 420 \
    --spatialAQStrength 0.0 \
    --temporalAQStrength 0.0 \
    --qualityLevel 4 \
    --usageHints transcoding \
    --contentHints default \
    --tuningMode default \
    --aqDumpDir /tmp/vulkan_nvpro_test
```

**Windows:**
```cmd
if exist C:\tmp\vulkan_nvpro_test rmdir /s /q C:\tmp\vulkan_nvpro_test
mkdir C:\tmp\vulkan_nvpro_test
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
C:\path\to\vulkan-video-samples\build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe ^
    -i C:\path\to\test\video.yuv ^
    -o C:\tmp\vulkan_nvpro_test\encoded_h264_spatial.264 ^
    -c avc ^
    --inputWidth 1920 ^
    --inputHeight 1080 ^
    --encodeWidth 1920 ^
    --encodeHeight 1080 ^
    --numFrames 32 ^
    --startFrame 0 ^
    --rateControlMode vbr ^
    --averageBitrate 5000000 ^
    --gopFrameCount 16 ^
    --idrPeriod 4294967295 ^
    --consecutiveBFrameCount 3 ^
    --inputChromaSubsampling 420 ^
    --spatialAQStrength 0.0 ^
    --temporalAQStrength -2.0 ^
    --qualityLevel 4 ^
    --usageHints transcoding ^
    --contentHints default ^
    --tuningMode default ^
    --aqDumpDir C:\tmp\vulkan_nvpro_test
```

### HEVC (H.265) Encoding Examples

#### Spatial AQ Only (HEVC)

**Linux:**
```bash
rm -rf /tmp/vulkan_nvpro_test/* && \
LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib \
/path/to/vulkan-video-samples/build/vk_video_encoder/test/vulkan-video-enc-test \
    -i /path/to/test/video.yuv \
    -o /tmp/vulkan_nvpro_test/encoded_hevc_spatial.265 \
    -c hevc \
    --inputWidth 1920 \
    --inputHeight 1080 \
    --encodeWidth 1920 \
    --encodeHeight 1080 \
    --numFrames 32 \
    --startFrame 0 \
    --rateControlMode vbr \
    --averageBitrate 5000000 \
    --gopFrameCount 16 \
    --idrPeriod 4294967295 \
    --consecutiveBFrameCount 3 \
    --inputChromaSubsampling 420 \
    --spatialAQStrength 0.0 \
    --temporalAQStrength -2.0 \
    --qualityLevel 4 \
    --usageHints transcoding \
    --contentHints default \
    --tuningMode default \
    --aqDumpDir /tmp/vulkan_nvpro_test
```

**Windows:**
```cmd
if exist C:\tmp\vulkan_nvpro_test rmdir /s /q C:\tmp\vulkan_nvpro_test
mkdir C:\tmp\vulkan_nvpro_test
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
C:\path\to\vulkan-video-samples\build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe ^
    -i C:\path\to\test\video.yuv ^
    -o C:\tmp\vulkan_nvpro_test\encoded_h264_spatial.264 ^
    -c avc ^
    --inputWidth 1920 ^
    --inputHeight 1080 ^
    --encodeWidth 1920 ^
    --encodeHeight 1080 ^
    --numFrames 32 ^
    --startFrame 0 ^
    --rateControlMode vbr ^
    --averageBitrate 5000000 ^
    --gopFrameCount 16 ^
    --idrPeriod 4294967295 ^
    --consecutiveBFrameCount 3 ^
    --inputChromaSubsampling 420 ^
    --spatialAQStrength 0.0 ^
    --temporalAQStrength -2.0 ^
    --qualityLevel 4 ^
    --usageHints transcoding ^
    --contentHints default ^
    --tuningMode default ^
    --aqDumpDir C:\tmp\vulkan_nvpro_test
```

#### Temporal AQ Only (HEVC)

**Linux:**
```bash
rm -rf /tmp/vulkan_nvpro_test/* && \
LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib \
/path/to/vulkan-video-samples/build/vk_video_encoder/test/vulkan-video-enc-test \
    -i /path/to/test/video.yuv \
    -o /tmp/vulkan_nvpro_test/encoded_hevc_temporal.265 \
    -c hevc \
    --inputWidth 1920 \
    --inputHeight 1080 \
    --encodeWidth 1920 \
    --encodeHeight 1080 \
    --numFrames 32 \
    --startFrame 0 \
    --rateControlMode vbr \
    --averageBitrate 5000000 \
    --gopFrameCount 16 \
    --idrPeriod 4294967295 \
    --consecutiveBFrameCount 3 \
    --inputChromaSubsampling 420 \
    --spatialAQStrength -2.0 \
    --temporalAQStrength 0.0 \
    --qualityLevel 4 \
    --usageHints transcoding \
    --contentHints default \
    --tuningMode default \
    --aqDumpDir /tmp/vulkan_nvpro_test
```

**Windows:**
```cmd
if exist C:\tmp\vulkan_nvpro_test rmdir /s /q C:\tmp\vulkan_nvpro_test
mkdir C:\tmp\vulkan_nvpro_test
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
C:\path\to\vulkan-video-samples\build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe ^
    -i C:\path\to\test\video.yuv ^
    -o C:\tmp\vulkan_nvpro_test\encoded_h264_spatial.264 ^
    -c avc ^
    --inputWidth 1920 ^
    --inputHeight 1080 ^
    --encodeWidth 1920 ^
    --encodeHeight 1080 ^
    --numFrames 32 ^
    --startFrame 0 ^
    --rateControlMode vbr ^
    --averageBitrate 5000000 ^
    --gopFrameCount 16 ^
    --idrPeriod 4294967295 ^
    --consecutiveBFrameCount 3 ^
    --inputChromaSubsampling 420 ^
    --spatialAQStrength 0.0 ^
    --temporalAQStrength -2.0 ^
    --qualityLevel 4 ^
    --usageHints transcoding ^
    --contentHints default ^
    --tuningMode default ^
    --aqDumpDir C:\tmp\vulkan_nvpro_test
```

#### Combined AQ (HEVC)

**Linux:**
```bash
rm -rf /tmp/vulkan_nvpro_test/* && \
LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib \
/path/to/vulkan-video-samples/build/vk_video_encoder/test/vulkan-video-enc-test \
    -i /path/to/test/video.yuv \
    -o /tmp/vulkan_nvpro_test/encoded_hevc_combined.265 \
    -c hevc \
    --inputWidth 1920 \
    --inputHeight 1080 \
    --encodeWidth 1920 \
    --encodeHeight 1080 \
    --numFrames 32 \
    --startFrame 0 \
    --rateControlMode vbr \
    --averageBitrate 5000000 \
    --gopFrameCount 16 \
    --idrPeriod 4294967295 \
    --consecutiveBFrameCount 3 \
    --inputChromaSubsampling 420 \
    --spatialAQStrength 0.0 \
    --temporalAQStrength 0.0 \
    --qualityLevel 4 \
    --usageHints transcoding \
    --contentHints default \
    --tuningMode default \
    --aqDumpDir /tmp/vulkan_nvpro_test
```

**Windows:**
```cmd
if exist C:\tmp\vulkan_nvpro_test rmdir /s /q C:\tmp\vulkan_nvpro_test
mkdir C:\tmp\vulkan_nvpro_test
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
C:\path\to\vulkan-video-samples\build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe ^
    -i C:\path\to\test\video.yuv ^
    -o C:\tmp\vulkan_nvpro_test\encoded_h264_spatial.264 ^
    -c avc ^
    --inputWidth 1920 ^
    --inputHeight 1080 ^
    --encodeWidth 1920 ^
    --encodeHeight 1080 ^
    --numFrames 32 ^
    --startFrame 0 ^
    --rateControlMode vbr ^
    --averageBitrate 5000000 ^
    --gopFrameCount 16 ^
    --idrPeriod 4294967295 ^
    --consecutiveBFrameCount 3 ^
    --inputChromaSubsampling 420 ^
    --spatialAQStrength 0.0 ^
    --temporalAQStrength -2.0 ^
    --qualityLevel 4 ^
    --usageHints transcoding ^
    --contentHints default ^
    --tuningMode default ^
    --aqDumpDir C:\tmp\vulkan_nvpro_test
```

### AV1 Encoding Examples

#### Spatial AQ Only (AV1)

**Linux:**
```bash
rm -rf /tmp/vulkan_nvpro_test/* && \
LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib \
/path/to/vulkan-video-samples/build/vk_video_encoder/test/vulkan-video-enc-test \
    -i /path/to/test/video.yuv \
    -o /tmp/vulkan_nvpro_test/encoded_av1_spatial.ivf \
    -c av1 \
    --inputWidth 1920 \
    --inputHeight 1080 \
    --encodeWidth 1920 \
    --encodeHeight 1080 \
    --numFrames 32 \
    --startFrame 0 \
    --rateControlMode vbr \
    --averageBitrate 5000000 \
    --gopFrameCount 16 \
    --idrPeriod 4294967295 \
    --consecutiveBFrameCount 3 \
    --inputChromaSubsampling 420 \
    --spatialAQStrength 0.0 \
    --temporalAQStrength -2.0 \
    --qualityLevel 4 \
    --usageHints transcoding \
    --contentHints default \
    --tuningMode default \
    --aqDumpDir /tmp/vulkan_nvpro_test
```

**Windows:**
```cmd
if exist C:\tmp\vulkan_nvpro_test rmdir /s /q C:\tmp\vulkan_nvpro_test
mkdir C:\tmp\vulkan_nvpro_test
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
C:\path\to\vulkan-video-samples\build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe ^
    -i C:\path\to\test\video.yuv ^
    -o C:\tmp\vulkan_nvpro_test\encoded_h264_spatial.264 ^
    -c avc ^
    --inputWidth 1920 ^
    --inputHeight 1080 ^
    --encodeWidth 1920 ^
    --encodeHeight 1080 ^
    --numFrames 32 ^
    --startFrame 0 ^
    --rateControlMode vbr ^
    --averageBitrate 5000000 ^
    --gopFrameCount 16 ^
    --idrPeriod 4294967295 ^
    --consecutiveBFrameCount 3 ^
    --inputChromaSubsampling 420 ^
    --spatialAQStrength 0.0 ^
    --temporalAQStrength -2.0 ^
    --qualityLevel 4 ^
    --usageHints transcoding ^
    --contentHints default ^
    --tuningMode default ^
    --aqDumpDir C:\tmp\vulkan_nvpro_test
```

#### Temporal AQ Only (AV1)

**Linux:**
```bash
rm -rf /tmp/vulkan_nvpro_test/* && \
LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib \
/path/to/vulkan-video-samples/build/vk_video_encoder/test/vulkan-video-enc-test \
    -i /path/to/test/video.yuv \
    -o /tmp/vulkan_nvpro_test/encoded_av1_temporal.ivf \
    -c av1 \
    --inputWidth 1920 \
    --inputHeight 1080 \
    --encodeWidth 1920 \
    --encodeHeight 1080 \
    --numFrames 32 \
    --startFrame 0 \
    --rateControlMode vbr \
    --averageBitrate 5000000 \
    --gopFrameCount 16 \
    --idrPeriod 4294967295 \
    --consecutiveBFrameCount 3 \
    --inputChromaSubsampling 420 \
    --spatialAQStrength -2.0 \
    --temporalAQStrength 0.0 \
    --qualityLevel 4 \
    --usageHints transcoding \
    --contentHints default \
    --tuningMode default \
    --aqDumpDir /tmp/vulkan_nvpro_test
```

**Windows:**
```cmd
if exist C:\tmp\vulkan_nvpro_test rmdir /s /q C:\tmp\vulkan_nvpro_test
mkdir C:\tmp\vulkan_nvpro_test
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
C:\path\to\vulkan-video-samples\build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe ^
    -i C:\path\to\test\video.yuv ^
    -o C:\tmp\vulkan_nvpro_test\encoded_h264_spatial.264 ^
    -c avc ^
    --inputWidth 1920 ^
    --inputHeight 1080 ^
    --encodeWidth 1920 ^
    --encodeHeight 1080 ^
    --numFrames 32 ^
    --startFrame 0 ^
    --rateControlMode vbr ^
    --averageBitrate 5000000 ^
    --gopFrameCount 16 ^
    --idrPeriod 4294967295 ^
    --consecutiveBFrameCount 3 ^
    --inputChromaSubsampling 420 ^
    --spatialAQStrength 0.0 ^
    --temporalAQStrength -2.0 ^
    --qualityLevel 4 ^
    --usageHints transcoding ^
    --contentHints default ^
    --tuningMode default ^
    --aqDumpDir C:\tmp\vulkan_nvpro_test
```

#### Combined AQ (AV1)

**Linux:**
```bash
rm -rf /tmp/vulkan_nvpro_test/* && \
LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib \
/path/to/vulkan-video-samples/build/vk_video_encoder/test/vulkan-video-enc-test \
    -i /path/to/test/video.yuv \
    -o /tmp/vulkan_nvpro_test/encoded_av1_combined.ivf \
    -c av1 \
    --inputWidth 1920 \
    --inputHeight 1080 \
    --encodeWidth 1920 \
    --encodeHeight 1080 \
    --numFrames 32 \
    --startFrame 0 \
    --rateControlMode vbr \
    --averageBitrate 5000000 \
    --gopFrameCount 16 \
    --idrPeriod 4294967295 \
    --consecutiveBFrameCount 3 \
    --inputChromaSubsampling 420 \
    --spatialAQStrength 0.0 \
    --temporalAQStrength 0.0 \
    --qualityLevel 4 \
    --usageHints transcoding \
    --contentHints default \
    --tuningMode default \
    --aqDumpDir /tmp/vulkan_nvpro_test
```

**Windows:**
```cmd
if exist C:\tmp\vulkan_nvpro_test rmdir /s /q C:\tmp\vulkan_nvpro_test
mkdir C:\tmp\vulkan_nvpro_test
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
C:\path\to\vulkan-video-samples\build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe ^
    -i C:\path\to\test\video.yuv ^
    -o C:\tmp\vulkan_nvpro_test\encoded_h264_spatial.264 ^
    -c avc ^
    --inputWidth 1920 ^
    --inputHeight 1080 ^
    --encodeWidth 1920 ^
    --encodeHeight 1080 ^
    --numFrames 32 ^
    --startFrame 0 ^
    --rateControlMode vbr ^
    --averageBitrate 5000000 ^
    --gopFrameCount 16 ^
    --idrPeriod 4294967295 ^
    --consecutiveBFrameCount 3 ^
    --inputChromaSubsampling 420 ^
    --spatialAQStrength 0.0 ^
    --temporalAQStrength -2.0 ^
    --qualityLevel 4 ^
    --usageHints transcoding ^
    --contentHints default ^
    --tuningMode default ^
    --aqDumpDir C:\tmp\vulkan_nvpro_test
```

## Sanity Test Command

The following command can be used as a sanity test for AV1 encoding with combined AQ:

**Linux:**
```bash
rm -rf /tmp/vulkan_nvpro_test/* && \
LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib \
/path/to/vulkan-video-samples/build/vk_video_encoder/test/vulkan-video-enc-test \
    -i /path/to/test/video.yuv \
    -o /tmp/vulkan_nvpro_test/encoded.ivf \
    -c av1 \
    --inputWidth 1920 \
    --inputHeight 1080 \
    --encodeWidth 1920 \
    --encodeHeight 1080 \
    --numFrames 32 \
    --startFrame 0 \
    --rateControlMode vbr \
    --averageBitrate 5000000 \
    --gopFrameCount 16 \
    --idrPeriod 4294967295 \
    --consecutiveBFrameCount 3 \
    --inputChromaSubsampling 420 \
    --spatialAQStrength 0.0 \
    --temporalAQStrength 0.0 \
    --qualityLevel 4 \
    --usageHints transcoding \
    --contentHints default \
    --tuningMode default \
    --aqDumpDir /tmp/vulkan_nvpro_test
```

**Windows:**
```cmd
if exist C:\tmp\vulkan_nvpro_test rmdir /s /q C:\tmp\vulkan_nvpro_test
mkdir C:\tmp\vulkan_nvpro_test
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
C:\path\to\vulkan-video-samples\build\vk_video_encoder\test\Debug\vulkan-video-enc-test.exe ^
    -i C:\path\to\test\video.yuv ^
    -o C:\tmp\vulkan_nvpro_test\encoded.ivf ^
    -c av1 ^
    --inputWidth 1920 ^
    --inputHeight 1080 ^
    --encodeWidth 1920 ^
    --encodeHeight 1080 ^
    --numFrames 32 ^
    --startFrame 0 ^
    --rateControlMode vbr ^
    --averageBitrate 5000000 ^
    --gopFrameCount 16 ^
    --idrPeriod 4294967295 ^
    --consecutiveBFrameCount 3 ^
    --inputChromaSubsampling 420 ^
    --spatialAQStrength 0.0 ^
    --temporalAQStrength 0.0 ^
    --qualityLevel 4 ^
    --usageHints transcoding ^
    --contentHints default ^
    --tuningMode default ^
    --aqDumpDir C:\tmp\vulkan_nvpro_test
```

## AQ Dump Files

When AQ is enabled, analysis data is written to the directory specified by `--aqDumpDir`. These files can be used for debugging and analysis of the AQ algorithm behavior.

## Troubleshooting

### Library Not Found

**Linux:**
If you encounter library loading errors, ensure `LD_LIBRARY_PATH` includes the build directory:

```bash
export LD_LIBRARY_PATH=/path/to/vulkan-video-samples/build/lib:$LD_LIBRARY_PATH
```

**Windows:**
If you encounter DLL loading errors, ensure the build directory is in your `PATH`:

```cmd
set PATH=C:\path\to\vulkan-video-samples\build\lib;%PATH%
```

Alternatively, copy the required DLLs to the executable directory.

### AQ Not Enabled

Verify that:
1. The AQ library was built successfully:
   - **Linux**: `libnvenc_aq_vulkan.so` exists in `build/lib/`
   - **Windows**: `nvenc_aq_vulkan.dll` exists in `build\lib\`
2. At least one of `--spatialAQStrength` or `--temporalAQStrength` is set to a value in [-1.0, 1.0] (values < -1.0 disable AQ)
3. The `NV_AQ_GPU_LIB` CMake variable was set correctly during configuration

### Build Errors

If CMake configuration fails:

**Linux:**
```bash
# Verify the AQ library path exists
ls -la /path/to/vulkan-video-verification/aq-vulkan/CMakeLists.txt

# Check CMake version
cmake --version

# Verify CMake variable was set correctly
# Check CMakeCache.txt in build directory
grep NV_AQ_GPU_LIB build/CMakeCache.txt
```

**Windows:**
```cmd
REM Verify the AQ library path exists
dir C:\path\to\vulkan-video-verification\aq-vulkan\CMakeLists.txt

REM Check CMake version
cmake --version

REM Verify CMake variable was set correctly
REM Check CMakeCache.txt in build directory
findstr NV_AQ_GPU_LIB build\CMakeCache.txt
```

**Common Issues:**
1. Check that all required dependencies are installed (Vulkan SDK, shaderc, etc.)
2. Ensure CMake version is 3.20 or later
3. Verify the `NV_AQ_GPU_LIB` path uses forward slashes (`/`) even on Windows
4. Ensure the path to `aq-vulkan` is absolute, not relative

## Additional Resources

- Vulkan Video Samples Repository: https://github.com/nvpro-samples/vk_video_samples/tree/vulkan-aq-lib-integration
- AQ Library Repository: https://gitlab-master.nvidia.com/vulkan-video/vulkan-video-verification
- Vulkan Video Specification: Refer to Vulkan Spec with Video Extensions

## Notes

- The AQ strength values use range [-1.0, 1.0] where:
  - `< -1.0` = disabled (e.g., -2.0)
  - `0.0` = default/neutral strength
  - `-1.0` = minimum adjustment, `1.0` = maximum adjustment
- In combined mode, the ratio between spatial and temporal strengths determines the mixing balance
- Higher strength values (toward 1.0) result in more aggressive quantization adjustments
- Lower strength values (toward -1.0) result in more subtle adjustments
- AQ dump files are useful for analyzing algorithm performance and debugging

## Python Script for AQ Encoding

A cross-platform Python script is provided to simplify running the encoder with AQ parameters on both Windows and Linux. The script automatically detects the platform and constructs the appropriate command with correct library paths.

### Script Location

The script is located at:
```
scripts/run_aq_encoder.py
```

### Usage

**Linux:**
```bash
python3 scripts/run_aq_encoder.py [OPTIONS]
```

**Windows:**
```cmd
python scripts\run_aq_encoder.py [OPTIONS]
```

### Required Parameters

- `--codec <codec>`: Codec type (`avc`, `h264`, `hevc`, `h265`, or `av1`)
- `--input <path>`: Input YUV file path
- `--width <pixels>`: Input video width in pixels
- `--height <pixels>`: Input video height in pixels
- `--output <path>`: Output encoded file path

### AQ Parameters

- `--spatial-aq <float>`: Spatial AQ strength in range [-1.0, 1.0] (default: -2.0 = disabled)
  - `0.0` = default/neutral, `-1.0` = minimum, `1.0` = maximum
- `--temporal-aq <float>`: Temporal AQ strength in range [-1.0, 1.0] (default: -2.0 = disabled)
  - `0.0` = default/neutral, `-1.0` = minimum, `1.0` = maximum
- `--aq-dump-dir <path>`: Directory for AQ dump files (optional)

### Frame Parameters

- `--num-frames <count>`: Number of frames to encode (optional)
- `--start-frame <number>`: Start frame number (default: 0)
- `--encode-width <pixels>`: Encoded width (default: same as input width)
- `--encode-height <pixels>`: Encoded height (default: same as input height)

### Rate Control Parameters

- `--rate-control-mode <mode>`: Rate control mode (`default`, `disabled`, `cbr`, or `vbr`) (default: `vbr`)
- `--average-bitrate <bps>`: Average bitrate in bits/sec, e.g., 5000000 = 5 Mbps (optional)

### GOP Structure Parameters

- `--gop-frame-count <count>`: Number of frames in GOP (default: 16)
- `--idr-period <period>`: IDR period (default: 4294967295)
- `--consecutive-b-frame-count <count>`: Number of consecutive B frames (default: 3)

### Video Quality Parameters

- `--chroma-subsampling <format>`: Chroma subsampling (`400`, `420`, `422`, or `444`) (default: `420`)
- `--quality-level <level>`: Quality level (default: 4)
- `--usage-hints <hint>`: Usage hints (`default`, `transcoding`, `streaming`, `recording`, or `conferencing`) (default: `transcoding`)
- `--content-hints <hint>`: Content hints (`default`, `camera`, `desktop`, or `rendered`) (default: `default`)
- `--tuning-mode <mode>`: Tuning mode (`default`, `highquality`, `lowlatency`, `ultralowlatency`, or `lossless`) (default: `default`)

### Build Configuration Parameters

- `--vulkan-samples-root <path>`: Root directory of vulkan-video-samples (default: current directory)
- `--build-type <type>`: Build type (`Debug` or `Release`) (default: `Debug`)

### Execution Options

- `--dry-run`: Print the command without executing it
- `--verbose`: Print verbose output

### Examples

#### AV1 Encoding with Combined AQ

**Linux:**
```bash
python3 scripts/run_aq_encoder.py \
    --codec av1 \
    --input /path/to/video.yuv \
    --width 1920 \
    --height 1080 \
    --num-frames 32 \
    --spatial-aq 0.0 \
    --temporal-aq -2.0 \
    --output /tmp/encoded.ivf \
    --aq-dump-dir /tmp/aq_dump \
    --vulkan-samples-root /path/to/vulkan-video-samples
```

**Windows:**
```cmd
python scripts\run_aq_encoder.py ^
    --codec av1 ^
    --input C:\path\to\video.yuv ^
    --width 1920 ^
    --height 1080 ^
    --num-frames 32 ^
    --spatial-aq 0.0 ^
    --temporal-aq -2.0 ^
    --output C:\tmp\encoded.ivf ^
    --aq-dump-dir C:\tmp\aq_dump ^
    --vulkan-samples-root C:\path\to\vulkan-video-samples
```

#### H.264 Encoding with Spatial AQ Only

**Linux:**
```bash
python3 scripts/run_aq_encoder.py \
    --codec h264 \
    --input /path/to/video.yuv \
    --width 1920 \
    --height 1080 \
    --num-frames 32 \
    --spatial-aq 0.0 \
    --temporal-aq -2.0 \
    --output /tmp/encoded.264 \
    --rate-control-mode vbr \
    --average-bitrate 5000000
```

#### HEVC Encoding with Temporal AQ Only

**Linux:**
```bash
python3 scripts/run_aq_encoder.py \
    --codec hevc \
    --input /path/to/video.yuv \
    --width 1920 \
    --height 1080 \
    --num-frames 32 \
    --spatial-aq 0.0 \
    --temporal-aq -2.0 \
    --output /tmp/encoded.265 \
    --rate-control-mode vbr \
    --average-bitrate 5000000
```

### Script Features

- **Cross-platform**: Automatically detects Windows or Linux and adjusts paths and commands accordingly
- **Library path management**: Automatically sets `LD_LIBRARY_PATH` (Linux) or `PATH` (Windows) for library loading
- **Path validation**: Checks that input files exist and creates output directories if needed
- **Parameter validation**: Validates AQ strength ranges and other parameters
- **Error handling**: Provides helpful error messages if the encoder executable is not found
- **Dry-run mode**: Use `--dry-run` to see the command that would be executed without running it

### Getting Help

To see all available options and their descriptions:

```bash
python3 scripts/run_aq_encoder.py --help
```

Or on Windows:

```cmd
python scripts\run_aq_encoder.py --help
```

---

## AQ Quality Benchmark Script

A comprehensive benchmarking script that automatically runs the encoder with different AQ configurations and generates quality comparison reports with file sizes, PSNR, and VMAF metrics.

### Script Location

```
scripts/aq_quality_benchmark.py
```

### Overview

The benchmark script tests **4 AQ configurations** in a single run:

| Configuration | Spatial AQ | Temporal AQ | Description |
|---------------|------------|-------------|-------------|
| **No AQ** | -2.0 (disabled) | -2.0 (disabled) | Baseline without any AQ |
| **Spatial Only** | 0.0 (enabled) | -2.0 (disabled) | Intra-frame complexity analysis |
| **Temporal Only** | -2.0 (disabled) | 0.0 (enabled) | Inter-frame motion analysis |
| **Combined** | 0.0 (enabled) | 0.0 (enabled) | Both spatial and temporal AQ |

### How It Works

1. **Reference Preparation**: Extracts the specified number of frames from the source YUV file as the quality reference.

2. **Encoding Phase**: Runs the Vulkan Video Encoder 4 times with different AQ settings, producing 4 encoded bitstreams.

3. **Quality Analysis**: For each encoded bitstream:
   - Decodes to raw YUV using FFmpeg
   - Calculates PSNR (Y, U, V, average, min, max) using FFmpeg
   - Calculates VMAF score using FFmpeg with libvmaf

4. **Report Generation**: Produces both human-readable (TXT) and machine-readable (JSON) reports with comparative analysis.

### Prerequisites

- **FFmpeg** with libvmaf support (for quality metrics)
- **Vulkan Video Encoder** built with AQ library

Verify FFmpeg has VMAF support:
```bash
ffmpeg -filters | grep vmaf
```

### Basic Usage

```bash
python3 scripts/aq_quality_benchmark.py \
    --input /path/to/video.yuv \
    --width 1920 \
    --height 1080 \
    --codec h264 \
    --num-frames 64 \
    --output-dir /tmp/aq_benchmark \
    --vulkan-samples-root /path/to/vulkan-video-samples \
    --build-type Release
```

### Command-Line Parameters

#### Required Parameters

| Parameter | Description |
|-----------|-------------|
| `--input <path>` | Path to input YUV file |
| `--width <pixels>` | Input video width in pixels |
| `--height <pixels>` | Input video height in pixels |
| `--codec <codec>` | Codec type: `avc`, `h264`, `hevc`, `h265`, or `av1` |
| `--output-dir <path>` | Output directory for encoded files and reports |

#### Video Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--num-frames <count>` | all | Number of frames to encode |
| `--start-frame <number>` | 0 | Starting frame number |
| `--encode-width <pixels>` | input width | Encoded video width |
| `--encode-height <pixels>` | input height | Encoded video height |
| `--chroma-subsampling <format>` | 420 | Chroma format: `400`, `420`, `422`, `444` |
| `--bit-depth <bits>` | 8 | Bit depth: `8` or `10` |

#### Rate Control Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--rate-control-mode <mode>` | vbr | Rate control: `default`, `disabled`, `cbr`, `vbr` |
| `--average-bitrate <bps>` | encoder default | Target bitrate in bits/sec (e.g., 5000000 = 5 Mbps) |

#### GOP Structure Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--gop-frame-count <count>` | 16 | Number of frames in GOP |
| `--idr-period <period>` | 4294967295 | IDR period (frames between IDR frames) |
| `--consecutive-b-frame-count <count>` | 3 | Number of consecutive B-frames |

#### Quality Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--quality-level <level>` | 4 | Encoder quality level |
| `--usage-hints <hint>` | transcoding | Usage: `default`, `transcoding`, `streaming`, `recording` |
| `--content-hints <hint>` | default | Content: `default`, `camera`, `desktop`, `rendered` |
| `--tuning-mode <mode>` | default | Tuning: `default`, `highquality`, `lowlatency`, `lossless` |

#### Build Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--vulkan-samples-root <path>` | current directory | Path to vulkan-video-samples root |
| `--build-type <type>` | Release | Build type: `Debug` or `Release` |

#### Benchmark Options

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--spatial-aq-strength <float>` | 0.0 | Spatial AQ strength for spatial/combined modes |
| `--temporal-aq-strength <float>` | 0.0 | Temporal AQ strength for temporal/combined modes |
| `--aq-dump-dir <path>` | `<output-dir>` | Base directory for AQ library dumps |
| `--skip-vmaf` | false | Skip VMAF calculation (faster benchmarks) |
| `--skip-psnr` | false | Skip PSNR calculation |
| `--verbose` | false | Enable verbose output |

### Output Files

The benchmark generates the following files in the output directory:

| File/Directory | Description |
|----------------|-------------|
| `encoded_no_aq.<ext>` | Baseline encoded bitstream (no AQ) |
| `encoded_spatial_only.<ext>` | Encoded with spatial AQ only |
| `encoded_temporal_only.<ext>` | Encoded with temporal AQ only |
| `encoded_combined.<ext>` | Encoded with combined AQ |
| `benchmark_report.txt` | Human-readable summary report with all command lines |
| `benchmark_results.json` | Machine-readable JSON results with command lines |
| `commands_no_aq.txt` | Command lines for baseline configuration |
| `commands_spatial_only.txt` | Command lines for spatial-only configuration |
| `commands_temporal_only.txt` | Command lines for temporal-only configuration |
| `commands_combined.txt` | Command lines for combined configuration |
| `aq_dump_no_aq/` | AQ library dumps for baseline (minimal output) |
| `aq_dump_spatial_only/` | AQ library dumps for spatial-only configuration |
| `aq_dump_temporal_only/` | AQ library dumps for temporal-only configuration |
| `aq_dump_combined/` | AQ library dumps for combined configuration |
| `work/` | Temporary working directory (cleaned up after benchmark) |

File extension `<ext>` is `.264` for H.264, `.265` for HEVC, `.ivf` for AV1.

### Command Line Files

Each `commands_<config>.txt` file contains the exact command lines used:

```
# Command lines for Spatial AQ Only
# Generated: 2026-01-12 10:30:00
#==============================================================================

## ENCODE COMMAND
# Encodes input to Spatial AQ Only configuration
/path/to/vulkan-video-enc-test -i video.yuv -o encoded_spatial_only.264 ...

## DECODE COMMAND
# Decodes encoded bitstream to raw YUV for quality analysis
ffmpeg -y -i encoded_spatial_only.264 -f rawvideo -pix_fmt yuv420p decoded.yuv

## PSNR CALCULATION COMMAND
# Calculates PSNR between decoded and reference YUV
ffmpeg -s 1920x1080 -pix_fmt yuv420p -i decoded.yuv -s 1920x1080 -pix_fmt yuv420p -i reference.yuv -lavfi [0:v][1:v]psnr -f null -

## VMAF CALCULATION COMMAND
# Calculates VMAF score between decoded and reference YUV
ffmpeg -s 1920x1080 -pix_fmt yuv420p -i decoded.yuv -s 1920x1080 -pix_fmt yuv420p -i reference.yuv -lavfi [0:v][1:v]libvmaf -f null -

## AQ DUMP DIRECTORY
# AQ library output location
/tmp/benchmark/aq_dump_spatial_only
```

### AQ Dump Directory Control

By default, AQ dumps go to `<output-dir>/aq_dump_<config>/`. Use `--aq-dump-dir` to specify a different base:

```bash
# Default: dumps go to /tmp/benchmark/aq_dump_*/
python3 scripts/aq_quality_benchmark.py \
    --output-dir /tmp/benchmark \
    ...

# Custom: dumps go to /data/aq_analysis/aq_dump_*/
python3 scripts/aq_quality_benchmark.py \
    --output-dir /tmp/benchmark \
    --aq-dump-dir /data/aq_analysis \
    ...
```

### AQ Dump Directory Contents

Each `aq_dump_<config>/` directory contains the AQ library's debug output:

| File Pattern | Description |
|--------------|-------------|
| `hevc_spatial_qp_delta_*.bin` | Spatial QP delta maps per frame |
| `hevc_temporal_qp_delta_*.bin` | Temporal QP delta maps per frame |
| `aq_statistics.csv` | Per-frame AQ statistics (if enabled) |

These files are useful for debugging and verifying that AQ is producing expected QP adjustments.

### Example Report Output

```
================================================================================
           AQ QUALITY BENCHMARK REPORT
================================================================================

Generated: 2026-01-09 13:29:10

## Test Configuration

  Input File:     /path/to/video.yuv
  Resolution:     1920x1080
  Codec:          H264
  Frames:         64
  Rate Control:   vbr
  Bitrate:        2000 kbps
  GOP Size:       16
  B-Frames:       3

## Results Summary

+----------------------+------------+----------+----------+----------+----------+
| Configuration        | Size (KB)  | vs Base  | PSNR     | VMAF     | vs Base  |
+----------------------+------------+----------+----------+----------+----------+
| No AQ (Baseline)     |       82.5 |    +0.0% |    22.83 |    19.68 |    +0.00 |
| Spatial AQ Only      |       48.6 |   -41.1% |    29.56 |    21.68 |    +1.99 |
| Temporal AQ Only     |       47.9 |   -41.9% |    29.57 |    21.63 |    +1.95 |
| Combined             |       48.6 |   -41.1% |    29.56 |    21.68 |    +1.99 |
+----------------------+------------+----------+----------+----------+----------+

## Analysis

  Best VMAF:      Spatial AQ Only (21.68)
  Best PSNR:      Temporal AQ Only (29.57 dB)
  Smallest Size:  Temporal AQ Only (47.9 KB)

  AQ Improvements vs Baseline:
    Spatial AQ Only: Size -41.1%, PSNR +6.74 dB, VMAF +1.99
    Temporal AQ Only: Size -41.9%, PSNR +6.75 dB, VMAF +1.95
    Combined: Size -41.1%, PSNR +6.74 dB, VMAF +1.99
```

### JSON Output Format

The `benchmark_results.json` file contains structured data for programmatic analysis:

```json
{
  "timestamp": "2026-01-09T13:29:10.523189",
  "configuration": {
    "input": "/path/to/video.yuv",
    "width": 1920,
    "height": 1080,
    "codec": "h264",
    "num_frames": 64,
    "bitrate": 2000,
    "gop_size": 16,
    "b_frames": 3
  },
  "results": [
    {
      "config_name": "no_aq",
      "description": "No AQ (Baseline)",
      "spatial_aq": -2.0,
      "temporal_aq": -2.0,
      "success": true,
      "file_size": 84443,
      "encode_time": 0.47,
      "psnr": {
        "y": 21.82,
        "u": 25.07,
        "v": 27.19,
        "average": 22.83,
        "min": 11.99,
        "max": 30.13
      },
      "vmaf": 19.68,
      "output_file": "/tmp/aq_benchmark/encoded_no_aq.264"
    },
    // ... additional configurations
  ]
}
```

### Advanced Examples

#### HEVC Benchmark at Multiple Bitrates

Run benchmarks at different bitrates to analyze AQ effectiveness:

```bash
for bitrate in 1000000 2000000 5000000 10000000; do
    python3 scripts/aq_quality_benchmark.py \
        --input video.yuv --width 1920 --height 1080 \
        --codec hevc --num-frames 100 \
        --average-bitrate $bitrate \
        --output-dir /tmp/benchmark_${bitrate}bps \
        --vulkan-samples-root /path/to/vulkan-video-samples
done
```

#### 4K AV1 Benchmark with Custom AQ Strength

```bash
python3 scripts/aq_quality_benchmark.py \
    --input video.yuv --width 3840 --height 2160 \
    --codec av1 --num-frames 60 \
    --average-bitrate 15000000 \
    --spatial-aq-strength 0.5 \
    --temporal-aq-strength 0.3 \
    --output-dir /tmp/av1_4k_benchmark \
    --vulkan-samples-root /path/to/vulkan-video-samples
```

> **4K Bitrate Note**: For 4K (3840x2160), use 10000000-20000000 bps (10-20 Mbps) for good quality.

#### Fast Benchmark (Skip VMAF)

VMAF calculation is slow. For quick comparisons, skip it:

```bash
python3 scripts/aq_quality_benchmark.py \
    --input video.yuv --width 1920 --height 1080 \
    --codec h264 --num-frames 64 \
    --skip-vmaf \
    --output-dir /tmp/quick_benchmark \
    --vulkan-samples-root /path/to/vulkan-video-samples
```

### Interpreting Results

#### Quality Metrics

| Metric | Excellent | Good | Fair | Poor |
|--------|-----------|------|------|------|
| **PSNR** | > 40 dB | 35-40 dB | 30-35 dB | < 30 dB |
| **VMAF** | > 90 | 80-90 | 70-80 | < 70 |

#### Expected AQ Impact

- **Size Reduction**: 20-40% at constrained bitrates
- **PSNR Improvement**: +2-7 dB depending on content
- **VMAF Improvement**: +1-5 points depending on content

#### Content Sensitivity

- **High motion content**: Temporal AQ provides larger benefits
- **Complex textures**: Spatial AQ provides larger benefits
- **Mixed content**: Combined mode usually performs best
- **High bitrates (>5 Mbps)**: AQ effects are less pronounced

### Troubleshooting

#### VMAF Not Available

If you see "VMAF calculation failed", use the provided installation script:

```bash
# Automatic installation (tries package manager first, then builds from source)
python3 scripts/install_ffmpeg_vmaf.py

# Build from source directly
python3 scripts/install_ffmpeg_vmaf.py --from-source

# Skip VMAF (PSNR only)
python3 scripts/install_ffmpeg_vmaf.py --skip-vmaf
```

Or manually install FFmpeg with libvmaf:

```bash
# Ubuntu/Debian (may not include libvmaf)
sudo apt-get install ffmpeg

# Verify VMAF support
ffmpeg -filters 2>&1 | grep libvmaf
```

#### Encoder Not Found

Ensure the encoder is built and the path is correct:

```bash
ls /path/to/vulkan-video-samples/build-release/vk_video_encoder/test/vulkan-video-enc-test
```

#### Low Quality Scores

Low PSNR/VMAF is normal at low bitrates. Try:
- Increasing `--average-bitrate`
- Using fewer frames with `--num-frames`
- Checking if input YUV format matches `--chroma-subsampling`

### Getting Help

```bash
python3 scripts/aq_quality_benchmark.py --help
```
