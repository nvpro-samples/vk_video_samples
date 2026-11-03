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
 * direct-encode submit's waits into a FIXED 8-slot stack array, and the loop
 * that injects the caller's external waits is bounded by that capacity. A
 * wait list that does not fit must be REFUSED, because a dropped wait names a
 * producer that has not finished writing the input image: vkCmdEncodeVideoKHR
 * would read the surface while it is still being written, with a SUCCESS
 * status handed back to the caller.
 *
 * WHY THE ENTRY POINT HAS TO ANSWER. SubmitExternalFrame() carries a raw
 * VkImage, and the encoder core routes it from the frame's own format and
 * tiling: an OPTIMAL NV12 frame is directly encodable, and its waits are
 * assembled into the fixed array. A frame is recorded and submitted from the
 * deferred-GOP flush. With no reordering that flush runs per frame, inline,
 * so an over-capacity list is refused on the caller's own thread. WITH
 * reordering the queue is drained on a later call -- after the entry point
 * has answered -- so the entry point's own bound is what stands between the
 * caller and a success it can never collect on. The session is therefore
 * configured with B frames.
 *
 * THE READOUT IS COMPLETION. Every wait is a timeline semaphore ALREADY
 * SIGNALLED to its target before the submit, so nothing legitimately holds a
 * frame back:
 *
 *   accepted and completes       -> the frame was really submitted.
 *   accepted and NEVER completes -> the caller holds a success for a frame
 *       that was dropped: no bitstream, nothing that will ever arrive.
 *   refused                      -> the over-capacity list was reported.
 *
 * --h265 runs the same cases on an H.265 session: the bidirectional reference
 * list a B frame needs is assembled per codec, so each codec is a case of its
 * own.
 */

#include "vulkan_video_encoder_ext.h"
#include "encode_device_probe.h"

#include "vk_video/vulkan_video_codec_h264std.h"
#include "vk_video/vulkan_video_codec_h265std.h"

#include <dlfcn.h>
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

// OPTIMAL + VIDEO_ENCODE_SRC + a profile list at create time is what the
// encoder can read without a staging copy, and is therefore what routes the
// frames below DIRECT.
bool CreateInputImage(const DeviceFns& fns, VkPhysicalDevice phys,
                      VkDevice device, InputImage* out, bool h265)
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
    // MAIN for the reason HIGH is named above: it is the profile the SESSION
    // uses. The config leaves |profile| at VK_VIDEO_ENCODER_PROFILE_DEFAULT
    // and the library derives general_profile_idc Main for 8-bit 4:2:0 input.
    VkVideoEncodeH265ProfileInfoKHR h265Profile{
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR};
    h265Profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
    VkVideoProfileInfoKHR profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    profile.pNext               = h265 ? (const void*)&h265Profile
                                       : (const void*)&h264Profile;
    profile.videoCodecOperation =
        h265 ? VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR
             : VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    profile.chromaSubsampling   = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profile.lumaBitDepth        = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profile.chromaBitDepth      = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    VkVideoProfileListInfoKHR profileList{
        VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    profileList.profileCount = 1;
    profileList.pProfiles    = &profile;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext         = &profileList;
    // WHAT THE LIBRARY WILL DO TO THIS IMAGE, DECLARED AT CREATION.
    //
    // A frame that routes DIRECT is viewed PER PLANE -- R8_UNORM over the
    // luma, R8G8_UNORM over the chroma of an NV12 image -- and a per-plane
    // view of a multi-planar image requires MUTABLE_FORMAT
    // (VUID-VkImageViewCreateInfo-image-01762).
    //
    // Without it this image is one the encoder cannot legally view, and the
    // cases below would be measuring that rather than wait capacity.
    ci.flags         = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                       VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ci.extent        = {kWidth, kHeight, 1};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    // THE USAGE THE VIEWS WILL NAME, not the narrowest one an encode source
    // could get away with. A view may not name a usage bit the image was not
    // created with (VUID-VkImageViewCreateInfo-pNext-02662), and the wrap the
    // direct path performs builds its views over the full set the encoder can
    // put an input image to -- the transfer pair for a staged copy and the
    // sampled/storage pair for the preprocess filter -- whichever rung this
    // particular frame ends up on. So a producer handing an image to this
    // path declares all of them, and this test declares what a producer
    // would.
    ci.usage         = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_STORAGE_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fns.CreateImage(device, &ci, nullptr, &out->image) != VK_SUCCESS) {
        std::printf("  ERROR: vkCreateImage(NV12) failed\n");
        return false;
    }

    VkMemoryRequirements req{};
    fns.GetImageMemoryRequirements(device, out->image, &req);

    VkPhysicalDeviceMemoryProperties memProps{};
    fns.GetPhysicalDeviceMemoryProperties(phys, &memProps);
    uint32_t typeIndex = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
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
    return true;
}

//=============================================================================
// The session under test.
//=============================================================================
VkSharedBaseObj<VulkanVideoEncoderExt> g_encoder;
DeviceFns                              g_fns;
VkDevice                               g_device   = VK_NULL_HANDLE;
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

//=============================================================================
// The measurement. CASE ORDER IS LOAD-BEARING: a flush that refuses a frame
// discards the whole deferred chain around it, so the over-capacity case is
// run LAST, and the arms that must stay green are measured on a session
// nothing has poisoned.
//=============================================================================

struct LegacyResult {
    VkResult result      = VK_SUCCESS;   // what the entry point told the caller
    VkResult flushResult = VK_SUCCESS;   // what the later, flushing call said
    bool     completed   = false;
};

