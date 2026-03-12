DRM Format Modifier + Video Usage Issue
=======================================

Date: 2026-02-15
Driver: NVIDIA 610.01 (Debug Build, Blackwell GB20X / RTX 5080)
Related: Khronos issue #4624 (Image validation errors with video usage)
         VVL PR: https://github.com/KhronosGroup/Vulkan-ValidationLayers/pull/11472


Problem
-------
vkCreateImage fails (VK_ERROR_FORMAT_NOT_SUPPORTED) when creating a
DRM-modifier-tiled, DMA-BUF-exportable image with video usage bits.

The driver's internal vkGetPhysicalDeviceImageFormatProperties2 check
rejects the combination of:
  - VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
  - VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
  - VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR  (or VIDEO_DECODE_DST)

This blocks cross-process video encode/decode via DMA-BUF sharing.
The same image creation works with VK_IMAGE_TILING_OPTIMAL (no DRM modifier).


Failing vkCreateImage parameters
---------------------------------
  format:      VK_FORMAT_G8_B8R8_2PLANE_420_UNORM (NV12)
  tiling:      VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
  usage:       VIDEO_ENCODE_SRC | SAMPLED | TRANSFER_SRC | TRANSFER_DST
               (also fails with VIDEO_DECODE_DST instead of ENCODE_SRC)
  flags:       MUTABLE_FORMAT | EXTENDED_USAGE | VIDEO_PROFILE_INDEPENDENT
  pNext chain:
    -> VkImageFormatListCreateInfo        (NV12 + R8 + RG8 view formats)
    -> VkImageDrmFormatModifierListCreateInfoEXT  (modifier 0x0 = LINEAR)
    -> VkExternalMemoryImageCreateInfo    (DMA_BUF_BIT)

  Result: VK_ERROR_FORMAT_NOT_SUPPORTED
          (from vkGetPhysicalDeviceImageFormatProperties2 internal check)


Expected behavior
-----------------
The driver should support DRM modifier tiling + video usage for NV12.
The DRM modifier query (vkGetPhysicalDeviceFormatProperties2) returns
6 modifiers (including LINEAR) for NV12 with SAMPLED | TRANSFER_SRC.

Per Khronos #4624 discussion:
  - The video format query (vkGetPhysicalDeviceVideoFormatPropertiesKHR)
    should only be called with video usage bits
  - Non-video usage (SAMPLED, TRANSFER, STORAGE) is validated separately
    via the general format properties query
  - EXTENDED_USAGE allows STORAGE on per-plane views (R8, RG8)
  - VIDEO_PROFILE_INDEPENDENT avoids needing a VkVideoProfileListInfoKHR
    at image creation time (from VK_KHR_video_maintenance1)

