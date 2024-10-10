# Build Instructions

Instructions for building this repository on Linux, Windows, L4.

## Index

1. [Repository Set-Up](#repository-set-up)
2. [Windows Build](#building-on-windows)
3. [Linux Build](#building-on-linux)
4. [Linux for Tegra Build](#building-on-linux-for-tegra)
5. [Android Build](#building-on-android)

## Contributing to the Repository

If you intend to contribute, the preferred work flow is for you to develop
your contribution in a fork of this repository in your GitHub account and
then submit a pull request.
Please see the [CONTRIBUTING.md](CONTRIBUTING.md) file in this repository for more details.

## Repository Set-Up

  Please make sure you have installed the latest NVIDIA BETA drivers from https://developer.nvidia.com/vulkan-driver.
  The minimum supported BETA driver versions by this application are 538.31 (Windows) / 535.43.23 (Linux) that
  must support Vulkan API version 1.3.274 or later.
  The Windows and Linux BETA drivers are available for download at https://developer.nvidia.com/vulkan-beta-51769-windows
  and https://developer.nvidia.com/vulkan-beta-5154924-linux, respectively.

### Download the Repository

   Vulkan Video Test application Gerrit repository:
   VULKAN_VIDEO_GIT_REPO="https://github.com/nvpro-samples/vk_video_samples.git"

   $ git clone $VULKAN_VIDEO_GIT_REPO

   APP_INSTALLED_LOC=$(pwd)/"vk_video_samples/vk_video_decoder"
   APP_INSTALLED_LOC=$(PWD)/"vk_video_samples/vk_video_encoder"

## Building On Windows

### Windows Build Requirements

Windows 10 or Windows 11 with the following software packages:

- Microsoft Visual Studio VS2019 or later (any version).
- [CMake](http://www.cmake.org/download/)
  - Tell the installer to "Add CMake to the system PATH" environment variable.
- [Python 3](https://www.python.org/downloads)
  - Select to install the optional sub-package to add Python to the system PATH
    environment variable.
  - Ensure the `pip` module is installed (it should be by default)
  - Python3.3 or later is necessary for the Windows py.exe launcher that is used to select python3
  rather than python2 if both are installed
- [Git](http://git-scm.com/download/win)
  - Tell the installer to allow it to be used for "Developer Prompt" as well as "Git Bash".
  - Tell the installer to treat line endings "as is" (i.e. both DOS and Unix-style line endings).
  - Install both the 32-bit and 64-bit versions, as the 64-bit installer does not install the
    32-bit libraries and tools.
- [Vulkan SDK](https://vulkan.lunarg.com)
  - install current Vulkan SDK (i.e. VulkanSDK-1.3.*-Installer.exe) from https://vulkan.lunarg.com/
- [FFMPEG libraries for Windows]
    Download the latest version of the FFMPEG shared libraries archive from https://github.com/BtbN/FFmpeg-Builds/releases.
    The archive must have the following pattern in the name ffmpeg-*-win64-*-shared.zip
    For example:
    https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip
    Then extract the archive to <APP_INSTALLED_LOC>\bin\libs\ffmpeg and add the path of the <APP_INSTALLED_LOC>\bin\libs\ffmpeg\win64\ of
    the application. Please make sure that <APP_INSTALLED_LOC>\bin\libs\ffmpeg\win64\bin location contains
    avformat-59.dll, avutil-59.dll and avcodec-59.dll shared libraries and <APP_INSTALLED_LOC>\bin\libs\ffmpeg\win64\lib contains the
    corresponding lib files.
- Notes for using [Windows Subsystem for Linux](https://docs.microsoft.com/en-us/windows/wsl/install-win10)
  - Vulkan Video currently does not support [Windows Subsystem for Linux].

### Windows Build - Microsoft Visual Studio

1. Open a Developer Command Prompt for VS201x
2. Change directory to `<APP_INSTALLED_LOC>` -- the root of the cloned git repository
3. Run `update_external_sources.bat` -- this will download and build external components
4. Create a `build` directory, change into that directory, and run cmake and build as follows:

### Windows Build for debug - using shell

        cmake .. -DCMAKE_GENERATOR_PLATFORM=x64 -DCMAKE_INSTALL_PREFIX="$(PWD)/install/Debug" -DCMAKE_BUILD_TYPE=Debug
        cmake --build . --parallel 16 --config Debug
        cmake --build . --config Debug --target INSTALL

### Windows Build for release - using shell

        cmake .. -DCMAKE_GENERATOR_PLATFORM=x64 -DCMAKE_INSTALL_PREFIX="$(PWD)/install/Release" -DCMAKE_BUILD_TYPE=Release
        cmake --build . --parallel 16 --config Release
        cmake --build . --config Release --target INSTALL

### Windows Notes

#### The Vulkan Loader Library

Vulkan programs must be able to find and use the vulkan-1.dll library.
While several of the test and demo projects in the Windows solution set this up automatically, doing so manually may be necessary for custom projects or solutions.
Make sure the library is either installed in the C:\Windows\System folder, or that the PATH environment variable includes the folder where the library resides.

## Building On Linux

### Linux Build Requirements

This repository has been built and tested on the two most recent Ubuntu LTS versions with
Vulkan SDK vulkansdk-linux-x86_64-1.2.189.0.tar.gz from https://vulkan.lunarg.com/sdk/home#linux.
Currently, the oldest supported version is Ubuntu 18.04.5 LTS, meaning that the minimum supported
compiler versions are GCC 7.5.0 and Clang 6.0.0, although earlier versions may work.
It should be straightforward to adapt this repository to other Linux distributions.

**Required Package List:**

1. sudo apt-get install git cmake build-essential libx11-xcb-dev libxkbcommon-dev libmirclient-dev libwayland-dev libxrandr-dev libavcodec-dev libavformat-dev libavutil-dev ninja-build

### Linux Vulkan Video Demo Build

2. In a Linux terminal, `cd <APP_INSTALLED_LOC>` -- the root of the cloned git repository
        cd $APP_INSTALLED_LOC
3. Execute `./update_external_sources.sh` -- this will download and build external components
        $ ./update_external_sources.sh
4. Create a `build` directory, change into that directory:

        mkdir build
        cd build

5. Run cmake to configure the build:

        # Decoder/Encoder build configuration (please select Debug or Release build type):
        For example, for Debug build:
        cmake -DCMAKE_BUILD_TYPE=Debug ..

        OR for Debug build:
        cmake -DCMAKE_BUILD_TYPE=Release ..

6. Run `make -j16` to build the sample application.

7. Then install it with 'sudo make install'

### WSI Support Build Options

By default, the Vulkan video demo is built with support for all 3 Vulkan-defined WSI display servers: Xcb, Xlib and Wayland, as well as direct-to-display.
It is recommended to build the repository components with support for these display servers to maximize their usability across Linux platforms.
If it is necessary to build these modules without support for one of the display servers, the appropriate CMake option of the form `BUILD_WSI_xxx_SUPPORT` can be set to `OFF`.
See the top-level CMakeLists.txt file for more info.

### Linux Decoder Tests

Before you begin, check if your driver has Vulkan Video extensions enabled:

        $ vulkaninfo | grep VK_KHR_video

The output should be:
           VK_KHR_video_decode_queue : extension revision 1
           VK_KHR_video_encode_queue : extension revision 1
           VK_KHR_video_queue : extension revision 1

To run the Vulkan Video Decode sample from the build dir (for Windows change to build\install\Debug\bin\ or build\install\Release\bin\):

        $ ./demos/vk-video-dec-test -i '<Video content file with h.264 or h.265 format>'
        # -- Optional parameter is the max number of frames to be displayed with --c N.
        # For example if the max frames required are 100, then:
        $ ./demos/vk-video-dec-test -i /data/nvidia-l4t/video-samples/Palm_Trees_4Videvo.mov --c 100
        # One can find some sample videos in h.264 and h.265 formats here:
        # http://jell.yfish.us/

You can select which WSI subsystem is used to build the demos using a CMake option
called DEMOS_WSI_SELECTION.
Supported options are XCB (default), XLIB, WAYLAND, and MIR.
Note that you must build using the corresponding BUILD_WSI_*_SUPPORT enabled at the
base repository level (all SUPPORT options are ON by default).
For instance, creating a build that will use Xlib to build the demos,
your CMake command line might look like:

    cmake -H. -Bbuild -DCMAKE_BUILD_TYPE=Debug -DDEMOS_WSI_SELECTION=XLIB

### Linux Encoder Tests

Before you begin, check if your driver has Vulkan Video extensions enabled:

        $ vulkaninfo | grep VK_KHR_video

The output should be:
VK_KHR_video_encode_h264                      : extension revision 14
        VK_KHR_video_encode_h265                      : extension revision 14
        VK_KHR_video_encode_queue                     : extension revision 12
        VK_KHR_video_maintenance1                     : extension revision 1
        VK_KHR_video_queue                            : extension revision 8

To run the Vulkan Video Encode sample from the build dir (for Windows change to build\install\Debug\bin\ or build\install\Release\bin\):

        $ ./demos/vk-video-enc-test -i <yuv-video-input-file.yuv> --codec <"h264" | "h265" | "av1"> --inputNumPlanes <2 | 3> --inputWidth <input Y width> --inputHeight <input Y height> --startFrame 0 --numFrames <max frame num>

You can select which WSI subsystem is used to build the demos using a CMake option
called DEMOS_WSI_SELECTION.
Supported options are XCB (default), XLIB, WAYLAND, and MIR.
Note that you must build using the corresponding BUILD_WSI_*_SUPPORT enabled at the
base repository level (all SUPPORT options are ON by default).
For instance, creating a build that will use Xlib to build the demos,
your CMake command line might look like:

    cmake -H. -Bbuild -DCMAKE_BUILD_TYPE=Debug -DDEMOS_WSI_SELECTION=XLIB

## Building On Linux for Tegra

### Linux for Tegra Build Requirements

This repository has been built and tested on the two most recent Ubuntu LTS versions.
Currently, the oldest supported version is Ubuntu 18.04.5 LTS, meaning that the minimum supported compiler versions are GCC 7.5.0 and Clang 6.0.0, although earlier versions may work.
It should be straightforward to adapt this repository to other Linux distributions.

**Required Package List:**

    sudo apt-get install git cmake build-essential libx11-xcb-dev libxkbcommon-dev libmirclient-dev libwayland-dev libxrandr-dev libavcodec-dev libavformat-dev libavutil-dev

### Linux for Tegra Vulkan Video Demo Build

1. Set the TEGRA_TOP environment variables to the location of the application
        export TEGRA_TOP=$APP_INSTALLED_LOC

2. Follow the Linux Build instructions above

## Ninja Builds - All Platforms

The [Qt Creator IDE](https://qt.io/download-open-source/#section-2) can open a root CMakeList.txt
as a project directly, and it provides tools within Creator to configure and generate Vulkan SDK
build files for one to many targets concurrently.
Alternatively, when invoking CMake, use the `-G "Codeblocks - Ninja"` option to generate Ninja build
files to be used as project files for QtCreator

- Follow the steps defined elsewhere for the OS using the update\_external\_sources script or as
  shown in **Loader and Validation Layer Dependencies** below
- Open, configure, and build the glslang CMakeList.txt files. Note that building the glslang
  project will provide access to spirv-tools and spirv-headers
- Then do the same with the Vulkan-LoaderAndValidationLayers CMakeList.txt file
- In order to debug with QtCreator, a
  [Microsoft WDK: eg WDK 10](http://go.microsoft.com/fwlink/p/?LinkId=526733) is required.

Note that installing the WDK breaks the MSVC vcvarsall.bat build scripts provided by MSVC,
requiring that the LIB, INCLUDE, and PATHenv variables be set to the WDK paths by some other means

