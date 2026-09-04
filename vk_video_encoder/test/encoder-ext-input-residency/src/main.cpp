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
 * INPUT RESIDENCY ON AN OS-HANDLE REGISTRATION: does an explicit
 * VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL survive the ext layer?
 *
 * ------------------------------------------------------------------------
 * THE COVERAGE HOLE THIS FILLS, stated first because it is why the defect
 * this suite covers survived three rounds of review.
 * ------------------------------------------------------------------------
 * `grep -rn handleType vk_video_encoder/test --include=*.cpp` returned EIGHT
  * registration sites outside this file, and every one of them declares
 * VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE. Zero OPAQUE_FD, zero
 * DMA_BUF -- so the entire OS-handle import path, and with it every rule the
 * ext layer applies ONLY to an OS handle, had no test of any kind. The
 * residency read in SubmitRegisteredFrame is exactly such a rule:
 *
 *     residency = (slot->handleType == ..._VK_IMAGE) ? slot->residency
 *                                                    : ..._FOREIGN;
 *
 * A suite that only ever registers VK_IMAGE takes the left arm every time and
 * cannot see the right one at all. That is the shape this file adds.
 *
 * It is also the shape Chromium's SHIPPING CPU staging tier uses -- OPAQUE_FD,
 * self-exported from the library's own VkDevice and re-imported into it,
 * declaring RESIDENCY_LOCAL -- and that tier has no library-side coverage
 * whatsoever.
 *
 * ------------------------------------------------------------------------
 * WHY THE ASSERTION IS A COUNTER AND NOT A VALIDATION-ERROR COUNT.
 * ------------------------------------------------------------------------
 * This was settled by construction before the file was written, because a
 * test that cannot fail is worse than no test. The two barrier programs the
 * residency decision selects between are BOTH spec-clean and BOTH leave the
 * image in the same layout:
 *
 *   derived FOREIGN     acquire GENERAL -> TRANSFER_SRC_OPTIMAL with
 *                       srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
 *                       srcStageMask NONE; handback releases back to FOREIGN
 *                       with newLayout = srcOldLayout, residual CLEARED.
 *   honoured LOCAL      the same layout pair with both families IGNORED and
 *                       srcStageMask HOST|TRANSFER; handback RESTORES to
 *                       srcOldLayout and RECORDS it.
 *
 * Both round-trip to the same layout, so frames 2..N are validation-clean
 * either way. The three real differences -- a frame-1 acquire from FOREIGN
 * that nothing released, N ownership transfers that transfer nothing, and the
 * LOSS OF THE AVAILABILITY OPERATION for the caller's host writes -- are none
 * of them core VUIDs. Arm 1 emits no validation message whether its LOCAL
 * declaration is honoured or discarded, while its counters move from
 * foreign-only to local-only. The layer is live for both readings -- a
 * temporary probe arm declaring a usage-incompatible layout does raise
 * messages through the same harness -- so the silence is the layer's answer
 * and not a dead harness.
 *
 * So the observable is VkVideoEncoderInputResidencyInfo, chained onto
 * GetCompletionInfo()'s pNext, which reports the two sides of that decision
 * directly. It was added for this suite and is what makes arm 1 red.
 *
 * ------------------------------------------------------------------------
 * THE ARMS. One registration each, EIGHT frames through it -- reuse is not
 * incidental, it is the case the residency declaration exists for -- with a
 * different host-written pattern mapped in between frames.
 * ------------------------------------------------------------------------
 *   --local-opaque-fd    SUBJECT. OPAQUE_FD + explicit LOCAL. Expects the
 *                        local acquire. RED before the fix (8 foreign),
 *                        green after.
 *   --local-vk-image     CONTROL. The IDENTICAL image and the identical
 *                        declaration registered as VK_IMAGE, the one arm
 *                        where residency is already honoured. Subject-red
 *                        with control-green isolates handleType and nothing
 *                        else -- not the host, not the format, not reuse.
 *   --foreign-opaque-fd  NEGATIVE CONTROL. Explicit FOREIGN must still take
 *                        the foreign acquire.
 *   --auto-opaque-fd     PIN. residency left zero (AUTO) must still DERIVE
 *                        foreign. Together with the arm above this is what
 *                        stops the fix degenerating into "never acquire":
 *                        deleting the ternary's FOREIGN branch outright would
 *                        turn the subject green and break the Wayland/GBM
 *                        zero-copy lane and the renderer app, both of which
 *                        inherit AUTO.
 *   --preinit-opaque-fd  THE CHROMIUM-TODAY MIRROR. OPAQUE_FD + LOCAL, but
 *                        declaring VK_IMAGE_LAYOUT_PREINITIALIZED as both the
 *                        registration default and the per-frame layout, which
 *                        is what media/gpu/vulkan/vulkan_video_encode_
 *                        accelerator.cc did before the sentinel change. It
 *                        PASSES -- counters and validation both -- and it was
 *                        expected to fail. Its declaration is nonetheless
 *                        FALSE from frame 2 on (the library's handback leaves
 *                        the image in GENERAL, because PREINITIALIZED is not
 *                        a legal barrier destination), and the layer does not
 *                        say so. See the CMake entry: that combination is why
 *                        this defect class keeps surviving review, and why
 *                        this suite asserts on counters.
 *   --local-tso-opaque-fd  THE LAYOUT-TABLE HOLE. OPAQUE_FD + LOCAL declaring
 *                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL. Before the
 *                        transition table gained a
 *                        (TRANSFER_SRC_OPTIMAL -> TRANSFER_SRC_OPTIMAL) arm
 *                        this did not fail -- it ABORTED, via the table's
 *                        terminal `throw` in a build with no handler.
 *
 * Exits 0 all assertions held, 1 an assertion failed, 77 (CTest SKIP) with no
 * encode-capable Vulkan device -- same contract as the sibling suites.
 */

