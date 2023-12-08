# VK_VULKAN_VIDEO

This project conatins two Vulkan Video sample applications which demonstrate video decoding and encoding, respectively. These samples can be independently compiled. Instructions on how to build either the encoder or decoder sample are present within each respective folder.

## VK_VIDEO_DECODE

![vk_video_decode](vk_video_decoder/doc/VideoDecode.png)


This project is a Vulkan Video Sample Application demonstrating an end-to-end, all-Vulkan, processing of h.264/5 compressed video content. The application decodes the  h.264/5 compressed content using an HW accelerated decoder, the decoded YCbCr frames are processed with Vulkan Graphics and then presented via the Vulkan WSI.

Currently, the sample application supports Linux and Windows10 operating systems.


Features
========

- [x] Extracts (DEMUX via FFMPEG) compressed video from .mp4, .mkv .mov and others video containers using h.264 (AVC) or h.265 (HEVC) compression formats. 
- [x] The HW video decoder processes textures to Vulkan Video Images that can be directly sampled from Vulkan Samplers (Textures). 
- [x] Converts the YCbCr (YUV) Images to RGB while sampling the decoded images using the [VK_KHR_sampler_ycbcr_conversion](https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VK_KHR_sampler_ycbcr_conversion.html)
- [x] Displays the post-processed video frames using Vulkan WSI.
- [x] Provides the h.264/5 SPS/PPS video picture parameters inlined with each frame's parameters. This isn't compliant with the Vulkan Video Specification. Proper handling of such parameters must be done using an object of type VkVideoSessionParametersKHR.
- [x] Added support for VkVideoSessionParametersKHR for full compliance with the Vulkan Video Specification.
- [ ] Use Video display timing synchronization (such as VK_EXT_present_timing) at the WSI side - currently the video is played at the maximum frame rate that the display device can support. The video may be played at a faster rate than it is authored.
- [ ] Convert the sample's framework to be compatible with the rest of the nvpro-samples.


For instructions on how to build the sample decode application, please see [the build instructions.](https://github.com/nvpro-samples/vk_video_samples/blob/main/vk_video_decoder/BUILD.md)

Please download and install [Beta NVIDIA Driver with Vulkan Video Enabled](https://developer.nvidia.com/vulkan-driver).

For Vulkan Video Specification please refer to [Vulkan Spec with Video Extensions](https://www.khronos.org/registry/vulkan/specs/1.2-extensions/html/vkspec.html).

For deep-dive information on Vulkan Video please refer to the [Deep Dive Slide Deck](https://www.khronos.org/assets/uploads/apis/Vulkan-Video-Deep-Dive-Apr21.pdf).

## VK_VIDEO_ENCODE

This project is a Nvpro-based Vulkan Video sample application demonstrating video encoding. By using the Vulkan video encoding extensions to drive the HW-accelerated video encoder, this application encodes YCbCr content and writes the h.264 or h.265 compressed video to a file.

The sample is still in development and has issues such as missing POC numbers and corrupted frames.

Currently, the sample application supports Linux and Windows operating systems.

Features
========

- [x] Video encoding using h.264 standard
- [x] Support for all-Intra GOP structure
- [x] Support for P frames
- [x] Support for B frames
- [x] Encoding frames from graphical application
- [x] Different YCbCr chroma subsampling and bit depth options
- [x] Support for h.265 standard
- [ ] Support for encoding tuning and quality levels
- [ ] Better Support for rate control
- [ ] Support for multi-threaded encoding
- [ ] Test 10-bit encoding

For instructions on how to build the sample decode application, please see [the build instructions.](https://github.com/nvpro-samples/vk_video_samples/blob/main/vk_video_encoder/BUILD.md)

Please download and install [Beta NVIDIA Driver with Vulkan Video Enabled](https://developer.nvidia.com/vulkan-driver).

For Vulkan Video Specification please refer to [Vulkan Spec with Video Extensions](https://www.khronos.org/registry/vulkan/specs/1.3-extensions/html/vkspec.html).

For deep-dive information on Vulkan Video please refer to the [Deep Dive Slide Deck](https://www.khronos.org/assets/uploads/apis/Vulkan-Video-Deep-Dive-Apr21.pdf).
