# Build Instructions

Instructions for building the Vulkan Video Samples repository on Linux and Windows.

## Index

1. [Repository Set-Up](#repository-set-up)
2. [Build Options](#build-options)
3. [Windows Build](#building-on-windows)
4. [Linux Build](#building-on-linux)

## Repository Set-Up

Please make sure you have installed the latest NVIDIA BETA drivers from https://developer.nvidia.com/vulkan-driver.
The minimum supported BETA driver versions by this application are 553.51 (Windows) / 550.40.82 (Linux) that
must support Vulkan API version 1.3.230 or later.
The Windows and Linux BETA drivers are available for download at https://developer.nvidia.com/vulkan-driver.

### Download the Repository

> Note: An alternative version of this repository is also available:
> https://github.com/nvpro-samples/vk_video_samples

```bash
VULKAN_VIDEO_GIT_REPO="https://github.com/KhronosGroup/Vulkan-Video-Samples.git"
git clone $VULKAN_VIDEO_GIT_REPO
cd vk_video_samples
```

## Build Options

The project supports the following main build options:

- `BUILD_DECODER` (Default: ON) - Build the Vulkan video decoder components
- `BUILD_ENCODER` (Default: ON) - Build the Vulkan video encoder components
- `BUILD_VIDEO_PARSER` (Default: ON) - Build the video parser library used by both encoder and decoder

These options can be specified during CMake configuration. For example:
```bash
cmake -DBUILD_DECODER=ON -DBUILD_ENCODER=OFF -DBUILD_VIDEO_PARSER=ON ...
```

## Building On Windows

### Windows Build Requirements

Windows 10 or Windows 11 with the following software packages:

- Microsoft Visual Studio VS2019 or later (any version)
- [CMake](http://www.cmake.org/download/)
  - Tell the installer to "Add CMake to the system PATH" environment variable
- [Python 3](https://www.python.org/downloads)
  - Select to install the optional sub-package to add Python to the system PATH
  - Ensure the `pip` module is installed (it should be by default)
  - Python3.3 or later is necessary for the Windows py.exe launcher
- [Git](http://git-scm.com/download/win)
  - Tell the installer to allow it to be used for "Developer Prompt" as well as "Git Bash"
  - Tell the installer to treat line endings "as is"
  - Install both 32-bit and 64-bit versions
- [Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows)
  - Install current Vulkan SDK (i.e. VulkanSDK-1.4.304.0-Installer.exe or later)
  - Make sure to install the the correct SDK for the targeted system arch - x86_64 or ARM64
- [FFMPEG libraries for Windows]
  - Download the latest version of the FFMPEG shared libraries archive from https://github.com/BtbN/FFmpeg-Builds/releases
  - The archive must have the following pattern in the name: For Windows x86_64 ffmpeg-*-win64-*-shared.zip
  - Example download link:
         For Windows x86_64 https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip
         For Windows ARM64  https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-winarm64-lgpl-shared.zip
  - Extract to <APP_INSTALLED_LOC>\vk_video_decoder\bin\libs\ffmpeg
  - Verify that <APP_INSTALLED_LOC>\vk_video_decoder\bin\libs\ffmpeg\win64\bin or <APP_INSTALLED_LOC>\vk_video_decoder\bin\libs\ffmpeg\winarm64\bin contains:
    - avformat-59.dll
    - avutil-59.dll
    - avcodec-59.dll
  - Verify that <APP_INSTALLED_LOC>\vk_video_decoder\bin\libs\ffmpeg\win64\lib or <APP_INSTALLED_LOC>\vk_video_decoder\bin\libs\ffmpeg\winarm64\lib contains the corresponding .lib files

### Windows Build Commands

For Debug build:
```bash
For X86_64 based platforms:
  cmake . -B build -DCMAKE_GENERATOR_PLATFORM=x64 -DCMAKE_INSTALL_PREFIX="$(PWD)/build/install/Debug" -DCMAKE_BUILD_TYPE=Debug
For ARM64 based platforms:
  cmake . -B build -DCMAKE_GENERATOR_PLATFORM=ARM64 -DCMAKE_INSTALL_PREFIX="$(PWD)/build/install/Debug" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 16 --config Debug
cmake --build build --config Debug --target INSTALL
```

For Release build:
```bash
For X86_64 based platforms:
  cmake . -B build -DCMAKE_GENERATOR_PLATFORM=x64 -DCMAKE_INSTALL_PREFIX="$(PWD)/build/install/Release" -DCMAKE_BUILD_TYPE=Release
For ARM64 based platforms:
  cmake . -B build -DCMAKE_GENERATOR_PLATFORM=ARM64 -DCMAKE_INSTALL_PREFIX="$(PWD)/build/install/Release" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 16 --config Release
cmake --build build --config Release --target INSTALL
```

## Building On Linux

### Linux Build Requirements

This repository has been tested on recent Ubuntu LTS versions. Minimum requirements:
- Ubuntu 18.04.5 LTS or later
- GCC 7.5.0 or Clang 6.0.0
- Vulkan SDK vulkansdk-linux-x86_64-1.4.304.0.tar.xz or later for x86_64

Required packages:
```bash
sudo apt-get install git cmake build-essential libx11-xcb-dev libxkbcommon-dev \
    libmirclient-dev libwayland-dev libxrandr-dev libavcodec-dev \
    libavformat-dev libavutil-dev ninja-build
```

### Linux Build Steps

1. Execute the dependency script:
```bash
./vk_video_decoder/ubuntu-update-dependencies.sh
```

2. Configure and build:
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..  # or Release
make -j16
sudo make install
```

### Testing the Installation

Before running tests, verify Vulkan Video extensions:
```bash
vulkaninfo | grep VK_KHR_video
```

For decoder testing:
```bash
./vk_video_decoder/demos/vk-video-dec-test -i '<Video content file with h.264 or h.265 format>' [--c N]
```

For encoder testing:
```bash
./vk_video_encoder/demos/vk-video-enc-test -i <yuv-video-input-file.yuv> --codec <"h264" | "h265" | "av1"> \
    --inputNumPlanes <2 | 3> --inputWidth <input Y width> --inputHeight <input Y height> \
    --startFrame 0 --numFrames <max frame num>
```
