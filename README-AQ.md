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
- NVIDIA Beta Driver with Vulkan Video enabled
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
    --averageBitrate 5000 \
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
    --averageBitrate 5000 ^
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
    --averageBitrate 5000 \
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
    --averageBitrate 5000 ^
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
    --averageBitrate 5000 \
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
    --averageBitrate 5000 ^
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
    --averageBitrate 5000 \
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
    --averageBitrate 5000 ^
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
    --averageBitrate 5000 \
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
    --averageBitrate 5000 ^
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
    --averageBitrate 5000 \
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
    --averageBitrate 5000 ^
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
    --averageBitrate 5000 \
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
    --averageBitrate 5000 ^
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
    --averageBitrate 5000 \
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
    --averageBitrate 5000 ^
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
    --averageBitrate 5000 \
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
    --averageBitrate 5000 ^
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
    --averageBitrate 5000 \
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
    --averageBitrate 5000 ^
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
- `--average-bitrate <kbps>`: Average bitrate in kbits/sec (optional)

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
    --average-bitrate 5000
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
    --average-bitrate 5000
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
