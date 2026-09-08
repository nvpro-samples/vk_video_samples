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

//=============================================================================
// The dma-buf IMPORT-ORDINAL GUARD, under test.
//
// WHAT IT IS NOW. The guard (VkEncEnsureImportOrdinalGuard, at the top of
// VkEncImportExternalImage) is RETIRED: the library builds with
// kVkEncImportOrdinalGuardCount = 0, so it takes no sacrificial import and
// reports DISABLED -- retaining K imports shifts the PHASE of the driver's
// damage pattern and nothing else, the damage rate being the same for every
// K. The mechanism is kept, so this suite keeps testing it, in both
// directions:
//   * at the shipped count it asserts the guard is INERT -- one image for one
//     caller import, nothing retained, nothing released, verdict DISABLED;
//   * with --guard-count=N, against a library rebuilt with that N, it asserts
//     the full positional behaviour exactly as it did when N was 2.
//
// WHY THIS SUITE EXISTS: without it nothing in the tree can regress the
// guard. No test, CMake
// file or CI entry referenced it, and -- the load-bearing half --
// VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF did not reach
// VkEncImportExternalImage from ANY encoder-ext test. Every registration
// under vk_video_encoder/test was VK_IMAGE or OPAQUE_FD, and the guard
// returns early on both. So the workaround shipped with a coverage floor of
// zero, on a code path whose failure mode is green frames.
//
// WHAT THIS BINARY DOES. It exports a REAL dma-buf from a real NVIDIA VkDevice
// (NV12, a DRM format modifier the driver advertises as EXPORTABLE and
// IMPORTABLE, the same shape the sibling linux_dmabuf_import test measures)
// and hands it to VkEncImportExternalImage as HANDLE_TYPE_DMA_BUF. That is the
// first time that combination is driven anywhere in this tree.
//
// WHAT IT ASSERTS, AND WHY THAT AND NOT SOMETHING ELSE.
//
// The guard's claim is not "an import succeeded" -- an import succeeds with
// the guard deleted. Its claim is POSITIONAL, it is stated in terms of the
// build's count K, and it has four parts:
//
//   1. K sacrificial imports are taken BEFORE the caller's, so the caller's
//      VkImage is the (K+1)-th image created on the device by this path. At
//      the shipped K = 0 that same assertion is the INERTNESS claim: the
//      caller's import is the first and only image on the device.
//   2. They are RETAINED, not created and thrown away. The original
//      measurement table has a row for "2, created then DESTROYED" and it is
//      100% bad, so a guard that freed them would pass any "did it import"
//      test while not even delivering the phase shift it claims.
//   3. It is a no-op after the first import: the second and every later
//      caller import costs exactly one image, not K+1.
//   4. And it comes DOWN with the device: VkEncReleaseImportOrdinalGuard
//      really destroys K images and frees K allocations, and a second call
//      destroys nothing. That function has no caller on any other test
//      path -- its production call site is inside a full encode
//      session's Deinitialize(), and no session in the corpus performs a
//      dma-buf import -- so the leak it fixes could return with CI green.
//
// All three are read off the DEVICE, not off a log line and not off a
// self-report: the test swaps counting thunks into its own
// VulkanDeviceContext's dispatch table (vk::VkInterfaceFunctions is a plain
// struct of function pointers) and counts the vkCreateImage,
// import-chained vkAllocateMemory, vkDestroyImage and vkFreeMemory calls the
// library actually makes, and in what order. The identity assertion --
// "the caller's VkImage is the one created third" -- is the positional claim
// stated in the only terms the driver understands.
//
// AND THE REPORT IS CHECKED AGAINST THEM, not instead of them. A report is a
// claim about behaviour; the counters are the behaviour. A guard that
// reported COMPLETE while taking one import would pass a report-based test
// and fail this one -- so this file asserts the library's own
// VkEncImportOrdinalGuardReport AND that its retainedCount agrees with what
// the driver was actually asked to do. The sibling encoder-ext-import-guard
// suite gates that report's CARRIER device-free and says in as many words
// that the COMPLETE and INCOMPLETE verdicts "are covered only by a hardware
// run". This is that run.
//
// WHY NOT THE PIXELS. The defect's symptom is a dead plane on one registered
// buffer, and reproducing THAT needs a producer feeding a real encode session
// for hundreds of frames per registration, swept over enough import ordinals
// to cover several periods of the damage pattern. It is not something a CI
// binary can do in 900 seconds, and a per-run sample of it would be a
// coin-flip gate. This binary asserts the MECHANISM, never the pixels.
// Stated plainly so a green run is not over-read: it means the guard does
// exactly what its build count says -- nothing at all, at the shipped 0 --
// and says nothing whatever about whether the driver in front of it still
// damages imports. It does. That is why the count is 0.
//
// EXIT CODES, and the skip is deliberately narrow:
//   0   every assertion held.
//   1   an assertion failed -- including every library refusal once the host
//       question has been settled. g_failures is checked BEFORE any skip, so
//       a skip cannot launder a failure.
//   77  the host cannot run this at all: no Vulkan device, no dma-buf export,
//       or a non-NVIDIA GPU. The guard is NVIDIA-gated by design, so on any
//       other vendor there is nothing to prove and reporting PASSED would be
//       a lie. CTest reports these SKIPPED.
//
// ARMS
//   (default)          the library's shipped count, which is 0: asserts the
//                      guard is INERT -- ONE image for the caller's import,
//                      nothing retained, nothing released, verdict DISABLED.
//   --guard-count=N    the same assertions against a library rebuilt with
//                      kVkEncImportOrdinalGuardCount = N. This is what keeps
//                      the ARMED behaviour covered while the shipped build
//                      does not use it: with N > 0 the positional, retention
//                      and release claims above are all live again.
//   --expect-disabled  VK_VIDEO_ENCODER_NO_IMPORT_ORDINAL_GUARD is set in the
//                      environment. Asserts the kill switch forces the inert
//                      shape whatever the build count is. It is that
//                      control's only coverage, and the permanent form of
//                      this suite's mutation proof -- but only against a
//                      build with a non-zero count, since at the shipped 0
//                      both arms assert the same observable. See the
//                      CMakeLists.
//=============================================================================