#include "vulkan_video_encoder_ext_internal.h"

// The public header reaches the Xlib platform headers, whose macros collide
// with ordinary identifiers. Same scrub, same reason, as the sibling tests.
#undef Status
#undef None
#undef Bool
#undef Window

#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
        std::printf("  ok   %s\n", what);
        return;
    }
    g_failures++;
    std::printf("  FAIL %s : %s\n", what, detail.c_str());
}

std::string U64(unsigned long long v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", v);
    return buf;
}

std::string I64(long long v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", v);
    return buf;
}

const uint32_t kWidth  = 1280;
const uint32_t kHeight = 720;

// EIGHT, and the number is load-bearing rather than arbitrary. The residency
// declaration exists for a REUSED input image -- the header says so in as many
// words ("A caller pooling/reusing input images must state the residency
// explicitly") -- and a single-frame registration cannot distinguish the two
// handbacks at all, because nothing ever reads back what they left behind.
const uint32_t kFrames = 8;

// ---------------------------------------------------------------------------
// Device-level entry points, resolved off the LIBRARY's own device.
//
// GetMemoryFdKHR is the one this suite adds over its siblings: the test
// EXPORTS from the library's device and hands the fd straight back to the
// library, which re-imports it into that same device. That self-import is not
// a contrivance -- it is precisely what Chromium's CPU staging tier does
// (vulkan_video_encode_accelerator.cc takes `device = lib_state_->encoder->
// GetVkDevice()`), and it is the reason the "the memory is foreign to the
// encode device" premise behind the derived FOREIGN is false on that tier.
// ---------------------------------------------------------------------------
struct DeviceFns {
    PFN_vkCreateImage                CreateImage                = nullptr;
    PFN_vkDestroyImage               DestroyImage               = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory             AllocateMemory             = nullptr;
    PFN_vkFreeMemory                 FreeMemory                 = nullptr;
    PFN_vkBindImageMemory            BindImageMemory            = nullptr;
    PFN_vkMapMemory                  MapMemory                  = nullptr;
    PFN_vkUnmapMemory                UnmapMemory                = nullptr;
    PFN_vkGetMemoryFdKHR             GetMemoryFdKHR             = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties =
        nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
};

