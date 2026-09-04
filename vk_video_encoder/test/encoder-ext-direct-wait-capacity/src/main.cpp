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
 * DIRECT-path WAIT-ARRAY CAPACITY, on a real device.
 *
 * WHAT IS UNDER TEST. VkVideoEncoder::SubmitVideoCodingCmds assembles the
 * direct-encode submit's waits into a FIXED 8-slot stack array
 * (VkVideoEncoder.cpp: `const uint32_t waitSemaphoreMaxCount = 8;`), and the
 * loop that injects the caller's external waits is bounded by
 * `waitSemaphoreCount < waitSemaphoreMaxCount`. There is no overflow -- and
 * there was no diagnostic either: surplus waits were DISCARDED, silently, and
 * the submit went to the queue as though they had never been named.
 *
 * WHY THAT IS A CORRECTNESS DEFECT AND NOT A CAPACITY LIMIT. A wait names a
 * producer that has not finished writing the input image. Dropping one does
 * not degrade the encode, it makes vkCmdEncodeVideoKHR read a surface while
 * the producer is still writing it -- a read-before-write race, intermittent
 * by nature, with a SUCCESS status and a valid release fence handed back to a
 * caller who has been given no way to find out. The library already says this
 * about the other direction, in its own words, at
 * vulkan_video_encoder_ext.cpp: the release fence is gated by
 * `kMaxCallerSignalsForReleaseFence = 4` precisely because "the direct-encode
 * submit ... silently stops appending when it fills", and a dropped signal
 * would be "the hang this fence exists to prevent, caused by the fence". The
 * wait side carried the same 8-slot hazard with no gate at all.
 *
 * WHY THE ACQUIRE FENCE IS THE FIRST CASUALTY. The per-frame acquire fence is
 * imported and APPENDED to whichever wait array the frame ended up using
 * (vulkan_video_encoder_ext.cpp, the `waitWithAcquire` block), unconditionally
 * on count. It is therefore the LAST entry of the merged array, so it is the
 * first thing truncation reaches. A caller that supplied 8 waits and an armed
 * acquireFenceFd got a 9-entry array whose 9th entry -- the producer fence the
 * whole handle API exists to honour -- was dropped, while the public header
 * promises "Supplying an acquireFenceFd and a pWaitSemaphores array together
 * is legal and loses neither".
 *
 * WHY THE EXISTING SUITE COULD NOT SEE IT. EncoderExtAcquireFdKeepsCallerWaitArray
 * asserts that same header promise, and holds -- but it registers a
 * VK_IMAGE_TILING_LINEAR, TRANSFER_SRC-only image, which sets encodeCapable
 * false and routes STAGED. The staged submit assembles its waits into a
 * std::vector (VkVideoEncoder.cpp, SubmitStagedInputFrame) that never
 * truncates, and it exercises ONE caller wait. Neither the path nor the count
 * that the defect needs is reachable from it. That is why this file exists.
 *
 * HOW EACH CASE IS MADE TO FAIL WHEN THE LIBRARY IS BROKEN. Every wait this
 * test supplies is a TIMELINE semaphore. All but one are host-signalled to
 * their target value BEFORE the submit, so they cannot hold the frame back.
 * Exactly one is left at 0. The frame therefore has precisely one reason not
 * to run, and the release fence fd is the readout: poll() it.
 *
 *   - honoured  -> the frame is parked, the fence has not signalled, poll 0.
 *   - discarded -> nothing is holding the frame, it encodes, poll 1.
 *
 * A poll of 1 while the semaphore it was told to wait on is still at 0 IS the
 * race, observed. It is not a proxy for it.
 *
 * THE CONTROL ARM IS PART OF THE EVIDENCE. --staged runs the identical
 * 9-wait case against a LINEAR/TRANSFER_SRC registration, which routes to the
 * non-truncating vector. It must report poll 0 -- the same apparatus, the same
 * counts, the same semaphores, reading a wait that WAS honoured. Without it a
 * poll of 0 on the direct arm would be indistinguishable from a test that
 * cannot fail, and there is no way to assert the routing directly: no public
 * or seam accessor reports a slot's resolved inputPath.
 */

#include "vulkan_video_encoder_ext.h"

#include "vk_video/vulkan_video_codec_h264std.h"

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

const uint32_t kWidth  = 1920;
const uint32_t kHeight = 1080;

// The library's own capacity, restated here so the arms below read as
// intentions rather than as magic numbers. Kept as a literal on purpose: if
// VkVideoEncoder.cpp ever raises its array, this test must be re-derived
// deliberately, not silently follow along and stop testing the boundary.
const uint32_t kDirectWaitCapacity = 8;

int  g_failures = 0;
int  g_checks   = 0;
const char* g_case = "";

void Check(bool ok, const char* what, const std::string& detail)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("  FAIL [%s] %s\n        %s\n", g_case, what,
                    detail.c_str());
    } else {
        std::printf("  ok   [%s] %s\n", g_case, what);
    }
}

std::string I64(long long v) { return std::to_string(v); }

// poll() for readability. A sync_fd becomes readable when its fence signals.
// Returns 1 signalled, 0 not yet, <0 error.
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