#if !defined(__linux__)

#include <cstdio>
int main()
{
    std::printf("SKIP: a dma-buf is a Linux kernel object; there is no "
                "import-ordinal guard to test on this platform (it is not "
                "even compiled).\n");
    return 77;
}

#else  // __linux__

#include "vulkan_video_encoder_ext.h"

// The public header reaches the Xlib platform headers, whose macros collide
// with ordinary identifiers. Same scrub, same reason, as the sibling tests.
#undef Status
#undef None
#undef Bool
#undef Window

#include "vulkan_video_encoder_ext_internal.h"
#include "VkCodecUtils/VulkanDeviceContext.h"

#undef Status
#undef None
#undef Bool
#undef Window

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

//=============================================================================
// Verdict plumbing. Identical in shape to the sibling encoder-ext suites.
//=============================================================================

int  g_failures = 0;
int  g_checks   = 0;
bool g_verbose  = false;

void Check(bool ok, const std::string& what,
           const std::string& detail = std::string())
{
    g_checks++;
    if (ok) {
        std::printf("  ok   %s\n", what.c_str());
        return;
    }
    g_failures++;
    std::printf("  FAIL %s : %s\n", what.c_str(), detail.c_str());
}

std::string U32(uint32_t v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%u", v);
    return b;
}

std::string Hex64(uint64_t v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v);
    return b;
}

std::string Ptr(const void* p)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%p", p);
    return b;
}

const char* GuardStateName(VkVideoEncoderImportGuardState s)
{
    switch (s) {
        case VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_EVALUATED:
            return "NOT_EVALUATED";
        case VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_APPLICABLE:
            return "NOT_APPLICABLE";
        case VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_NVIDIA:
            return "NOT_NVIDIA";
        case VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_DISABLED:
            return "DISABLED";
        case VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_COMPLETE:
            return "COMPLETE";
        case VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_INCOMPLETE:
            return "INCOMPLETE";
        default:
            return "(unknown)";
    }
}

//=============================================================================
// THE OBSERVABLE: counting thunks over the device dispatch table.
//
// VulkanDeviceContext derives from vk::VkInterfaceFunctions, which is a plain
// struct of PFN_vk* members, so a test that OWNS the context can put its own
// function in front of the driver's and forward. Nothing about the library is
// recompiled or stubbed -- the real driver entry point runs, and the only
// thing added is a counter and an ordered record of the handles it minted.
//
// The export half of this test runs BEFORE these are installed, so nothing
// the harness itself allocates is ever counted.
//=============================================================================

struct DispatchCounters {
    uint32_t createImage  = 0;
    uint32_t importAlloc  = 0;  // vkAllocateMemory carrying an fd import chain
    uint32_t plainAlloc   = 0;  // any other vkAllocateMemory
    uint32_t destroyImage = 0;
    uint32_t freeMemory   = 0;
    // In creation order. images[0] is the first VkImage the library created
    // after the thunks went in -- which, with the guard on, is a guard image
    // and NOT the caller's.
    std::vector<VkImage>        images;
    std::vector<VkDeviceMemory> imports;
};

DispatchCounters g_c;

PFN_vkCreateImage    g_realCreateImage    = nullptr;
PFN_vkAllocateMemory g_realAllocateMemory = nullptr;
PFN_vkDestroyImage   g_realDestroyImage   = nullptr;
PFN_vkFreeMemory     g_realFreeMemory     = nullptr;