bool LoadDeviceFns(VkInstance instance, VkDevice device, DeviceFns* fns)
{
    // vkGetInstanceProcAddr is linked, not dlopen'd: this binary links the
    // Vulkan loader like its siblings and never stands up an instance of its
    // own -- every handle it uses came out of the library.
    auto gipa  = (PFN_vkGetInstanceProcAddr)vkGetInstanceProcAddr;
    auto gdpa  = (PFN_vkGetDeviceProcAddr)gipa(instance,
                                               "vkGetDeviceProcAddr");
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
    // NOT under LOAD_DEV's hard failure: a device without
    // VK_KHR_external_memory_fd cannot run the OS-handle arms at all, and
    // that is a HOST fact, not a library defect. main() turns a null here
    // into a 77, so it can never be mistaken for a pass.
    fns->GetMemoryFdKHR =
        (PFN_vkGetMemoryFdKHR)gdpa(device, "vkGetMemoryFdKHR");
#undef LOAD_DEV
    fns->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
            instance, "vkGetPhysicalDeviceMemoryProperties");
    fns->GetPhysicalDeviceProperties2 =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(
            instance, "vkGetPhysicalDeviceProperties2");
    return (fns->GetPhysicalDeviceMemoryProperties != nullptr) &&
           (fns->GetPhysicalDeviceProperties2 != nullptr);
}

struct InputImage {
    VkImage        image      = VK_NULL_HANDLE;
    VkDeviceMemory memory     = VK_NULL_HANDLE;
    void*          mapped     = nullptr;
    VkDeviceSize   size       = 0;
    uint32_t       typeIndex  = UINT32_MAX;
    uint32_t       typeBits   = 0;
};

