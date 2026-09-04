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
 * ADOPT-MODE SESSION: the library creates its OWN VkDevice on an instance and
 * a physical device the EMBEDDER already owns.
 *
 * WHY THIS TEST EXISTS. Every external-handle field this API has --
 * externalInstance / externalPhysicalDevice / externalDevice and the two queue
 * family indices -- had ZERO coverage anywhere in the tree. A grep for
 * externalDevice under vk_video_encoder/test returned nothing, so the only
 * thing exercising them was the Chromium embedder, out-of-tree, in the
 * configuration where all three handles are supplied.
 *
 * WHAT ADOPT IS, and how it differs from the configuration next to it:
 *
 *   IMPORT externalInstance + externalPhysicalDevice + externalDevice.
 *          The library encodes on the EMBEDDER's logical device and binds the
 *          embedder's queue families.
 *
 *   ADOPT  externalInstance + externalPhysicalDevice, externalDevice NULL.
 *          The library BORROWS the instance and is PINNED to the physical
 *          device, then runs vkCreateDevice itself and probes its own queue
 *          families. This is the recommended shape: the embedder hands over
 *          identity, never a logical device.
 *
 *   OWN    no external handles at all. Library creates everything. Unchanged,
 *          and run here as the control.
 *
 * THE FOUR THINGS THIS PROVES, none of which a device-free test can:
 *
 *  1. IDENTITY. GetVkInstance() is the embedder's instance,
 *     GetVkPhysicalDevice() is the embedder's physical device, and
 *     GetVkDevice() is NEITHER null nor anything the embedder handed over --
 *     there is no embedder device to confuse it with, because this test
 *     deliberately never creates one.
 *
 *  2. IT ENCODES. Registration, submit and bitstream capture over a real
 *     queue on the library-created device. An ADOPT path that stands a device
 *     up and then cannot encode on it is not an embedding path at all.
 *
 *  3. THE PIN IS LOAD-BEARING. --conflict supplies the physical device AND a
 *     deviceId that names a different GPU. The library must REFUSE, not
 *     silently select some other enumerated device. PLAN:427 makes
 *     physical-device pinning REQUIRED once a device is named; a pin that
 *     falls back is not a pin, and on a single-GPU host refusal is the only
 *     observable that distinguishes the two.
 *
 *  4. VALIDATION OVER A BORROWED INSTANCE. --validate asks for
 *     config.validate on an ADOPTED instance. The library must not assume the
 *     borrowed instance enabled the layers and instance extensions that the
 *     LOADER merely offers -- it never created that instance and cannot have
 *     enabled anything on it. See the note in InitVulkanDevice.
 *
 * WHY THE EMBEDDER HERE NEVER CREATES A VkDevice. If it did, this test could
 * pass on a library that silently ignored externalDevice==NULL and reused some
 * device of the caller's -- and it would also stop being a test of the shape
 * the user actually asked for ("the library always creates its OWN VkDevice").
 * Every device-level call this file makes goes through GetVkDevice(), i.e.
 * through the device the LIBRARY created. That is deliberate: it is what makes
 * "the library stood up a working device on a borrowed physical device"
 * observable at all.
 *
 * Exits 77 (CTest SKIP) with no encode-capable Vulkan device, like its
 * siblings. 0 all assertions held, 1 an assertion failed.
 */

#include "vulkan_video_encoder_ext.h"

// The public header reaches the Xlib platform headers, whose macros collide
// with ordinary identifiers. Same scrub, same reason, as the sibling tests.
#undef Status
#undef None
#undef Bool
#undef Window

#include <dlfcn.h>
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

std::string U64Hex(unsigned long long v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", v);
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
const uint32_t kFrames = 24;

// ---------------------------------------------------------------------------
// THE EMBEDDER. A VkInstance and a VkPhysicalDevice, and deliberately NOTHING
// else -- this stands in for a compositor that has a Vulkan implementation up
// but is not going to lend its logical device to anyone.
// ---------------------------------------------------------------------------
struct Embedder {
    void*                     lib      = nullptr;
    PFN_vkGetInstanceProcAddr gipa     = nullptr;
    VkInstance                instance = VK_NULL_HANDLE;
    VkPhysicalDevice          phys     = VK_NULL_HANDLE;
    uint32_t                  deviceID = 0;
    char                      name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};

    PFN_vkDestroyInstance DestroyInstance = nullptr;
};