struct DeviceFns {
    PFN_vkCreateImage                       CreateImage = nullptr;
    PFN_vkDestroyImage                      DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements        GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory                    AllocateMemory = nullptr;
    PFN_vkFreeMemory                        FreeMemory = nullptr;
    PFN_vkBindImageMemory                   BindImageMemory = nullptr;
    PFN_vkMapMemory                         MapMemory = nullptr;
    PFN_vkUnmapMemory                       UnmapMemory = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties =
        nullptr;
    PFN_vkGetPhysicalDeviceProperties       GetPhysicalDeviceProperties = nullptr;
    PFN_vkCreateSemaphore                   CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore                  DestroySemaphore = nullptr;
    PFN_vkSignalSemaphore                   SignalSemaphore = nullptr;
    PFN_vkGetSemaphoreCounterValue          GetSemaphoreCounterValue = nullptr;
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
    LOAD_DEV(SignalSemaphore)
    LOAD_DEV(GetSemaphoreCounterValue)
#undef LOAD_DEV
    fns->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
            instance, "vkGetPhysicalDeviceMemoryProperties");
    fns->GetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)gipa(
            instance, "vkGetPhysicalDeviceProperties");
    return (fns->GetPhysicalDeviceMemoryProperties != nullptr) &&
           (fns->GetPhysicalDeviceProperties != nullptr);
}

