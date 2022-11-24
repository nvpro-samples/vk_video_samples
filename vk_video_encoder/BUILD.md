# Build Instructions

Instructions for building this repository on Linux.

## Repository Requirements 

  Please make sure you have installed the latest NVIDIA BETA drivers from https://developer.nvidia.com/vulkan-driver.
  The minimum supported BETA driver versions by this application are 517.69 (Windows) / 515.49.24 (Linux) that
  must support Vulkan API version 1.3.230 or later.
  The Windows and Linux BETA drivers are available for download at https://developer.nvidia.com/vulkan-beta-51769-windows
  and https://developer.nvidia.com/vulkan-beta-5154924-linux, respectively.

Set the path to Vulkan SDK via the VK_SDK_PATH environment variable. For ex:

On Windows:
  set VK_SDK_PATH=C:\VulkanSDK\1.3.231.0

On Linux
  export VK_SDK_PATH=/usr/share/vulkan

## Building the Video Encode Sample

  Follow instructions and requirements available in https://github.com/nvpro-samples/build_all

  Example bellow:
  1. In Linux Terminal or Windows Command Prompt create a folder for the nvpro samples
  ```
    mkdir nvpro_samples
    cd nvpro_samples
  ```

  2. Clone the nvpro core and nvpro samples
  ```
    git clone https://github.com/nvpro-samples/build_all
    cd build_all
 In Linux
    ./clone_all.sh
 In Windows
    .\clone_all.bat
  ```

  3. Regenerate the Vulkan entry point trampoline with support for API calls
     from beta extensions
  ```
    cd ../nvpro_core/nvvk
    python3 extensions_vk.py --beta
    cd ../../
  ```

  4. Clone the vulkan video samples (encoder and decoder)
  ```
    git clone https://github.com/nvpro-samples/vk_video_samples.git
  ```

  5. Compile vulkan video encoder sample
  ```
    cd vk_video_samples/vk_video_encoder
    mkdir build
    cd build
    cmake -DCMAKE_BUILD_TYPE=Debug ..
    cmake --build . --parallel 16 --config Debug
  ```

## Running the Video Encode Sample

  The output video encode executable is stored at nvpro_samples/bin_x64/Debug/vk_video_encoder.exe
  To encode an input YCbCr file:
  ```
  cd nvpro_samples/bin_x64/Debug/
  ./vk_video_encoder.exe -i <inputYCbCrFileName>.yuv --width <inputContentWidth> --height <inputContentHeight> --startFrame <startFrame> --numFrames <NumberOfFramesToEncode>
  For ex:
  ./vk_video_encoder.exe -i ./BasketballDrive_1920x1080_50.yuv --width 1920 --height 1080 --startFrame 0 --numFrames 160
  ```
