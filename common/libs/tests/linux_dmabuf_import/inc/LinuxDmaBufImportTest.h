/*
 * Copyright 2024-2026 NVIDIA Corporation.
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
// Linux dma-buf SECOND-DEVICE import test.
//
// What this proves (and nothing else does today):
//
//   An image allocated and exported by one VkDevice can be imported, bound
//   and READ by a DIFFERENT VkDevice on the SAME VkPhysicalDevice.
//
// That claim is the load-bearing premise of the whole "library creates its
// own device" direction: a different logical device on the same physical GPU
// has to work, or the library cannot own its VkDevice and still import a
// producer's buffers.
//
// The Windows sibling creates the second device but stops at
// vkBindImageMemory - it never touches the memory. This test goes further:
// it writes a known pattern through device A, hands the dma-buf to device B,
// and reads it back through device B. "Import returned VK_SUCCESS" is not
// accepted as proof that the memory is usable.
//
// Two arms run per modifier, through the SAME code path, differing only in
// which VkDevice performs the import:
//
//   "second-device"  import on VkDevice B   <- the claim under test
//   "same-device"    import on VkDevice A   <- control
//
// The control exists so a failure is attributable. second-device FAIL +
// same-device PASS is a device-independence defect. Both FAIL is an
// export/environment defect and says nothing about device independence.
//
// The IMPORT image's usage is separable from the exporter's, because that is
// the only half a Chromium-shaped consumer controls: the buffer is exported by
// GBM/Ozone and the VulkanVideoEncodeAccelerator picks imageUsage for its own
// VkImage from a vkGetPhysicalDeviceImageFormatProperties2 answer. Use
// --import-no-video-usage (or the raw --import-usage/--import-flags) to hold
// the exporter fixed and move only that half.
//
// WHAT THIS TEST IS LOOKING FOR, on an NV12 buffer:
//
//   a block-linear modifier imported WITHOUT
//   VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR can lose the ENTIRE chroma
//   plane. Luma compares clean, the first bad byte is at the chroma plane
//   offset, and CbCr reads back all zeros. Every Vulkan call returns
//   VK_SUCCESS, so only a content compare catches it.
//
//   It is the IMPORT image's usage that decides, and one bit of it. Holding the
//   modifier and the plane layouts fixed:
//
//     export usage        import usage                    result
//     ----------------------------------------------------------
//     0x4007 (encode)     0x4007 / 0x4001 (encode)        PASS
//     0x4007 (encode)     0x0007 / 0x0001 (no encode)     CHROMA LOST
//     0x0007 (no encode)  0x4001 (encode)                 PASS
//     0x0007 (no encode)  0x0007 (no encode)              CHROMA LOST
//
//   The create flags are not implicated: every import-flag combination
//   gives the same verdict for a given usage. Not every modifier is
//   affected, and LINEAR is clean without the bit -- it cannot carry it,
//   since vkCreateImage returns VK_ERROR_FORMAT_NOT_SUPPORTED for LINEAR
//   NV12 + VIDEO_ENCODE_SRC.
//
// Deliberately NOT covered:
//   - 3-plane I420 (VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM). That path hangs
//     the GPU on hardware; this test must not
//     be the thing that wedges a test machine.
//   - Cross-PHYSICAL-device import. The plan (section 3.2) says that is a
//     hard refusal by design (DEVICE_MISMATCH), so there is no claim to test.
//   - Semaphore/fence handle exchange. Synchronisation here is host-side
//     (fence wait + vkQueueWaitIdle) on purpose, so that a failure is a
//     memory-import failure and not a sync-import failure.
//=============================================================================

#pragma once

#if defined(__linux__)

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "VkCodecUtils/VulkanDeviceContext.h"

namespace linux_dmabuf_import_test {

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

//=============================================================================
// Pipeline steps. Every failure is reported as "the run stopped at <Step>",
// so a red result names the operation that broke rather than a bare VkResult.
//=============================================================================

enum class Step {
    NotStarted = 0,
    ExportDeviceInit,           // VulkanDeviceContext: instance + phys dev + device A
    ImportDeviceCreate,         // vkCreateDevice for device B on the same phys dev
    ImportDeviceDispatch,       // vkGetDeviceProcAddr for every entry point B needs
    ModifierEnumerate,          // vkGetPhysicalDeviceFormatProperties2 modifier list
    ModifierCapability,         // vkGetPhysicalDeviceImageFormatProperties2 (exportable/importable)
    ExportImageCreate,          // vkCreateImage on A, DRM modifier list + external memory
    ExportModifierReadback,     // vkGetImageDrmFormatModifierPropertiesEXT on A
    ExportMemoryAllocate,       // vkAllocateMemory on A, export + dedicated
    ExportBindMemory,           // vkBindImageMemory on A
    ExportPlaneLayouts,         // vkGetImageSubresourceLayout, MEMORY_PLANE_i aspects
    ExportFd,                   // vkGetMemoryFdKHR, handleType = DMA_BUF
    UploadPattern,              // staging buffer -> image on A
    ReleaseToForeign,           // A's queue family -> VK_QUEUE_FAMILY_FOREIGN_EXT
    ImportImageCreate,          // vkCreateImage on B, explicit modifier + plane layouts
    ImportFdMemoryTypes,        // vkGetMemoryFdPropertiesKHR on B for THIS fd
    ImportMemoryTypeSelect,     // intersect fd mask with B's image requirements
    ImportMemoryAllocate,       // vkAllocateMemory on B with VkImportMemoryFdInfoKHR
    ImportBindMemory,           // vkBindImageMemory on B
    AcquireFromForeign,         // VK_QUEUE_FAMILY_FOREIGN_EXT -> B's queue family
    Readback,                   // image -> staging buffer on B
    ContentCompare,             // byte compare against what A wrote
    Complete
};

const char* stepName(Step s);
const char* vkResultName(VkResult r);

//=============================================================================
// Configuration
//=============================================================================

struct TestConfig {
    bool        verbose{false};
    bool        validation{false};
    uint32_t    width{1920};
    uint32_t    height{1080};
    VkFormat    format{VK_FORMAT_G8_B8R8_2PLANE_420_UNORM};   // NV12
    bool        videoUsage{true};       // VIDEO_ENCODE_SRC + the flags that go with it
    // Import-side override for videoUsage. -1 follows videoUsage; 0 or 1
    // forces the IMPORT image's usage/flags independently of the exporter's.
    //
    // This exists because Chromium's VulkanVideoEncodeAccelerator is only
    // ever the importer: the buffer is exported by GBM/Ozone, and the VEA
    // chooses imageUsage for its own VkImage from
    // VveaModifierSupportsEncodeSrc(). Moving both halves together cannot
    // tell us whether a chroma loss is attributable to the half Chromium
    // controls.
    int         importVideoUsage{-1};
    // Raw overrides, applied after importVideoUsage. UINT32_MAX means
    // "not set". These exist to separate the usage bit from the create
    // flags, which otherwise move together.
    uint32_t    importUsageRaw{UINT32_MAX};
    uint32_t    importFlagsRaw{UINT32_MAX};
    bool        linearOnly{false};      // only DRM_FORMAT_MOD_LINEAR
    bool        contentCheck{true};     // write on A, read back on B, compare
    bool        sameDeviceControl{true};// also run the single-device control arm
    uint64_t    onlyModifier{DRM_FORMAT_MOD_INVALID};  // restrict to one modifier
};

//=============================================================================
// Per-device function table.
//
// Device B is created with raw vkCreateDevice, so it has no VulkanDeviceContext
// behind it and needs its own dispatch. Device A gets an identical table loaded
// the same way, which is what lets both arms run the SAME code: if the two arms
// disagree, the difference is the VkDevice and not the code path.
//=============================================================================

struct DeviceFns {
    PFN_vkGetDeviceQueue                GetDeviceQueue{nullptr};
    PFN_vkDestroyDevice                 DestroyDevice{nullptr};
    PFN_vkCreateImage                   CreateImage{nullptr};
    PFN_vkDestroyImage                  DestroyImage{nullptr};
    PFN_vkGetImageMemoryRequirements    GetImageMemoryRequirements{nullptr};
    PFN_vkGetImageSubresourceLayout     GetImageSubresourceLayout{nullptr};
    PFN_vkAllocateMemory                AllocateMemory{nullptr};
    PFN_vkFreeMemory                    FreeMemory{nullptr};
    PFN_vkBindImageMemory               BindImageMemory{nullptr};
    PFN_vkCreateBuffer                  CreateBuffer{nullptr};
    PFN_vkDestroyBuffer                 DestroyBuffer{nullptr};
    PFN_vkGetBufferMemoryRequirements   GetBufferMemoryRequirements{nullptr};
    PFN_vkBindBufferMemory              BindBufferMemory{nullptr};
    PFN_vkMapMemory                     MapMemory{nullptr};
    PFN_vkUnmapMemory                   UnmapMemory{nullptr};
    PFN_vkFlushMappedMemoryRanges       FlushMappedMemoryRanges{nullptr};
    PFN_vkInvalidateMappedMemoryRanges  InvalidateMappedMemoryRanges{nullptr};
    PFN_vkCreateCommandPool             CreateCommandPool{nullptr};
    PFN_vkDestroyCommandPool            DestroyCommandPool{nullptr};
    PFN_vkAllocateCommandBuffers        AllocateCommandBuffers{nullptr};
    PFN_vkFreeCommandBuffers            FreeCommandBuffers{nullptr};
    PFN_vkBeginCommandBuffer            BeginCommandBuffer{nullptr};
    PFN_vkEndCommandBuffer              EndCommandBuffer{nullptr};
    PFN_vkCmdPipelineBarrier            CmdPipelineBarrier{nullptr};
    PFN_vkCmdCopyBufferToImage          CmdCopyBufferToImage{nullptr};
    PFN_vkCmdCopyImageToBuffer          CmdCopyImageToBuffer{nullptr};
    PFN_vkQueueSubmit                   QueueSubmit{nullptr};
    PFN_vkQueueWaitIdle                 QueueWaitIdle{nullptr};
    PFN_vkDeviceWaitIdle                DeviceWaitIdle{nullptr};
    PFN_vkCreateFence                   CreateFence{nullptr};
    PFN_vkDestroyFence                  DestroyFence{nullptr};
    PFN_vkWaitForFences                 WaitForFences{nullptr};
    PFN_vkGetMemoryFdKHR                GetMemoryFdKHR{nullptr};
    PFN_vkGetMemoryFdPropertiesKHR      GetMemoryFdPropertiesKHR{nullptr};
    PFN_vkGetImageDrmFormatModifierPropertiesEXT
                                        GetImageDrmFormatModifierPropertiesEXT{nullptr};

    // Loads every pointer above. On the first null it returns false and puts
    // the entry-point name in missingOut - a missing extension shows up as a
    // named symbol rather than as a segfault on first use.
    bool loadAll(PFN_vkGetDeviceProcAddr gdpa, VkDevice device,
                 std::string& missingOut);
};

//=============================================================================
// A device participating in the test (A = exporter/control, B = importer).
//=============================================================================

struct TestDevice {
    std::string     label;
    VkDevice        device{VK_NULL_HANDLE};
    VkQueue         queue{VK_NULL_HANDLE};
    uint32_t        queueFamily{UINT32_MAX};
    VkCommandPool   cmdPool{VK_NULL_HANDLE};
    DeviceFns       fn;
    bool            ownsDevice{false};   // true only for B; A belongs to VulkanDeviceContext
};

//=============================================================================
// Format description used for the content round-trip.
//=============================================================================

struct PlaneDesc {
    VkImageAspectFlagBits aspect;
    uint32_t              widthDiv;
    uint32_t              heightDiv;
    uint32_t              texelBytes;
};

struct FormatDesc {
    bool                   known{false};
    std::vector<PlaneDesc> planes;
};

FormatDesc describeFormat(VkFormat format);
const char* formatName(VkFormat format);

//=============================================================================
// One modifier's capability, from the PHYSICAL-device-scoped query. The plan
// (section 3.2) leans on that query being physical-device scoped, which is
// exactly why a second logical device is expected to be able to import.
//=============================================================================

struct ModifierCandidate {
    uint64_t            modifier{DRM_FORMAT_MOD_INVALID};
    uint32_t            memoryPlaneCount{0};
    VkFormatFeatureFlags tilingFeatures{0};
    bool                queried{false};
    VkResult            queryResult{VK_SUCCESS};
    bool                exportable{false};
    bool                importable{false};
    bool                usable{false};   // exportable && importable && image format supported
    std::string         note;
};

//=============================================================================
// One end-to-end export/import cycle.
//=============================================================================

struct ArmResult {
    std::string     arm;                 // "second-device" | "same-device"
    uint64_t        modifier{DRM_FORMAT_MOD_INVALID};

    Step            lastStepStarted{Step::NotStarted};
    Step            failedAt{Step::NotStarted};
    VkResult        vkResult{VK_SUCCESS};
    std::string     detail;

    // Evidence that the two devices really are two devices on one GPU.
    uint64_t        exportDeviceHandle{0};
    uint64_t        importDeviceHandle{0};

    VkDeviceSize    exportAllocSize{0};
    uint32_t        exportMemTypeIndex{UINT32_MAX};
    VkDeviceSize    importMemReqSize{0};
    uint32_t        importMemTypeBits{0};
    uint32_t        fdMemTypeBits{0};
    uint32_t        importMemTypeIndex{UINT32_MAX};
    uint64_t        readbackModifier{DRM_FORMAT_MOD_INVALID};

    bool            importSucceeded{false};   // create + alloc + bind all OK on the import device
    bool            contentAttempted{false};
    bool            contentVerified{false};
    uint64_t        firstMismatchOffset{UINT64_MAX};
    uint32_t        expectedByte{0};
    uint32_t        actualByte{0};
    uint64_t        comparedBytes{0};

    bool ok() const { return failedAt == Step::NotStarted; }
};

//=============================================================================
// Overall verdict. Deliberately tri-state: "the test did not run" must never
// be reportable as "the claim holds".
//=============================================================================

enum class Verdict {
    ClaimVerified,      // >= 1 modifier: second-device import AND content round-trip OK
    ClaimFailed,        // a second-device arm failed (or every arm did)
    CouldNotRun         // no GPU / no extension / nothing to test - NOT a pass
};

class LinuxDmaBufImportTest {
public:
    LinuxDmaBufImportTest() = default;
    ~LinuxDmaBufImportTest();

    // Brings up device A, then device B. A non-VK_SUCCESS return means the
    // test could not run at all; the caller must report CouldNotRun, not pass.
    VkResult init(const TestConfig& config);

    // Runs every usable modifier x {second-device, same-device}.
    std::vector<ArmResult> run();

    const std::vector<ModifierCandidate>& modifiers() const { return m_modifiers; }
    const std::string& initFailureDetail() const { return m_initFailureDetail; }
    Step initFailedAt() const { return m_initFailedAt; }

    void printResults(const std::vector<ArmResult>& results) const;
    Verdict verdict(const std::vector<ArmResult>& results, std::string& reasonOut) const;

private:
    VkResult createImportDevice();
    VkResult setUpDevice(TestDevice& dev);          // command pool + queue
    void     tearDownDevice(TestDevice& dev);

    VkResult enumerateModifiers();                  // fills m_modifiers

    ArmResult runCycle(const char* armLabel,
                       TestDevice& importDev,
                       const ModifierCandidate& mod);

    // Records + submits a one-shot command buffer, waits on a fence.
    VkResult submitOneShot(TestDevice& dev,
                           VkCommandBuffer cmd,
                           const char*& failedCall);

    uint32_t chooseMemoryType(uint32_t candidateMask,
                              VkMemoryPropertyFlags required) const;

    VkImageUsageFlags  exportUsage() const;
    VkImageCreateFlags exportFlags() const;
    // The import image's usage/flags. Equal to the export pair unless
    // TestConfig::importVideoUsage overrides it.
    bool               importVideoUsage() const;
    VkImageUsageFlags  importUsage() const;
    VkImageCreateFlags importFlags() const;

    // Host-visible staging buffer helper. Returns the buffer + memory + mapped
    // pointer; caller destroys via destroyStaging().
    struct Staging {
        VkBuffer        buffer{VK_NULL_HANDLE};
        VkDeviceMemory  memory{VK_NULL_HANDLE};
        void*           mapped{nullptr};
        VkDeviceSize    size{0};
        bool            coherent{false};
    };
    VkResult createStaging(TestDevice& dev, VkDeviceSize size,
                           VkBufferUsageFlags usage, Staging& out);
    void     destroyStaging(TestDevice& dev, Staging& s);

    TestConfig                      m_config;
    VulkanDeviceContext             m_vkDevCtx;         // owns instance + device A
    TestDevice                      m_devA;             // export / control device
    TestDevice                      m_devB;             // second device under test
    VkPhysicalDeviceMemoryProperties m_memProps{};
    std::vector<ModifierCandidate>  m_modifiers;
    FormatDesc                      m_formatDesc;
    std::string                     m_initFailureDetail;
    Step                            m_initFailedAt{Step::NotStarted};
    std::string                     m_deviceName;
};

} // namespace linux_dmabuf_import_test

#endif // __linux__
