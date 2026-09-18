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

/*
 * Per-frame RELEASE fence coverage, on a real device.
 *
 * WHAT IS UNDER TEST. VkVideoEncoderFrameFenceDescriptor::pReleaseFenceFd --
 * the export half of the per-frame acquire/release fence entry point. The
 * library appends a binary, SYNC_FD-exportable semaphore to the frame's
 * signal list so the submission that CONSUMES the input image signals it,
 * then exports a SYNC_FD from it once that submission has been issued.
 *
 * WHY THIS CANNOT BE A NULL-BACKEND TEST, unlike its sibling
 * encoder-ext-sync. The three things worth proving are all properties of a
 * real queue: that the fd is a live sync_fd (>= 0), that it is NOT already
 * signalled when it is handed over (which is what "exported from a PENDING
 * signal" means, and what a fd of -1 would have told us instead), and that it
 * becomes signalled once the GPU has read the input. A session with no device
 * can express none of them. So this one needs a GPU, and says so: with no
 * encode-capable device it exits 77, which CTest is configured to read as a
 * SKIP rather than a pass.
 *
 * THE LEAK ASSERTION IS THE POINT OF THE 300-FRAME LOOP. The exported fd is
 * the CALLER's to close -- the reverse of every other fd rule in this API,
 * all of which govern handles the library is GIVEN. A rule stated in a header
 * comment and not exercised is a rule that drifts, so this counts entries in
 * /proc/self/fd across the whole run: an over-retaining library shows up as
 * growth even though every fd this test itself receives is closed.
 */

#include "vulkan_video_encoder_ext.h"

#include "vk_video/vulkan_video_codec_h264std.h"

// The public header reaches the Xlib platform headers, whose macros collide
// with ordinary identifiers. Same scrub, same reason, as the sibling tests.
#undef Status
#undef None
#undef Bool
#undef Window

#include <dirent.h>
#include <dlfcn.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

void Check(bool ok, const char* what, const std::string& detail)
{
    g_checks++;
    if (ok) {
        return;
    }
    g_failures++;
    std::printf("  FAIL %s : %s\n", what, detail.c_str());
}

std::string I64(long long v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", v);
    return buf;
}

// Live entries in /proc/self/fd, minus the handle the scan itself holds open.
int OpenFdCount()
{
    DIR* d = opendir("/proc/self/fd");
    if (d == nullptr) {
        return -1;
    }
    int n = 0;
    while (struct dirent* e = readdir(d)) {
        if ((std::strcmp(e->d_name, ".") == 0) ||
            (std::strcmp(e->d_name, "..") == 0)) {
            continue;
        }
        n++;
    }
    closedir(d);
    return n - 1;
}

// poll() for readability. -1 timeout blocks; 0 polls. Returns 1 signalled,
// 0 not yet, <0 error. A sync_fd becomes readable when its fence signals.
int PollSignalled(int fd, int timeoutMs)
{
    struct pollfd p = {};
    p.fd     = fd;
    p.events = POLLIN;
    const int r = poll(&p, 1, timeoutMs);
    if (r < 0) {
        return -1;
    }
    if (r == 0) {
        return 0;
    }
    return ((p.revents & POLLIN) != 0) ? 1 : -1;
}

const uint32_t kWidth  = 1920;
const uint32_t kHeight = 1080;
const uint32_t kFrames = 300;

struct DeviceFns {
    PFN_vkCreateImage                      CreateImage = nullptr;
    PFN_vkDestroyImage                     DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements       GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory                   AllocateMemory = nullptr;
    PFN_vkFreeMemory                       FreeMemory = nullptr;
    PFN_vkBindImageMemory                  BindImageMemory = nullptr;
    PFN_vkMapMemory                        MapMemory = nullptr;
    PFN_vkUnmapMemory                      UnmapMemory = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties =
        nullptr;
    PFN_vkCreateSemaphore                  CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore                 DestroySemaphore = nullptr;
    PFN_vkGetSemaphoreCounterValue         GetSemaphoreCounterValue = nullptr;
};

