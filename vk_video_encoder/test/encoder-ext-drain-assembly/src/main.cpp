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
 * DrainPendingFrames() must NOT be terminal for the COMPLETION SURFACE.
 *
 * ---------------------------------------------------------------------------
 * FIXED. This test passes, and is a plain gating test (no WILL_FAIL).
 * ---------------------------------------------------------------------------
 * It was registered WILL_FAIL while the defect diagnosed below was open. The
 * fix is VkVideoEncoder::DrainAndRestartThreads(): DrainPendingFrames()
 * performs the same drain as before and then RESTARTS the assembly workers,
 * so the completion surface survives a drain instead of ending at it. The
 * WILL_FAIL line is gone from this directory's CMakeLists.txt.
 *
 * The diagnosis is kept in full below, because it is what this test guards.
 *
 * THE SHAPE THIS EXISTS FOR. A frame carrying an armed acquireFenceFd is
 * accepted, its input IS read (its release fence signals), and it is then
 * never assembled into a capture; an unarmed control on the identical path
 * retires normally. It presents as an armed loop that submits every frame and
 * retires none, against a control that retires all of them, ending with every
 * armed frame undelivered and unreleased.
 *
 * THE ACQUIRE FENCE IS NOT THE VARIABLE. That reading came from running the
 * unarmed CONTROL loop first and the ARMED loop second, and
 * the control loop ends with DrainPendingFrames(). Everything submitted after
 * that call fails to retire, fence or no fence. This file carries NO fence of
 * any kind -- every acquireFenceFd here is -1 and no release fd is requested
 * -- so if the second batch still fails to retire, the fence is exonerated by
 * construction and cannot be the cause.
 *
 * THE DEFECT, mechanically, three links:
 *
 *   1. VulkanVideoEncoderExtImpl::DrainPendingFrames()
 *      (vulkan_video_encoder_ext.cpp:3268) calls
 *      VkVideoEncoder::WaitForThreadsToComplete().
 *
 *   2. WaitForThreadsToComplete() (VkVideoEncoder.cpp:5896-5920) joins the
 *      assembly workers, clears m_assemblyThreads, and sets
 *      m_asyncAssemblyEnabled = false (:5913). m_asyncAssemblyEnabled is set
 *      true at exactly one site, VkVideoEncoder.cpp:3702 inside InitEncoder,
 *      and the workers are emplaced at exactly one site, :3704, in the same
 *      block. Neither runs again. Async assembly is off for the rest of the
 *      session.
 *
 *   3. With it off, ProcessOrderedFrames (:5719; the branch at :5728, the
 *      async arm at :5774) skips QueueFramesForAssembly and appends
 *      AssembleBitstreamData instead. AssembleBitstreamData (:2126) never
 *      calls PushCapturedBitstream -- and PushCapturedBitstream is the ONLY
 *      producer of a CapturedBitstream in production code (VkVideoEncoder.cpp
 *      :2305 inside WriteBitstreamToFile, which only the assembly worker
 *      calls, and :2422 a direct call in that worker's readback-failure arm;
 *      VkVideoEncoderAV1.cpp:1050 inside the AV1 override; the ext.cpp:7297
 *      site is a device-free unit-test seam, not a live path).
 *
 *      NOTE: the contract, not the line numbers, is what this test rests
 *      on; encoder-sync-assembly is the bar that pins it directly.
 *
 * So after DrainPendingFrames() every subsequent frame is encoded and then
 * has no completion record made for it. No capture => AcquireNextEncodedFrame
 * never yields it => ReleaseEncodedFrame is never called => the PendingFrame
 * and the resource registration are held forever and inFlight only grows.
 * That is the "16 undelivered/unreleased frames" at teardown, exactly.
 *
 * THE FIX, against those same three links. Link 2 is the one that was wrong:
 * WaitForThreadsToComplete() is the TEARDOWN drain and Flush()/Deinitialize()
 * still call it directly. DrainPendingFrames() now calls
 * DrainAndRestartThreads(), which runs that same drain -- so link 1 is
 * unchanged and everything already submitted still completes -- and then
 * calls StartAssemblyThreads() to bring the workers back. Link 3 therefore
 * never happens: m_asyncAssemblyEnabled is true again by the time the next
 * frame is submitted, ProcessOrderedFrames takes QueueFramesForAssembly, and
 * the single publisher inside WriteBitstreamToFile runs for every frame in
 * every batch.
 *
 * WHY NOT "make the synchronous fallback publish too", which looks like the
 * smaller change: it cannot work. The
 * synchronous assembly runs INLINE inside SetExternalInputFrame(), and
 * SubmitExternalFrameCommon only calls EnqueuePendingFrame() -- which creates
 * the PendingFrame a record has to land on -- AFTER that call returns. So a
 * record published from AssembleBitstreamData arrives before its own
 * PendingFrame exists, DrainCapturesLocked() finds nothing to match it to,
 * and discards it: batch 2 stayed at retired=0 and gained eight
 * "late capture for released frame N discarded" lines. It converts a silent
 * drop into a counted late capture, which is worse -- m_lateCaptures exists
 * to detect deadline/fence-cap collisions and would now be reporting noise.
 *
 * THIS IS A CONTRACT VIOLATION IN BOTH OUTPUT MODES, not a capture-mode
 * quirk. vulkan_video_encoder_ext.h says of disableFileOutput: "The COMPLETION
 * surface does not depend on this flag. In both modes every submitted frame
 * raises the completion edge ... and becomes acquirable exactly once." The
 * single publisher lives inside WriteBitstreamToFile, which handles BOTH arms
 * (VkVideoEncoder.cpp:1851-1856) and which only the assembly worker calls. In
 * file-output mode the bytes still reach the file through the synchronous
 * fallback, so the encode itself is fine; it is only the completion record
 * that is dropped. This test therefore runs in file-output mode too, under
 * --file-output, to show the divergence.
 *
 * And the header says of DrainPendingFrames that it is a "Non-terminal drain
 * ... The encoder stays usable for retrieval / Release" -- true for frames
 * already captured, false for anything submitted afterwards.
 *
 * WHY IT NEEDS A GPU. It submits real frames to a real encode queue. With no
 * encode-capable device it exits 77, which CTest reads as SKIP.
 */