struct InputImage {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

// |direct| selects the arm. OPTIMAL + VIDEO_ENCODE_SRC + a profile list at
// create time is what the encoder can read without a staging copy, and is
// therefore what makes the registration below encodeCapable; LINEAR +
// TRANSFER_SRC is the control that routes STAGED. Copied in shape from the
// sibling release-fence test, which measures the same two routings.
bool CreateInputImage(const DeviceFns& fns, VkPhysicalDevice phys,
                      VkDevice device, InputImage* out, bool direct)
{
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
    // WHAT THE LIBRARY WILL DO TO THIS IMAGE, DECLARED AT CREATION.
    //
    // A frame that routes DIRECT is viewed PER PLANE -- R8_UNORM over the
    // luma, R8G8_UNORM over the chroma of an NV12 image -- and a per-plane
    // view of a multi-planar image requires MUTABLE_FORMAT
    // (VUID-VkImageViewCreateInfo-image-01762).
    //
    // Without it this image is one the encoder cannot legally view, and the
    // arms below would be measuring that rather than wait capacity. The
    // staged control needs none of it: it is copied, not viewed per plane.
    ci.flags         = direct
                           ? (VkImageCreateFlags)(
                                 VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
                           : (VkImageCreateFlags)0;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ci.extent        = {kWidth, kHeight, 1};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = direct ? VK_IMAGE_TILING_OPTIMAL
                              : VK_IMAGE_TILING_LINEAR;
    // THE USAGE THE VIEWS WILL NAME, not the narrowest one an encode source
    // could get away with. A view may not name a usage bit the image was not
    // created with (VUID-VkImageViewCreateInfo-pNext-02662), and the wrap the
    // direct path performs builds its views over the full set the encoder can
    // put an input image to -- the transfer pair for a staged copy and the
    // sampled/storage pair for the preprocess filter -- whichever rung this
    // particular frame ends up on. So a producer handing an image to this
    // path declares all of them, and this test declares what a producer
    // would. The staged control declares only what its copy reads.
    ci.usage         = direct
                           ? (VkImageUsageFlags)(
                                 VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT)
                           : (VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = direct ? VK_IMAGE_LAYOUT_UNDEFINED
                              : VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (fns.CreateImage(device, &ci, nullptr, &out->image) != VK_SUCCESS) {
        std::printf("  ERROR: vkCreateImage(NV12, direct=%d) failed\n",
                    (int)direct);
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
        std::printf("  ERROR: no suitable memory type\n");
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
    if (!direct) {
        void* mapped = nullptr;
        if (fns.MapMemory(device, out->memory, 0, req.size, 0, &mapped) ==
            VK_SUCCESS) {
            uint8_t* bytes = (uint8_t*)mapped;
            for (VkDeviceSize i = 0; i < req.size; i++) {
                bytes[i] = (uint8_t)((i * 7u) ^ (i >> 9));
            }
            fns.UnmapMemory(device, out->memory);
        }
    }
    return true;
}

//=============================================================================
// The session under test.
//=============================================================================
VkSharedBaseObj<VulkanVideoEncoderExt> g_encoder;
DeviceFns                              g_fns;
VkDevice                               g_device   = VK_NULL_HANDLE;
VkVideoEncoderResource                 g_resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
uint64_t                               g_frameId  = 0;

// WHAT THIS SESSION ACTUALLY CODED, indexed by VkVideoEncoderPictureType.
// Counted at EVERY acquisition site, because a frame is delivered once and
// the sites are not interchangeable: RunLegacyWaitCase drains inside its own
// bounded wait and DrainCaptures() takes whatever is left.
uint32_t g_pictureTypeCount[3] = {0, 0, 0};

void CountPictureType(const VkVideoEncodeResult& r)
{
    if ((uint32_t)r.pictureType < 3) {
        g_pictureTypeCount[(uint32_t)r.pictureType]++;
    }
}

void DrainCaptures()
{
    VkVideoEncodeResult r;
    while (g_encoder->AcquireNextEncodedFrame(r) == VK_SUCCESS) {
        CountPictureType(r);
        g_encoder->ReleaseEncodedFrame(r.frameId);
    }
}

VkVideoEncoderFrameSubmitInfo BaseInfo()
{
    VkVideoEncoderFrameSubmitInfo info = {};
    info.sType         = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS;
    info.resource      = g_resource;
    info.frameId       = g_frameId;
    info.pts           = g_frameId;
    info.qpOverride    = -1;
    info.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

// A bank of TIMELINE semaphores. |signalledCount| of them are host-signalled
// to kTargetValue before the caller uses them, so they are already satisfied;
// the remainder stay at 0 and are the only thing that can hold a frame back.
const uint64_t kTargetValue = 1;

struct WaitBank {
    std::vector<VkSemaphore> semaphores;
    std::vector<uint64_t>    values;

    bool Create(uint32_t count, uint32_t signalledCount)
    {
        VkSemaphoreTypeCreateInfo typeInfo{
            VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeInfo.initialValue  = 0;
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semInfo.pNext = &typeInfo;

        for (uint32_t i = 0; i < count; i++) {
            VkSemaphore s = VK_NULL_HANDLE;
            if (g_fns.CreateSemaphore(g_device, &semInfo, nullptr, &s) !=
                VK_SUCCESS) {
                return false;
            }
            semaphores.push_back(s);
            values.push_back(kTargetValue);
        }
        for (uint32_t i = 0; i < signalledCount; i++) {
            VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            si.semaphore = semaphores[i];
            si.value     = kTargetValue;
            if (g_fns.SignalSemaphore(g_device, &si) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    // Release whatever is still holding a frame back, so the parked submit can
    // retire and the semaphores become destroyable.
    void SignalRest(uint32_t from)
    {
        for (uint32_t i = from; i < semaphores.size(); i++) {
            VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            si.semaphore = semaphores[i];
            si.value     = kTargetValue;
            g_fns.SignalSemaphore(g_device, &si);
        }
    }

    void Destroy()
    {
        for (VkSemaphore s : semaphores) {
            g_fns.DestroySemaphore(g_device, s, nullptr);
        }
        semaphores.clear();
        values.clear();
    }
};

// One already-signalled real sync_fd, exported by the library from a frame of
// its own. Carries no acquire fence itself, so a retry here can never lose a
// handle.
int MintSyncFd()
{
    for (int attempt = 0; attempt < 64; attempt++) {
        int fd = -1;
        VkVideoEncoderFrameFenceDescriptor fence;
        fence.acquireFenceFd  = -1;
        fence.pReleaseFenceFd = &fd;

        VkVideoEncoderFrameSubmitInfo info = BaseInfo();
        info.pNext = &fence;

        VkVideoEncoderStatusCode status =
            g_encoder->SubmitRegisteredFrame(info, nullptr);
        for (int retry = 0;
             (status == VK_VIDEO_ENCODER_STATUS_NOT_READY) && (retry < 2000);
             retry++) {
            DrainCaptures();
            status = g_encoder->SubmitRegisteredFrame(info, nullptr);
            if (status == VK_VIDEO_ENCODER_STATUS_NOT_READY) {
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, nullptr);
            }
        }
        if (status != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            std::printf("  MINT: submit failed, status %d\n", (int)status);
            return -1;
        }
        g_frameId++;
        DrainCaptures();
        if (fd >= 0) {
            // Wait for it, so the arm that uses it as an acquire fence is
            // supplying a fence that is already satisfied and therefore adds
            // no reason of its own for the frame not to run.
            PollSignalled(fd, 10000);
            return fd;
        }
        std::printf("  MINT: frame %llu answered -1, retrying\n",
                    (unsigned long long)(g_frameId - 1));
    }
    return -1;
}

//=============================================================================
// The measurement.
//
// |callerWaits| timeline waits, of which all but the LAST are already
// satisfied. Optionally an already-signalled acquire fd on top. Returns
// through its out-params what the library did.
//=============================================================================
struct WaitCaseResult {
    VkVideoEncoderStatusCode status = VK_VIDEO_ENCODER_STATUS_SUCCESS;
    int  releaseFd  = -1;
    int  earlyPoll  = -1;
    int  latePoll   = -1;
    bool submitted  = false;
};

WaitCaseResult RunWaitCase(uint32_t callerWaits, bool withAcquireFd)
{
    WaitCaseResult out;

    WaitBank bank;
    if (!bank.Create(callerWaits, callerWaits - 1)) {
        Check(false, "the timeline semaphore bank could be created",
              "vkCreateSemaphore/vkSignalSemaphore failed");
        bank.Destroy();
        return out;
    }

    int acquireFd = -1;
    if (withAcquireFd) {
        acquireFd = MintSyncFd();
        if (acquireFd < 0) {
            Check(false, "an acquire sync_fd could be minted",
                  "MintSyncFd returned -1");
            bank.Destroy();
            return out;
        }
    }

    int releaseFd = -1;
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = acquireFd;
    fence.pReleaseFenceFd = &releaseFd;

    VkVideoEncoderFrameSubmitInfo info = BaseInfo();
    info.pNext                = &fence;
    info.waitSemaphoreCount   = (uint32_t)bank.semaphores.size();
    info.pWaitSemaphores      = bank.semaphores.data();
    info.pWaitSemaphoreValues = bank.values.data();

    VkVideoEncoderStatusCode status =
        g_encoder->SubmitRegisteredFrame(info, nullptr);
    for (int retry = 0;
         (status == VK_VIDEO_ENCODER_STATUS_NOT_READY) && (retry < 2000);
         retry++) {
        DrainCaptures();
        status = g_encoder->SubmitRegisteredFrame(info, nullptr);
        if (status == VK_VIDEO_ENCODER_STATUS_NOT_READY) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, nullptr);
        }
    }
    out.status = status;
    out.submitted = (status == VK_VIDEO_ENCODER_STATUS_SUCCESS);

    if (out.submitted) {
        g_frameId++;
        out.releaseFd = releaseFd;
        if (releaseFd >= 0) {
            // THE READOUT. One wait is still at 0. A fence that has already
            // signalled means the submit did not wait for it.
            out.earlyPoll = PollSignalled(releaseFd, 300);
        }
        // Let the frame go, whatever the verdict, so teardown is clean.
        bank.SignalRest(0);
        if (releaseFd >= 0) {
            out.latePoll = PollSignalled(releaseFd, 10000);
            ::close(releaseFd);
        }
    } else if (releaseFd >= 0) {
        ::close(releaseFd);
    }

    DrainCaptures();
    g_encoder->DrainPendingFrames();
    DrainCaptures();
    bank.Destroy();
    return out;
}

//=============================================================================
// BASELINE -- A WAIT ARRAY WELL WITHIN CAPACITY, ON THE DIRECT PATH ITSELF.
//
// Three caller waits, the last unsignalled, with and without an armed acquire
// fence. Both must park the frame.
//
// This is not decoration and it is not a smoke test. Without it, the surplus
// cases below are uninterpretable: "the 9th wait was discarded because it did
// not fit" and "this path never honours a caller wait at all" produce the
// IDENTICAL reading (submit accepted, release fence already signalled), and
// only this arm tells them apart. The staged control answers a different
// question -- whether the apparatus works -- and cannot substitute, because it
// exercises the other assembly entirely.
//=============================================================================
void CaseDirectSmallWaitArrayIsHonoured(bool withAcquire)
{
    g_case = withAcquire ? "DirectBaselineFewWaitsPlusAcquire"
                         : "DirectBaselineFewWaits";

    const uint32_t waits = 3;   // +1 acquire at most = 4, half the capacity
    const WaitCaseResult r = RunWaitCase(waits, withAcquire);

    std::printf("    [%s] waits=%u acquire=%d status=%d releaseFd=%d "
                "earlyPoll=%d latePoll=%d\n",
                g_case, waits, (int)withAcquire, (int)r.status, r.releaseFd,
                r.earlyPoll, r.latePoll);

    Check(r.submitted, "the submit was accepted",
          "status " + I64((long long)r.status));

    // The early poll GATES only on the arm without an acquire fence, and that
    // is a limitation of this harness rather than a statement about the
    // library. The acquire arm has to mint its sync_fd from a real frame of
    // its own (MintSyncFd), and that frame consumes a release fence
    // immediately before the frame under test exports one; the fd numbers are
    // reused and the early poll on the acquire arm was observed to report
    // signalled on a frame that instrumentation showed was correctly parked
    // with all four waits present. So the reading is not trustworthy there and
    // is printed rather than asserted -- an unreliable check that sometimes
    // goes red is worse than no check, because it teaches people to re-run.
    //
    // Nothing is lost by that: the no-acquire arm establishes the fact the
    // surplus cases need ("this path does honour caller waits"), and the
    // acquire path's own merge is pinned by Case C, which fits exactly and
    // must still complete.
    if (!withAcquire) {
        Check(r.earlyPoll == 0,
              "BASELINE: a wait array well within capacity is honoured on DIRECT",
              "release fence polled " + I64(r.earlyPoll) + " while wait #" +
                  I64(waits) + " was still at 0. If THIS fails, the direct path "
                  "is not honouring caller waits at any count and the surplus "
                  "cases below say nothing about capacity");
    }
    Check(r.latePoll == 1, "and completes once every wait is signalled",
          "release fence polled " + I64(r.latePoll));
}

//=============================================================================
// Case A -- MORE CALLER WAITS THAN THE DIRECT ARRAY HOLDS.
//
// capacity+1 caller waits, the last one unsignalled. Nothing else is in the
// array on this path, so the surplus entry IS the unsignalled one.
//=============================================================================
void CaseSurplusCallerWaitIsNotDiscarded(bool direct)
{
    g_case = direct ? "DirectSurplusCallerWait" : "StagedSurplusCallerWait";

    const uint32_t waits = kDirectWaitCapacity + 1;   // 9
    const WaitCaseResult r = RunWaitCase(waits, false);

    std::printf("    [%s] waits=%u status=%d releaseFd=%d earlyPoll=%d "
                "latePoll=%d\n",
                g_case, waits, (int)r.status, r.releaseFd, r.earlyPoll,
                r.latePoll);

    if (!direct) {
        // CONTROL. The staged path assembles into a std::vector, so all nine
        // waits are honoured and the frame must be parked on the ninth.
        Check(r.submitted, "the staged submit was accepted",
              "status " + I64((long long)r.status));
        Check(r.earlyPoll == 0,
              "CONTROL: the staged path honoured the 9th wait",
              "release fence polled " + I64(r.earlyPoll) +
                  " while wait #9 was still at 0. This arm is the proof that "
                  "the apparatus can SEE an honoured wait; if it reports 1 "
                  "the readout is broken and the direct arm proves nothing");
        Check(r.latePoll == 1,
              "CONTROL: and it completed once every wait was signalled",
              "release fence polled " + I64(r.latePoll));
        return;
    }

    // SUBJECT. Nine waits cannot fit an eight-slot array. The only two honest
    // outcomes are to refuse the submit, or to accept it and still honour
    // every wait. Accepting it and dropping one is the defect.
    const bool silentlyDiscarded = r.submitted && (r.earlyPoll == 1);
    Check(!silentlyDiscarded,
          "a wait that does not fit is not silently discarded",
          "the submit returned SUCCESS and the release fence had ALREADY "
          "signalled (poll " + I64(r.earlyPoll) + ") while the semaphore it "
          "was told to wait on was still at 0. vkCmdEncodeVideoKHR read the "
          "input image without waiting for the producer that wait named -- "
          "the read-before-write race, observed, with a SUCCESS status and a "
          "valid release fd already handed back to the caller");

    if (!r.submitted) {
        std::printf("    [%s] submit refused with status %d -- the surplus "
                    "wait was reported, not dropped\n",
                    g_case, (int)r.status);
    }
}

//=============================================================================
// Case B -- A FULL CALLER ARRAY PLUS AN ARMED ACQUIRE FENCE.
//
// Exactly |capacity| caller waits, all satisfied, and an armed acquireFenceFd
// on top. The acquire semaphore is appended LAST, so it is entry 9 of 9 and
// the first thing truncation reaches. This is the header's "loses neither"
// promise at the one count where it was not tested.
//=============================================================================
void CaseAcquireFenceOnAFullWaitArray()
{
    g_case = "DirectAcquireOnFullWaitArray";

    // All |capacity| caller waits satisfied, so the ONLY entry that could park
    // the frame is gone either way; what is under test here is whether the
    // library will quietly build a 9-entry array it cannot submit.
    const uint32_t waits = kDirectWaitCapacity;   // 8, +1 acquire = 9
    const WaitCaseResult r = RunWaitCase(waits, true);

    std::printf("    [%s] waits=%u +acquire status=%d releaseFd=%d "
                "earlyPoll=%d latePoll=%d\n",
                g_case, waits, (int)r.status, r.releaseFd, r.earlyPoll,
                r.latePoll);

    Check(!r.submitted,
          "a merged wait array larger than the submit array is refused, "
          "not truncated",
          "the submit returned SUCCESS with " + I64(waits) +
              " caller waits and an armed acquireFenceFd -- 9 entries into an "
              "8-slot array. The 9th is the imported ACQUIRE semaphore, "
              "because the ext layer appends it last, so the producer fence "
              "the handle API exists to honour is exactly what got dropped, "
              "while the header promises the pair 'loses neither'");
}

//=============================================================================
// Case C -- THE BOUNDARY THAT MUST STILL WORK.
//
// capacity-1 caller waits plus an acquire fence is exactly |capacity| entries.
// It fits, so it must be accepted and must complete. Without this, "refuse
// everything" would pass Cases A and B.
//=============================================================================
void CaseExactlyFullIsStillAccepted()
{
    g_case = "DirectExactlyFullStillWorks";

    const uint32_t waits = kDirectWaitCapacity - 1;   // 7, +1 acquire = 8
    const WaitCaseResult r = RunWaitCase(waits, true);

    std::printf("    [%s] waits=%u +acquire status=%d releaseFd=%d "
                "earlyPoll=%d latePoll=%d\n",
                g_case, waits, (int)r.status, r.releaseFd, r.earlyPoll,
                r.latePoll);

    Check(r.submitted,
          "a merged wait array that exactly fills the submit array is accepted",
          "status " + I64((long long)r.status) + " -- a fix that refuses this "
          "has traded a silent drop for a refusal of legal input");
    Check(r.latePoll == 1,
          "and that frame completes",
          "release fence polled " + I64(r.latePoll));
}

//=============================================================================
// THE LEGACY ENTRY POINT, WHICH REACHES THE SAME FIXED ARRAY BY A DIFFERENT
// DOOR -- AND CAN ANSWER ITS CALLER BEFORE THE ARRAY IS EVER BUILT.
//
// SubmitExternalFrame() carries a raw VkImage and no registration, so the
// encoder core routes it from the frame's own format and tiling: an OPTIMAL
// NV12 frame is directly encodable, and its waits are assembled into the same
// 8-slot array the registered direct path uses.
//
// WHY THE ASSEMBLY'S OWN REFUSAL IS NOT A SUBSTITUTE FOR A GATE AT THE ENTRY
// POINT, and why this arm runs with B-frames while the registered arms above
// do not. A frame is recorded and submitted from the deferred-GOP flush. With
// no reordering that flush runs per frame, inline, so an over-capacity list
// is refused on the caller's own thread and the caller learns of it. WITH
// reordering the queue is drained on a later call -- so the entry point has
// already returned, and whatever the assembly then decides has no route back
// to whoever holds that answer.
//
// So this arm configures consecutiveBFrames > 0 deliberately. It is the shape
// in which "the submit refuses it" and "the caller is told" come apart, and
// therefore the only shape in which a bound at the entry point is doing any
// work at all.
//
// THE READOUT IS COMPLETION, not a release fence. This entry point refuses
// any pNext chain, so it can carry no fence descriptor; what every accepted
// frame does have is the completion path. Every wait here is a timeline
// semaphore ALREADY SIGNALLED to its target before the submit, so nothing
// legitimately holds a frame back:
//
//   accepted and completes      -> the frame was really submitted.
//   accepted and NEVER completes-> the caller holds a success for a frame
//       that was dropped: no bitstream, no completion edge, nothing that will
//       ever arrive. That is the silent hang, observed rather than inferred.
//   refused                     -> the over-capacity list was reported.
//
// CASE ORDER IS LOAD-BEARING. A flush that refuses a frame discards the whole
// deferred chain around it, so the over-capacity case is run LAST: the two
// arms that must stay green are measured on a session nothing has poisoned.
//=============================================================================

struct LegacyResult {
    VkResult result      = VK_SUCCESS;   // what the entry point told the caller
    VkResult flushResult = VK_SUCCESS;   // what the later, flushing call said
    bool     completed   = false;
};

// Fill a legacy input frame naming |input|, with |waits| already-satisfied
// timeline waits taken from |bank|.
VkVideoEncodeInputFrame LegacyFrame(const InputImage& input,
                                    const WaitBank& bank,
                                    uint64_t frameId,
                                    bool forceIdr)
{
    VkVideoEncodeInputFrame frame = {};
    frame.sType  = VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_FRAME;
    frame.pNext  = nullptr;
    frame.image  = input.image;
    frame.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    frame.width  = kWidth;
    frame.height = kHeight;
    // OPTIMAL NV12 is what makes the core route this frame DIRECT, onto the
    // fixed-array assembly. LINEAR here would route staged and measure
    // nothing.
    frame.imageTiling   = VK_IMAGE_TILING_OPTIMAL;
    frame.currentLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
    frame.frameId       = frameId;
    frame.pts           = frameId;
    frame.forceIDR      = forceIdr ? VK_TRUE : VK_FALSE;
    frame.isLastFrame   = VK_FALSE;
    frame.qpOverride    = -1;
    // Declared LOCAL, so the direct path records no queue-family transfer and
    // the declared layout above stays inert -- the frame under test differs
    // from the ones beside it in wait COUNT and nothing else.
    frame.inputResidency       = VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
    frame.waitSemaphoreCount   = (uint32_t)bank.semaphores.size();
    frame.pWaitSemaphores      = bank.semaphores.empty()
                                     ? nullptr
                                     : const_cast<VkSemaphore*>(bank.semaphores.data());
    frame.pWaitSemaphoreValues = bank.values.empty()
                                     ? nullptr
                                     : const_cast<uint64_t*>(bank.values.data());
    return frame;
}

VkResult LegacySubmit(const VkVideoEncodeInputFrame& frame)
{
    VkVideoEncodeInputFrame f = frame;
    VkResult status = g_encoder->SubmitExternalFrame(f, nullptr);
    for (int retry = 0; (status == VK_NOT_READY) && (retry < 2000); retry++) {
        DrainCaptures();
        f = frame;
        status = g_encoder->SubmitExternalFrame(f, nullptr);
        if (status == VK_NOT_READY) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, nullptr);
        }
    }
    return status;
}

LegacyResult RunLegacyWaitCase(const InputImage& input, uint32_t callerWaits)
{
    LegacyResult out;

    WaitBank bank;
    // EVERY wait satisfied. This case is not about whether a wait is honoured
    // -- the registered arms above measure that -- it is about whether a
    // frame its caller was told was accepted ever runs.
    if (!bank.Create(callerWaits, callerWaits)) {
        Check(false, "the timeline semaphore bank could be created",
              "vkCreateSemaphore/vkSignalSemaphore failed");
        bank.Destroy();
        return out;
    }

    const uint64_t frameId = g_frameId;
    out.result = LegacySubmit(LegacyFrame(input, bank, frameId, false));

    if (out.result == VK_SUCCESS) {
        g_frameId++;

        // FLUSH THE DEFERRED GOP. A forced-IDR frame drains the queue before
        // it is itself inserted, so this later call is where the frame above
        // is finally recorded and submitted -- and where a refusal it can no
        // longer be told about would land.
        WaitBank empty;
        const uint64_t flushId = g_frameId;
        out.flushResult = LegacySubmit(LegacyFrame(input, empty, flushId, true));
        if (out.flushResult == VK_SUCCESS) {
            g_frameId++;
        }

        // Bounded, because the failure under test is a frame that never
        // arrives: an unbounded wait would hang the suite rather than report.
        for (int i = 0; (i < 400) && !out.completed; i++) {
            const VkVideoEncoderFrameState state =
                g_encoder->GetFrameStatus(frameId);
            if ((state == VK_VIDEO_ENCODER_FRAME_STATE_READY) ||
                (state == VK_VIDEO_ENCODER_FRAME_STATE_ACQUIRED)) {
                out.completed = true;
                break;
            }
            VkVideoEncodeResult r;
            while (g_encoder->AcquireNextEncodedFrame(r) == VK_SUCCESS) {
                CountPictureType(r);
                if (r.frameId == frameId) {
                    out.completed = true;
                }
                g_encoder->ReleaseEncodedFrame(r.frameId);
            }
            if (out.completed) {
                break;
            }
            struct timespec ts = {0, 5000000};   // 5 ms
            nanosleep(&ts, nullptr);
        }
    }

    DrainCaptures();
    bank.Destroy();
    return out;
}

//=============================================================================
// Legacy baseline -- a wait list well inside capacity.
//
// Not decoration. Without it, "the 9-wait frame never completed" and "under
// reordering this harness never sees a completion at all" read identically,
// and the case below would prove nothing either way.
//=============================================================================
void CaseLegacySmallWaitListCompletes(const InputImage& input)
{
    g_case = "LegacyBaselineFewWaits";

    const uint32_t waits = 3;
    const LegacyResult r = RunLegacyWaitCase(input, waits);

    std::printf("    [%s] waits=%u result=%d flush=%d completed=%d\n",
                g_case, waits, (int)r.result, (int)r.flushResult,
                (int)r.completed);

    Check(r.result == VK_SUCCESS, "the legacy submit was accepted",
          "VkResult " + I64((long long)r.result));
    Check(r.completed,
          "BASELINE: a legacy frame inside capacity completes under reordering",
          "the frame was accepted and never became retrievable. If THIS "
          "fails, this harness cannot see a completion on this arm at all and "
          "the surplus case says nothing");
}

//=============================================================================
// Legacy Case A -- THE BOUNDARY THAT MUST STILL WORK.
//
// Exactly |capacity| waits fits the array. This entry point takes no pNext
// chain and therefore no acquire fence, so nothing else is competing for a
// slot: 8 is legal input, and a refusal here would be a regression rather
// than a fix.
//=============================================================================
void CaseLegacyExactlyFullIsStillAccepted(const InputImage& input)
{
    g_case = "LegacyExactlyFullStillWorks";

    const uint32_t waits = kDirectWaitCapacity;   // 8
    const LegacyResult r = RunLegacyWaitCase(input, waits);

    std::printf("    [%s] waits=%u result=%d flush=%d completed=%d\n",
                g_case, waits, (int)r.result, (int)r.flushResult,
                (int)r.completed);

    Check(r.result == VK_SUCCESS,
          "a wait list that exactly fills the direct array is accepted",
          "VkResult " + I64((long long)r.result) + " -- a gate that refuses "
          "this has traded a silent drop for a refusal of legal input");
    Check(r.completed, "and that frame completes",
          "the frame was accepted and never became retrievable");
}

//=============================================================================
// Legacy Case B -- MORE CALLER WAITS THAN THE DIRECT ARRAY HOLDS.
//
// capacity+1 waits, every one already satisfied, on a session that reorders.
// The frame has no legitimate reason not to run, and the call that would
// discover it cannot fit has not happened yet when this entry point answers.
// So an accepted frame that never completes is a frame dropped after its
// caller was told it was taken -- and told nothing since.
//
// Run LAST: the flush that refuses it discards the deferred chain around it.
//=============================================================================
void CaseLegacySurplusWaitIsReported(const InputImage& input)
{
    g_case = "LegacySurplusCallerWait";

    const uint32_t waits = kDirectWaitCapacity + 1;   // 9
    const LegacyResult r = RunLegacyWaitCase(input, waits);

    std::printf("    [%s] waits=%u result=%d flush=%d completed=%d\n",
                g_case, waits, (int)r.result, (int)r.flushResult,
                (int)r.completed);

    const bool acceptedAndLost = (r.result == VK_SUCCESS) && !r.completed;
    Check(!acceptedAndLost,
          "a wait list the direct array cannot hold is reported to the caller, "
          "not accepted and dropped",
          "SubmitExternalFrame returned VK_SUCCESS for " + I64(waits) +
              " waits into an 8-slot array and the frame never completed "
              "(the later flushing call said " +
              I64((long long)r.flushResult) + ", which no holder of the "
              "success can see). Every wait was already signalled, so nothing "
              "was holding it: the caller holds a success for a frame that "
              "was never submitted, raises no completion edge, and can only "
              "be waited on forever");

    if (r.result != VK_SUCCESS) {
        // The public header names this code on this entry point. A different
        // refusal would still avoid the hang but would not be the documented
        // contract, so it is asserted rather than merely tolerated.
        Check(r.result == VK_ERROR_TOO_MANY_OBJECTS,
              "and the refusal is the code the header names",
              "VkResult " + I64((long long)r.result) +
                  ", expected VK_ERROR_TOO_MANY_OBJECTS");
    }
}

}  // namespace

int main(int argc, char** argv)
{
    bool direct = true;
    bool legacy = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--staged") == 0) {
            direct = false;
        } else if (std::strcmp(argv[i], "--legacy") == 0) {
            legacy = true;
        }
    }

    std::printf("Encoder-ext DIRECT wait-array capacity (real device) -- %s "
                "arm\n",
                legacy ? "LEGACY SubmitExternalFrame"
                       : (direct ? "DIRECT" : "STAGED CONTROL"));
    std::printf("------------------------------------------------\n");

    if ((CreateVulkanVideoEncoderExt(g_encoder) != VK_SUCCESS) || !g_encoder) {
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
    // No reordering on the registered arms: the input-consuming submit is
    // issued inline with the frame that produced it, which is what makes the
    // release fd readable on the same call and the early poll meaningful.
    //
    // The LEGACY arm needs the opposite, and needs it to mean anything at
    // all. With no reordering the deferred-GOP queue is flushed per frame, so
    // the assembly runs on the caller's own thread and an over-capacity wait
    // list is refused back to the caller by the assembly itself. Reordering
    // moves that flush to a LATER call -- after the entry point has answered
    // -- which is the one shape where the entry point's own bound is what
    // stands between the caller and a success it can never collect on.
    config.consecutiveBFrames = legacy ? 2 : 0;
    config.idrPeriod          = 30;
    config.frameRateNum       = 30;
    config.frameRateDen       = 1;
    config.deviceId           = -1;
    config.disableFileOutput  = VK_TRUE;

    if (g_encoder->InitializeExt(config) != VK_SUCCESS) {
        std::printf("SKIP: InitializeExt failed -- no encode-capable Vulkan "
                    "device on this host\n");
        return 77;
    }

    VkInstance       instance = g_encoder->GetVkInstance();
    VkDevice         device   = g_encoder->GetVkDevice();
    VkPhysicalDevice phys     = g_encoder->GetVkPhysicalDevice();
    g_device = device;
    if (!LoadDeviceFns(instance, device, &g_fns)) {
        std::printf("SKIP: could not load the Vulkan entry points needed\n");
        return 77;
    }

    // Printed, not assumed: a silent fall-through to a software ICD would make
    // every number below meaningless.
    VkPhysicalDeviceProperties props{};
    g_fns.GetPhysicalDeviceProperties(phys, &props);
    std::printf("  device: %s (vendor 0x%04X, driver 0x%08X)\n",
                props.deviceName, props.vendorID, props.driverVersion);

    InputImage input;
    if (!CreateInputImage(g_fns, phys, device, &input, direct)) {
        std::printf("SKIP: could not create the input image\n");
        return 77;
    }

    VkVideoEncoderExternalImageDescriptor desc = {};
    desc.sType         = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
    desc.handleType    = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
    desc.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    desc.width         = kWidth;
    desc.height        = kHeight;
    desc.tiling        = direct ? VK_IMAGE_TILING_OPTIMAL
                                : VK_IMAGE_TILING_LINEAR;
    // Declaring VIDEO_ENCODE_SRC on a non-LINEAR image is exactly the
    // encodeCapable predicate, and therefore what routes this registration
    // DIRECT -- onto the fixed-array submit the defect lives in.
    desc.imageUsage    = direct
                             ? (VkImageUsageFlags)(
                                   VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                             : (VkImageUsageFlags)
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    desc.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    desc.planeCount    = 0;
    desc.residency     = VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
    desc.defaultLayout = direct ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR
                                : VK_IMAGE_LAYOUT_PREINITIALIZED;
    desc.existingImage = input.image;

    // The legacy arm registers NOTHING. SubmitExternalFrame carries the raw
    // VkImage and consults no registration, so leaving the descriptor unused
    // is what keeps that arm on the lane it is measuring.
    if (!legacy) {
        const VkVideoEncoderStatusCode regStatus =
            g_encoder->RegisterImageResource(desc, 0, &g_resource, nullptr);
        if ((regStatus != VK_VIDEO_ENCODER_STATUS_SUCCESS) ||
            (g_resource == VK_VIDEO_ENCODER_RESOURCE_NULL)) {
            std::printf("SKIP: RegisterImageResource failed, status %d\n",
                        (int)regStatus);
            return 77;
        }
    }

    if (legacy) {
        // Baseline first: it is what makes the surplus case interpretable.
        // The surplus case runs LAST because the flush that refuses it
        // discards the deferred chain around it.
        CaseLegacySmallWaitListCompletes(input);
        CaseLegacyExactlyFullIsStillAccepted(input);
        CaseLegacySurplusWaitIsReported(input);
    } else if (direct) {
        // Baselines first: they are what make the rest interpretable.
        CaseDirectSmallWaitArrayIsHonoured(false);
        CaseDirectSmallWaitArrayIsHonoured(true);
        CaseSurplusCallerWaitIsNotDiscarded(true);
        CaseAcquireFenceOnAFullWaitArray();
        CaseExactlyFullIsStillAccepted();
    } else {
        CaseSurplusCallerWaitIsNotDiscarded(false);
    }

    g_encoder->DrainPendingFrames();
    DrainCaptures();

    if (legacy) {
        // THE POSITIVE WITNESS FOR THIS ARM, and the only one there is.
        // Everything above asserts a VkResult and a completion, and every one
        // of those assertions is equally true of a session that coded no B
        // picture -- which is the one shape in which the deferred-GOP flush
        // this arm exists to measure never happens, and in which the codec's
        // L0/L1 walk and the slot de-duplication over it are never reached.
        // consecutiveBFrames is a REQUEST; what the GOP machine and the
        // device then type is what decides whether the path under test ran,
        // and the delivered picture type is the only reading of that.
        std::printf("    picture types coded: I=%u P=%u B=%u\n",
                    g_pictureTypeCount[0], g_pictureTypeCount[1],
                    g_pictureTypeCount[2]);
        Check(g_pictureTypeCount[2] > 0,
              "a B picture was coded on this arm, so the bidirectional "
              "reference walk under test ran",
              "I=" + I64(g_pictureTypeCount[0]) + " P=" +
                  I64(g_pictureTypeCount[1]) + " B=" +
                  I64(g_pictureTypeCount[2]) +
                  "; with no B picture this arm reorders nothing and every "
                  "result above is green having exercised no reordering");
    }

    std::printf("------------------------------------------------\n");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);

    g_fns.DestroyImage(device, input.image, nullptr);
    g_fns.FreeMemory(device, input.memory, nullptr);
    g_encoder = nullptr;

    return (g_failures == 0) ? 0 : 1;
}
