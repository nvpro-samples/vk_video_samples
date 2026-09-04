/*
 * Copyright 2026 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// qfot_foreign_release_repro.c -- a DRIVER-DEFECT reproducer, not a test.
//
// ---------------------------------------------------------------------------
// WHY THIS IS NOT WIRED INTO CMake OR ctest, AND MUST NOT BE.
// ---------------------------------------------------------------------------
// Every RED mode here DESTROYS THE VULKAN DEVICE and raises an Xid on the
// running GPU. Under a test runner that is not a failing assertion, it is a
// machine-wide event that takes out whatever else is using the card. Build it
// by hand, on a host you are willing to disturb:
//
//     gcc -O0 -g -o qfot_foreign_release_repro qfot_foreign_release_repro.c -lvulkan
//
// It links no part of this library. It creates no VkVideoSessionKHR, records
// no CmdBeginVideoCodingKHR and no CmdEncodeVideoKHR, and runs no encode. One
// image, one command buffer, one submit, one fence wait. The ONLY thing that
// varies between modes is which image memory barriers are recorded.
//
// ---------------------------------------------------------------------------
// WHAT IT ISOLATES.
// ---------------------------------------------------------------------------
// VkVideoEncoder::RecordVideoCodingCmd's Path-A queue-family RELEASE (see
// VkVideoEncoder::ReleaseImageToForeignQueue) loses the device on an
// affected driver. The encoder-ext-format-encode --foreign-residency DIRECT
// rows are
// where it shows: VK_ERROR_DEVICE_LOST, a zero-byte bitstream, and the
// validation layer silent throughout. That barrier is spec-legal -- VVL 1.4.355
// raises nothing about it, the acquire/release are balanced, the transfer is
// recorded OUTSIDE the video coding scope, and the aspect mask is right for a
// non-disjoint multi-planar image.
//
// ---------------------------------------------------------------------------
// THE DEFECT, as a conjunction.
// ---------------------------------------------------------------------------
// The device is lost iff ALL THREE hold:
//
//   (1) the image was created with VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR or
//       VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
//   (2) the barrier is a RELEASE -- dstQueueFamilyIndex is
//       VK_QUEUE_FAMILY_FOREIGN_EXT;
//   (3) it is recorded on a queue family other than graphics or optical flow.
//
// Failure signature: Xid 32 (invalid or corrupted push buffer stream) on the
// recording engine's channel, "HCE_DBG0 00000124 HCE_DBG1 00000002" followed
// immediately by "HCE_DBG0 00000800". VK_EXT_device_fault is supported and
// returns addressInfoCount=0, vendorInfoCount=0 and an empty description --
// consistent with a method-parse error rather than a memory fault.
//
// RESULT MATRIX (each row one process; verdict is vkWaitForFences):
//
//   ctl                       no ownership transfer                  SURVIVED
//   acq                       FOREIGN -> encode, acquire direction   SURVIVED
//   rel                       encode -> FOREIGN, no layout change    LOST
//   rel-xition                encode -> FOREIGN, ENCODE_SRC->GENERAL LOST
//   pair                      acq then rel-xition (Path-A shape)     LOST
//   rel --home-general        GENERAL -> GENERAL, no transition      LOST
//   rel --stage-none          srcStageMask NONE                      LOST
//   rel --stage-transfer      srcStageMask TRANSFER                  LOST
//   rel --legacy              same release via v1 vkCmdPipelineBarrier LOST
//   rel --dpb                 VIDEO_ENCODE_DPB usage instead of SRC  LOST
//   rel-external              dstQF = VK_QUEUE_FAMILY_EXTERNAL       SURVIVED
//   rel-gfx                   dstQF = a real family (graphics)       SURVIVED
//
// FULL RECORDING-FAMILY SWEEP, one process each, `rel` in every row. The
// device's six families on this part are 0 graphics|compute|transfer|sparse,
// 1 transfer|sparse, 2 compute|transfer|sparse, 3 transfer|sparse|video
// decode, 4 transfer|sparse|video encode, 5 transfer|sparse|optical flow:
//
//   --fam 0 (graphics)        SURVIVED
//   --fam 1 (transfer)        LOST
//   --fam 2 (compute)         LOST
//   --fam 3 (video decode)    LOST
//   --fam 4 (video encode)    LOST   <- the default, and Path A's family
//   --fam 5 (optical flow)    SURVIVED
//   rel --novideo             no video usage bits on the image       SURVIVED
//   rel --profile-only        video profile list, NO video usage     SURVIVED
//   rel --rgba                R8G8B8A8 TRANSFER image                SURVIVED
//
// READ THE GREEN ROWS AS CAREFULLY AS THE RED ONES:
//   * --profile-only green and --novideo green isolate the USAGE BIT. It is
//     the NVENC input-surface allocation, not the video profile and not the
//     multi-planar format.
//   * --home-general LOST is strictly stronger than "changing newLayout does
//     not help": that mode performs no layout transition at all.
//   * --legacy LOST rules out the barrier API generation, so the library
//     cannot dodge this by re-expressing the release.
//   * --fam 5 SURVIVED while --fam 1 LOST, and family 5 (optical flow) has no
//     VK_QUEUE_GRAPHICS_BIT either -- both are plain transfer|sparse plus one
//     engine bit. So the rule is NOT "any non-graphics family"; graphics and
//     optical flow are exempt and the other four engines are not. Stated as
//     measured, because no theory offered so far predicts that split.
//
// ---------------------------------------------------------------------------
// WHY THE LIBRARY CANNOT ROUTE AROUND IT.
// ---------------------------------------------------------------------------
// A release must be recorded on a queue of its SOURCE family, and on Path A
// the only family that owns the input image is the encode family. Condition
// (3) is therefore not a choice the library gets to make. The four responses
// -- drop the Path-A release, substitute VK_QUEUE_FAMILY_EXTERNAL (which means
// "another Vulkan instance", not "a non-Vulkan agent", so it would be a
// correctness regression dressed as a fix), a two-hop encode->graphics->FOREIGN
// transfer costing a second per-frame submit on a second queue, or fix the
// driver -- are a design decision. None of them is applied in this tree.
//
// Attach this file as-is to a driver bug; it is self-contained.

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "FAIL %s -> %d (line %d)\n", #x, (int)_r, __LINE__); exit(2);} } while(0)

static const char* ResName(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    default: return "other";
    }
}

int main(int argc, char** argv)
{
    const char* mode = (argc > 1) ? argv[1] : "ctl";
    int useRgba = 0, useGfxQueue = 0, noVideoUsage = 0, homeGeneral = 0, famOverride = -1;
    int relStage = 0; // 0 = home stage, 1 = TRANSFER, 2 = NONE
    int profileOnly = 0, dpbUsage = 0, legacyBarrier = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rgba")) useRgba = 1;
        if (!strcmp(argv[i], "--gfxq")) useGfxQueue = 1;
        if (!strcmp(argv[i], "--novideo")) noVideoUsage = 1;
        if (!strcmp(argv[i], "--home-general")) homeGeneral = 1;
        if (!strcmp(argv[i], "--fam") && i + 1 < argc) famOverride = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--stage-transfer")) relStage = 1;
        if (!strcmp(argv[i], "--stage-none")) relStage = 2;
        if (!strcmp(argv[i], "--profile-only")) profileOnly = 1;
        if (!strcmp(argv[i], "--dpb")) dpbUsage = 1;
        if (!strcmp(argv[i], "--legacy")) legacyBarrier = 1;
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_3;
    app.pApplicationName = "qfot_repro";
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    const char* instLayers[1]; uint32_t nInstLayers = 0;
    if (getenv("QFOT_VALIDATE")) instLayers[nInstLayers++] = "VK_LAYER_KHRONOS_validation";
    ici.enabledLayerCount = nInstLayers; ici.ppEnabledLayerNames = instLayers;
    VkInstance inst; CHK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t nPhys = 0; vkEnumeratePhysicalDevices(inst, &nPhys, NULL);
    VkPhysicalDevice phys[8]; if (nPhys > 8) nPhys = 8;
    vkEnumeratePhysicalDevices(inst, &nPhys, phys);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t encFam = UINT32_MAX, gfxFam = UINT32_MAX;
    for (uint32_t i = 0; i < nPhys && pd == VK_NULL_HANDLE; i++) {
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &nq, NULL);
        VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &nq, qf);
        uint32_t e = UINT32_MAX, g = UINT32_MAX;
        for (uint32_t q = 0; q < nq; q++) {
            if ((qf[q].queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) && e == UINT32_MAX) e = q;
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && g == UINT32_MAX) g = q;
        }
        if (e != UINT32_MAX) { pd = phys[i]; encFam = e; gfxFam = g;
            VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
            printf("device=%s encodeFamily=%u gfxFamily=%u\n", p.deviceName, e, g);
            for (uint32_t q = 0; q < nq; q++)
                printf("  family[%u] flags=0x%x count=%u\n", q, qf[q].queueFlags, qf[q].queueCount);
        }
    }
    if (pd == VK_NULL_HANDLE) { fprintf(stderr, "no video-encode queue family\n"); return 3; }

    uint32_t submitFam = useGfxQueue ? gfxFam : encFam;
    if (famOverride >= 0) submitFam = (uint32_t)famOverride;

    // Device extensions
    uint32_t nExt = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &nExt, NULL);
    VkExtensionProperties* ext = malloc(sizeof(*ext) * nExt);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &nExt, ext);
    int hasFault = 0;
    for (uint32_t i = 0; i < nExt; i++)
        if (!strcmp(ext[i].extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) hasFault = 1;
    printf("VK_EXT_device_fault present=%d\n", hasFault);

    const char* devExts[8]; uint32_t nDevExts = 0;
    devExts[nDevExts++] = VK_KHR_VIDEO_QUEUE_EXTENSION_NAME;
    devExts[nDevExts++] = VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME;
    devExts[nDevExts++] = VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME;
    devExts[nDevExts++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
    if (hasFault) devExts[nDevExts++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq[2]; uint32_t nDq = 0;
    dq[nDq] = (VkDeviceQueueCreateInfo){ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    dq[nDq].queueFamilyIndex = submitFam; dq[nDq].queueCount = 1; dq[nDq].pQueuePriorities = &prio; nDq++;

    VkPhysicalDeviceFaultFeaturesEXT faultF = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    faultF.deviceFault = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.synchronization2 = VK_TRUE;
    if (hasFault) f13.pNext = &faultF;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f13;
    dci.queueCreateInfoCount = nDq; dci.pQueueCreateInfos = dq;
    dci.enabledExtensionCount = nDevExts; dci.ppEnabledExtensionNames = devExts;
    VkDevice dev; CHK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, submitFam, 0, &queue);

    PFN_vkGetDeviceFaultInfoEXT pfnFault = hasFault
        ? (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(dev, "vkGetDeviceFaultInfoEXT") : NULL;

    // ---- image -------------------------------------------------------------
    VkVideoEncodeH264ProfileInfoKHR h264 = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR };
    h264.stdProfileIdc = 77; // STD_VIDEO_H264_PROFILE_IDC_MAIN
    VkVideoProfileInfoKHR prof = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR };
    prof.pNext = &h264;
    prof.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    prof.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    prof.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    prof.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    VkVideoProfileListInfoKHR plist = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR };
    plist.profileCount = 1; plist.pProfiles = &prof;

    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.extent = (VkExtent3D){ 1920, 1088, 1 };
    ic.mipLevels = 1; ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (useRgba) {
        ic.format = VK_FORMAT_R8G8B8A8_UNORM;
        ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    } else if (profileOnly) {
        // Multi-planar NV12 carrying the video PROFILE LIST at creation but no
        // video usage: separates "created against a video profile" from
        // "created with VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR".
        ic.pNext = &plist;
        ic.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    } else if (noVideoUsage) {
        // Same multi-planar NV12 format, NO video-encode usage and no profile
        // list: separates "multi-planar image" from "video-encode resource".
        ic.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    } else {
        ic.pNext = &plist;
        ic.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        ic.usage = (dpbUsage ? VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR
                             : VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR) | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    VkImage img; CHK(vkCreateImage(dev, &ic, NULL, &img));

    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory mem; CHK(vkAllocateMemory(dev, &mai, NULL, &mem));
    CHK(vkBindImageMemory(dev, img, mem, 0));

    // ---- command buffer ----------------------------------------------------
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.queueFamilyIndex = submitFam;
    VkCommandPool pool; CHK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb; CHK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkImageAspectFlags aspect = useRgba
        ? VK_IMAGE_ASPECT_COLOR_BIT
        : (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);
    // The library passes COLOR on a 2-plane image; mirror that unless asked not to.
    if (!getenv("QFOT_PLANE_ASPECT")) aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageSubresourceRange rng = { aspect, 0, 1, 0, 1 };

    int plain = useRgba || noVideoUsage || profileOnly;
    VkImageLayout homeLayout = plain ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                     : VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
    VkPipelineStageFlags2 homeStage = plain ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
                                            : VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    VkAccessFlags2 homeAccess = plain ? VK_ACCESS_2_TRANSFER_WRITE_BIT
                                      : VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
    if (homeGeneral) {
        // Video-encode resource, but handed over in GENERAL rather than
        // VIDEO_ENCODE_SRC_KHR -- the shape the (green) filter-arm release uses.
        homeLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHK(vkBeginCommandBuffer(cb, &bi));

    #define BARRIER(b) do { VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; \
        di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &(b); \
        vkCmdPipelineBarrier2(cb, &di); } while (0)

    // Control transition: UNDEFINED -> home layout, no ownership transfer.
    VkImageMemoryBarrier2 init = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    init.srcStageMask = VK_PIPELINE_STAGE_2_NONE; init.srcAccessMask = 0;
    init.dstStageMask = homeStage; init.dstAccessMask = homeAccess;
    if (relStage) { init.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    init.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT; }
    init.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; init.newLayout = homeLayout;
    init.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    init.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    init.image = img; init.subresourceRange = rng;
    BARRIER(init);

    if (!strcmp(mode, "acq") || !strcmp(mode, "pair")) {
        // Put it in GENERAL first so the acquire has the library's exact pair.
        VkImageMemoryBarrier2 g = init;
        g.oldLayout = homeLayout; g.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        g.srcStageMask = homeStage; g.srcAccessMask = homeAccess;
        g.dstStageMask = homeStage; g.dstAccessMask = homeAccess;
        BARRIER(g);
        // The library's Path-A acquire, verbatim: srcStage COMPUTE_SHADER,
        // srcAccess SHADER_WRITE (both IGNORED for an acquire), FOREIGN -> enc.
        VkImageMemoryBarrier2 a = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        a.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        a.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        a.dstStageMask = homeStage; a.dstAccessMask = homeAccess;
        a.oldLayout = VK_IMAGE_LAYOUT_GENERAL; a.newLayout = homeLayout;
        a.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        a.dstQueueFamilyIndex = submitFam;
        a.image = img; a.subresourceRange = rng;
        BARRIER(a);
    }

    if (!strcmp(mode, "rel") || !strcmp(mode, "rel-xition") || !strcmp(mode, "pair") ||
        !strcmp(mode, "rel-external") || !strcmp(mode, "rel-gfx")) {
        VkImageMemoryBarrier2 r = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        r.srcStageMask = homeStage; r.srcAccessMask = homeAccess;
        if (relStage == 1) { r.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                             r.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT; }
        if (relStage == 2) { r.srcStageMask = VK_PIPELINE_STAGE_2_NONE; r.srcAccessMask = 0; }
        r.dstStageMask = VK_PIPELINE_STAGE_2_NONE; r.dstAccessMask = 0;
        r.oldLayout = homeLayout;
        r.newLayout = (!strcmp(mode, "rel-xition") || !strcmp(mode, "pair"))
                          ? VK_IMAGE_LAYOUT_GENERAL : homeLayout;
        r.srcQueueFamilyIndex = submitFam;
        r.dstQueueFamilyIndex = !strcmp(mode, "rel-external") ? VK_QUEUE_FAMILY_EXTERNAL
                              : !strcmp(mode, "rel-gfx")      ? gfxFam
                                                              : VK_QUEUE_FAMILY_FOREIGN_EXT;
        r.image = img; r.subresourceRange = rng;
        if (legacyBarrier) {
            VkImageMemoryBarrier r1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            r1.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            r1.dstAccessMask = 0;
            r1.oldLayout = r.oldLayout; r1.newLayout = r.newLayout;
            r1.srcQueueFamilyIndex = r.srcQueueFamilyIndex;
            r1.dstQueueFamilyIndex = r.dstQueueFamilyIndex;
            r1.image = img; r1.subresourceRange = rng;
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, NULL, 0, NULL, 1, &r1);
        } else {
            BARRIER(r);
        }
    }

    CHK(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CHK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VkResult sr = vkQueueSubmit(queue, 1, &si, fence);
    printf("mode=%s rgba=%d submitFamily=%u  vkQueueSubmit -> %d (%s)\n",
           mode, useRgba, submitFam, (int)sr, ResName(sr));
    VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    printf("mode=%s rgba=%d submitFamily=%u  vkWaitForFences -> %d (%s)\n",
           mode, useRgba, submitFam, (int)wr, ResName(wr));

    if (wr == VK_ERROR_DEVICE_LOST && pfnFault) {
        VkDeviceFaultCountsEXT counts = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        if (pfnFault(dev, &counts, NULL) == VK_SUCCESS) {
            printf("  deviceFault: addressInfoCount=%u vendorInfoCount=%u vendorBinarySize=%llu\n",
                   counts.addressInfoCount, counts.vendorInfoCount,
                   (unsigned long long)counts.vendorBinarySize);
            VkDeviceFaultInfoEXT info = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
            info.pAddressInfos = counts.addressInfoCount
                ? calloc(counts.addressInfoCount, sizeof(VkDeviceFaultAddressInfoEXT)) : NULL;
            info.pVendorInfos = counts.vendorInfoCount
                ? calloc(counts.vendorInfoCount, sizeof(VkDeviceFaultVendorInfoEXT)) : NULL;
            if (pfnFault(dev, &counts, &info) == VK_SUCCESS) {
                printf("  faultDescription: %s\n", info.description);
                for (uint32_t i = 0; i < counts.vendorInfoCount; i++)
                    printf("  vendorFault[%u]: %s code=%llu data=%llu\n", i,
                           info.pVendorInfos[i].description,
                           (unsigned long long)info.pVendorInfos[i].vendorFaultCode,
                           (unsigned long long)info.pVendorInfos[i].vendorFaultData);
            }
        }
    }

    printf("VERDICT mode=%s rgba=%d : %s\n", mode, useRgba,
           (wr == VK_SUCCESS) ? "SURVIVED" : "LOST");
    return (wr == VK_SUCCESS) ? 0 : 1;
}