#include "vulkan_video_encoder_ext.h"

// The public header reaches the Xlib platform headers, whose macros collide
// with ordinary identifiers. Same scrub, same reason, as the sibling tests.
#undef Status
#undef None
#undef Bool
#undef Window

#include <dlfcn.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;
int g_checks   = 0;
const char* g_case = "setup";

void Check(bool ok, const char* what, const std::string& detail)
{
    g_checks++;
    if (ok) {
        std::printf("  ok   [%s] %s\n", g_case, what);
        return;
    }
    g_failures++;
    std::printf("  FAIL [%s] %s : %s\n", g_case, what, detail.c_str());
}

std::string I64(long long v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", v);
    return buf;
}

const uint32_t kWidth  = 1920;
const uint32_t kHeight = 1080;
// Small enough to stay far inside admission control (so a NOT_READY retry is
// not the thing under test) and large enough that "none of them retired" is
// not a one-frame coincidence.
const uint32_t kBatch  = 8;

struct DeviceFns {
    PFN_vkCreateImage                       CreateImage = nullptr;
    PFN_vkDestroyImage                      DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements        GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory                    AllocateMemory = nullptr;
    PFN_vkFreeMemory                        FreeMemory = nullptr;
    PFN_vkBindImageMemory                   BindImageMemory = nullptr;
    PFN_vkMapMemory                         MapMemory = nullptr;
    PFN_vkUnmapMemory                       UnmapMemory = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties       GetPhysicalDeviceProperties = nullptr;
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
        return false;
    }
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    if (gdpa == nullptr) {
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

// A host-written LINEAR NV12 image, transfer-source usage only, so the
// registration routes STAGED -- the same routing as the acquire-fd and
// release-fence siblings, so the numbers here are comparable to theirs.
bool CreateInputImage(const DeviceFns& fns, VkPhysicalDevice phys,
                      VkDevice device, InputImage* out)
{
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ci.extent        = {kWidth, kHeight, 1};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_LINEAR;
    ci.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
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
        (VkMemoryPropertyFlags)(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (((req.memoryTypeBits & (1u << i)) != 0) &&
            ((memProps.memoryTypes[i].propertyFlags & want) == want)) {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == UINT32_MAX) {
        std::printf("  ERROR: no host-visible memory type\n");
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

VkSharedBaseObj<VulkanVideoEncoderExt> g_encoder;
VkVideoEncoderResource g_resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
uint64_t g_frameId       = 0;
uint32_t g_captured      = 0;
uint64_t g_capturedBytes = 0;

void DrainCaptures()
{
    VkVideoEncodeResult r;
    while (g_encoder->AcquireNextEncodedFrame(r) == VK_SUCCESS) {
        g_captured++;
        g_capturedBytes += r.bitstreamSize;
        g_encoder->ReleaseEncodedFrame(r.frameId);
    }
}

// Submit `count` frames carrying NO fence descriptor at all, and return how
// many of them came back as captures. Every submit is plain: no pNext chain,
// no acquireFenceFd, no pReleaseFenceFd, no caller semaphores. There is
// nothing here for a fence to be the cause of.
uint32_t SubmitBatchAndCount(uint32_t count, uint32_t* outSubmitted)
{
    const uint32_t capturedBefore = g_captured;
    uint32_t submitted = 0;
    for (uint32_t f = 0; f < count; f++) {
        VkVideoEncoderFrameSubmitInfo info = {};
        info.sType         = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS;
        info.resource      = g_resource;
        info.frameId       = g_frameId;
        info.pts           = g_frameId;
        info.qpOverride    = -1;
        info.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkVideoEncoderStatusCode status =
            g_encoder->SubmitRegisteredFrame(info, nullptr);
        for (int retry = 0;
             (status == VK_VIDEO_ENCODER_STATUS_NOT_READY) && (retry < 500);
             retry++) {
            DrainCaptures();
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, nullptr);
            status = g_encoder->SubmitRegisteredFrame(info, nullptr);
        }
        if (status != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            std::printf("    submit of frame %llu returned %d\n",
                        (unsigned long long)g_frameId, (int)status);
            break;
        }
        g_frameId++;
        submitted++;
        DrainCaptures();
    }
    g_encoder->DrainPendingFrames();
    DrainCaptures();
    *outSubmitted = submitted;
    return g_captured - capturedBefore;
}

}  // namespace

int main(int argc, char** argv)
{
    bool fileOutput = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--file-output") == 0) {
            fileOutput = true;
        }
    }

    std::printf("DrainPendingFrames() and the completion surface "
                "(real device, NO fences anywhere)\n");
    std::printf("mode: %s\n",
                fileOutput ? "FILE OUTPUT (disableFileOutput=VK_FALSE)"
                           : "CAPTURE (disableFileOutput=VK_TRUE)");
    std::printf("--------------------------------------------------------\n");

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
    // No reordering, so a frame is submitted on the call that offers it and
    // "did it retire" is not confounded by a deferred B-frame batch.
    config.consecutiveBFrames = 0;
    config.idrPeriod          = 30;
    config.frameRateNum       = 30;
    config.frameRateDen       = 1;
    config.deviceId           = -1;
    config.disableFileOutput  = fileOutput ? VK_FALSE : VK_TRUE;
    if (fileOutput) {
        config.outputPath = "encoder_ext_drain_assembly_out.264";
    }

    if (g_encoder->InitializeExt(config) != VK_SUCCESS) {
        std::printf("SKIP: InitializeExt failed -- no encode-capable Vulkan "
                    "device on this host\n");
        return 77;
    }

    VkInstance       instance = g_encoder->GetVkInstance();
    VkDevice         device   = g_encoder->GetVkDevice();
    VkPhysicalDevice phys     = g_encoder->GetVkPhysicalDevice();
    DeviceFns fns;
    if (!LoadDeviceFns(instance, device, &fns)) {
        std::printf("SKIP: could not load the Vulkan entry points needed\n");
        return 77;
    }

    // Printed, not assumed. Nine ICDs are installed on the test host and a
    // silent fall-through to a software driver would make every number below
    // meaningless.
    VkPhysicalDeviceProperties props{};
    fns.GetPhysicalDeviceProperties(phys, &props);
    std::printf("  device: %s (vendor 0x%04X, driver 0x%08X)\n",
                props.deviceName, props.vendorID, props.driverVersion);

    InputImage input;
    if (!CreateInputImage(fns, phys, device, &input)) {
        std::printf("SKIP: could not create the input image\n");
        return 77;
    }

    VkVideoEncoderExternalImageDescriptor desc = {};
    desc.sType         = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
    desc.handleType    = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
    desc.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    desc.width         = kWidth;
    desc.height        = kHeight;
    desc.tiling        = VK_IMAGE_TILING_LINEAR;
    desc.imageUsage    = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    desc.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    desc.planeCount    = 0;
    desc.residency     = VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
    desc.defaultLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    desc.existingImage = input.image;

    const VkVideoEncoderStatusCode regStatus =
        g_encoder->RegisterImageResource(desc, 0, &g_resource, nullptr);
    if ((regStatus != VK_VIDEO_ENCODER_STATUS_SUCCESS) ||
        (g_resource == VK_VIDEO_ENCODER_RESOURCE_NULL)) {
        std::printf("SKIP: RegisterImageResource failed, status %d\n",
                    (int)regStatus);
        return 77;
    }

    //=========================================================================
    // BATCH 1 -- the control. Ends with DrainPendingFrames().
    //=========================================================================
    g_case = "Batch1BeforeAnyDrain";
    uint32_t submitted1 = 0;
    const uint32_t retired1 = SubmitBatchAndCount(kBatch, &submitted1);
    std::printf("    batch 1: submitted=%u retired=%u\n",
                submitted1, retired1);

    Check(submitted1 == kBatch, "every frame in batch 1 was accepted",
          I64(submitted1) + " of " + I64(kBatch));
    // This one MUST hold today. If it does not, the harness is broken and the
    // batch-2 result below proves nothing -- so it is asserted first and
    // separately, rather than being folded into the interesting claim.
    Check(retired1 > 0,
          "batch 1 retires, so the completion surface works at all",
          I64(retired1) + " captures for " + I64(submitted1) +
              " submits -- if this is 0 the rest of this file is not "
              "measuring what it claims");

    //=========================================================================
    // BATCH 2 -- IDENTICAL frames, IDENTICAL code path, submitted after
    // DrainPendingFrames() has been called once. THIS IS THE DEFECT.
    //=========================================================================
    g_case = "Batch2AfterDrainPendingFrames";
    uint32_t submitted2 = 0;
    const uint32_t retired2 = SubmitBatchAndCount(kBatch, &submitted2);
    std::printf("    batch 2: submitted=%u retired=%u\n",
                submitted2, retired2);

    Check(submitted2 == kBatch,
          "every frame in batch 2 was accepted -- the submit path is fine",
          I64(submitted2) + " of " + I64(kBatch));

    // THE ASSERTION THAT FAILS TODAY.
    Check(retired2 > 0,
          "batch 2 retires too: DrainPendingFrames() is a drain, not an "
          "end-of-stream for the completion surface",
          I64(retired2) + " captures for " + I64(submitted2) +
              " submits after a DrainPendingFrames(). "
              "WaitForThreadsToComplete (VkVideoEncoder.cpp:4303) set "
              "m_asyncAssemblyEnabled=false and joined the only threads that "
              "ever call PushCapturedBitstream; nothing sets it true again "
              "outside InitEncoder (:3054), so no frame submitted after that "
              "call can ever produce a completion record. Every one of these "
              "frames now holds its PendingFrame and its resource "
              "registration forever. NO FENCE WAS INVOLVED IN THIS RUN.");

    g_case = "teardown";
    std::printf("  captured frames=%u  bitstream bytes=%llu\n",
                g_captured, (unsigned long long)g_capturedBytes);

    // Order matters and the header states it: unregister BEFORE destroying
    // the image, and destroy the image BEFORE releasing the encoder.
    g_encoder->UnregisterImageResource(g_resource);
    fns.DestroyImage(device, input.image, nullptr);
    fns.FreeMemory(device, input.memory, nullptr);
    g_encoder = nullptr;

    std::printf("--------------------------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