bool LoadDeviceFns(VkInstance instance, VkDevice device, DeviceFns* fns)
{
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (lib == nullptr) {
        lib = dlopen("libvulkan.so", RTLD_NOW);
    }
    if (lib == nullptr) {
        std::printf("  ERROR: dlopen(libvulkan) failed: %s\n", dlerror());
        return false;
    }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (gipa == nullptr) {
        std::printf("  ERROR: no vkGetInstanceProcAddr\n");
        return false;
    }
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    if (gdpa == nullptr) {
        std::printf("  ERROR: no vkGetDeviceProcAddr\n");
        return false;
    }
#define LOAD_DEV(name)                                                       \
    fns->name = (PFN_vk##name)gdpa(device, "vk" #name);                      \
    if (fns->name == nullptr) {                                              \
        std::printf("  ERROR: missing vk" #name "\n");                       \
        return false;                                                        \
    }
    LOAD_DEV(CreateImage)
    LOAD_DEV(DestroyImage)
    LOAD_DEV(GetImageMemoryRequirements)
    LOAD_DEV(AllocateMemory)
    LOAD_DEV(FreeMemory)
    LOAD_DEV(BindImageMemory)
    LOAD_DEV(MapMemory)
    LOAD_DEV(UnmapMemory)
    LOAD_DEV(CreateSemaphore)
    LOAD_DEV(DestroySemaphore)
    LOAD_DEV(GetSemaphoreCounterValue)
#undef LOAD_DEV
    fns->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
            instance, "vkGetPhysicalDeviceMemoryProperties");
    return (fns->GetPhysicalDeviceMemoryProperties != nullptr);
}

// A host-written LINEAR NV12 image on the encoder's own device, registered as
// VK_IMAGE. Transfer-source usage only, so the registration routes STAGED --
// the staging copy is then the submission that reads the input, and therefore
// the one that must signal the release fence.
struct InputImage {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

bool CreateInputImage(const DeviceFns& fns, VkPhysicalDevice phys,
                      VkDevice device, InputImage* out, bool direct)
{
    // The direct arm needs an image the ENCODER can read: OPTIMAL tiling,
    // VIDEO_ENCODE_SRC usage, a profile list at create time, and therefore
    // device-local memory this test cannot host-fill. It encodes undefined
    // content, deliberately -- the subject here is WHEN the release fence
    // signals, not what the bitstream contains, and the staged arm already
    // covers real pixels.
    // THE CODEC-SPECIFIC PROFILE STRUCT IS PART OF THE PROFILE, not an
    // optional decoration on it. A VkVideoProfileInfoKHR naming an H.264
    // encode operation is only a complete profile once a
    // VkVideoEncodeH264ProfileInfoKHR is chained onto it
    // (VUID-VkVideoProfileInfoKHR-videoCodecOperation-07181). Without it the
    // profile list below describes no profile the session can be matched
    // against, so vkCreateImage is asked about an image no session can read
    // (VUID-VkImageCreateInfo-pNext-06811) and every encode that names the
    // resulting view is incompatible with the bound session
    // (VUID-vkCmdEncodeVideoKHR-pEncodeInfo-08206).
    //
    // HIGH because that is the profile the SESSION will use, not because it
    // is the richest one available. The config below leaves |profile| at
    // VK_VIDEO_ENCODER_PROFILE_DEFAULT, and the library derives profile_idc
    // 100 for 8-bit 4:2:0 input under the default adaptive-transform mode.
    // Naming a profile here that the session does not use is the same
    // mismatch as naming none.
    VkVideoEncodeH264ProfileInfoKHR h264Profile{
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
    h264Profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    VkVideoProfileInfoKHR profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    profile.pNext               = &h264Profile;
    profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    profile.chromaSubsampling   = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profile.lumaBitDepth        = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profile.chromaBitDepth      = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    VkVideoProfileListInfoKHR profileList{
        VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    profileList.profileCount = 1;
    profileList.pProfiles    = &profile;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext         = direct ? (const void*)&profileList : nullptr;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ci.extent        = {kWidth, kHeight, 1};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = direct ? VK_IMAGE_TILING_OPTIMAL
                              : VK_IMAGE_TILING_LINEAR;
    ci.usage         = direct
                           ? (VkImageUsageFlags)(
                                 VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                           : (VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = direct ? VK_IMAGE_LAYOUT_UNDEFINED
                              : VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (fns.CreateImage(device, &ci, nullptr, &out->image) != VK_SUCCESS) {
        std::printf("  ERROR: vkCreateImage(LINEAR NV12) failed\n");
        return false;
    }

    VkMemoryRequirements req{};
    fns.GetImageMemoryRequirements(device, out->image, &req);

    VkPhysicalDeviceMemoryProperties memProps{};
    fns.GetPhysicalDeviceMemoryProperties(phys, &memProps);
    uint32_t typeIndex = UINT32_MAX;
    const VkMemoryPropertyFlags want =
        direct ? (VkMemoryPropertyFlags)VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
               : (VkMemoryPropertyFlags)(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (((req.memoryTypeBits & (1u << i)) != 0) &&
            ((memProps.memoryTypes[i].propertyFlags & want) == want)) {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == UINT32_MAX) {
        std::printf("  ERROR: no host-visible memory type for a linear NV12 "
                    "image\n");
        return false;
    }

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = typeIndex;
    if (fns.AllocateMemory(device, &ai, nullptr, &out->memory) != VK_SUCCESS) {
        std::printf("  ERROR: vkAllocateMemory failed\n");
        return false;
    }
    if (fns.BindImageMemory(device, out->image, out->memory, 0) != VK_SUCCESS) {
        std::printf("  ERROR: vkBindImageMemory failed\n");
        return false;
    }

    // Real content, not zeros: a flat surface encodes to a degenerate
    // bitstream and would make "the encode actually ran" hard to assert.
    if (direct) {
        return true;  // device-local: nothing to map
    }
    void* mapped = nullptr;
    if (fns.MapMemory(device, out->memory, 0, req.size, 0, &mapped) ==
        VK_SUCCESS) {
        uint8_t* bytes = (uint8_t*)mapped;
        for (VkDeviceSize i = 0; i < req.size; i++) {
            bytes[i] = (uint8_t)((i * 7u) ^ (i >> 9));
        }
        fns.UnmapMemory(device, out->memory);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    // Which submission ends up consuming the input image is a REGISTRATION-
    // time routing decision, and the two answers exercise different halves of
    // the library's "was the input-consuming submit issued?" test:
    //
    //   staged  (default)  transfer-source usage only -> the staging copy
    //                      reads the input, and its submit is issued inline
    //                      from StageInputFrame; the check reads
    //                      inputCmdBuffer.
    //   direct             VIDEO_ENCODE_SRC usage -> vkCmdEncodeVideoKHR
    //                      reads the input directly and there is no staging
    //                      copy at all, so the check has to fall through to
    //                      encodeCmdBuffer->IsCommandBufferSubmitted().
    //
    // Both are run, as two CTest cases, because a release fence that works
    // only on the staged arm is a release fence that silently answers -1 for
    // every zero-copy producer -- which is the arm this API exists for.
    const bool direct = (argc > 1) && (std::strcmp(argv[1], "direct") == 0);
    // A THIRD routing, and this one is about the pNext CHAIN rather than the
    // queue: "chained" submits the same staged frame but hangs the fence
    // descriptor off a VkVideoEncoderFrameSyncDescriptor's pNext instead of
    // off the submit info directly. The header documents a flat,
    // order-independent chain, so a fence honoured only in the leading
    // position would make that documentation false -- and a node the sType
    // gate accepts but the walk never reads is dead code that looks wired.
    // Only a real export can tell those apart: fd >= 0, not yet signalled at
    // handover, signalled afterwards.
    const bool chained = (argc > 1) && (std::strcmp(argv[1], "chained") == 0);
    std::printf("Encoder-ext per-frame release fence (real device, %s)\n",
                direct ? "DIRECT encode input"
                       : (chained
                              ? "STAGED input, fence chained BEHIND a sync "
                                "descriptor"
                              : "STAGED input"));
    std::printf("------------------------------------------------\n");

    VkSharedBaseObj<VulkanVideoEncoderExt> encoder;
    if ((CreateVulkanVideoEncoderExt(encoder) != VK_SUCCESS) || !encoder) {
        std::printf("SKIP: CreateVulkanVideoEncoderExt failed\n");
        return 77;
    }

    VkVideoEncoderConfig config = {};
    config.sType              = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
    config.codec              = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    config.encodeWidth        = kWidth;
    config.encodeHeight       = kHeight;
    config.inputFormat        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    config.inputWidth         = kWidth;
    config.inputHeight        = kHeight;
    config.rateControlMode    = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    config.averageBitrate     = 5000000;
    config.maxBitrate         = 5000000;
    config.gopLength          = 30;
    // No reordering: the direct-encode submit is then issued inline with the
    // frame that produced it, which keeps this test's expectations about WHEN
    // the fd exists free of GOP scheduling.
    config.consecutiveBFrames = 0;
    config.idrPeriod          = 30;
    config.frameRateNum       = 30;
    config.frameRateDen       = 1;
    // -1 is auto-select. 0 is NOT a default here -- it names physical device
    // index 0, which on a multi-ICD host is whatever enumerated first.
    config.deviceId           = -1;
    // In-memory capture. Left FALSE the library writes out.264 and delivers
    // EMPTY VkVideoEncodeResult records -- a real encode with nothing for
    // this test to weigh, which is how a first run of this file mistook a
    // working 6.4 MB bitstream for a broken one.
    config.disableFileOutput  = VK_TRUE;

    if (encoder->InitializeExt(config) != VK_SUCCESS) {
        std::printf("SKIP: InitializeExt failed -- no encode-capable Vulkan "
                    "device on this host\n");
        return 77;
    }

    VkInstance       instance = encoder->GetVkInstance();
    VkDevice         device   = encoder->GetVkDevice();
    VkPhysicalDevice phys     = encoder->GetVkPhysicalDevice();
    DeviceFns fns;
    if (!LoadDeviceFns(instance, device, &fns)) {
        std::printf("SKIP: could not load the Vulkan entry points this test "
                    "needs\n");
        return 77;
    }

    InputImage input;
    if (!CreateInputImage(fns, phys, device, &input, direct)) {
        std::printf("SKIP: could not create the input image\n");
        return 77;
    }

    VkVideoEncoderExternalImageDescriptor desc = {};
    desc.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
    desc.handleType    = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
    desc.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    desc.width         = kWidth;
    desc.height        = kHeight;
    desc.tiling        = direct ? VK_IMAGE_TILING_OPTIMAL
                                : VK_IMAGE_TILING_LINEAR;
    // Declaring VIDEO_ENCODE_SRC is what routes the registration DIRECT: the
    // library never grants access the caller did not declare, so leaving this
    // at transfer-source is also how the staged arm gets selected.
    desc.imageUsage    = direct
                             ? (VkImageUsageFlags)(
                                   VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                             : (VkImageUsageFlags)
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    desc.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    // Zero for a VK_IMAGE registration: the library did not perform the
    // import, so it has no plane layouts to validate, and two declared planes
    // both at offset 0 is exactly the shape it refuses as un-allocatable.
    desc.planeCount    = 0;
    // A reused, locally allocated, host-written image: declaring FOREIGN here
    // would make the staging copy take a queue-family acquire with no
    // matching release.
    desc.residency     = VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
    desc.defaultLayout = direct ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR
                                : VK_IMAGE_LAYOUT_PREINITIALIZED;
    desc.existingImage = input.image;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode regStatus =
        encoder->RegisterImageResource(desc, 0, &resource, nullptr);
    if ((regStatus != VK_VIDEO_ENCODER_STATUS_SUCCESS) ||
        (resource == VK_VIDEO_ENCODER_RESOURCE_NULL)) {
        std::printf("SKIP: RegisterImageResource failed, status %d\n",
                    (int)regStatus);
        return 77;
    }

    // THE CALLER'S OWN SIGNAL SEMAPHORE -- the half of this entry point that
    // has to be exercised against a real queue to mean anything.
    //
    // The library appends its release-fence semaphore to whatever signal list
    // the frame ended up with, reading frame.pSignalSemaphores to copy the
    // caller's entries across first. Every existing check on that block was a
    // null-backend one (encoder-ext-sync drives the arm that returns before
    // SetExternalInputFrame*), and this test supplied no signal semaphores at
    // all -- so a clamp, a reorder or a dropped entry in the append would have
    // turned nothing red anywhere in the tree, which is exactly the shape of
    // the signal-zeroing defect this API's tests exist to catch.
    //
    // TIMELINE, because a timeline signal is observable from the host with no
    // queue of our own (vkGetSemaphoreCounterValue) and is harmless left
    // unwaited, whereas a binary semaphore signalled and never waited leaves
    // the queue in an invalid state at teardown.
    VkSemaphoreTypeCreateInfo semType{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    semType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semType.initialValue  = 0;
    VkSemaphoreCreateInfo semCi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semCi.pNext = &semType;
    VkSemaphore callerSignal = VK_NULL_HANDLE;
    if (fns.CreateSemaphore(device, &semCi, nullptr, &callerSignal) !=
        VK_SUCCESS) {
        std::printf("SKIP: could not create a timeline semaphore\n");
        return 77;
    }

    // Baseline AFTER every one-time allocation, so the delta below is the
    // per-frame behaviour and nothing else.
    const int fdBefore = OpenFdCount();
    std::printf("  /proc/self/fd before the loop: %d\n", fdBefore);

    uint32_t submitted        = 0;
    uint32_t fencesReceived   = 0;
    uint32_t fencesUnsignalledAtHandover = 0;
    uint32_t fencesSignalled  = 0;
    uint32_t capturedFrames   = 0;
    uint64_t capturedBytes    = 0;
    int      firstBadFd       = 0;
    bool     pollFailed       = false;

    uint64_t lastSignalValue = 0;
    for (uint32_t f = 0; f < kFrames; f++) {
        int releaseFenceFd = 0x5EED;  // must be overwritten by the library
        // Monotonic, one step per frame, so the final counter value names
        // exactly how many frames' signal lists survived the walk.
        const uint64_t callerSignalValue = (uint64_t)f + 1;

        VkVideoEncoderFrameFenceDescriptor fence;
        fence.acquireFenceFd  = -1;
        fence.pReleaseFenceFd = &releaseFenceFd;

        // Names NEITHER direction, so it changes no sync decision and cannot
        // account for any difference in the numbers below. Its only job is to
        // push the fence descriptor into the TRAILING chain position.
        VkVideoEncoderFrameSyncDescriptor sync;
        sync.pNext = &fence;

        VkVideoEncoderFrameSubmitInfo info = {};
        info.sType         = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS;
        info.pNext         = chained ? (const void*)&sync : (const void*)&fence;
        info.resource      = resource;
        info.frameId       = f;
        info.pts           = f;
        info.qpOverride    = -1;
        // UNDEFINED means "as declared at registration", i.e. PREINITIALIZED
        // on every frame. That is the reused host-written LINEAR staging
        // image shape the library's PREINITIALIZED -> TRANSFER_SRC_OPTIMAL
        // barrier exists for, and the same shape Chromium's shmem staging
        // path submits.
        info.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.signalSemaphoreCount   = 1;
        info.pSignalSemaphores      = &callerSignal;
        info.pSignalSemaphoreValues = &callerSignalValue;

        VkVideoEncoderStatusCode status =
            encoder->SubmitRegisteredFrame(info, nullptr);

        // Admission-control backpressure is transient by contract: drain and
        // retry the SAME call.
        for (int retry = 0;
             (status == VK_VIDEO_ENCODER_STATUS_NOT_READY) && (retry < 2000);
             retry++) {
            VkVideoEncodeResult drained;
            while (encoder->AcquireNextEncodedFrame(drained) == VK_SUCCESS) {
                capturedFrames++;
                capturedBytes += drained.bitstreamSize;
                if (capturedFrames <= 3) {
                    std::printf("  [diag] frame %llu status=%d size=%u ptr=%p\n",
                                (unsigned long long)drained.frameId,
                                (int)drained.status, drained.bitstreamSize,
                                (const void*)drained.pBitstreamData);
                }
                encoder->ReleaseEncodedFrame(drained.frameId);
            }
            status = encoder->SubmitRegisteredFrame(info, nullptr);
            if (status == VK_VIDEO_ENCODER_STATUS_NOT_READY) {
                // BACK OFF. The drain above returns instantly when nothing has
                // completed yet, so an unpaced retry burns all 2000 attempts
                // in well under the time one 1080p frame takes to encode. The
                // real build never noticed, because its per-frame
                // PollSignalled(fd, 5000) blocks until the release fence
                // signals and paces the loop for free -- which meant the
                // pacing lived in the very thing a negative control removes.
                // A control that dies of its own retry budget at frame 10 of
                // 300 cannot tell -1-always from -1-for-ten-frames, which is
                // the only thing it exists to tell apart.
                struct timespec ts = {0, 1000000};  // 1 ms
                nanosleep(&ts, nullptr);
            }
        }
        if (status != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            Check(false, "SubmitRegisteredFrame",
                  "frame " + I64(f) + " status " + I64((long long)status));
            break;
        }
        submitted++;
        lastSignalValue = callerSignalValue;

        // A BRANCH, not a `continue`. The per-iteration drain below is what
        // keeps admission control fed, and skipping it starved the loop: a
        // build with the export disabled -- the negative control this test's
        // discriminating power rests on -- died at frame 10 of 300 with
        // VK_VIDEO_ENCODER_STATUS_NOT_READY instead of demonstrating -1 across
        // the whole run. A control that stops at 3% cannot tell "answers -1
        // always" apart from "answers -1 for ten frames", which is the only
        // thing it was there to tell apart.
        if (releaseFenceFd < 0) {
            if (firstBadFd == 0) {
                firstBadFd = (int)f + 1;
            }
        } else {
            Check(releaseFenceFd != 0x5EED, "pReleaseFenceFd was written",
                  "library left the caller's sentinel in place");
            fencesReceived++;

            // Not-yet-signalled at handover is the strong form of "exported
            // from a PENDING signal, not from an already-signalled
            // semaphore". It is inherently a race with the GPU, so it is
            // COUNTED rather than asserted per frame; the assertion is a
            // floor on the count.
            if (PollSignalled(releaseFenceFd, 0) == 0) {
                fencesUnsignalledAtHandover++;
            }

            const int signalled = PollSignalled(releaseFenceFd, 5000);
            if (signalled == 1) {
                fencesSignalled++;
            } else {
                pollFailed = true;
                Check(false, "release fence signalled",
                      "frame " + I64(f) + " poll returned " + I64(signalled));
            }

            // The exported fd is the CALLER's. Closing it here is not
            // tidiness, it is the contract the header states, and the
            // fd-count assertion below is what keeps that statement honest.
            close(releaseFenceFd);
        }

        VkVideoEncodeResult result;
        while (encoder->AcquireNextEncodedFrame(result) == VK_SUCCESS) {
            capturedFrames++;
            capturedBytes += result.bitstreamSize;
            if (capturedFrames <= 3) {
                std::printf("  [diag] frame %llu status=%d size=%u ptr=%p\n",
                            (unsigned long long)result.frameId,
                            (int)result.status, result.bitstreamSize,
                            (const void*)result.pBitstreamData);
            }
            encoder->ReleaseEncodedFrame(result.frameId);
        }
        if (pollFailed) {
            break;
        }
    }

    encoder->DrainPendingFrames();
    VkVideoEncodeResult tail;
    while (encoder->AcquireNextEncodedFrame(tail) == VK_SUCCESS) {
        capturedFrames++;
        capturedBytes += tail.bitstreamSize;
        encoder->ReleaseEncodedFrame(tail.frameId);
    }

    uint64_t callerSignalCounter = 0;
    if (fns.GetSemaphoreCounterValue(device, callerSignal,
                                     &callerSignalCounter) != VK_SUCCESS) {
        callerSignalCounter = UINT64_MAX;  // reported, never silently passed
    }

    const int fdAfter = OpenFdCount();
    std::printf("  /proc/self/fd after  the loop: %d\n", fdAfter);
    std::printf("  submitted=%u  fences>=0=%u  signalled=%u  "
                "unsignalled-at-handover=%u\n",
                submitted, fencesReceived, fencesSignalled,
                fencesUnsignalledAtHandover);
    std::printf("  unsignalled-at-handover ratio: %u/%u (%.1f%%)\n",
                fencesUnsignalledAtHandover, fencesReceived,
                (fencesReceived != 0)
                    ? (100.0 * (double)fencesUnsignalledAtHandover /
                       (double)fencesReceived)
                    : 0.0);
    std::printf("  caller signal timeline: value=%llu, last requested=%llu\n",
                (unsigned long long)callerSignalCounter,
                (unsigned long long)lastSignalValue);
    std::printf("  captured frames=%u  bitstream bytes=%llu\n",
                capturedFrames, (unsigned long long)capturedBytes);
    if (firstBadFd != 0) {
        std::printf("  first frame answering -1: %d\n", firstBadFd - 1);
    }

    Check(submitted == kFrames, "all frames submitted",
          I64(submitted) + " of " + I64(kFrames));
    Check(fencesReceived == kFrames, "every frame returned a release fd >= 0",
          I64(fencesReceived) + " of " + I64(kFrames));
    Check(fencesSignalled == fencesReceived,
          "every release fence became signalled",
          I64(fencesSignalled) + " of " + I64(fencesReceived));
    // A FLOOR, not "> 0". The count exists to rule out "exported after the
    // fact" -- an export moved behind a fence wait would come back already
    // signalled on nearly every frame, and "> 0" passes that as long as ONE
    // frame races ahead, i.e. it passes the exact state it was written to
    // forbid. Half the run is far below what the property actually produces
    // (298/300 staged, 299/300 direct, as measured) and far above what a
    // regressed export could reach.
    Check(fencesUnsignalledAtHandover >= (kFrames / 2),
          "most fences were still unsignalled at handover",
          I64(fencesUnsignalledAtHandover) + " of " + I64(fencesReceived) +
              ", floor " + I64(kFrames / 2) +
              " -- an export that did not ride a pending submit comes back "
              "already signalled");
    // The caller's raw signal array reached the REAL queue and was not
    // clamped, reordered or dropped by the release-fence append. Zero here
    // means the library submitted without it: the silent-hang defect.
    Check(callerSignalCounter == lastSignalValue,
          "the caller's signal semaphore was signalled on every frame",
          "timeline at " + I64((long long)callerSignalCounter) +
              ", last requested " + I64((long long)lastSignalValue));
    Check(capturedFrames > 0, "the encode actually produced frames",
          I64(capturedFrames) + " captured");
    Check(capturedBytes > 0, "the encode actually produced a bitstream",
          I64((long long)capturedBytes) + " bytes");
    Check((fdBefore >= 0) && (fdAfter >= 0) && (fdAfter <= fdBefore),
          "no fd growth across the run",
          "before " + I64(fdBefore) + ", after " + I64(fdAfter));

    // Order matters and the header states it: unregister BEFORE destroying
    // the image, and destroy the image BEFORE releasing the encoder -- the
    // VkDevice these objects live on is the encoder's, and it goes away with
    // it.
    encoder->UnregisterImageResource(resource);
    fns.DestroySemaphore(device, callerSignal, nullptr);
    fns.DestroyImage(device, input.image, nullptr);
    fns.FreeMemory(device, input.memory, nullptr);
    encoder = nullptr;

    std::printf("------------------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