bool ChainHasFdImport(const void* pNext)
{
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)pNext;
         p != nullptr; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkResult VKAPI_CALL CountingCreateImage(
    VkDevice device, const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkImage* pImage)
{
    const VkResult r = g_realCreateImage(device, pCreateInfo, pAllocator, pImage);
    if (r == VK_SUCCESS) {
        g_c.createImage++;
        g_c.images.push_back(*pImage);
        if (g_verbose) {
            std::printf("    [dispatch] vkCreateImage  #%u -> %p\n",
                        g_c.createImage, (void*)(uintptr_t)*pImage);
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL CountingAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory)
{
    const bool isImport = ChainHasFdImport(pAllocateInfo->pNext);
    const VkResult r =
        g_realAllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
    if (r == VK_SUCCESS) {
        if (isImport) {
            g_c.importAlloc++;
            g_c.imports.push_back(*pMemory);
        } else {
            g_c.plainAlloc++;
        }
        if (g_verbose) {
            std::printf("    [dispatch] vkAllocateMemory %s #%u\n",
                        isImport ? "(fd import)" : "(plain)   ",
                        isImport ? g_c.importAlloc : g_c.plainAlloc);
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL CountingDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator)
{
    if (image != VK_NULL_HANDLE) {
        g_c.destroyImage++;
    }
    g_realDestroyImage(device, image, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL CountingFreeMemory(
    VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator)
{
    if (memory != VK_NULL_HANDLE) {
        g_c.freeMemory++;
    }
    g_realFreeMemory(device, memory, pAllocator);
}

bool InstallCountingDispatch(VulkanDeviceContext& ctx)
{
    if ((ctx.CreateImage == nullptr) || (ctx.AllocateMemory == nullptr) ||
        (ctx.DestroyImage == nullptr) || (ctx.FreeMemory == nullptr)) {
        return false;
    }
    g_realCreateImage    = ctx.CreateImage;
    g_realAllocateMemory = ctx.AllocateMemory;
    g_realDestroyImage   = ctx.DestroyImage;
    g_realFreeMemory     = ctx.FreeMemory;

    ctx.CreateImage    = CountingCreateImage;
    ctx.AllocateMemory = CountingAllocateMemory;
    ctx.DestroyImage   = CountingDestroyImage;
    ctx.FreeMemory     = CountingFreeMemory;
    return true;
}

void RemoveCountingDispatch(VulkanDeviceContext& ctx)
{
    if (g_realCreateImage != nullptr) {
        ctx.CreateImage    = g_realCreateImage;
        ctx.AllocateMemory = g_realAllocateMemory;
        ctx.DestroyImage   = g_realDestroyImage;
        ctx.FreeMemory     = g_realFreeMemory;
    }
}

//=============================================================================
// The harness.
//=============================================================================

// The subject's own count, mirrored. The library's
// kVkEncImportOrdinalGuardCount lives in an anonymous namespace, so it cannot
// be read from here -- and mirroring it is the more useful behaviour anyway,
// because the count is a claim about a driver defect and a change to it
// should have to come here and say so.
//
// THE DEFAULT IS 0 BECAUSE THE GUARD IS RETIRED. A non-zero K here would be
// an OBSERVATION, not a fix. A run that registers only a few buffers samples
// one period of the damage pattern at one phase, so a K that moves the
// damaged ordinals off those buffers reads as a fix. Sweeping the import
// ordinal far enough shows that the damage is periodic in the ordinal and
// that its RATE does not move with K: K changes which imports are damaged,
// never how many.
//
// --guard-count=N is what keeps this suite a test of the KNOB and not only of
// its retirement. Point it at a library built with
// kVkEncImportOrdinalGuardCount = N and every positional assertion below runs
// as it did when N was the default. At the shipped N = 0 those same
// assertions say the guard is INERT.
constexpr uint32_t kDefaultGuardCount = 0;
uint32_t g_guardCount = kDefaultGuardCount;

// 4:2:0, so both dimensions must be even. Small on purpose: nothing here
// looks at a pixel, and a 1920x1080 NV12 allocation per import x3 buys
// nothing but runtime.
constexpr uint32_t kWidth  = 640;
constexpr uint32_t kHeight = 480;
constexpr VkFormat kFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;  // NV12

// Exactly the exporter shape the sibling linux_dmabuf_import test measures
// green on this host, and the shape a Chromium/GBM producer presents: usage
// 0x4007, flags 0x100108. The library copies both into its own
// VkImageCreateInfo verbatim (it never invents a usage), so the importer's
// create info can only be right if the exporter's was.
constexpr VkImageUsageFlags kExportUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
constexpr VkImageCreateFlags kExportFlags =
    VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
    VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;

struct Exported {
    VkImage             image         = VK_NULL_HANDLE;
    VkDeviceMemory      memory        = VK_NULL_HANDLE;
    uint64_t            modifier      = 0;
    uint32_t            memPlaneCount = 0;
    VkSubresourceLayout planes[VK_VIDEO_ENCODER_MAX_PLANES]{};
};

class Harness {
public:
    // 0 ok, 1 assertion failed, 77 cannot run here.
    int Run(bool expectDisabled);
    ~Harness() { Teardown(); }

private:
    int  Setup();                 // 0 ok, 77 cannot run here
    bool ExportDmaBuf();
    bool PickModifier(uint64_t* outModifier, uint32_t* outPlaneCount);
    int  ExportFd();              // a new dma-buf fd, or -1
    void BuildDescriptor(VkVideoEncoderExternalImageDescriptor* outDesc) const;
    void Teardown();

    VulkanDeviceContext m_ctx;
    VkDevice            m_device = VK_NULL_HANDLE;
    bool                m_deviceUp = false;
    bool                m_thunksIn = false;
    Exported            m_exp;
    VkPhysicalDeviceMemoryProperties m_memProps{};
    std::string         m_why;   // why Setup() gave up

    // The caller-visible imports, destroyed by Teardown() with the thunks
    // already removed so the cleanup cannot move a counter an assertion read.
    std::vector<VkEncImportedImage> m_callerImports;
};

bool Harness::PickModifier(uint64_t* outModifier, uint32_t* outPlaneCount)
{
    VkPhysicalDevice phys = m_ctx.getPhysicalDevice();

    VkDrmFormatModifierPropertiesListEXT list{
        VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 fmtProps{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    fmtProps.pNext = &list;
    m_ctx.GetPhysicalDeviceFormatProperties2(phys, kFormat, &fmtProps);
    if (list.drmFormatModifierCount == 0) {
        m_why = "the driver reports no DRM format modifiers for NV12";
        return false;
    }
    std::vector<VkDrmFormatModifierPropertiesEXT> props(
        list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = props.data();
    m_ctx.GetPhysicalDeviceFormatProperties2(phys, kFormat, &fmtProps);

    VkFormat viewFormat = kFormat;
    for (const auto& p : props) {
        if ((p.drmFormatModifierPlaneCount == 0) ||
            (p.drmFormatModifierPlaneCount > VK_VIDEO_ENCODER_MAX_PLANES)) {
            continue;
        }
        uint64_t mod = p.drmFormatModifier;

        VkPhysicalDeviceExternalImageFormatInfo extInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        extInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        modInfo.pNext             = &extInfo;
        modInfo.drmFormatModifier = mod;
        modInfo.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;

        VkImageFormatListCreateInfo formatListCI{
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        formatListCI.pNext           = &modInfo;
        formatListCI.viewFormatCount = 1;
        formatListCI.pViewFormats    = &viewFormat;

        VkPhysicalDeviceImageFormatInfo2 info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.pNext  = (void*)&formatListCI;
        info.format = kFormat;
        info.type   = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.usage  = kExportUsage;
        info.flags  = kExportFlags;

        VkExternalImageFormatProperties extProps{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 out{
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        out.pNext = &extProps;

        if (m_ctx.GetPhysicalDeviceImageFormatProperties2(phys, &info, &out) !=
            VK_SUCCESS) {
            continue;
        }
        const VkExternalMemoryFeatureFlags f =
            extProps.externalMemoryProperties.externalMemoryFeatures;
        if (((f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0) ||
            ((f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0)) {
            continue;
        }
        *outModifier   = mod;
        *outPlaneCount = p.drmFormatModifierPlaneCount;
        return true;
    }
    m_why = "no DRM modifier for NV12 is advertised as both EXPORTABLE and "
            "IMPORTABLE for dma-buf with the encode-source usage/flags";
    return false;
}

bool Harness::ExportDmaBuf()
{
    if (!PickModifier(&m_exp.modifier, &m_exp.memPlaneCount)) {
        return false;
    }
    std::printf("[INFO] exporting NV12 %ux%u modifier %s (%u memory plane%s)\n",
                kWidth, kHeight, Hex64(m_exp.modifier).c_str(),
                m_exp.memPlaneCount, (m_exp.memPlaneCount == 1) ? "" : "s");

    VkFormat viewFormat = kFormat;
    uint64_t modifier   = m_exp.modifier;

    VkExternalMemoryImageCreateInfo extMemCI{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extMemCI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageDrmFormatModifierListCreateInfoEXT drmList{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
    drmList.pNext                  = &extMemCI;
    drmList.drmFormatModifierCount = 1;
    drmList.pDrmFormatModifiers    = &modifier;

    VkImageFormatListCreateInfo formatListCI{
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    formatListCI.pNext           = &drmList;
    formatListCI.viewFormatCount = 1;
    formatListCI.pViewFormats    = &viewFormat;

    VkImageCreateInfo imageCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCI.pNext         = (void*)&formatListCI;
    imageCI.flags         = kExportFlags;
    imageCI.imageType     = VK_IMAGE_TYPE_2D;
    imageCI.format        = kFormat;
    imageCI.extent        = {kWidth, kHeight, 1};
    imageCI.mipLevels     = 1;
    imageCI.arrayLayers   = 1;
    imageCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageCI.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    imageCI.usage         = kExportUsage;
    imageCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (m_ctx.CreateImage(m_device, &imageCI, nullptr, &m_exp.image) !=
        VK_SUCCESS) {
        m_why = "vkCreateImage rejected the modifier the driver had just "
                "advertised as exportable";
        return false;
    }

    VkMemoryRequirements memReqs{};
    m_ctx.GetImageMemoryRequirements(m_device, m_exp.image, &memReqs);

    uint32_t typeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; i++) {
        if (((memReqs.memoryTypeBits & (1u << i)) != 0) &&
            ((m_memProps.memoryTypes[i].propertyFlags &
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)) {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == UINT32_MAX) {
        m_why = "no DEVICE_LOCAL memory type in the export image's "
                "requirements mask";
        return false;
    }

    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = m_exp.image;

    VkExportMemoryAllocateInfo exportAI{
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exportAI.pNext       = &dedicated;
    exportAI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.pNext           = &exportAI;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = typeIndex;

    if (m_ctx.AllocateMemory(m_device, &allocInfo, nullptr, &m_exp.memory) !=
        VK_SUCCESS) {
        m_why = "the exportable dedicated allocation failed";
        return false;
    }
    if (m_ctx.BindImageMemory(m_device, m_exp.image, m_exp.memory, 0) !=
        VK_SUCCESS) {
        m_why = "vkBindImageMemory failed on the export image";
        return false;
    }

    // For a DRM-modifier image the layouts come from the MEMORY_PLANE aspects
    // and the count is the modifier's drmFormatModifierPlaneCount -- not the
    // colour plane count, which for NV12 happens to agree but need not.
    for (uint32_t p = 0; p < m_exp.memPlaneCount; p++) {
        VkImageSubresource sub{};
        sub.aspectMask =
            (VkImageAspectFlags)(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p);
        m_ctx.GetImageSubresourceLayout(m_device, m_exp.image, &sub,
                                        &m_exp.planes[p]);
    }
    return true;
}

int Harness::ExportFd()
{
    if (m_ctx.GetMemoryFdKHR == nullptr) {
        return -1;
    }
    VkMemoryGetFdInfoKHR getFd{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    getFd.memory     = m_exp.memory;
    getFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if ((m_ctx.GetMemoryFdKHR(m_device, &getFd, &fd) != VK_SUCCESS) ||
        (fd < 0)) {
        return -1;
    }
    return fd;
}

void Harness::BuildDescriptor(
    VkVideoEncoderExternalImageDescriptor* outDesc) const
{
    VkVideoEncoderExternalImageDescriptor& d = *outDesc;
    d = VkVideoEncoderExternalImageDescriptor{};
    d.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
    // THE POINT OF THE WHOLE FILE. This is the only encoder-ext test that sets
    // this member to DMA_BUF; without it VkEncEnsureImportOrdinalGuard returns
    // at its handleType check on every test in the tree.
    d.handleType  = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF;
    d.format      = kFormat;
    d.width       = kWidth;
    d.height      = kHeight;
    d.tiling      = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    d.imageUsage  = kExportUsage;
    d.imageFlags  = kExportFlags;
    d.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    d.hasDrmFormatModifier = VK_TRUE;
    d.drmFormatModifier    = m_exp.modifier;
    d.planeCount           = m_exp.memPlaneCount;
    for (uint32_t p = 0; p < m_exp.memPlaneCount; p++) {
        d.planeLayouts[p].offset     = m_exp.planes[p].offset;
        // MUST be 0 on the explicit DRM path
        // (VUID-VkImageDrmFormatModifierExplicitCreateInfoEXT-size-02267);
        // the import zeroes it anyway, and passing the exporter's value here
        // would make this descriptor a poor model of a real producer's.
        d.planeLayouts[p].size       = 0;
        d.planeLayouts[p].rowPitch   = m_exp.planes[p].rowPitch;
        d.planeLayouts[p].arrayPitch = m_exp.planes[p].arrayPitch;
        d.planeLayouts[p].depthPitch = m_exp.planes[p].depthPitch;
    }

    // Left UNKNOWN on purpose, which is both what the public header
    // prescribes for dma-buf ("Deriving allocationSize from
    // vkGetImageMemoryRequirements is wrong for dma-buf on NVIDIA. 0 means
    // unknown") and what a GBM/Ozone producer actually has: it holds a
    // gfx::NativePixmapHandle, which carries no Vulkan memory type at all.
    d.allocationSize  = 0;
    d.memoryTypeBits  = 0;
    d.memoryTypeIndex = UINT32_MAX;

    VkPhysicalDeviceIDProperties idProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &idProps;
    m_ctx.GetPhysicalDeviceProperties2(m_ctx.getPhysicalDevice(), &props2);
    std::memcpy(d.deviceUUID, idProps.deviceUUID, VK_UUID_SIZE);
    std::memcpy(d.driverUUID, idProps.driverUUID, VK_UUID_SIZE);
}

int Harness::Setup()
{
    static const char* const instanceExtensions[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        nullptr
    };
    m_ctx.AddReqInstanceExtensions(instanceExtensions, g_verbose);

    // The precondition set the library itself gates the dma-buf import on,
    // plus the video pair the encode-source usage needs. Requiring them here
    // means a host that cannot possibly run this says so by name at init
    // rather than failing an import later for an unrelated-looking reason.
    static const char* const requiredDeviceExtensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        nullptr
    };
    m_ctx.AddReqDeviceExtensions(requiredDeviceExtensions, g_verbose);

    // VIDEO_ENCODE_SRC usage and VIDEO_PROFILE_INDEPENDENT are what a real
    // encode producer declares; optional here so the arm degrades to a named
    // skip rather than an unexplained device-selection failure.
    static const char* const videoExtensions[] = {
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        nullptr
    };
    m_ctx.AddOptDeviceExtensions(videoExtensions, g_verbose);

    if (m_ctx.InitVulkanDevice("EncoderExtImportOrdinalGuardTest",
                               VK_NULL_HANDLE, g_verbose) != VK_SUCCESS) {
        m_why = "InitVulkanDevice failed (no Vulkan loader or no ICD)";
        return 77;
    }

    vk::DeviceUuidUtils deviceUuid;
    if (m_ctx.InitPhysicalDevice(-1, deviceUuid,
                                 VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT,
                                 nullptr,
                                 0, VK_VIDEO_CODEC_OPERATION_NONE_KHR,
                                 0, VK_VIDEO_CODEC_OPERATION_NONE_KHR) !=
        VK_SUCCESS) {
        m_why = "InitPhysicalDevice found no device offering the dma-buf / "
                "DRM-modifier extension set";
        return 77;
    }
    for (const char* const* p = requiredDeviceExtensions; *p != nullptr; ++p) {
        if (m_ctx.FindRequiredDeviceExtension(*p) == nullptr) {
            m_why = std::string("required device extension not available: ") + *p;
            return 77;
        }
    }

    VkPhysicalDeviceProperties props{};
    m_ctx.GetPhysicalDeviceProperties(m_ctx.getPhysicalDevice(), &props);
    std::printf("[INFO] physical device: %s  vendorID 0x%04X  driver %u.%u.%u\n",
                props.deviceName, props.vendorID,
                VK_VERSION_MAJOR(props.driverVersion),
                VK_VERSION_MINOR(props.driverVersion),
                VK_VERSION_PATCH(props.driverVersion));

    // THE VENDOR GATE, AND WHY IT IS A SKIP AND NOT A PASS. The guard refuses
    // to run on anything but 0x10DE, deliberately -- "a workaround must not
    // run where the defect it works around has never been observed". On any
    // other vendor every assertion below would be asserting the ABSENCE of
    // the behaviour under test, which is not evidence that the behaviour is
    // right where it does run. Reporting PASSED there is precisely how a gate
    // stops being able to fail.
    if (props.vendorID != 0x10DE) {
        m_why = std::string("this host's GPU is vendor 0x") +
                Hex64(props.vendorID) +
                ", and the import-ordinal guard is gated to NVIDIA (0x10DE) "
                "by design -- there is nothing here to prove either way";
        return 77;
    }

    if (m_ctx.CreateVulkanDevice(0, 0, VK_VIDEO_CODEC_OPERATION_NONE_KHR,
                                 /*transfer*/ true, /*graphics*/ false,
                                 /*present*/ false, /*compute*/ true) !=
        VK_SUCCESS) {
        m_why = "CreateVulkanDevice failed";
        return 77;
    }
    m_deviceUp = true;
    m_device   = m_ctx.getDevice();
    if (m_device == VK_NULL_HANDLE) {
        m_why = "the device context produced a null VkDevice";
        return 77;
    }
    m_ctx.GetPhysicalDeviceMemoryProperties(m_ctx.getPhysicalDevice(),
                                            &m_memProps);

    if (m_ctx.GetMemoryFdKHR == nullptr) {
        m_why = "the device has no vkGetMemoryFdKHR, so no dma-buf can be "
                "exported to import";
        return 77;
    }
    if (!ExportDmaBuf()) {
        return 77;  // m_why already set
    }
    return 0;
}

void Harness::Teardown()
{
    if (!m_deviceUp) {
        return;
    }
    // Thunks out FIRST. Cleanup must not be able to move a counter that an
    // assertion has already read, and more importantly must not be able to
    // make a later assertion true by accident.
    RemoveCountingDispatch(m_ctx);
    m_thunksIn = false;

    m_ctx.DeviceWaitIdle();
    for (auto& imp : m_callerImports) {
        if (imp.image != VK_NULL_HANDLE) {
            m_ctx.DestroyImage(m_device, imp.image, nullptr);
        }
        if (imp.memory != VK_NULL_HANDLE) {
            m_ctx.FreeMemory(m_device, imp.memory, nullptr);
        }
    }
    m_callerImports.clear();
    if (m_exp.image != VK_NULL_HANDLE) {
        m_ctx.DestroyImage(m_device, m_exp.image, nullptr);
        m_exp.image = VK_NULL_HANDLE;
    }
    if (m_exp.memory != VK_NULL_HANDLE) {
        m_ctx.FreeMemory(m_device, m_exp.memory, nullptr);
        m_exp.memory = VK_NULL_HANDLE;
    }
    // The guard's own two imports are not destroyed HERE and could not be --
    // their handles never leave the library. Run() has already asked the
    // library to take them down, through the same
    // VkEncReleaseImportOrdinalGuard() call its Deinitialize() makes, and
    // asserted that it did. Nothing is papered over on this path: if Run()
    // exited early the guards are still live, ~VulkanDeviceContext destroys
    // the device under them, and that is the spec violation the release
    // exists to close -- visible to a validation layer, not hidden by this
    // teardown.
    m_deviceUp = false;
}

int Harness::Run(bool expectDisabled)
{
    const int setup = Setup();
    if (setup != 0) {
        std::printf("SKIP: %s\n", m_why.c_str());
        return 77;
    }

    if (!InstallCountingDispatch(m_ctx)) {
        std::printf("SKIP: the device dispatch table is missing one of "
                    "vkCreateImage / vkAllocateMemory / vkDestroyImage / "
                    "vkFreeMemory\n");
        return 77;
    }
    m_thunksIn = true;

    VkVideoEncoderExternalImageDescriptor desc{};
    BuildDescriptor(&desc);

    // What the library is expected to do on THIS run: the build's count, or 0
    // when the environment kill switch has taken it out. Both zeroes produce
    // the same observable -- one image, nothing retained, verdict DISABLED --
    // and that is not a coincidence to paper over: the retired default IS the
    // kill switch, permanently, which is exactly why the count-0 path reports
    // DISABLED rather than a vacuously-satisfied COMPLETE.
    const uint32_t effectiveCount = expectDisabled ? 0u : g_guardCount;
    const uint32_t expectedFirst  = effectiveCount + 1u;

    std::printf("\n=== import 1: the first DMA_BUF import on this VkDevice "
                "===\n");
    if (effectiveCount == 0) {
        std::printf("[INFO] guard expectation: INERT (%s) -- the caller's "
                    "import must be the ONLY one and the verdict DISABLED\n",
                    expectDisabled
                        ? "VK_VIDEO_ENCODER_NO_IMPORT_ORDINAL_GUARD is set"
                        : "this build's guard count is 0, the retired "
                          "default");
    } else {
        std::printf("[INFO] guard expectation: ARMED -- %u sacrificial "
                    "import(s) must be taken and RETAINED ahead of the "
                    "caller's, and the verdict must be COMPLETE\n",
                    effectiveCount);
    }

    int fd1 = ExportFd();
    if (fd1 < 0) {
        Check(false, "exported a dma-buf fd for import 1",
              "vkGetMemoryFdKHR(DMA_BUF) failed after the export image was "
              "already built, which is a driver fact and not a host one");
        return 1;
    }
    std::printf("[INFO] exported dma-buf fd %d\n", fd1);

    // The thread's verdict record, cleared exactly as RegisterImageResource
    // clears it at the top of every registration -- so what is read back
    // below is this import's answer and not a previous one's.
    VkEncResetImportOrdinalGuardReport();

    VkEncImportedImage first{};
    const VkVideoEncoderStatusCode s1 =
        VkEncImportExternalImage(m_ctx, desc, (uint64_t)fd1, &first);
    // TRANSFER ownership: the fd is consumed on EVERY exit, success or not.
    fd1 = -1;

    Check(s1 == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "the caller's DMA_BUF import succeeded",
          "VkEncImportExternalImage returned status " +
              U32((uint32_t)s1));
    if (s1 != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
        // Nothing below can mean anything if the subject never ran. This is a
        // FAILURE and not a skip: the host question was settled in Setup(),
        // which built this very image on this very device and exported this
        // very fd from it.
        std::printf("\n%d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }
    m_callerImports.push_back(first);

    Check(g_c.createImage == expectedFirst,
          "vkCreateImage call count for the first caller import",
          "expected " + U32(expectedFirst) + " (the guard's " +
              U32(effectiveCount) +
              " sacrificial import(s) plus the caller's), got " +
              U32(g_c.createImage));

    Check(g_c.importAlloc == expectedFirst,
          "fd-importing vkAllocateMemory call count for the first caller "
          "import",
          "expected " + U32(expectedFirst) + ", got " + U32(g_c.importAlloc));

    // THE POSITIONAL CLAIM, stated in the only terms the driver understands.
    // With the guard on, images[0] and images[1] are the sacrificial pair and
    // the caller's is images[2] -- i.e. the caller's import is at
    // live-position 3, which is the entire content of the workaround.
    Check((g_c.images.size() == expectedFirst) &&
              (g_c.images.back() == first.image),
          "the caller's VkImage is the one created LAST, at live-position " +
              U32(expectedFirst),
          "created " + U32((uint32_t)g_c.images.size()) +
              " image(s); the caller got " + Ptr((void*)(uintptr_t)first.image) +
              " and the last created was " +
              (g_c.images.empty()
                   ? std::string("(none)")
                   : Ptr((void*)(uintptr_t)g_c.images.back())));

    if (effectiveCount > 0) {
        bool distinct = (g_c.images.size() >= effectiveCount);
        for (uint32_t i = 0; distinct && (i < effectiveCount); i++) {
            distinct = (g_c.images[i] != first.image);
        }
        Check(distinct,
              "the " + U32(effectiveCount) +
                  " image(s) created before the caller's are NOT the caller's",
              "the guard did not take a distinct set ahead of the import");
    } else {
        // THE INERTNESS CLAIM, which is the one the SHIPPED build makes: a
        // retired guard creates nothing of its own. This is not the same
        // assertion as the count above -- that one would still hold if the
        // library created an extra image and handed it to the caller -- and
        // it is what goes red if someone re-arms the guard without telling
        // this suite.
        Check(g_c.images.size() == 1,
              "a retired guard created exactly ONE image for one import",
              "created " + U32((uint32_t)g_c.images.size()) + " image(s)");
    }

    // THE RETENTION CLAIM. "2, created then DESTROYED" is a row in the
    // original measurement table and it is 100% bad, so a guard that freed
    // its imports would satisfy every count above while not even delivering
    // the phase shift it claims. That is what this assertion separates: a
    // mechanism that does what it says from a decoration. It says nothing
    // about whether doing what it says is worth anything -- measurement says
    // it is not, which is why the shipped count is 0.
    Check((g_c.destroyImage == 0) && (g_c.freeMemory == 0),
          (effectiveCount == 0)
              ? std::string("nothing has been destroyed, and a retired guard "
                            "had nothing to destroy")
              : std::string("nothing has been destroyed -- the sacrificial "
                            "imports are RETAINED"),
          "vkDestroyImage x" + U32(g_c.destroyImage) + ", vkFreeMemory x" +
              U32(g_c.freeMemory) +
              "; a guard whose imports are not LIVE alongside the caller's "
              "does not shift the caller's import ordinal at all");

    // ---------------------------------------------------------------------
    // THE LIBRARY'S OWN VERDICT, CHECKED AGAINST WHAT THE DRIVER WAS ASKED.
    //
    // The sibling encoder-ext-import-guard suite gates this record's carrier
    // device-free, and states plainly that COMPLETE and INCOMPLETE "are
    // covered only by a hardware run". This is that run: it is the only place
    // in the tree where a real dma-buf import can produce either verdict.
    //
    // The cross-check is the load-bearing line. A report that said COMPLETE
    // while one import had been taken would satisfy any report-only test; it
    // cannot satisfy retainedCount == (images created - the caller's one).
    // ---------------------------------------------------------------------
    VkEncImportOrdinalGuardReport report{};
    VkEncGetImportOrdinalGuardReport(&report);
    std::printf("[INFO] guard report: state=%s requested=%u retained=%u "
                "failureStatus=%u errno=%d\n",
                GuardStateName(report.state), report.requestedCount,
                report.retainedCount, (uint32_t)report.failureStatus,
                report.failureErrno);

    Check(report.requestedCount == g_guardCount,
          "the guard report's requestedCount is this build's count",
          "expected " + U32(g_guardCount) + ", got " +
              U32(report.requestedCount) +
              "; requestedCount is a BUILD constant, stamped even when the "
              "kill switch is set, so it does NOT follow --expect-disabled. "
              "If this is the only failing line, the library's "
              "kVkEncImportOrdinalGuardCount and this run's --guard-count "
              "disagree");

    // DISABLED covers BOTH zeroes: the environment kill switch returns before
    // the count is consulted, and a build count of 0 returns just after the
    // vendor probe with the same verdict. COMPLETE is reserved for a build
    // that actually retained something. 0 == 0 must never report COMPLETE --
    // vulkan_video_encoder_ext_internal.h defines that verdict as caller
    // imports landing past a position they did not, at 0, move to, and a
    // workaround that reports
    // success on a build which removed it is worse than one that reports
    // nothing.
    const VkVideoEncoderImportGuardState expectedState =
        (effectiveCount == 0) ? VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_DISABLED
                              : VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_COMPLETE;
    Check(report.state == expectedState,
          std::string("the guard report's state is ") +
              GuardStateName(expectedState),
          std::string("got ") + GuardStateName(report.state));

    const uint32_t expectedRetained = effectiveCount;
    Check(report.retainedCount == expectedRetained,
          "the guard report's retainedCount",
          "expected " + U32(expectedRetained) + ", got " +
              U32(report.retainedCount));

    Check(report.retainedCount == (g_c.createImage - 1),
          "the report AGREES with the driver: retainedCount == images created "
          "minus the caller's one",
          "the library reported retaining " + U32(report.retainedCount) +
              " sacrificial import(s) but " + U32(g_c.createImage) +
              " image(s) were created for one caller import; a report that "
              "does not match the calls is worse than no report");

    std::printf("\n=== import 2: the guard must NOT run again ===\n");
    const uint32_t createBefore  = g_c.createImage;
    const uint32_t importBefore  = g_c.importAlloc;

    int fd2 = ExportFd();
    if (fd2 < 0) {
        Check(false, "exported a second dma-buf fd", "vkGetMemoryFdKHR failed");
        std::printf("\n%d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }
    VkEncResetImportOrdinalGuardReport();
    VkEncImportedImage second{};
    const VkVideoEncoderStatusCode s2 =
        VkEncImportExternalImage(m_ctx, desc, (uint64_t)fd2, &second);
    fd2 = -1;
    Check(s2 == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "the second caller DMA_BUF import succeeded",
          "status " + U32((uint32_t)s2));
    if (s2 == VK_VIDEO_ENCODER_STATUS_SUCCESS) {
        m_callerImports.push_back(second);
    }

    Check(g_c.createImage == (createBefore + 1),
          "the second caller import cost exactly ONE vkCreateImage",
          "expected " + U32(createBefore + 1) + ", got " +
              U32(g_c.createImage) +
              "; the guard is documented as a no-op after the first import "
              "and carries no per-frame cost");
    Check(g_c.importAlloc == (importBefore + 1),
          "the second caller import cost exactly ONE fd import",
          "expected " + U32(importBefore + 1) + ", got " +
              U32(g_c.importAlloc));
    Check((s2 != VK_VIDEO_ENCODER_STATUS_SUCCESS) ||
              (g_c.images.back() == second.image),
          "the second caller import is the last image created",
          "the library minted an image after the caller's");
    Check((g_c.destroyImage == 0) && (g_c.freeMemory == 0),
          "still nothing destroyed after the second import",
          "vkDestroyImage x" + U32(g_c.destroyImage) + ", vkFreeMemory x" +
              U32(g_c.freeMemory));

    // The second import takes the guard's already-satisfied early return, and
    // that return still owes the caller a verdict -- a registration that
    // reported NOT_EVALUATED here would tell a consumer the guard had not run
    // on an import it is in fact protecting.
    VkEncImportOrdinalGuardReport report2{};
    VkEncGetImportOrdinalGuardReport(&report2);
    std::printf("[INFO] guard report: state=%s requested=%u retained=%u\n",
                GuardStateName(report2.state), report2.requestedCount,
                report2.retainedCount);
    Check(report2.state == expectedState,
          std::string("the second import still reports ") +
              GuardStateName(expectedState),
          std::string("got ") + GuardStateName(report2.state) +
              " -- the already-guarded early return must not drop the verdict");
    Check(report2.retainedCount == expectedRetained,
          "the second import reports the SAME retainedCount",
          "expected " + U32(expectedRetained) + ", got " +
              U32(report2.retainedCount) +
              "; the guard is documented as a no-op after the first import");

    // ---------------------------------------------------------------------
    // THE OTHER END OF THE GUARD'S LIFETIME.
    //
    // VkEncReleaseImportOrdinalGuard is what stops vkDestroyDevice running
    // with two VkImage and two VkDeviceMemory still alive on the device
    // (VUID-vkDestroyDevice-device-05137). Nothing else in the tree calls it:
    // the production call site is
    // VulkanVideoEncoderExtImpl::Deinitialize(), reachable only through a
    // full encode session, and no session in the corpus performs a dma-buf
    // import -- so the function ran on exactly zero test paths and the leak
    // it fixes could come back with the suite green.
    //
    // It is asserted the same way as everything else here: through the
    // driver. Two destroys and two frees must actually reach the device, not
    // just a return value saying they did.
    //
    // THIS IS ALSO WHY THE TEST OWNS ITS OWN DEVICE. Releasing the guard is
    // only safe where no further import on this device is possible; here that
    // is true by construction, because the next thing that happens is
    // teardown.
    // ---------------------------------------------------------------------
    std::printf("\n=== release: the guard must come down with the device ===\n");
    const uint32_t destroyBefore = g_c.destroyImage;
    const uint32_t freeBefore    = g_c.freeMemory;

    const uint32_t released = VkEncReleaseImportOrdinalGuard(m_ctx);
    Check(released == expectedRetained,
          "VkEncReleaseImportOrdinalGuard returned the guard count",
          "expected " + U32(expectedRetained) + ", got " + U32(released) +
              "; the count is a RETURN VALUE and not only a log line because "
              "the shipping embedder sets silenceStdio");
    Check((g_c.destroyImage == (destroyBefore + expectedRetained)) &&
              (g_c.freeMemory == (freeBefore + expectedRetained)),
          "the release destroyed exactly the guard's images and memories",
          "vkDestroyImage " + U32(destroyBefore) + " -> " +
              U32(g_c.destroyImage) + ", vkFreeMemory " + U32(freeBefore) +
              " -> " + U32(g_c.freeMemory) + ", expected +" +
              U32(expectedRetained) + " each");

    // Idempotent and null-safe, per its own contract: a second call must
    // return 0 and destroy nothing. Without the take-it-off-the-registry-
    // first ordering this is a double free.
    const uint32_t destroyAfterFirst = g_c.destroyImage;
    const uint32_t freeAfterFirst    = g_c.freeMemory;
    const uint32_t releasedAgain = VkEncReleaseImportOrdinalGuard(m_ctx);
    Check(releasedAgain == 0,
          "a second release returns 0",
          "got " + U32(releasedAgain));
    Check((g_c.destroyImage == destroyAfterFirst) &&
              (g_c.freeMemory == freeAfterFirst),
          "a second release destroys nothing",
          "vkDestroyImage " + U32(destroyAfterFirst) + " -> " +
              U32(g_c.destroyImage) + ", vkFreeMemory " + U32(freeAfterFirst) +
              " -> " + U32(g_c.freeMemory) + " -- that is a double free");

    std::printf("\n=== summary ===\n");
    std::printf("  vkCreateImage            %u\n", g_c.createImage);
    std::printf("  vkAllocateMemory(import) %u\n", g_c.importAlloc);
    std::printf("  vkAllocateMemory(plain)  %u\n", g_c.plainAlloc);
    std::printf("  vkDestroyImage           %u\n", g_c.destroyImage);
    std::printf("  vkFreeMemory             %u\n", g_c.freeMemory);
    std::printf("  caller imports           2\n");
    std::printf("  guard imports (derived)  %u\n",
                (g_c.createImage >= 2) ? (g_c.createImage - 2) : 0);
    std::printf("  guard count expected     %u (%s)\n", effectiveCount,
                (effectiveCount == 0) ? "INERT" : "ARMED");
    std::printf("  (the destroy/free counts above are the guard RELEASE; the "
                "two caller imports are torn down after the thunks come "
                "out)\n");

    std::printf("\n%d/%d checks failed\n", g_failures, g_checks);
    return (g_failures == 0) ? 0 : 1;
}

void Usage(const char* argv0)
{
    std::printf(
        "usage: %s [--guard-count=N] [--expect-disabled] [--verbose]\n"
        "  (default)         assert the import-ordinal guard is INERT, which\n"
        "                    is what the shipped library does: its build\n"
        "                    constant kVkEncImportOrdinalGuardCount is 0, so\n"
        "                    a caller's dma-buf import must be the ONLY image\n"
        "                    created and the verdict must be DISABLED.\n"
        "  --guard-count=N   assert the ARMED behaviour of a library rebuilt\n"
        "                    with kVkEncImportOrdinalGuardCount = N: N\n"
        "                    sacrificial imports taken and retained ahead of\n"
        "                    the caller's, verdict COMPLETE, and N destroyed\n"
        "                    on release. N must match the library under test\n"
        "                    and must fit kVkEncImportOrdinalGuardCapacity.\n"
        "  --expect-disabled assert the kill switch forces the inert shape.\n"
        "                    That is what setting the environment variable\n"
        "                    VK_VIDEO_ENCODER_NO_IMPORT_ORDINAL_GUARD=1 must\n"
        "                    produce. Against an ARMED library\n"
        "                    (--guard-count=N, N>0) run WITHOUT the variable\n"
        "                    set, this arm fails -- which is the point;\n"
        "                    against the retired default both arms agree.\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv)
{
    bool expectDisabled = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--expect-disabled") {
            expectDisabled = true;
        } else if (a.rfind("--guard-count=", 0) == 0) {
            // Hand-parsed: no new include, and a silently-misread count would
            // turn every positional assertion below into a different claim.
            const std::string v = a.substr(sizeof("--guard-count=") - 1);
            if (v.empty() ||
                (v.find_first_not_of("0123456789") != std::string::npos)) {
                std::printf("--guard-count needs a non-negative integer, got "
                            "'%s'\n", v.c_str());
                return 1;
            }
            uint32_t n = 0;
            for (size_t k = 0; k < v.size(); k++) {
                n = (n * 10u) + (uint32_t)(v[k] - '0');
            }
            g_guardCount = n;
        } else if (a == "--verbose") {
            g_verbose = true;
        } else if ((a == "-h") || (a == "--help")) {
            Usage(argv[0]);
            return 0;
        } else {
            std::printf("unrecognised argument '%s'\n", a.c_str());
            Usage(argv[0]);
            return 1;
        }
    }

    std::printf("=====================================================\n");
    std::printf(" encoder-ext import-ordinal guard\n");
    std::printf("=====================================================\n");

    Harness h;
    const int rc = h.Run(expectDisabled);

    // A failure is never reported as a skip. Run() already returns 1 whenever
    // g_failures is non-zero, and this restates it so a future edit to Run()
    // cannot quietly relax it.
    if ((g_failures > 0) && (rc != 1)) {
        std::printf("INTERNAL: %d check(s) failed but the harness returned "
                    "%d; reporting FAILURE\n", g_failures, rc);
        return 1;
    }
    return rc;
}

#endif  // __linux__