// Fill an input frame naming |input|, with |waits| already-satisfied
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
    // but about whether a frame its caller was told was accepted ever runs.
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
          "fails, this harness cannot see a completion at all and the "
          "surplus case says nothing");
}

//=============================================================================
// Legacy Case A -- THE BOUNDARY THAT MUST STILL WORK.
//
// Exactly |capacity| waits fits the array. This entry point takes no pNext
// chain, so nothing else is competing for a slot: 8 is legal input, and a
// refusal here would be a regression rather than a fix.
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
              "was never submitted and can only be waited on forever");

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

static int RunTest(int argc, char** argv)
{
    // THE CODEC IS A PARAMETER. The bidirectional reference list a B frame
    // needs is assembled by per-codec code: H.264 and H.265 fill
    // referenceSlotsInfo[] in separate files, so a run that exercises one of
    // them leaves the other unread.
    bool h265 = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--h265") == 0) {
            h265 = true;
        }
    }

    std::printf("Encoder-ext DIRECT wait-array capacity (real device) -- "
                "SubmitExternalFrame, %s\n",
                h265 ? "H.265" : "H.264");
    std::printf("------------------------------------------------\n");

    const VkResult created = CreateVulkanVideoEncoderExt(g_encoder);
    if ((created != VK_SUCCESS) || !g_encoder) {
        return enctest::Fail("CreateVulkanVideoEncoderExt", created);
    }

    VkVideoEncoderConfig config = {};
    config.sType              = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
    config.codec              =
        h265 ? VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR
             : VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    config.encodeWidth        = kWidth;
    config.encodeHeight       = kHeight;
    config.inputFormat        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    config.inputWidth         = kWidth;
    config.inputHeight        = kHeight;
    config.rateControlMode    = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    config.averageBitrate     = 5000000;
    config.maxBitrate         = 5000000;
    config.gopLength          = 30;
    // Reordering, deliberately. With no reordering the deferred-GOP queue is
    // flushed per frame, so the assembly runs on the caller's own thread and
    // an over-capacity wait list is refused back to the caller by the
    // assembly itself. Reordering moves that flush to a LATER call -- after
    // the entry point has answered -- which is the one shape where the entry
    // point's own bound is what stands between the caller and a success it
    // can never collect on.
    config.consecutiveBFrames = 2;
    config.idrPeriod          = 30;
    config.frameRateNum       = 30;
    config.frameRateDen       = 1;
    config.deviceId           = -1;
    config.disableFileOutput  = VK_TRUE;

    // The library failed before it had a device: the probe, not the library,
    // says whether this machine has one.
    const VkResult initialized = g_encoder->InitializeExt(config);
    if (initialized != VK_SUCCESS) {
        return enctest::NoDeviceOrFail("InitializeExt", initialized, config.codec);
    }

    VkInstance       instance = g_encoder->GetVkInstance();
    VkDevice         device   = g_encoder->GetVkDevice();
    VkPhysicalDevice phys     = g_encoder->GetVkPhysicalDevice();
    g_device = device;
    if (!LoadDeviceFns(instance, device, &g_fns)) {
        return enctest::Fail("could not load the Vulkan entry points needed");
    }

    // Printed, not assumed: a silent fall-through to a software ICD would make
    // every number below meaningless.
    VkPhysicalDeviceProperties props{};
    g_fns.GetPhysicalDeviceProperties(phys, &props);
    std::printf("  device: %s (vendor 0x%04X, driver 0x%08X)\n",
                props.deviceName, props.vendorID, props.driverVersion);

    InputImage input;
    if (!CreateInputImage(g_fns, phys, device, &input, h265)) {
        return enctest::Fail("could not create the input image");
    }

    // Baseline first: it is what makes the surplus case interpretable.
    // The surplus case runs LAST because the flush that refuses it discards
    // the deferred chain around it.
    CaseLegacySmallWaitListCompletes(input);
    CaseLegacyExactlyFullIsStillAccepted(input);
    CaseLegacySurplusWaitIsReported(input);

    g_encoder->DrainPendingFrames();
    DrainCaptures();

    // THE POSITIVE WITNESS, and the only one there is.
    // Everything above asserts a VkResult and a completion, and every one
    // of those assertions is equally true of a session that coded no B
    // picture -- which is the one shape in which the deferred-GOP flush
    // this test exists to measure never happens, and in which the codec's
    // L0/L1 walk and the slot de-duplication over it are never reached.
    // consecutiveBFrames is a REQUEST; what the GOP machine and the
    // device then type is what decides whether the path under test ran,
    // and the delivered picture type is the only reading of that.
    std::printf("    picture types coded: I=%u P=%u B=%u\n",
                g_pictureTypeCount[0], g_pictureTypeCount[1],
                g_pictureTypeCount[2]);
    Check(g_pictureTypeCount[2] > 0,
          "a B picture was coded, so the bidirectional "
          "reference walk under test ran",
          "I=" + I64(g_pictureTypeCount[0]) + " P=" +
              I64(g_pictureTypeCount[1]) + " B=" +
              I64(g_pictureTypeCount[2]) +
              "; with no B picture this run reorders nothing and every "
              "result above is green having exercised no reordering");

    std::printf("------------------------------------------------\n");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);

    g_fns.DestroyImage(device, input.image, nullptr);
    g_fns.FreeMemory(device, input.memory, nullptr);
    g_encoder = nullptr;

    return (g_failures == 0) ? 0 : 1;
}

int main(int argc, char** argv)
{
    const int status = RunTest(argc, argv);
    // g_encoder holds the library's VkInstance. Released here, on every path, vkDestroyInstance runs while the
    // layers loaded with that instance are still alive; left to exit(), it runs after their static destructors.
    g_encoder = nullptr;
    return status;
}