// A host-written LINEAR NV12 image on the library's own device, allocated
// EXPORTABLE as OPAQUE_FD and DEDICATED.
//
// EVERY FIELD HERE IS CONSTRAINED BY THE IMPORT SIDE, not chosen for taste.
// VkEncImportExternalImage rebuilds the VkImage from the descriptor as 2D /
// desc.format / extent / mip 1 / layers 1 / samples 1 / desc.tiling /
// desc.imageUsage / desc.imageFlags / desc.sharingMode, chaining
// VkExternalMemoryImageCreateInfo{OPAQUE_FD} and forcing
// initialLayout = VK_IMAGE_LAYOUT_UNDEFINED. If the exporting create info
// disagrees with that in any field, the import is a different image over the
// same bytes and the aliasing is not defined.
//
// initialLayout is UNDEFINED here for a second, independent reason: an image
// created with external memory CANNOT be PREINITIALIZED
// (VUID-VkImageCreateInfo-pNext-01443). The VK_IMAGE control arm uses this
// same image, deliberately, so that the two arms differ in handleType and in
// nothing else -- a control that used a differently-created image would leave
// initialLayout as an alternative explanation for a red subject.
bool CreateInputImage(const DeviceFns& fns, VkPhysicalDevice phys,
                      VkDevice device, bool exportable, InputImage* out)
{
    VkExternalMemoryImageCreateInfo extCI{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extCI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext         = exportable ? &extCI : nullptr;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ci.extent        = {kWidth, kHeight, 1};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_LINEAR;
    ci.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fns.CreateImage(device, &ci, nullptr, &out->image) != VK_SUCCESS) {
        std::printf("  SKIP-CAUSE: vkCreateImage(LINEAR NV12, exportable=%d) "
                    "failed\n", (int)exportable);
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
        std::printf("  SKIP-CAUSE: no host-visible+coherent memory type\n");
        fns.DestroyImage(device, out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        return false;
    }

    // DEDICATED, and it must be: the library performs every OS-handle import
    // as a dedicated allocation (the VkMemoryDedicatedAllocateInfo chain is
    // what avoids a known NVIDIA zero-fill defect on imported encode images),
    // so an import of a non-dedicated export is a mismatch the driver
    // reports late and unhelpfully.
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = out->image;

    VkExportMemoryAllocateInfo exportAI{
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exportAI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    exportAI.pNext       = &dedicated;

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext           = exportable ? (const void*)&exportAI
                                    : (const void*)&dedicated;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = typeIndex;
    if (fns.AllocateMemory(device, &ai, nullptr, &out->memory) != VK_SUCCESS) {
        std::printf("  SKIP-CAUSE: vkAllocateMemory(exportable=%d) failed\n",
                    (int)exportable);
        fns.DestroyImage(device, out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        return false;
    }
    if (fns.BindImageMemory(device, out->image, out->memory, 0) !=
        VK_SUCCESS) {
        std::printf("  SKIP-CAUSE: vkBindImageMemory failed\n");
        fns.DestroyImage(device, out->image, nullptr);
        fns.FreeMemory(device, out->memory, nullptr);
        out->image  = VK_NULL_HANDLE;
        out->memory = VK_NULL_HANDLE;
        return false;
    }

    // PERSISTENTLY mapped, which is the shape the residency declaration is
    // about: a caller that keeps a mapping and host-writes the same staging
    // image between submits. HOST_COHERENT, so there is nothing to flush.
    if (fns.MapMemory(device, out->memory, 0, req.size, 0, &out->mapped) !=
        VK_SUCCESS) {
        std::printf("  SKIP-CAUSE: vkMapMemory failed\n");
        fns.DestroyImage(device, out->image, nullptr);
        fns.FreeMemory(device, out->memory, nullptr);
        out->image  = VK_NULL_HANDLE;
        out->memory = VK_NULL_HANDLE;
        return false;
    }
    out->size      = req.size;
    out->typeIndex = typeIndex;
    out->typeBits  = req.memoryTypeBits;
    return true;
}

void DestroyInputImage(const DeviceFns& fns, VkDevice device, InputImage* in)
{
    if (in->mapped != nullptr) {
        fns.UnmapMemory(device, in->memory);
        in->mapped = nullptr;
    }
    if (in->image != VK_NULL_HANDLE) {
        fns.DestroyImage(device, in->image, nullptr);
        in->image = VK_NULL_HANDLE;
    }
    if (in->memory != VK_NULL_HANDLE) {
        fns.FreeMemory(device, in->memory, nullptr);
        in->memory = VK_NULL_HANDLE;
    }
}

// Real content, and a DIFFERENT pattern per frame. Two reasons, both about
// this suite specifically: a flat surface encodes to a degenerate bitstream,
// and identical frames would let a run in which the staging copy silently read
// stale bytes look exactly like a correct one.
void WriteFramePattern(InputImage* in, uint32_t frame)
{
    uint8_t* bytes = (uint8_t*)in->mapped;
    for (VkDeviceSize i = 0; i < in->size; i++) {
        bytes[i] = (uint8_t)(((i * 7u) ^ (i >> 9)) + (frame * 37u));
    }
}

void FillConfig(VkVideoEncoderConfig* config)
{
    config->sType              = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
    config->codec              = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    config->encodeWidth        = kWidth;
    config->encodeHeight       = kHeight;
    config->inputFormat        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    config->inputWidth         = kWidth;
    config->inputHeight        = kHeight;
    config->rateControlMode    = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    config->averageBitrate     = 5000000;
    config->maxBitrate         = 5000000;
    config->gopLength          = 30;
    config->consecutiveBFrames = 0;
    config->idrPeriod          = 30;
    config->frameRateNum       = 30;
    config->frameRateDen       = 1;
    config->deviceId           = -1;
    config->disableFileOutput  = VK_TRUE;
}

struct Arm {
    const char*                      flag;
    const char*                      what;
    VkVideoEncoderExternalHandleType handleType;
    VkVideoEncoderInputResidency     residency;
    VkImageLayout                    defaultLayout;
    // UNDEFINED here is THE SENTINEL, which the public contract defines as
    // "as declared at registration" -- not a missing value.
    VkImageLayout                    perFrameLayout;
    uint64_t                         expectForeign;
    uint64_t                         expectLocal;
};

const Arm kArms[] = {
    {"--local-opaque-fd",
     "SUBJECT -- OPAQUE_FD self-import declaring RESIDENCY_LOCAL",
     VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD,
     VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL,
     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_UNDEFINED,
     /*foreign=*/0, /*local=*/kFrames},

    {"--local-vk-image",
     "CONTROL -- the IDENTICAL image and declaration, registered as VK_IMAGE",
     VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE,
     VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL,
     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_UNDEFINED,
     /*foreign=*/0, /*local=*/kFrames},

    {"--foreign-opaque-fd",
     "NEGATIVE CONTROL -- OPAQUE_FD declaring RESIDENCY_FOREIGN",
     VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD,
     VK_VIDEO_ENCODER_INPUT_RESIDENCY_FOREIGN,
     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_UNDEFINED,
     /*foreign=*/kFrames, /*local=*/0},

    {"--auto-opaque-fd",
     "PIN -- OPAQUE_FD leaving residency zero (AUTO); FOREIGN is still DERIVED",
     VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD,
     VK_VIDEO_ENCODER_INPUT_RESIDENCY_AUTO,
     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_UNDEFINED,
     /*foreign=*/kFrames, /*local=*/0},

    {"--preinit-opaque-fd",
     "CHROMIUM MIRROR -- OPAQUE_FD + LOCAL declaring PREINITIALIZED per frame",
     VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD,
     VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL,
     VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_PREINITIALIZED,
     /*foreign=*/0, /*local=*/kFrames},

    // THE LAYOUT-TABLE HOLE. Everything above declares GENERAL or
    // PREINITIALIZED, and those are the only two input layouts the transition
    // table was ever exercised with on this lane. A caller that declares
    // TRANSFER_SRC_OPTIMAL -- which the public header invites, which
    // VkVideoEncoder.cpp:1290-1294 and :1513-1517 both explicitly promise
    // round-trips, and which is the natural declaration for a staging source
    // the caller last used as a copy source -- produces the acquire pair
    // (TRANSFER_SRC_OPTIMAL -> TRANSFER_SRC_OPTIMAL), because this arm's
    // newLayout for the staging copy IS TRANSFER_SRC_OPTIMAL.
    //
    // That pair had NO ARM. It fell to the table's terminal else, which is
    // `throw std::invalid_argument` wherever __cpp_exceptions is defined --
    // and this standalone CMake build defines it (flags.make carries no
    // -fno-exceptions), with no handler anywhere on StageInputFrame's call
    // stack. So this arm did not fail an assertion: the process ABORTED.
    //
    // The counters are the same 0/kFrames as the LOCAL arms above because the
    // residency decision is not what is under test here -- reaching the end
    // of eight frames AT ALL is. Kept identical on purpose: if a future change
    // makes this arm take a foreign acquire, that is also a defect and this
    // catches it too.
    {"--local-tso-opaque-fd",
     "LAYOUT HOLE -- OPAQUE_FD + LOCAL declaring TRANSFER_SRC_OPTIMAL",
     VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD,
     VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL,
     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_UNDEFINED,
     /*foreign=*/0, /*local=*/kFrames},
};

}  // namespace

int main(int argc, const char** argv)
{
    const Arm* arm = nullptr;
    bool validate  = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--validate") == 0) {
            validate = true;
            continue;
        }
        for (const Arm& a : kArms) {
            if (std::strcmp(argv[i], a.flag) == 0) {
                arm = &a;
            }
        }
    }
    if (arm == nullptr) {
        std::printf("usage: %s <arm> [--validate]\n  arms:\n", argv[0]);
        for (const Arm& a : kArms) {
            std::printf("    %-22s %s\n", a.flag, a.what);
        }
        // NOT 77. An unrecognised argument is a harness defect, and a harness
        // defect that reports SKIP is how a suite comes to sit green having
        // run nothing.
        return 1;
    }

    std::printf("Encoder-ext INPUT RESIDENCY: %s\n", arm->what);
    std::printf("  handleType=%d residency=%d defaultLayout=%d "
                "perFrameLayout=%d frames=%u\n",
                (int)arm->handleType, (int)arm->residency,
                (int)arm->defaultLayout, (int)arm->perFrameLayout, kFrames);
    // Layer provenance, echoed for the same reason the adopt suite echoes it:
    // a validation claim without the layer configuration that produced it is
    // not a measurement. The wording avoids the bare token used by the CTest
    // skip regex.
    {
        const char* layerPath     = std::getenv("VK_LAYER_PATH");
        const char* layerSettings = std::getenv("VK_LAYER_SETTINGS_PATH");
        std::printf("  VK_LAYER_PATH=%s\n",
                    layerPath ? layerPath : "(unset)");
        std::printf("  VK_LAYER_SETTINGS_PATH=%s\n",
                    layerSettings ? layerSettings : "(unset)");
    }
    std::printf("------------------------------------------------\n");

    int rc = 0;
    {
        VkSharedBaseObj<VulkanVideoEncoderExt> encoder;
        if ((CreateVulkanVideoEncoderExt(encoder) != VK_SUCCESS) || !encoder) {
            std::printf("SKIP: CreateVulkanVideoEncoderExt failed\n");
            return 77;
        }

        VkVideoEncoderConfig config = {};
        FillConfig(&config);
        if (validate) {
            config.validate = VK_TRUE;
        }
        const VkResult init = encoder->InitializeExt(config);
        if (init != VK_SUCCESS) {
            // A SKIP, and only here: no encode-capable device, no codec, no
            // encode queue. Every failure BELOW this line is an assertion,
            // because by then the library has stood a working device up.
            std::printf("SKIP: InitializeExt failed (%d)\n", (int)init);
            encoder.reset();
            return 77;
        }

        const VkDevice         device = encoder->GetVkDevice();
        const VkPhysicalDevice phys   = encoder->GetVkPhysicalDevice();
        const VkInstance       inst   = encoder->GetVkInstance();
        DeviceFns fns;
        if ((device == VK_NULL_HANDLE) || (phys == VK_NULL_HANDLE) ||
            (inst == VK_NULL_HANDLE) || !LoadDeviceFns(inst, device, &fns)) {
            std::printf("SKIP: could not resolve device entry points\n");
            encoder.reset();
            return 77;
        }

        const bool isOsHandle =
            (arm->handleType !=
             VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE);
        if (isOsHandle && (fns.GetMemoryFdKHR == nullptr)) {
            std::printf("SKIP: the library's device has no vkGetMemoryFdKHR; "
                        "the OS-handle arms cannot run here\n");
            encoder.reset();
            return 77;
        }

        // THE IMAGE IS ALWAYS EXPORTABLE, on every arm including VK_IMAGE.
        // That is what makes the control a control: the two registrations
        // describe the SAME VkImage with the SAME create info, and differ
        // only in which door they come through.
        InputImage input;
        if (!CreateInputImage(fns, phys, device, /*exportable=*/true,
                              &input)) {
            std::printf("SKIP: could not create the input image\n");
            encoder.reset();
            return 77;
        }
        WriteFramePattern(&input, 0);

        int fd = -1;
        if (isOsHandle) {
            VkMemoryGetFdInfoKHR getFd{
                VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
            getFd.memory     = input.memory;
            getFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            const VkResult fr = fns.GetMemoryFdKHR(device, &getFd, &fd);
            if ((fr != VK_SUCCESS) || (fd < 0)) {
                std::printf("SKIP: vkGetMemoryFdKHR failed (%d)\n", (int)fr);
                DestroyInputImage(fns, device, &input);
                encoder.reset();
                return 77;
            }
            std::printf("  exported OPAQUE_FD %d from the LIBRARY's own "
                        "device (self-import)\n", fd);
        }

        VkVideoEncoderExternalImageDescriptor desc = {};
        desc.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
        desc.handleType  = arm->handleType;
        desc.format      = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        desc.width       = kWidth;
        desc.height      = kHeight;
        desc.tiling      = VK_IMAGE_TILING_LINEAR;
        desc.imageUsage  = (VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        desc.planeCount  = 0;
        desc.residency   = arm->residency;
        desc.defaultLayout = arm->defaultLayout;
        if (isOsHandle) {
            // The exporter's own allocation parameters, which an OPAQUE_FD
            // import REQUIRES rather than prefers
            // (VUID-VkMemoryAllocateInfo-allocationSize-01742): the ext layer
            // sets exporterIndexExact for this handle type, so a guessed
            // index or a zero size is a refusal, not a fallback. This test
            // has all three because it performed the export.
            desc.allocationSize  = (uint64_t)input.size;
            desc.memoryTypeIndex = input.typeIndex;
            desc.memoryTypeBits  = input.typeBits;
            // PROVENANCE. Propagated rather than left zero so the descriptor
            // is the shape a real consumer sends; a zeroed deviceUUID is
            // accepted with a warning and would make the multi-GPU check
            // vacuous on this arm.
            VkPhysicalDeviceIDProperties idProps{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 props2{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            props2.pNext = &idProps;
            fns.GetPhysicalDeviceProperties2(phys, &props2);
            std::memcpy(desc.deviceUUID, idProps.deviceUUID, VK_UUID_SIZE);
            std::memcpy(desc.driverUUID, idProps.driverUUID, VK_UUID_SIZE);
        } else {
            desc.existingImage = input.image;
        }

        VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        VkVideoEncoderStatus   regStatus = {};
        regStatus.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS;
        const VkVideoEncoderStatusCode reg = encoder->RegisterImageResource(
            desc, (uint64_t)(int64_t)(isOsHandle ? fd : -1), &resource,
            &regStatus);

        // AN ASSERTION, NOT A SKIP -- the same reclassification the adopt
        // suite argues at length. The host question was settled above:
        // CreateInputImage built this very image on this very device, and
        // (on the OS arms) vkGetMemoryFdKHR already exported from it. A
        // library refusal from here on is a library fact.
        Check((reg == VK_VIDEO_ENCODER_STATUS_SUCCESS) &&
                  (resource != VK_VIDEO_ENCODER_RESOURCE_NULL),
              "RegisterImageResource", "status " + I64((long long)reg));
        if (isOsHandle) {
            // The ownership echo, asserted rather than assumed: ownership is
            // TRANSFER by zero-init, so the library consumes the fd on EVERY
            // exit including failure, and the test must not close it again.
            Check(regStatus.handlesConsumed == VK_TRUE,
                  "the library consumed the transferred fd",
                  "handlesConsumed was VK_FALSE");
            fd = -1;
        }

        uint32_t captured  = 0;
        uint32_t submitted = 0;
        uint64_t bytes     = 0;
        if (resource != VK_VIDEO_ENCODER_RESOURCE_NULL) {
            for (uint32_t f = 0; f < kFrames; f++) {
                // THE REUSE. A new pattern is host-written into the SAME
                // mapping before every frame, which is the whole point of the
                // shape: the image the library staged from last frame is the
                // image being rewritten now.
                WriteFramePattern(&input, f);

                VkVideoEncoderFrameSubmitInfo info = {};
                info.sType    = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS;
                info.resource = resource;
                info.frameId  = f;
                info.pts      = f;
                info.qpOverride    = -1;
                info.currentLayout = arm->perFrameLayout;

                VkVideoEncoderStatusCode status =
                    encoder->SubmitRegisteredFrame(info, nullptr);
                for (int retry = 0;
                     (status == VK_VIDEO_ENCODER_STATUS_NOT_READY) &&
                     (retry < 2000);
                     retry++) {
                    VkVideoEncodeResult drained;
                    while (encoder->AcquireNextEncodedFrame(drained) ==
                           VK_SUCCESS) {
                        captured++;
                        bytes += drained.bitstreamSize;
                        encoder->ReleaseEncodedFrame(drained.frameId);
                    }
                    status = encoder->SubmitRegisteredFrame(info, nullptr);
                    if (status == VK_VIDEO_ENCODER_STATUS_NOT_READY) {
                        struct timespec ts = {0, 1000000};  // 1 ms
                        nanosleep(&ts, nullptr);
                    }
                }
                if (status != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
                    Check(false, "SubmitRegisteredFrame",
                          "frame " + I64(f) + " status " +
                              I64((long long)status));
                    break;
                }
                submitted++;

                VkVideoEncodeResult drained;
                while (encoder->AcquireNextEncodedFrame(drained) ==
                       VK_SUCCESS) {
                    captured++;
                    bytes += drained.bitstreamSize;
                    encoder->ReleaseEncodedFrame(drained.frameId);
                }
            }
        }

        // DRAIN BEFORE READING THE COUNTERS, and before freeing anything the
        // submitted work reads. DrainPendingFrames joins the encoder and
        // assembly threads, so every submitted frame has been through
        // StageInputFrame -- and therefore counted -- by the time it returns.
        // It does NOT clear m_encoder, so the side-channel below still has
        // something to answer from; Flush() and teardown DO, which is why
        // neither is called first.
        Check(encoder->DrainPendingFrames() == VK_SUCCESS,
              "DrainPendingFrames",
              "the encoder could not flush its in-flight work");
        {
            VkVideoEncodeResult drained;
            while (encoder->AcquireNextEncodedFrame(drained) == VK_SUCCESS) {
                captured++;
                bytes += drained.bitstreamSize;
                encoder->ReleaseEncodedFrame(drained.frameId);
            }
        }

        // ---- THE OBSERVABLE ----
        VkVideoEncoderInputResidencyInfo residencyInfo = {};
        residencyInfo.sType =
            VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_RESIDENCY_INFO;
        residencyInfo.pNext = nullptr;
        VkVideoEncoderCompletionInfo completion = {};
        completion.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_COMPLETION_INFO;
        completion.pNext = &residencyInfo;
        const VkResult ci = encoder->GetCompletionInfo(&completion);
        Check(ci == VK_SUCCESS,
              "GetCompletionInfo accepts a chained "
              "VkVideoEncoderInputResidencyInfo",
              "returned " + I64((long long)ci));

        std::printf("  submitted=%u captured=%u bytes=%llu "
                    "foreignAcquireCount=%llu localAcquireCount=%llu\n",
                    submitted, captured, (unsigned long long)bytes,
                    (unsigned long long)residencyInfo.foreignAcquireCount,
                    (unsigned long long)residencyInfo.localAcquireCount);

        Check(submitted == kFrames, "every frame was submitted",
              U64(submitted) + " of " + U64(kFrames));
        Check(captured == kFrames, "every frame produced a bitstream",
              U64(captured) + " of " + U64(kFrames));
        Check(bytes > 0, "the bitstreams are non-empty", "0 bytes");

        // THE TWO ASSERTIONS THIS SUITE EXISTS FOR. Both sides, not one: an
        // arm that only checked foreignAcquireCount == 0 would also pass on a
        // build where no frame reached the staging tier at all.
        Check(residencyInfo.foreignAcquireCount == arm->expectForeign,
              "foreignAcquireCount",
              "expected " + U64(arm->expectForeign) + ", got " +
                  U64(residencyInfo.foreignAcquireCount));
        Check(residencyInfo.localAcquireCount == arm->expectLocal,
              "localAcquireCount",
              "expected " + U64(arm->expectLocal) + ", got " +
                  U64(residencyInfo.localAcquireCount));

        // UNREGISTER BEFORE DESTROYING, which the public header requires in
        // as many words: a driver may recycle the handle value and a
        // surviving registration would then name freed memory.
        if (resource != VK_VIDEO_ENCODER_RESOURCE_NULL) {
            Check(encoder->UnregisterImageResource(resource) ==
                      VK_VIDEO_ENCODER_STATUS_SUCCESS,
                  "UnregisterImageResource",
                  "the registration id did not resolve");
        }
        DestroyInputImage(fns, device, &input);
        if (fd >= 0) {
            // Only reachable if registration never happened, i.e. the library
            // never took the transfer.
            close(fd);
        }
        encoder.reset();
    }

    std::printf("------------------------------------------------\n");
    if (g_failures == 0) {
        std::printf("PASSED : %d checks, 0 failures\n", g_checks);
    } else {
        std::printf("FAILED : %d checks, %d failures\n", g_checks, g_failures);
        rc = 1;
    }
    return rc;
}