The NVIDIA driver internally splits video vs non-video query
(per Tony Zlatinski's comment in #4624), but the DRM modifier path
does not appear to handle this split correctly.


How to reproduce
----------------
Build:
  cd /data/nvidia/android-extra/video-apps/vulkan-video-samples/build
  cmake --build . -j4 -- drm_format_mod_test

Run (VIDEO_ENCODE_SRC):
  ./bin/drm_format_mod_test --video-encode --format NV12 -v

Run (VIDEO_DECODE_DST):
  ./bin/drm_format_mod_test --video-decode --format NV12 -v

Run (both):
  ./bin/drm_format_mod_test --video-encode --video-decode --format NV12 -v

Run all YCbCr formats with encode:
  ./bin/drm_format_mod_test --video-encode --ycbcr-only -v

Run without video flags (should PASS):
  ./bin/drm_format_mod_test --format NV12 -v

Run with P010 (10-bit):
  ./bin/drm_format_mod_test --video-encode --format P010 -v


Test output (encode)
--------------------
  [PASS] TC1_Query_NV12: 6 modifiers found (LINEAR supported)
  [video] usage=0x4007 flags=0x100108
  Assertion `!"CreateImage Failed!"' failed.
    -> vkCreateImage returns VK_ERROR_FORMAT_NOT_SUPPORTED

  usage  0x4007   = TRANSFER_SRC | TRANSFER_DST | SAMPLED | VIDEO_ENCODE_SRC
  flags  0x100108 = MUTABLE_FORMAT | EXTENDED_USAGE | VIDEO_PROFILE_INDEPENDENT


Test output (decode)
--------------------
  [PASS] TC1_Query_NV12: 6 modifiers found (LINEAR supported)
  [video] usage=0x407 flags=0x100108
  Assertion `!"CreateImage Failed!"' failed.

  usage  0x407    = TRANSFER_SRC | TRANSFER_DST | SAMPLED | VIDEO_DECODE_DST


Use case
--------
The ThreadedRenderingVk renderer exports NV12 frames via DMA-BUF to a
spawned vk-encoder-instance child process for cross-process Vulkan Video
encoding. The export images need both DRM modifier tiling (for DMA-BUF)
and VIDEO_ENCODE_SRC (for direct zero-copy encode on the consumer side).

Without this, the encoder must stage (copy) every frame to an internal
OPTIMAL image with VIDEO_ENCODE_SRC, adding latency and bandwidth.


Debugging with GDB
-------------------
Break on the vkCreateImage failure:

  cd /data/nvidia/android-extra/video-apps/vulkan-video-samples/build
  gdb --args ./bin/drm_format_mod_test --video-encode --format NV12 -v

  (gdb) break vkCreateImage
  (gdb) run
  # hits vkCreateImage — step into the driver
  (gdb) step
  # or set a conditional break on the return value:
  (gdb) break VkImageResource.cpp:339
  (gdb) run

Break inside the driver's image format check:

  (gdb) break vkGetPhysicalDeviceImageFormatProperties2
  (gdb) run
  # When hit, inspect the pImageFormatInfo:
  (gdb) print *pImageFormatInfo
  # Check usage, tiling, format fields
  # Step through the driver's split query logic

Break on the specific driver function (if symbols available):

  (gdb) break __VkPhysDevFeatures::DmaBufSupported
  (gdb) break vkimage.cpp:__vkGetPhysicalDeviceImageFormatProperties2

Quick backtrace when the assert fires:

  (gdb) run
  # program hits assert → SIGABRT
  (gdb) bt
  # shows full call stack through driver

Disable the assert and continue to see the VkResult:

  (gdb) break VkImageResource.cpp:337
  (gdb) run
  (gdb) next
  (gdb) print result
  # Should show VK_ERROR_FORMAT_NOT_SUPPORTED (-11)


Root cause (found in driver source)
------------------------------------
File: vulkan/features/full/vkphysdevfeatures.cpp

GetPhysicalDeviceImageFormatProperties2() at line 5179:

  1. Line 5200-5211: DRM modifier path converts tiling:
       if (modifier == DRM_FORMAT_MOD_LINEAR)
           tiling = VK_IMAGE_TILING_LINEAR;
       else
           tiling = VK_IMAGE_TILING_OPTIMAL;

  2. Line 5239-5256: Video + non-LINEAR modifier check (GOB block height).
     This check is SKIPPED for LINEAR modifier — correct.

  3. Line 5280-5285: Calls the VK 1.0 GetPhysicalDeviceImageFormatProperties
     with the FULL usage (including VIDEO_ENCODE_SRC) and tiling=LINEAR:

       result = GetPhysicalDeviceImageFormatProperties(
           format, type, tiling=LINEAR,
           usage=VIDEO_ENCODE_SRC|SAMPLED|TRANSFER_SRC|TRANSFER_DST,
           flags=MUTABLE_FORMAT|EXTENDED_USAGE|VIDEO_PROFILE_INDEPENDENT,
           ...);

     *** THIS IS WHERE IT FAILS ***

     The VK 1.0 entry point doesn't understand video usage bits.
     It sees VIDEO_ENCODE_SRC in the usage and rejects it because
     LINEAR tiling doesn't "support" video encode usage in the
     general format properties table.

Fix: Before the call at line 5280, strip video usage bits from `usage`
when a DRM modifier is present (pFormatModifierInfo != nullptr).
Video usage is validated separately via the video profile path
(lines 5267-5277) or via VIDEO_PROFILE_INDEPENDENT flag.

Fix applied in vkphysdevfeatures.cpp:

  Part 1 (line ~5239): Reject LINEAR modifier + video usage
    NVDEC/NVENC HW requires tiled (optimal) memory. LINEAR images
    cannot be used as encode source or decode destination.
    Previously this only checked non-linear modifiers with GOB=0.
    Now it also rejects LINEAR (modifier 0x0) with any video usage.

  Part 2 (line ~5280): Strip video bits from VK 1.0 query
    Before calling GetPhysicalDeviceImageFormatProperties (VK 1.0),
    strip video usage bits and VIDEO_PROFILE_INDEPENDENT flag when
    a DRM modifier is present. Video usage was already validated
    above (LINEAR/GOB rejection + video profile check). The VK 1.0
    path only needs to validate non-video usage (SAMPLED, TRANSFER, etc).

    This is the split query approach from Khronos issue #4624:
      video bits    → video core (lines 5239-5277)
      non-video bits → general format properties (line 5280)

  After this fix:
    - LINEAR + VIDEO_ENCODE_SRC → VK_ERROR_FORMAT_NOT_SUPPORTED (correct)
    - TILED(GOB>=2) + VIDEO_ENCODE_SRC → succeeds (video validated separately)
    - TILED(GOB=0) + VIDEO_ENCODE_SRC → VK_ERROR_FORMAT_NOT_SUPPORTED (correct)


Driver code reference
---------------------
  Extension registration: vulkan/features/full/vkphysdevfeatures.cpp
    DmaBufSupported()                            - line ~4740
    compatibleHandleTypes (DMA_BUF mask-off)     - line ~5422
    GetPhysicalDeviceImageFormatProperties2()    - line 5179
    DRM modifier tiling conversion               - line 5200-5211
    Video + modifier GOB check                   - line 5239-5256
    *** FAILING CALL ***                         - line 5280-5285
    Video profile list check                     - line 5267-5277