bool BuildEmbedder(Embedder* e, bool withValidationLayer)
{
    e->lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (e->lib == nullptr) {
        e->lib = dlopen("libvulkan.so", RTLD_NOW);
    }
    if (e->lib == nullptr) {
        std::printf("  SKIP-CAUSE: dlopen(libvulkan) failed: %s\n", dlerror());
        return false;
    }
    e->gipa = (PFN_vkGetInstanceProcAddr)dlsym(e->lib,
                                               "vkGetInstanceProcAddr");
    if (e->gipa == nullptr) {
        std::printf("  SKIP-CAUSE: no vkGetInstanceProcAddr\n");
        return false;
    }

    auto createInstance =
        (PFN_vkCreateInstance)e->gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (createInstance == nullptr) {
        std::printf("  SKIP-CAUSE: no vkCreateInstance\n");
        return false;
    }

    // A BARE instance by default: no layers, no instance extensions.
    // Everything the library needs off an adopted instance --
    // GetPhysicalDeviceProperties2, GetPhysicalDeviceQueueFamilyProperties2,
    // GetPhysicalDeviceFeatures2, the external-memory capability queries -- is
    // Vulkan 1.1 core, so a bare 1.3 instance is a legitimate embedder. It is
    // also the STRICTEST embedder: anything the library assumes an adopted
    // instance enabled for it fails here rather than being masked by a richer
    // instance. That is exactly what the --validate arm is for.
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "EncoderExtAdoptEmbedder";
    app.apiVersion       = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;

    // THE EMBEDDER INSTANCE IS BARE ON EVERY ARM, INCLUDING --validate, and
    // that is deliberate rather than lazy.
    //
    // An earlier draft enabled VK_LAYER_KHRONOS_validation here so the
    // --validate arm would have a layer to report through. The cost was that
    // the arm SKIPPED on any host where the layer is not on the loader's
    // search path -- which is every CI runner and, as it turned out, this
    // project's own GPU host unless VK_LAYER_PATH is exported by hand. A
    // red-then-green test that skips on the machine it guards proves nothing.
    //
    // It is also unnecessary. What --validate exercises is the LIBRARY's
    // reaction to `config.validate` over an instance IT DID NOT CREATE, and a
    // bare instance is the sharpest version of that: it has no layer and no
    // debug extension, so every assumption the library might make about what
    // the loader offers is false of this instance. Both failure modes the fix
    // addresses reproduce on it, and which one fires depends only on whether
    // the host happens to have the layer installed:
    //   * layer present in the LOADER -- the pre-fix loader-level checks pass,
    //     and InitDebugReport walks into a null dispatch entry: SIGSEGV.
    //   * layer absent from the loader -- the pre-fix CheckAllInstanceLayers
    //     fails the whole session with VK_ERROR_LAYER_NOT_PRESENT because the
    //     LOADER lacks a layer that has nothing to do with the borrowed
    //     instance.
    // Either way the arm is red before the fix and green after, on any host.
    //
    // The one arm that genuinely needs an installed layer is the
    // --own-validate control, where the LIBRARY creates the instance and asks
    // for the layer by name; that one skips (77) without it, which is correct
    // -- it is a control, and a missing layer really does make it unable to
    // say anything.
    (void)withValidationLayer;

    if (createInstance(&ici, nullptr, &e->instance) != VK_SUCCESS) {
        std::printf("  SKIP-CAUSE: vkCreateInstance failed\n");
        return false;
    }
    e->DestroyInstance =
        (PFN_vkDestroyInstance)e->gipa(e->instance, "vkDestroyInstance");

    auto enumPhys = (PFN_vkEnumeratePhysicalDevices)e->gipa(
        e->instance, "vkEnumeratePhysicalDevices");
    auto getProps = (PFN_vkGetPhysicalDeviceProperties)e->gipa(
        e->instance, "vkGetPhysicalDeviceProperties");
    auto getQueues = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)e->gipa(
        e->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if ((enumPhys == nullptr) || (getProps == nullptr) ||
        (getQueues == nullptr)) {
        std::printf("  SKIP-CAUSE: missing instance-level entry points\n");
        return false;
    }

    uint32_t count = 0;
    enumPhys(e->instance, &count, nullptr);
    if (count == 0) {
        std::printf("  SKIP-CAUSE: no physical devices\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    enumPhys(e->instance, &count, devices.data());

    // The embedder picks the first ENCODE-CAPABLE device, which is the choice
    // a compositor would make on the caller's behalf.
    for (uint32_t i = 0; i < count; i++) {
        uint32_t qcount = 0;
        getQueues(devices[i], &qcount, nullptr);
        if (qcount == 0) {
            continue;
        }
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        getQueues(devices[i], &qcount, qprops.data());
        bool encodes = false;
        for (uint32_t q = 0; q < qcount; q++) {
            if ((qprops[q].queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) != 0) {
                encodes = true;
                break;
            }
        }
        if (!encodes) {
            continue;
        }
        VkPhysicalDeviceProperties props{};
        getProps(devices[i], &props);
        e->phys     = devices[i];
        e->deviceID = props.deviceID;
        std::snprintf(e->name, sizeof(e->name), "%s", props.deviceName);
        return true;
    }
    std::printf("  SKIP-CAUSE: no encode-capable physical device\n");
    return false;
}

void TearDownEmbedder(Embedder* e)
{
    if ((e->instance != VK_NULL_HANDLE) && (e->DestroyInstance != nullptr)) {
        e->DestroyInstance(e->instance, nullptr);
        e->instance = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Device-level entry points, resolved off the LIBRARY's device.
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
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties =
        nullptr;
};

bool LoadDeviceFns(PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
                   VkDevice device, DeviceFns* fns)
{
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
#undef LOAD_DEV
    fns->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
            instance, "vkGetPhysicalDeviceMemoryProperties");
    return (fns->GetPhysicalDeviceMemoryProperties != nullptr);
}

struct InputImage {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

// A host-written LINEAR NV12 image, on the LIBRARY's device, registered as
// TRANSFER_SRC so the registration routes STAGED. Staged is the right arm
// here: it is the one that copies through a real queue on the device under
// test, so a device that came up but cannot actually submit work shows up.
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
        // Same lifetime rule as the registration exit in EncodeAndDrain: the
        // image already exists on the library's device, and main() answers
        // this false by resetting the encoder, which destroys that device. A
        // live VkImage at that point is a vkDestroyDevice VUID violation --
        // and unlike the registration exit, THIS one legitimately stays a
        // skip, so the violation would ride out on a green run.
        fns.DestroyImage(device, out->image, nullptr);
        out->image  = VK_NULL_HANDLE;
        out->memory = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = typeIndex;
    if (fns.AllocateMemory(device, &ai, nullptr, &out->memory) != VK_SUCCESS) {
        std::printf("  ERROR: vkAllocateMemory failed\n");
        fns.DestroyImage(device, out->image, nullptr);
        out->image  = VK_NULL_HANDLE;
        out->memory = VK_NULL_HANDLE;
        return false;
    }
    if (fns.BindImageMemory(device, out->image, out->memory, 0) != VK_SUCCESS) {
        std::printf("  ERROR: vkBindImageMemory failed\n");
        // Image then memory, matching the success path below. The reverse is
        // equally legal -- vkFreeMemory has no ordering constraint against
        // vkDestroyImage even for bound memory, only against submitted work --
        // so there is nothing to justify, and uniformity is worth more.
        fns.DestroyImage(device, out->image, nullptr);
        fns.FreeMemory(device, out->memory, nullptr);
        out->image  = VK_NULL_HANDLE;
        out->memory = VK_NULL_HANDLE;
        return false;
    }

    // Real content, not zeros: a flat surface encodes to a degenerate
    // bitstream and would make "the encode actually ran" hard to assert.
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

// Encode kFrames and report how many bitstream payloads came back.
// Returns false only on an infrastructure failure (which is a SKIP cause);
// assertion failures go through Check().
bool EncodeAndDrain(VkSharedBaseObj<VulkanVideoEncoderExt>& encoder,
                    const DeviceFns& fns, VkPhysicalDevice phys,
                    VkDevice device, uint32_t* framesOut, uint64_t* bytesOut)
{
    InputImage input;
    if (!CreateInputImage(fns, phys, device, &input)) {
        return false;
    }

    VkVideoEncoderExternalImageDescriptor desc = {};
    desc.sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
    desc.handleType    = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
    desc.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    desc.width         = kWidth;
    desc.height        = kHeight;
    desc.tiling        = VK_IMAGE_TILING_LINEAR;
    desc.imageUsage    = (VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    desc.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    desc.planeCount    = 0;
    desc.residency     = VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
    desc.defaultLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    desc.existingImage = input.image;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode regStatus =
        encoder->RegisterImageResource(desc, 0, &resource, nullptr);
    if ((regStatus != VK_VIDEO_ENCODER_STATUS_SUCCESS) ||
        (resource == VK_VIDEO_ENCODER_RESOURCE_NULL)) {
        // AN ASSERTION, NOT A SKIP.
        //
        // A refusal reaching this line is a LIBRARY answer, not a HOST
        // condition, so it must not take `return false`: this function's
        // contract maps that to "infrastructure failure" and main() maps it to
        // `return 77` -- ctest Skipped, which does not fail a run, and a
        // library refusal that does not fail a run is not asserted at all.
        // FIVE of the seven registered
        // arms reach this line (the two --*conflict arms refuse earlier), so
        // a registration regression could turn this whole suite green by
        // skipping it, and an ADOPT-only regression would skip the three
        // adoption arms while the two OWN controls stayed green -- destroying
        // the exact comparison this test exists to make.
        //
        // The host-capability question was already settled ABOVE, and that is
        // what makes the reclassification honest rather than merely stricter:
        // CreateInputImage builds this very LINEAR NV12 image on this very
        // device, and ITS failure is still the skip. By the time control
        // reaches this line the device has demonstrably created the image we
        // are handing straight back to the library.
        //
        // ON THIS DESCRIPTOR THE REGISTRATION MAKES NO DEVICE CALL AT ALL,
        // which is what makes the reclassification safe rather than merely
        // defensible. BuildRegisteredViewLocked looks like the device-facing
        // step and is not, for this input: CreateFromExternal and
        // VulkanVideoImagePoolNode::CreateExternal are plain allocations with
        // no Vulkan entry point, and the combined image view is SKIPPED --
        // VkImageResource declines to create a view over an image whose usage
        // holds no view-compatible bit (VUID-VkImageViewCreateInfo-image-04441)
        // and this descriptor declares VK_IMAGE_USAGE_TRANSFER_SRC_BIT alone,
        // while the per-plane views additionally need
        // VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, which it does not set. So every
        // refusal reachable from here -- descriptor validation, the 4096-slot
        // resource limit, a null existingImage -- is decided in library code
        // before the driver is touched.
        //
        // ONE GENUINE DEVICE-DEPENDENT REFUSAL EXISTS, and it is named rather
        // than hidden: the descriptor extent must cover the session's coded
        // extent, and InitializeExt clamps encodeWidth/encodeHeight UP to
        // videoCapabilities.minCodedExtent. A device reporting a minCodedExtent
        // larger than this test's image would raise the session above it and
        // refuse EXTENT_INVALID. No real encode hardware does; if one ever
        // does, this line is where it will announce itself, which is better
        // than a silent skip.
        //
        // The same file already makes this argument three times, for context
        // creation, for CreateVulkanVideoEncoderExtOnContext, and for
        // InitializeExt(ADOPT). This site APPLIES IT MORE BROADLY than any of
        // them, which is worth saying rather than leaving to be noticed: the
        // first two exist only on the --context* arms, and the third
        // explicitly carves the OWN arms back out into a skip. This line is
        // reached by --own and --own-validate too. That widening is
        // deliberate -- by here InitializeExt has already SUCCEEDED on every
        // arm, so "can this host encode?" is settled and a refusal can no
        // longer be answering it.
        // Status only: the resource half would be a constant. |resource| is
        // initialised to VK_VIDEO_ENCODER_RESOURCE_NULL right here in the
        // test, and a SUCCESS return can never overwrite it with 0
        // (MakeResourceId ORs index + 1), so inside this branch it is always
        // 0 and reporting it says nothing. (The library also zeroes
        // *outResource itself, but only after three earlier returns, so that
        // is not what makes this safe -- the initialiser above is.)
        Check(false, "RegisterImageResource(VK_IMAGE)",
              "status " + I64((long long)regStatus));

        // Unwind the input image on this exit too. InputImage is a plain POD
        // with no destructor and only the success path at the bottom of this
        // function frees it, so the old `return false` leaked both handles --
        // and then main() reset the encoder, destroying the library-created
        // VkDevice while one of its images was still live. That is a
        // vkDestroyDevice VUID violation (every child object must be
        // destroyed first). Only --own-validate could ever have reported it:
        // BuildEmbedder is called with withValidationLayer=false on every arm
        // including --validate, deliberately, so the borrowed instance carries
        // no layer and only the arm where the LIBRARY creates the instance has
        // one live.
        if (input.image != VK_NULL_HANDLE) {
            fns.DestroyImage(device, input.image, nullptr);
        }
        if (input.memory != VK_NULL_HANDLE) {
            fns.FreeMemory(device, input.memory, nullptr);
        }
        *framesOut = 0;
        *bytesOut  = 0;
        // TRUE: the contract this function documents is "false means SKIP",
        // and this is not one. main() goes on to fail `frames > 0` and
        // `bytes > 0` as well, which is accurate -- nothing encoded.
        return true;
    }

    uint32_t captured  = 0;
    uint64_t bytes     = 0;
    uint32_t submitted = 0;
    for (uint32_t f = 0; f < kFrames; f++) {
        VkVideoEncoderFrameSubmitInfo info = {};
        info.sType         = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS;
        info.resource      = resource;
        info.frameId       = f;
        info.pts           = f;
        info.qpOverride    = -1;
        info.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkVideoEncoderStatusCode status =
            encoder->SubmitRegisteredFrame(info, nullptr);
        for (int retry = 0;
             (status == VK_VIDEO_ENCODER_STATUS_NOT_READY) && (retry < 2000);
             retry++) {
            VkVideoEncodeResult drained;
            while (encoder->AcquireNextEncodedFrame(drained) == VK_SUCCESS) {
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
                  "frame " + I64(f) + " status " + I64((long long)status));
            break;
        }
        submitted++;

        VkVideoEncodeResult drained;
        while (encoder->AcquireNextEncodedFrame(drained) == VK_SUCCESS) {
            captured++;
            bytes += drained.bitstreamSize;
            encoder->ReleaseEncodedFrame(drained.frameId);
        }
    }

    // Flush whatever is still in flight.
    for (int spin = 0; (spin < 4000) && (captured < submitted); spin++) {
        VkVideoEncodeResult drained;
        bool got = false;
        while (encoder->AcquireNextEncodedFrame(drained) == VK_SUCCESS) {
            captured++;
            bytes += drained.bitstreamSize;
            encoder->ReleaseEncodedFrame(drained.frameId);
            got = true;
        }
        if (!got) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, nullptr);
        }
    }

    // THEN THE REAL DRAIN, because the poll above is only a poll. It is
    // bounded at 4000 spins and can exit with captured < submitted, and the
    // handles freed below are read by submitted work: destroying the image
    // while a frame still references it violates
    // VUID-vkDestroyImage-image-01000, and freeing its memory violates
    // VUID-vkFreeMemory-memory-00677 -- both, not just the first.
    //
    // DrainPendingFrames is the library's own answer to exactly this and the
    // test simply never called it. It flushes the deferred GOP tail and waits
    // for in-flight encodes WITHOUT releasing the encoder, by joining the
    // encoder-queue and assembly threads, so it needs no vkDeviceWaitIdle --
    // which DeviceFns does not load -- and no external synchronisation care
    // around the library's own submitting thread. It is documented threading
    // class (a), which this call site satisfies: single-threaded, not inside
    // a completion callback.
    //
    // It can release frames the poll never saw, so collect once more
    // afterwards -- otherwise this change would silently lower the frame
    // count it is meant to make trustworthy.
    Check(encoder->DrainPendingFrames() == VK_SUCCESS, "DrainPendingFrames",
          "the encoder could not flush its in-flight work");
    {
        VkVideoEncodeResult drained;
        while (encoder->AcquireNextEncodedFrame(drained) == VK_SUCCESS) {
            captured++;
            bytes += drained.bitstreamSize;
            encoder->ReleaseEncodedFrame(drained.frameId);
        }
    }

    // UNREGISTER BEFORE DESTROYING, which is the order the public header
    // requires in as many words: "the caller must Unregister BEFORE
    // destroying it -- a driver may recycle the handle value, and a surviving
    // registration would then name freed memory". This call was simply
    // missing; the test destroyed the image with the registration still live.
    //
    // NOT a bare call: this whole change exists because a library status was
    // being thrown away, so throwing this one away would be the same mistake
    // one screen further down. Unregister answers RESOURCE_UNKNOWN if the id
    // does not resolve, which is precisely the generation/slot regression
    // worth catching.
    //
    // Its detection power is bounded and the bound is worth knowing: the
    // deferred arm sets slot.retired without clearing slot.live, so a lookup
    // still succeeds and even a repeat Unregister answers SUCCESS -- which
    // does not match the header's "The id is invalid immediately on return".
    // That is a pre-existing library/header mismatch, not this test's to fix,
    // but it is why this assertion proves the id RESOLVED, not that the slot
    // retired.
    //
    // No null guard: |resource| is initialised non-null above and the only
    // branch that could leave it null already returned, so a guard here would
    // just tell a reader that null is reachable when it is not.
    Check(encoder->UnregisterImageResource(resource) ==
              VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "UnregisterImageResource", "the registration id did not resolve");
    if (input.image != VK_NULL_HANDLE) {
        fns.DestroyImage(device, input.image, nullptr);
    }
    if (input.memory != VK_NULL_HANDLE) {
        fns.FreeMemory(device, input.memory, nullptr);
    }

    *framesOut = captured;
    *bytesOut  = bytes;
    return true;
}

}  // namespace

int main(int argc, const char** argv)
{
    // --own-validate is the NEGATIVE CONTROL for the --validate arm. The
    // library's refusal to attach a debug callback must be scoped to an
    // IMPORTED instance; a fix that simply stopped attaching callbacks
    // altogether would make --validate green and would be indistinguishable
    // without this. Here the library owns the instance, so it created it with
    // whatever it needs and must still bring the callback up and encode.
    const bool ownValidate =
        (argc > 1) && (std::strcmp(argv[1], "--own-validate") == 0);
    const bool own =
        ownValidate || ((argc > 1) && (std::strcmp(argv[1], "--own") == 0));
    const bool conflict =
        (argc > 1) && (std::strcmp(argv[1], "--conflict") == 0);
    const bool validate =
        ownValidate ||
        ((argc > 1) && (std::strcmp(argv[1], "--validate") == 0));

    // THE CONTEXT PATH. Same adoption, expressed the way the context design
    // says it must be: a VulkanVideoEncoderContext in ADOPT mode, and a
    // session created ON it, instead of two handles copied onto the config.
    // --context-conflict is the refusal arm -- it builds the session on a
    // context AND leaves the config's external handles set, which must be a
    // typed error rather than one of the two silently winning.
    const bool contextConflict =
        (argc > 1) && (std::strcmp(argv[1], "--context-conflict") == 0);
    const bool useContext =
        contextConflict ||
        ((argc > 1) && (std::strcmp(argv[1], "--context") == 0));

    std::printf("Encoder-ext ADOPT-mode session (%s)\n",
                ownValidate
                    ? "OWN + config.validate control -- library owns the "
                      "instance and must still attach its callback"
                    : (own ? "OWN control -- no external handles"
                           : (conflict
                                  ? "PIN -- adopted physical device vs a "
                                    "conflicting deviceId"
                                  : (validate
                                         ? "ADOPT + config.validate over a "
                                           "borrowed instance"
                                         : "ADOPT -- borrowed instance + "
                                           "physical device, library-created "
                                           "device"))));
    if (useContext) {
        std::printf("  session built by CreateVulkanVideoEncoderExtOnContext "
                    "on an ADOPT-mode context%s\n",
                    contextConflict
                        ? ", with conflicting config.external* still set"
                        : "");
    }
    // LAYER PROVENANCE. A validation claim without the layer configuration
    // that produced it is not a measurement, and this suite has already
    // produced two uncomparable numbers for the same arm (10 messages once, 46
    // another time) because neither run recorded what was in effect.
    //
    // VK_LAYER_PATH decides whether a layer runs at all -- unset, the
    // --own-validate arm SKIPs with 77 rather than pretending to prove
    // something. VK_LAYER_SETTINGS_PATH decides whether repeated VUIDs can be
    // capped; it is deliberately NOT described as "capped at 10" when unset,
    // because measurement says otherwise: on the project GPU host this binary
    // reports the same 46 messages set and unset, with no cap notice either
    // way. Whether the documented default applies is a property of the layer
    // build, which is exactly why the path is echoed rather than assumed.
    // Read once each. The wording deliberately avoids the token "SKIP":
    // rc/t23run.sh greps this output for SKIP and keeps only the first
    // three matches, so an unconditional line carrying that word would
    // crowd out the real cause of a skip.
    const char* layerPath = std::getenv("VK_LAYER_PATH");
    const char* layerSettings = std::getenv("VK_LAYER_SETTINGS_PATH");
    std::printf("  VK_LAYER_PATH=%s\n",
                layerPath ? layerPath
                          : "(unset -- validation arms cannot run)");
    std::printf("  VK_LAYER_SETTINGS_PATH=%s\n",
                layerSettings
                    ? layerSettings
                    : "(unset -- layer defaults, message cap unknown)");
    std::printf("------------------------------------------------\n");

    Embedder emb;
    if (!own) {
        if (!BuildEmbedder(&emb, /*withValidationLayer=*/false)) {
            std::printf("SKIP: could not stand up an embedder instance / "
                        "encode-capable physical device\n");
            TearDownEmbedder(&emb);
            return 77;
        }
        std::printf("  embedder instance %s physicalDevice %s (%s, deviceID "
                    "0x%x)\n",
                    U64Hex((unsigned long long)(uintptr_t)emb.instance).c_str(),
                    U64Hex((unsigned long long)(uintptr_t)emb.phys).c_str(),
                    emb.name, emb.deviceID);
    }

    int rc = 0;
    {
        // Declared BEFORE |encoder| so it is released AFTER it. The session
        // takes its own reference, so this is belt-and-braces -- but it is the
        // same ordering the library's own member layout uses, and stating it
        // the same way here is free.
        VkSharedBaseObj<VulkanVideoEncoderContext> context;

        VkSharedBaseObj<VulkanVideoEncoderExt> encoder;
        if (useContext) {
            VkVideoEncoderContextCreateInfo ci = {};
            ci.mode                = VK_VIDEO_ENCODER_CONTEXT_MODE_ADOPT;
            ci.adoptInstance       = emb.instance;
            ci.adoptPhysicalDevice = emb.phys;

            // NOT A SKIP. An ADOPT context over an instance and physical
            // device this process just built successfully is a LIBRARY
            // failure, not a host condition -- the embedder stood up fine a
            // few lines above. Skipping here would let a regression that
            // breaks context creation outright report as ctest "Skipped",
            // which does not fail a run. Same rule the InitializeExt arm
            // further down already states for itself.
            const VkResult cr = CreateVulkanVideoEncoderContext(&ci, context);
            Check((cr == VK_SUCCESS) && (bool)context,
                  "CreateVulkanVideoEncoderContext(ADOPT) succeeded",
                  "returned " + I64((long long)cr));
            if ((cr != VK_SUCCESS) || !context) {
                std::printf("----------------------------------------------"
                            "--\n");
                std::printf("FAILED : %d checks, %d failures\n", g_checks,
                            g_failures);
                TearDownEmbedder(&emb);
                return 1;
            }
            Check(VkEncGetPhysicalDeviceCount(context.get()) == 1u,
                  "an ADOPT context enumerates exactly the adopted device",
                  "count is " + I64((long long)VkEncGetPhysicalDeviceCount(
                                        context.get())));

            // Out of range must be REFUSED, not clamped. A clamp would make
            // every wrong index quietly encode on device 0, which on a
            // multi-GPU host is the exact substitution the context exists to
            // prevent -- and is unobservable on a one-GPU test machine.
            {
                VkSharedBaseObj<VulkanVideoEncoderExt> outOfRange;
                const VkResult r =
                    CreateVulkanVideoEncoderExtOnContext(context, 99u,
                                                         outOfRange);
                Check((r != VK_SUCCESS) && !outOfRange,
                      "deviceIndex out of range is refused",
                      "returned " + I64((long long)r));
            }

            // THE OUT-PARAM CONTRACT. Asserting !outOfRange above proves
            // only that nothing was written into a null; it cannot tell
            // 'left alone' from 'cleared', and clearing is exactly the
            // behaviour that changed. Pre-load a real session and prove it
            // SURVIVES a failing create.
            {
                VkSharedBaseObj<VulkanVideoEncoderExt> survivor;
                const VkResult okRes =
                    CreateVulkanVideoEncoderExtOnContext(context, 0u,
                                                         survivor);
                Check((okRes == VK_SUCCESS) && (bool)survivor,
                      "a valid session for the out-param survival check",
                      "returned " + I64((long long)okRes));
                if (survivor) {
                    VulkanVideoEncoderExt* before = survivor.get();
                    const VkResult badRes =
                        CreateVulkanVideoEncoderExtOnContext(context, 99u,
                                                             survivor);
                    Check((badRes != VK_SUCCESS) &&
                              (survivor.get() == before),
                          "a failing create does NOT destroy the caller's "
                          "existing session",
                          "the out-param was cleared or overwritten");
                    survivor.reset();
                }
            }

            // NOT A SKIP, for the same reason: this is the entry point under
            // test.
            const VkResult er =
                CreateVulkanVideoEncoderExtOnContext(context, 0u, encoder);
            Check((er == VK_SUCCESS) && (bool)encoder,
                  "CreateVulkanVideoEncoderExtOnContext succeeded",
                  "returned " + I64((long long)er));
            if ((er != VK_SUCCESS) || !encoder) {
                std::printf("----------------------------------------------"
                            "--\n");
                std::printf("FAILED : %d checks, %d failures\n", g_checks,
                            g_failures);
                encoder.reset();
                context.reset();
                TearDownEmbedder(&emb);
                return 1;
            }
        } else if ((CreateVulkanVideoEncoderExt(encoder) != VK_SUCCESS) ||
                   !encoder) {
            std::printf("SKIP: CreateVulkanVideoEncoderExt failed\n");
            TearDownEmbedder(&emb);
            return 77;
        }

        VkVideoEncoderConfig config = {};
        FillConfig(&config);

        // On the context path the CONTEXT supplies the instance and physical
        // device, so the config must not -- except in --context-conflict,
        // which sets them precisely to prove the collision is refused.
        if (!own && (!useContext || contextConflict)) {
            config.externalInstance       = emb.instance;
            config.externalPhysicalDevice = emb.phys;
            // THE WHOLE POINT: no logical device crosses the boundary.
            config.externalDevice         = VK_NULL_HANDLE;
        }
        if (conflict) {
            // A deviceId that cannot be the adopted device. If the pin is
            // real the library refuses; if it is advisory it quietly selects
            // something else and we would never know on a one-GPU host.
            config.deviceId = (int32_t)(emb.deviceID ^ 0x5A5A);
        }
        if (validate) {
            config.validate = VK_TRUE;
        }

        const VkResult init = encoder->InitializeExt(config);

        if (contextConflict) {
            // TYPED, not merely non-success. Checking only != VK_SUCCESS would
            // pass for the wrong reason on a host that fails InitializeExt for
            // an unrelated cause -- a missing codec, no encode queue,
            // VK_ERROR_LAYER_NOT_PRESENT -- and the CMake comment and commit
            // record both claim a typed refusal.
            Check(init == VK_ERROR_INITIALIZATION_FAILED,
                  "a config that ALSO names externalInstance / "
                  "externalPhysicalDevice is refused with the TYPED error",
                  "InitializeExt returned " + I64((long long)init) +
                      ", expected VK_ERROR_INITIALIZATION_FAILED (" +
                      I64((long long)VK_ERROR_INITIALIZATION_FAILED) + ")");
            // THE OTHER TWO DEVICE SELECTORS. deviceId and gpuUUID name a
            // device just as externalPhysicalDevice does, and the context has
            // already chosen one. Untested, these refusals would be new code
            // no input reaches -- and their absence is not observable from the
            // arm above, which never sets either field. Each gets its OWN
            // session: InitializeExt is not re-entrant after a refusal in any
            // documented sense, and reusing the refused one would test
            // recovery rather than the gate.
            struct SelectorCase {
                const char* what;
                bool        setDeviceId;
                bool        setUuid;
            };
            static const SelectorCase kSelectorCases[] = {
                {"config.deviceId", true, false},
                {"config.gpuUUID", false, true},
            };
            for (const SelectorCase& sc : kSelectorCases) {
                VkSharedBaseObj<VulkanVideoEncoderExt> selEncoder;
                const VkResult sr = CreateVulkanVideoEncoderExtOnContext(
                    context, 0u, selEncoder);
                if ((sr != VK_SUCCESS) || !selEncoder) {
                    Check(false, "second session on the same context",
                          "CreateVulkanVideoEncoderExtOnContext returned " +
                              I64((long long)sr));
                    continue;
                }
                VkVideoEncoderConfig selConfig = {};
                FillConfig(&selConfig);
                if (sc.setDeviceId) {
                    // A deviceId that cannot be the adopted device, so a
                    // pass-through would be caught even if it were honoured
                    // rather than refused.
                    selConfig.deviceId = (int32_t)(emb.deviceID ^ 0x5A5A);
                }
                if (sc.setUuid) {
                    selConfig.gpuUUID[0] = 0xA5;
                }
                const VkResult si = selEncoder->InitializeExt(selConfig);
                const std::string selLabel =
                    std::string(sc.what) +
                    " is refused with the TYPED error on the context path";
                Check(si == VK_ERROR_INITIALIZATION_FAILED, selLabel.c_str(),
                      "InitializeExt returned " + I64((long long)si) +
                          ", expected VK_ERROR_INITIALIZATION_FAILED");
                selEncoder.reset();
            }

            rc = (g_failures == 0) ? 0 : 1;
            std::printf("------------------------------------------------\n");
            std::printf("%s : %d checks, %d failures\n",
                        (rc == 0) ? "PASSED" : "FAILED", g_checks, g_failures);
            encoder.reset();
            context.reset();
            TearDownEmbedder(&emb);
            return rc;
        }

        if (conflict) {
            Check(init != VK_SUCCESS,
                  "conflicting deviceId is REFUSED, not silently re-selected",
                  "InitializeExt returned VK_SUCCESS (" + I64((long long)init) +
                      ")");
            if (init == VK_SUCCESS) {
                Check(encoder->GetVkPhysicalDevice() == emb.phys,
                      "if it did succeed, at least the pin held",
                      "library selected a DIFFERENT physical device");
            }
            rc = (g_failures == 0) ? 0 : 1;
            std::printf("------------------------------------------------\n");
            std::printf("%s : %d checks, %d failures\n",
                        (rc == 0) ? "PASSED" : "FAILED", g_checks, g_failures);
            // The init == VK_SUCCESS sub-case just above is explicitly
            // contemplated, and in it the session owns a VkDevice created
            // on the borrowed instance. Release it before the instance.
            encoder.reset();
            context.reset();
            TearDownEmbedder(&emb);
            return rc;
        }

        if (init != VK_SUCCESS) {
            // In ADOPT mode this is the failure the whole test is about, so it
            // is an ASSERTION, not a skip: the OWN control proves the host can
            // encode, so a red here is the library and not the machine.
            if (own) {
                // VK_ERROR_LAYER_NOT_PRESENT (-6) has one cause on the OWN
                // path and it is not the GPU: the library asks for
                // VK_LAYER_KHRONOS_validation by name when config.validate is
                // set, and the loader does not have it. Saying "no
                // encode-capable device" there sent a reader looking at the
                // hardware for a missing file.
                if ((int)init == (int)VK_ERROR_LAYER_NOT_PRESENT) {
                    std::printf("SKIP: VK_LAYER_KHRONOS_validation is not on "
                                "this host's Vulkan loader search path, so the "
                                "OWN+validate control cannot run. Export "
                                "VK_LAYER_PATH to a directory containing its "
                                "manifest to enable it.\n");
                } else {
                    std::printf("SKIP: InitializeExt failed (%d) -- no "
                                "encode-capable Vulkan device on this host\n",
                                (int)init);
                }
                TearDownEmbedder(&emb);
                return 77;
            }
            Check(false, "InitializeExt(ADOPT)",
                  "returned " + I64((long long)init) +
                      " on a borrowed instance + physical device");
            std::printf("------------------------------------------------\n");
            std::printf("FAILED : %d checks, %d failures\n", g_checks,
                        g_failures);
            // InitializeExt has failure returns AFTER InitVulkanDevice
            // succeeded, so the session may hold a live VkDevice on the
            // borrowed instance. Release it before the instance.
            encoder.reset();
            context.reset();
            TearDownEmbedder(&emb);
            return 1;
        }

        VkInstance       instance = encoder->GetVkInstance();
        VkDevice         device   = encoder->GetVkDevice();
        VkPhysicalDevice phys     = encoder->GetVkPhysicalDevice();

        std::printf("  session instance %s physicalDevice %s device %s\n",
                    U64Hex((unsigned long long)(uintptr_t)instance).c_str(),
                    U64Hex((unsigned long long)(uintptr_t)phys).c_str(),
                    U64Hex((unsigned long long)(uintptr_t)device).c_str());

        if (!own) {
            Check(instance == emb.instance,
                  "the session BORROWED the embedder's VkInstance",
                  "session instance " +
                      U64Hex((unsigned long long)(uintptr_t)instance) +
                      " != embedder " +
                      U64Hex((unsigned long long)(uintptr_t)emb.instance));
            // WHAT THIS CANNOT CATCH ON A SINGLE-GPU HOST, stated so the
            // green is not read as more than it is. If the borrowed physical
            // device were lost on the way in, InitPhysicalDevice would fall
            // back to enumerating the (correct, borrowed) instance with
            // deviceId -1 and no UUID filter, pick the first encode-capable
            // device, and on a one-GPU box that is the SAME HANDLE VALUE.
            // This check passes either way here. The instance assertion above
            // does discriminate -- losing the borrowed instance makes the
            // library create its own, which is a different handle -- and the
            // deviceIndex 99 case discriminates on the factory's validation.
            // A falsifiable pin test needs two GPUs.
            Check(phys == emb.phys,
                  "the session is PINNED to the embedder's VkPhysicalDevice",
                  "session physicalDevice " +
                      U64Hex((unsigned long long)(uintptr_t)phys) +
                      " != embedder " +
                      U64Hex((unsigned long long)(uintptr_t)emb.phys));
        }
        Check(device != VK_NULL_HANDLE,
              "the library created its OWN VkDevice", "GetVkDevice() is null");

        DeviceFns fns;
        PFN_vkGetInstanceProcAddr gipa = emb.gipa;
        void* localLib = nullptr;
        if (gipa == nullptr) {
            localLib = dlopen("libvulkan.so.1", RTLD_NOW);
            if (localLib == nullptr) {
                localLib = dlopen("libvulkan.so", RTLD_NOW);
            }
            if (localLib != nullptr) {
                gipa = (PFN_vkGetInstanceProcAddr)dlsym(
                    localLib, "vkGetInstanceProcAddr");
            }
        }
        if ((gipa == nullptr) ||
            !LoadDeviceFns(gipa, instance, device, &fns)) {
            std::printf("SKIP: could not load the Vulkan entry points this "
                        "test needs\n");
            // Release the session BEFORE the embedder's instance: by here the
            // library has created a VkDevice on that instance, and
            // ~VulkanDeviceContext runs DeviceWaitIdle + DestroyDevice on it.
            // Letting TearDownEmbedder go first destroys the parent instance
            // out from under a live device.
            encoder.reset();
            context.reset();
            TearDownEmbedder(&emb);
            return 77;
        }

        uint32_t frames = 0;
        uint64_t bytes  = 0;
        if (!EncodeAndDrain(encoder, fns, phys, device, &frames, &bytes)) {
            std::printf("SKIP: the encode harness could not be set up\n");
            // Same ordering hazard as above: a live VkDevice on the borrowed
            // instance must go first.
            encoder.reset();
            context.reset();
            TearDownEmbedder(&emb);
            return 77;
        }
        std::printf("  encoded %u frames, %llu bitstream bytes\n", frames,
                    (unsigned long long)bytes);
        Check(frames > 0, "the library-created device ENCODED",
              "no frames came back");
        Check(bytes > 0, "the bitstream is non-empty",
              "captured " + I64((long long)frames) + " frames, 0 bytes");

        rc = (g_failures == 0) ? 0 : 1;
    }
    // The encoder is released before the embedder's instance is destroyed:
    // the library-created VkDevice lives on the borrowed instance, so the
    // reverse order is a use-after-free of the instance and would make a
    // teardown ordering bug look like an encode bug.
    TearDownEmbedder(&emb);

    std::printf("------------------------------------------------\n");
    std::printf("%s : %d checks, %d failures\n", (rc == 0) ? "PASSED" : "FAILED",
                g_checks, g_failures);
    return rc;
}
