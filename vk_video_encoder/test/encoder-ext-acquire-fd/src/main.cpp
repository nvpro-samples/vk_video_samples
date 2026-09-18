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
 * Per-frame ACQUIRE fence fd-ownership coverage, on a real device.
 *
 * WHAT IS UNDER TEST. VkVideoEncoderFrameFenceDescriptor::acquireFenceFd --
 * the IMPORT half of the per-frame fence entry point, and specifically the
 * ownership promise attached to it. The public header says of that field "the
 * library takes ownership and closes it on every exit path", and design
 * section 2.3 states the rule the whole handle API is built on: the library
 * consumes POSIX fds it is given, always, on every exit path, "including
 * argument-validation failures that never reach Vulkan". The SYNC_FD row of
 * that section's ownership table names the acquire fence explicitly.
 *
 * THE DEFECT THIS PINS. acquireFenceFd was read at exactly one site -- inside
 * the resolving walk over the chained descriptors -- and closed only inside
 * ImportAcquireFenceLocked. Every refusal that returns before that site
 * therefore leaked the caller's fd:
 *
 *   - a mis-stamped info.sType                        (top of the function)
 *   - a session that is not initialized               (top of the function)
 *   - a resource id that does not resolve             (top of the function)
 *   - an unknown chained sType AHEAD of the fence node    (in the walk)
 *   - an unresolvable registered id AHEAD of the fence node (in the walk)
 *
 * The last two are reachable only because the header blesses the nested chain
 * shape `info.pNext = &sync; sync.pNext = &fence;` -- a refusal raised while
 * walking the leading node returns before the fence node is ever visited.
 *
 * This is a live leak, not a theoretical one: Chromium's
 * media/gpu/vulkan/vulkan_video_encode_accelerator.cc arms the field on
 * essentially every frame with `fence_desc.acquireFenceFd = fd.release();`,
 * under a comment saying ownership transfers.
 *
 * HOW IT IS PROVED, and why each instrument is here.
 *
 *   fcntl(fd, F_GETFD) == -1 with errno EBADF  -- the fd is gone. This is the
 *       direct statement of the promise, and it is what goes red on the
 *       unfixed library.
 *   an interposed close(2)                     -- counts the closes the
 *       library performed on that EXACT fd number during the call. EBADF
 *       alone cannot tell one close from two, and a double close is worse
 *       than the leak: the number is free the instant the first close
 *       returns, so the second one can land on an unrelated descriptor the
 *       process has since opened. "Exactly once" needs a counter, so there is
 *       one. The wrapper is pure passthrough unless a watch is armed.
 *   /proc/self/fd                              -- an independent second
 *       opinion that does not depend on the interposer being wired at all.
 *
 * WHY THE FDS ARE REAL SYNC FDS. A pipe fd would exercise close(2) just as
 * well on the refusal paths, but not on the success path: there the fd must
 * survive vkImportSemaphoreFdKHR, which only a genuine sync_file will. So
 * every fd here is minted the way a producer mints one -- exported from a
 * semaphore carrying a real queue signal -- by asking the library itself for
 * a per-frame RELEASE fence and then handing that fd back as the next frame's
 * ACQUIRE fence. That is not a trick: the header documents exactly that
 * lifetime for an exported fence ("close(2) it, or hand it to exactly one
 * import, which consumes it"), and it is the true producer/consumer shape.
 *
 * WHY IT NEEDS A GPU. Minting a real sync_fd needs a real queue signal, and
 * the success case needs a real import. With no encode-capable device this
 * exits 77, which CTest reads as SKIP. A session that DID come up and then
 * could not mint an fd is a FAILURE, not a skip -- otherwise a regressed
 * export would turn this whole file green by making it vanish.
 */

#include "vulkan_video_encoder_ext.h"

// The public header reaches the Xlib platform headers, whose macros collide
// with ordinary identifiers. Same scrub, same reason, as the sibling tests.
#undef Status
#undef None
#undef Bool
#undef Window

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;
const char* g_case = "setup";

// Selected by argv. --caller-wait-order runs ONLY case 10, the ordering
// case, so that the claim it pins has a CTest entry of its own that names it.
// It is not a different assertion level: case 10 asserts the same things in
// both modes. The claim holds (see the note on case 10), so the split buys
// focus and nothing else: it gives the ordering claim a name of its own in
// the CTest output.
bool g_assertCallerWaitOrder = false;

// The per-case check floor for case 10. Named because two places assert it:
// the ledger table in main(), and the --caller-wait-order mode, which does
// not run the table and would otherwise be a mode that can pass by doing
// nothing.
const int kCallerWaitOrderFloor = 10;

void Check(bool ok, const char* what, const std::string& detail)
{
    g_checks++;
    if (ok) {
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

//=============================================================================
// The close(2) interposer.
//
// A strong definition of close() in the EXECUTABLE takes precedence over
// libc's for every call site linked into it, which includes the whole static
// encoder archive under test. Everything is forwarded to the real close via
// RTLD_NEXT, so this changes no behaviour; it only counts.
//
// It counts closes of ONE fd, and only while a watch is armed, deliberately.
// A global tally would be dominated by the driver's own fd traffic and could
// prove nothing about a particular handle.
//=============================================================================
std::atomic<int>   g_watchedFd(-1);
std::atomic<int>   g_closeCount(0);
std::atomic<void*> g_realClose(nullptr);

}  // namespace

extern "C" int close(int fd)
{
    void* real = g_realClose.load(std::memory_order_acquire);
    if (real == nullptr) {
        // Racing resolvers all compute the same address, so the last writer
        // wins harmlessly. dlsym does not itself close descriptors.
        real = dlsym(RTLD_NEXT, "close");
        g_realClose.store(real, std::memory_order_release);
    }
    if (fd == g_watchedFd.load(std::memory_order_relaxed)) {
        g_closeCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (real == nullptr) {
        errno = EBADF;
        return -1;
    }
    return ((int (*)(int))real)(fd);
}

namespace {

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

const uint32_t kWidth  = 1920;
const uint32_t kHeight = 1080;
// Enough repetitions for the fd table to show drift if one fd per refusal is
// retained, and short enough to stay well inside the CTest timeout.
const uint32_t kRefusalLoopIterations = 120;

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
    PFN_vkGetPhysicalDeviceProperties       GetPhysicalDeviceProperties =
        nullptr;
    // For case 10: a caller wait semaphore this test can hold unsignalled and
    // then signal from the host, which is what turns "both waits were
    // honoured" into something observable rather than merely consistent.
    PFN_vkCreateSemaphore                   CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore                  DestroySemaphore = nullptr;
    PFN_vkSignalSemaphore                   SignalSemaphore = nullptr;
    // The SIGNAL half of case 10: reading the caller timeline back is what
    // makes "the caller signal array survived the release-fence append" an
    // observation rather than an inference.
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

// A host-written LINEAR NV12 image, transfer-source usage only, so the
// registration routes STAGED: the staging copy is then the submission that
// reads the input and therefore the one that signals the release fence. That
// is the arm the sibling release-fence test measured at 300/300 fds, which is
// what makes it a dependable source of real sync_fds here.
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

//=============================================================================
// The session under test, plus the fd mint.
//=============================================================================
VkSharedBaseObj<VulkanVideoEncoderExt> g_encoder;
DeviceFns                              g_fns;
VkDevice                               g_device   = VK_NULL_HANDLE;
VkVideoEncoderResource                 g_resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
uint64_t                               g_frameId  = 0;
uint32_t                               g_captured = 0;
uint64_t                               g_capturedBytes = 0;

void DrainCaptures()
{
    VkVideoEncodeResult r;
    while (g_encoder->AcquireNextEncodedFrame(r) == VK_SUCCESS) {
        g_captured++;
        g_capturedBytes += r.bitstreamSize;
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

// One real sync_fd, exported by the library from a semaphore its own queue
// signals. Carries NO acquire fence itself, so a NOT_READY retry here can
// never lose a handle: the only thing at stake is the out-parameter, which
// the next attempt rewrites.
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
                struct timespec ts = {0, 1000000};  // 1 ms
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
            return fd;
        }
        // -1 is a legal answer ("no fence"), not a failure. Try the next
        // frame rather than giving up -- but never silently, so a run that
        // needed many attempts is visible.
        std::printf("  MINT: frame %llu answered -1, retrying\n",
                    (unsigned long long)(g_frameId - 1));
    }
    return -1;
}

//=============================================================================
// Everything observable about one watched call.
//=============================================================================
struct CallResult {
    VkVideoEncoderStatusCode status = VK_VIDEO_ENCODER_STATUS_SUCCESS;
    int  closes       = 0;
    bool stillOpen    = true;
    int  errnoAfter   = 0;
    int  fdCountDelta = 0;
};

CallResult RunWatched(VulkanVideoEncoderExt* enc,
                      VkVideoEncoderFrameSubmitInfo& info, int fd)
{
    CallResult r;
    const int before = OpenFdCount();
    g_closeCount.store(0, std::memory_order_relaxed);
    g_watchedFd.store(fd, std::memory_order_relaxed);

    r.status = enc->SubmitRegisteredFrame(info, nullptr);

    g_watchedFd.store(-1, std::memory_order_relaxed);
    r.closes = g_closeCount.load(std::memory_order_relaxed);
    // Immediately, before anything in this process can open a descriptor and
    // be handed the same number back.
    errno = 0;
    r.stillOpen   = (fcntl(fd, F_GETFD) != -1);
    r.errnoAfter  = errno;
    r.fdCountDelta = OpenFdCount() - before;
    return r;
}

// The three assertions every refusal owes, plus the status it must answer.
void ExpectRefusalConsumedTheFd(const CallResult& r,
                                VkVideoEncoderStatusCode want)
{
    Check(r.status == want, "status",
          "got " + I64((long long)r.status) + ", want " +
              I64((long long)want));
    Check(!r.stillOpen, "the acquire fd was closed",
          r.stillOpen ? "fcntl(F_GETFD) still succeeds -- THE FD LEAKED"
                      : "closed");
    Check(r.stillOpen || (r.errnoAfter == EBADF), "fcntl reports EBADF",
          "errno " + I64(r.errnoAfter));
    Check(r.closes == 1, "closed exactly once",
          I64(r.closes) + " close(2) calls on that fd -- 0 is a leak, "
          "2 can close an unrelated descriptor");
    Check(r.fdCountDelta == -1, "/proc/self/fd fell by exactly one",
          "delta " + I64(r.fdCountDelta));
    std::printf("    [%s] status=%d closes=%d stillOpen=%d fdDelta=%d\n",
                g_case, (int)r.status, r.closes, (int)r.stillOpen,
                r.fdCountDelta);
}

//=============================================================================
// Case 1 -- a mis-stamped info.sType. Refuses at the very top of the
// function, before the descriptor walk exists.
//=============================================================================
void CaseWrongTopLevelSType()
{
    g_case = "WrongTopLevelSType";
    const int fd = MintSyncFd();
    if (fd < 0) {
        Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
        return;
    }
    int releaseFd = 0x5EED;
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = fd;
    fence.pReleaseFenceFd = &releaseFd;

    VkVideoEncoderFrameSubmitInfo info = BaseInfo();
    // A real sType from this same header, just not the one this entry point
    // takes -- a likelier caller error than a random integer.
    info.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_SYNC_DESCRIPTOR;
    info.pNext = &fence;

    const CallResult r = RunWatched(g_encoder.get(), info, fd);
    ExpectRefusalConsumedTheFd(
        r, VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN);
    // The OUT half of the same promise, on the same exit. ext.h says the
    // library writes pReleaseFenceFd before anything in the call can refuse,
    // naming ERROR_STRUCTURE_TYPE_UNKNOWN specifically.
    Check(releaseFd == -1, "pReleaseFenceFd was written",
          "got " + I64(releaseFd) + ", want -1");
}

//=============================================================================
// Case 2 -- a session that is not initialized. Same top-of-function region,
// a different refusal, and the one a caller hits by mis-sequencing its own
// startup.
//=============================================================================
void CaseNotInitialized()
{
    g_case = "NotInitialized";
    const int fd = MintSyncFd();
    if (fd < 0) {
        Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
        return;
    }
    VkSharedBaseObj<VulkanVideoEncoderExt> fresh;
    if ((CreateVulkanVideoEncoderExt(fresh) != VK_SUCCESS) || !fresh) {
        Check(false, "CreateVulkanVideoEncoderExt", "second session");
        ::close(fd);
        return;
    }
    int releaseFd = 0x5EED;
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = fd;
    fence.pReleaseFenceFd = &releaseFd;

    VkVideoEncoderFrameSubmitInfo info = {};
    info.sType         = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS;
    info.pNext         = &fence;
    info.resource      = g_resource;  // meaningless to a session with none
    info.qpOverride    = -1;
    info.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const CallResult r = RunWatched(fresh.get(), info, fd);
    ExpectRefusalConsumedTheFd(
        r, VK_VIDEO_ENCODER_STATUS_ERROR_NOT_INITIALIZED);
    Check(releaseFd == -1, "pReleaseFenceFd was written",
          "got " + I64(releaseFd) + ", want -1");
    fresh = nullptr;
}

//=============================================================================
// Case 3 -- a resource id that does not resolve. Refuses under the resource
// lock, still ahead of the descriptor walk. This is the one a producer hits
// by racing an Unregister against an in-flight submit, which is exactly when
// it is holding a fence fd.
//=============================================================================
void CaseResourceUnknown()
{
    g_case = "ResourceUnknown";
    const int fd = MintSyncFd();
    if (fd < 0) {
        Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
        return;
    }
    int releaseFd = 0x5EED;
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = fd;
    fence.pReleaseFenceFd = &releaseFd;

    VkVideoEncoderFrameSubmitInfo info = BaseInfo();
    info.pNext    = &fence;
    info.resource = (VkVideoEncoderResource)0xDEADBEEFull;

    const CallResult r = RunWatched(g_encoder.get(), info, fd);
    ExpectRefusalConsumedTheFd(
        r, VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN);
    Check(releaseFd == -1, "pReleaseFenceFd was written",
          "got " + I64(releaseFd) + ", want -1");
}

// A chained struct this library does not know, laid out the way Vulkan's
// pNext convention requires: {sType, pNext} first.
struct UnknownChainNode {
    VkVideoEncoderStructureType sType;
    const void*                 pNext;
};

//=============================================================================
// Case 4 -- an unknown chained sType AHEAD of the fence descriptor. The walk
// refuses on the leading node and never reaches the fence node at all. Only
// reachable because the header blesses a flat, order-independent chain.
//=============================================================================
void CaseUnknownSTypeAheadOfFence()
{
    g_case = "UnknownSTypeAheadOfFence";
    const int fd = MintSyncFd();
    if (fd < 0) {
        Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
        return;
    }
    int releaseFd = 0x5EED;
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = fd;
    fence.pReleaseFenceFd = &releaseFd;

    UnknownChainNode unknown;
    unknown.sType = (VkVideoEncoderStructureType)0x7F00DEAD;
    unknown.pNext = &fence;

    VkVideoEncoderFrameSubmitInfo info = BaseInfo();
    info.pNext = &unknown;

    const CallResult r = RunWatched(g_encoder.get(), info, fd);
    ExpectRefusalConsumedTheFd(
        r, VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN);
    Check(releaseFd == -1, "pReleaseFenceFd was written",
          "got " + I64(releaseFd) + ", want -1");
}

//=============================================================================
// Case 5 -- an unresolvable registered WAIT id ahead of the fence descriptor.
// `info.pNext = &sync; sync.pNext = &fence;` is one of the three chain shapes
// the header blesses by name.
//=============================================================================
void CaseUnresolvableWaitIdAheadOfFence()
{
    g_case = "UnresolvableWaitIdAheadOfFence";
    const int fd = MintSyncFd();
    if (fd < 0) {
        Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
        return;
    }
    int releaseFd = 0x5EED;
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = fd;
    fence.pReleaseFenceFd = &releaseFd;

    const VkVideoEncoderResource bogus = (VkVideoEncoderResource)0xBADC0FFEEull;
    const uint64_t               value = 7;
    VkVideoEncoderFrameSyncDescriptor sync;
    sync.pNext           = &fence;
    sync.waitCount       = 1;
    sync.pWaitSemaphores = &bogus;
    sync.pWaitValues     = &value;

    VkVideoEncoderFrameSubmitInfo info = BaseInfo();
    info.pNext = &sync;

    const CallResult r = RunWatched(g_encoder.get(), info, fd);
    ExpectRefusalConsumedTheFd(
        r, VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN);
    Check(releaseFd == -1, "pReleaseFenceFd was written",
          "got " + I64(releaseFd) + ", want -1");
}

//=============================================================================
// Case 6 -- the same, on the SIGNAL side of the leading sync node. A separate
// refusal site in the walk, and the fence node is equally unreached.
//=============================================================================
void CaseUnresolvableSignalIdAheadOfFence()
{
    g_case = "UnresolvableSignalIdAheadOfFence";
    const int fd = MintSyncFd();
    if (fd < 0) {
        Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
        return;
    }
    int releaseFd = 0x5EED;
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = fd;
    fence.pReleaseFenceFd = &releaseFd;

    const VkVideoEncoderResource bogus = (VkVideoEncoderResource)0xBADC0FFEEull;
    const uint64_t               value = 7;
    VkVideoEncoderFrameSyncDescriptor sync;
    sync.pNext             = &fence;
    sync.signalCount       = 1;
    sync.pSignalSemaphores = &bogus;
    sync.pSignalValues     = &value;

    VkVideoEncoderFrameSubmitInfo info = BaseInfo();
    info.pNext = &sync;

    const CallResult r = RunWatched(g_encoder.get(), info, fd);
    ExpectRefusalConsumedTheFd(
        r, VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN);
    Check(releaseFd == -1, "pReleaseFenceFd was written",
          "got " + I64(releaseFd) + ", want -1");
}

//=============================================================================
// Case 6b -- A CYCLIC pNext CHAIN. `fence.pNext = &fence` is one assignment
// away in caller memory and nothing in the ABI forbids it. Before the chain
// walk was bounded this call did not refuse and did not return: it spun inside
// the library at 100% CPU, with the caller's fd still open, forever. A hang is
// a worse answer than any refusal, which is why this is asserted.
//
// That the case terminates at all is half the assertion, and CTest's TIMEOUT
// is the instrument for that half. The other half is that the bound did not
// cost the consumption promise: the fd here is real and armed, so a chain the
// library refuses to WALK is still a chain it was handed an fd on and still
// owes a close for.
//
// It also pins the pReleaseFenceFd half of the same pre-pass, which had the
// identical unbounded exposure.
//=============================================================================
void CaseCyclicChainIsRefused()
{
    g_case = "CyclicChainRefused";
    const int fd = MintSyncFd();
    if (fd < 0) {
        Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
        return;
    }
    int releaseFd = 0x5EED;
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = fd;
    fence.pReleaseFenceFd = &releaseFd;
    fence.pNext           = &fence;  // the cycle

    VkVideoEncoderFrameSubmitInfo info = BaseInfo();
    info.pNext = &fence;

    const CallResult r = RunWatched(g_encoder.get(), info, fd);
    ExpectRefusalConsumedTheFd(
        r, VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN);
    Check(releaseFd == -1, "pReleaseFenceFd was written",
          "got " + I64(releaseFd) + ", want -1");
}

//=============================================================================
// Case 6c -- ONE fd NAMED IN TWO FENCE DESCRIPTORS.
//
// The guard's Record() deduplicates by value so it can never close one number
// twice, and for a while a comment claimed that dedup also made the walk
// REPORT the duplicate. It did not: nothing compared the two nodes, the second
// Consume() was a silent no-op, and ImportAcquireFenceLocked ran a second time
// on a descriptor the first import had already consumed -- which is not
// guaranteed to fail, so the caller could come away with two semaphores whose
// payload came from one fence.
//
// The consume site now refuses. What is asserted here:
//
//   status == ERROR_IMPORT_FAILED -- a TYPED refusal, not silence.
//   libraryCloses == 0            -- the load-bearing one. The first import
//                                    owns the descriptor; a close on this
//                                    path is exactly the double close the
//                                    guard exists to prevent, and it is the
//                                    failure this whole file calls worse than
//                                    the leak it fixes.
//
// stillOpen is REPORTED, not asserted, for the same reason as on the success
// path: the fd went to the driver at the first import and the driver is under
// no obligation to close it.
//=============================================================================
void CaseDuplicateAcquireFdIsRefused()
{
    g_case = "DuplicateAcquireFd";
    const int fd = MintSyncFd();
    if (fd < 0) {
        Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
        return;
    }
    int releaseFdA = 0x5EED;
    int releaseFdB = 0x5EED;
    VkVideoEncoderFrameFenceDescriptor second;
    second.acquireFenceFd  = fd;   // the SAME number
    second.pReleaseFenceFd = &releaseFdB;

    VkVideoEncoderFrameFenceDescriptor first;
    first.acquireFenceFd  = fd;
    first.pReleaseFenceFd = &releaseFdA;
    first.pNext           = &second;

    VkVideoEncoderFrameSubmitInfo info = BaseInfo();
    info.pNext = &first;

    const CallResult r = RunWatched(g_encoder.get(), info, fd);
    Check(r.status == VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED,
          "a duplicated acquire fd is a typed refusal",
          "got " + I64((long long)r.status) + ", want IMPORT_FAILED (" +
              I64((long long)VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED) +
              ")");
    Check(r.closes == 0,
          "the library did not close what the first import had taken",
          I64(r.closes) + " close(2) calls -- anything above 0 here is the "
          "double close");
    Check(releaseFdA == -1, "the first pReleaseFenceFd was written",
          "got " + I64(releaseFdA) + ", want -1");
    Check(releaseFdB == -1, "the second pReleaseFenceFd was written",
          "got " + I64(releaseFdB) + ", want -1");
    std::printf("    [%s] status=%d libraryCloses=%d "
                "stillOpenAfterImport=%d (reported, not asserted) "
                "fdDelta=%d\n",
                g_case, (int)r.status, r.closes, (int)r.stillOpen,
                r.fdCountDelta);
}

//=============================================================================
// Case 7 -- THE SUCCESS PATH IS UNCHANGED, AND IS NOT DOUBLE-CONSUMED.
//
// WHAT THE LIBRARY MUST DO HERE IS THE OPPOSITE OF THE CASES ABOVE, and that
// is why it is worth running. On a refusal the library owes a close. On
// success it owes NO close at all: vkImportSemaphoreFdKHR takes ownership of
// a SYNC_FD, so from that call onward the fd belongs to the driver, and a
// library close would be a double close -- the worse of the two failures,
// ahead of the leak.
//
// So the assertion is `the library closed it ZERO times`, measured on the
// same counter the refusal cases use. That is exactly what goes red if the
// pre-pass guard is left holding the fd across the import, which is the one
// way this fix could have broken the working path. It was checked by deleting
// that handoff and rebuilding: the mutant reports 1 close here and stays
// green on all six refusals.
//
// AND THE FRAME REALLY ENCODED WITH THE FENCE IN ITS WAIT LIST. The proof is
// the RELEASE fence of the same frame. That fd is exported from a semaphore
// signalled by the submission which CONSUMES the input image -- so if it
// becomes signalled, that submission ran to completion, which it could not
// have done unless the imported acquire semaphore it was told to wait on was
// satisfied. A dropped import, a semaphore closed out from under the driver,
// or a wait on a payload that never arrives all show up here as a fence that
// never signals.
//
// WHY fcntl(F_GETFD) IS NOT ASSERTED HERE. It is reported. The driver takes
// ownership but is under no obligation to close the descriptor at import
// time, and NVIDIA's does not -- measured identically on both builds, fixed
// and unfixed. Asserting EBADF here would be asserting a driver implementation
// detail. Where the fds must provably go away is the refusal path, and that
// is asserted, per call and again in bulk in case 9.
//=============================================================================
void CaseSuccessPathConsumesOnce()
{
    g_case = "SuccessPathConsumesOnce";
    int       notReadyRetries = 0;
    long long notReadyCloses  = 0;
    for (int attempt = 0; attempt < 64; attempt++) {
        const int fd = MintSyncFd();
        if (fd < 0) {
            Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
            return;
        }
        int releaseFd = 0x5EED;
        VkVideoEncoderFrameFenceDescriptor fence;
        fence.acquireFenceFd  = fd;
        fence.pReleaseFenceFd = &releaseFd;

        VkVideoEncoderFrameSubmitInfo info = BaseInfo();
        info.pNext = &fence;

        const CallResult r = RunWatched(g_encoder.get(), info, fd);
        if (r.status == VK_VIDEO_ENCODER_STATUS_NOT_READY) {
            // Admission-control backpressure. That refusal sits BELOW the
            // import, so the fd went to the driver exactly as on the success
            // path and the library owes no close here either; the retry must
            // still carry a fresh fd rather than the same number.
            //
            // ACCUMULATED, NOT ASSERTED PER ITERATION. How many times this
            // arm is taken is timing dependent -- it moved between 0 and 1
            // across three consecutive runs -- so a Check() here made the
            // suite's total check count run-variable, which is exactly the
            // kind of number that gets quoted as evidence and cannot bear it.
            // One assertion below covers every retry instead, which is also
            // strictly stronger than asserting each one alone.
            notReadyRetries++;
            notReadyCloses += r.closes;
            DrainCaptures();
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, nullptr);
            continue;
        }
        g_frameId++;
        Check(r.status == VK_VIDEO_ENCODER_STATUS_SUCCESS, "status",
              "got " + I64((long long)r.status) + ", want SUCCESS");
        // SUCCESS means the import ran and returned a semaphore -- a failed
        // import is reported as ERROR_IMPORT_FAILED, never as SUCCESS -- so
        // the driver has the fd. The library must not also have closed it.
        Check(r.closes == 0,
              "the library did not close what it handed to the driver",
              I64(r.closes) + " close(2) calls on a descriptor "
              "vkImportSemaphoreFdKHR already owns -- that is a DOUBLE CLOSE, "
              "which can shut an unrelated fd opened at the same number");
        std::printf("    [%s] status=%d libraryCloses=%d "
                    "stillOpenAfterImport=%d (the driver's to close; reported, "
                    "not asserted) releaseFd=%d\n",
                    g_case, (int)r.status, r.closes, (int)r.stillOpen,
                    releaseFd);

        // The frame really ran WITH the acquire fence in its wait list.
        Check(releaseFd >= 0,
              "the input-consuming submit was issued for the fenced frame",
              "release fd " + I64(releaseFd));
        if (releaseFd >= 0) {
            const int signalled = PollSignalled(releaseFd, 10000);
            Check(signalled == 1,
                  "the input-consuming submit COMPLETED, so the imported "
                  "acquire wait was satisfied",
                  "poll returned " + I64(signalled) +
                      " -- a frame whose acquire semaphore never resolves "
                      "never gets here");
            ::close(releaseFd);
        }
        DrainCaptures();
        std::printf("    [%s] NOT_READY retries before the accepted submit: "
                    "%d (timing dependent; reported so a moving check count "
                    "can never be mistaken for a moving result)\n",
                    g_case, notReadyRetries);
        Check(notReadyCloses == 0,
              "no NOT_READY retry closed an fd the driver had taken",
              I64(notReadyCloses) + " library closes across " +
                  I64(notReadyRetries) + " NOT_READY retries");
        DrainCaptures();
        return;
    }
    std::printf("    [%s] NOT_READY retries: %d (loop exhausted)\n",
                g_case, notReadyRetries);
    Check(notReadyCloses == 0,
          "no NOT_READY retry closed an fd the driver had taken",
          I64(notReadyCloses) + " library closes across " +
              I64(notReadyRetries) + " NOT_READY retries");
    Check(false, "success path submitted", "never got past NOT_READY");
}

//=============================================================================
// Case 8 -- AN ARMED ACQUIRE FENCE, FRAME AFTER FRAME, against an unarmed
// control run through the identical code path.
//
// ASSERTED: every armed submit is accepted, closes the fd zero times, gets a
// release fence that signals, AND RETIRES like the control. That is the
// success-path contract repeated, so a fix that works once and leaks on the
// second frame is caught, and a fence that stalled assembly would be caught
// too.
//
// NO DrainPendingFrames() IN THIS FUNCTION, and that is a requirement of the
// measurement rather than a preference. The call is terminal for the
// completion surface: it reaches VkVideoEncoder::WaitForThreadsToComplete,
// which sets m_asyncAssemblyEnabled = false (VkVideoEncoder.cpp:4303) and
// joins the only threads that ever call PushCapturedBitstream; nothing turns
// it back on outside InitEncoder (:3054). A drain placed between the two
// loops therefore decides the outcome by POSITION: whichever loop runs after
// it can retire nothing, fence or no fence, and the fence stops being the
// variable under test.
//
// Swapping the loops with the drains left in moves every retirement to
// whichever loop ran first; removing the drains and keeping the order lets
// both loops retire.
// The variable is position relative to the first DrainPendingFrames(), not
// the fence. The drains are gone from this function accordingly, and the
// retirement claim is now asserted rather than excused.
//
// The defect itself is owned by the sibling test
// vk_video_encoder/test/encoder-ext-drain-assembly, which reproduces it with
// NO fence of any kind and is registered WILL_FAIL until it is fixed.
//=============================================================================
void CaseArmedFramesRepeatEdly()
{
    g_case = "ArmedFramesRepeated";
    const uint32_t kArmed = 8;

    // CONTROL FIRST: the identical frame with NO fence, so "did it retire?"
    // has a same-session baseline rather than a remembered one.
    const uint32_t capturedBeforeControl = g_captured;
    uint32_t controlSubmitted = 0;
    for (uint32_t f = 0; f < kArmed; f++) {
        int releaseFd = -1;
        VkVideoEncoderFrameFenceDescriptor fence;
        fence.acquireFenceFd  = -1;
        fence.pReleaseFenceFd = &releaseFd;
        VkVideoEncoderFrameSubmitInfo info = BaseInfo();
        info.pNext = &fence;
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
            break;
        }
        g_frameId++;
        controlSubmitted++;
        if (releaseFd >= 0) {
            ::close(releaseFd);
        }
        DrainCaptures();
    }
    // NO DrainPendingFrames() here. It is terminal for the completion surface
    // (see the note above), so calling it between the two loops would decide
    // this case's result before the fence had any say in it. Poll instead.
    for (int i = 0; i < 500; i++) {
        DrainCaptures();
        if ((g_captured - capturedBeforeControl) >= controlSubmitted) {
            break;
        }
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, nullptr);
    }
    const uint32_t controlRetired = g_captured - capturedBeforeControl;

    // NOW THE ARMED RUN.
    const uint32_t capturedBeforeArmed = g_captured;
    const int fdBefore = OpenFdCount();
    uint32_t armedSubmitted = 0;
    uint32_t armedFencesSignalled = 0;
    uint32_t armedLibraryCloses = 0;
    for (uint32_t f = 0; f < kArmed; f++) {
        const int fd = MintSyncFd();
        if (fd < 0) {
            Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
            break;
        }
        int releaseFd = -1;
        VkVideoEncoderFrameFenceDescriptor fence;
        fence.acquireFenceFd  = fd;
        fence.pReleaseFenceFd = &releaseFd;
        VkVideoEncoderFrameSubmitInfo info = BaseInfo();
        info.pNext = &fence;

        const CallResult r = RunWatched(g_encoder.get(), info, fd);
        armedLibraryCloses += (uint32_t)r.closes;
        if (r.status != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            Check(false, "armed submit accepted",
                  "frame " + I64(f) + " status " + I64((long long)r.status));
            break;
        }
        g_frameId++;
        armedSubmitted++;
        if (releaseFd >= 0) {
            if (PollSignalled(releaseFd, 10000) == 1) {
                armedFencesSignalled++;
            }
            ::close(releaseFd);
        }
        DrainCaptures();
    }
    // Same reason as the control loop: poll, never DrainPendingFrames().
    for (int i = 0; i < 500; i++) {
        DrainCaptures();
        if ((g_captured - capturedBeforeArmed) >= armedSubmitted) {
            break;
        }
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, nullptr);
    }
    const uint32_t armedRetired = g_captured - capturedBeforeArmed;
    const int fdAfter = OpenFdCount();

    std::printf("    [%s] control(no fence): submitted=%u retired=%u\n",
                g_case, controlSubmitted, controlRetired);
    std::printf("    [%s] armed:             submitted=%u retired=%u "
                "fencesSignalled=%u libraryCloses=%u  /proc/self/fd %d -> %d\n",
                g_case, armedSubmitted, armedRetired, armedFencesSignalled,
                armedLibraryCloses, fdBefore, fdAfter);
    Check(armedSubmitted == kArmed, "every armed frame was accepted",
          I64(armedSubmitted) + " of " + I64(kArmed));
    Check(armedLibraryCloses == 0,
          "the library never closed an fd the driver had taken",
          I64(armedLibraryCloses) + " closes across " + I64(armedSubmitted) +
              " armed frames");
    Check(armedFencesSignalled == armedSubmitted,
          "every armed frame's input-consuming submit completed",
          I64(armedFencesSignalled) + " of " + I64(armedSubmitted) +
              " release fences signalled");
    // THE CLAIM THIS CASE USED TO DUCK. An armed frame must come back through
    // AcquireNextEncodedFrame like any other. Measured 16 retired for 8 armed
    // submits on an A4000 (the extra 8 are the unarmed frames MintSyncFd
    // submits to source each sync_fd), against 8 for the 8-frame control.
    // The bar is therefore "at least as many as were submitted": a fence that
    // stalled assembly would show 0 here, which is what the old note wrongly
    // attributed to the fence when the cause was this function's own
    // DrainPendingFrames() call.
    Check(armedRetired >= armedSubmitted,
          "armed frames retire, exactly like the unarmed control",
          I64(armedRetired) + " retired for " + I64(armedSubmitted) +
              " armed submits; control retired " + I64(controlRetired) +
              " for " + I64(controlSubmitted));
    // ASSERTED, not merely printed. libraryCloses == 0 above is a
    // ONE-SIDED test: it catches a success path that closes too much and is
    // silent about one that closes too little. A future change that handed
    // the driver an fd AND kept a copy -- or that recorded an fd in the guard
    // and never consumed it on a path that then succeeded -- would leak eight
    // descriptors here and pass every other assertion in this file. The fd
    // table is the only instrument that can see that, so it has to be an
    // assertion rather than a number in a log line.
    //
    // Equality is the right bar, not a bound: this loop mints kArmed fds and
    // hands every one to the library, and closes every release fd it is given,
    // so a correct run returns the table to exactly where it started. It has
    // measured 88 -> 88 on both the fixed and the unfixed build.
    Check(fdAfter == fdBefore,
          "the success path left the fd table where it found it",
          "/proc/self/fd " + I64(fdBefore) + " -> " + I64(fdAfter) +
              ", delta " + I64(fdAfter - fdBefore) +
              " -- libraryCloses==0 catches over-closing only; this is the "
              "side that catches a leak");
}

//=============================================================================
// Case 9 -- THE LEAK, AT SCALE. One refusal is one fd; a session is millions.
//
// 120 refusals, each carrying a freshly minted real sync_fd, with the fd
// table read before and after. A single-call assertion cannot distinguish
// "closed" from "closed most of the time", and the fd table is the only
// instrument that sees an accumulating handle. The refusal chosen is
// RESOURCE_UNKNOWN because it is the one a producer actually hits in
// production -- an Unregister racing an in-flight submit, which is exactly
// when a caller is holding a fence fd.
//
// This is a REFUSAL loop rather than a success loop on purpose: an armed
// SUCCESS holds its fd inside a driver semaphore that lives until the frame
// retires, so a success loop's fd table would be measuring retirement timing
// rather than the leak. (An earlier version of this comment said armed frames
// "do not retire on this library". They do -- see the correction in case 8.
// The reason for preferring a refusal loop stands on its own.)
//=============================================================================
void CaseRefusalsAtScaleDoNotGrowTheFdTable()
{
    g_case = "RefusalsAtScale";
    DrainCaptures();
    const int fdBefore = OpenFdCount();

    uint32_t iterations   = 0;
    uint32_t leaked       = 0;
    uint32_t wrongCloses  = 0;
    for (uint32_t f = 0; f < kRefusalLoopIterations; f++) {
        const int fd = MintSyncFd();
        if (fd < 0) {
            Check(false, "mint a real sync_fd",
                  "MintSyncFd returned -1 at iteration " + I64(f));
            break;
        }
        int releaseFd = 0x5EED;
        VkVideoEncoderFrameFenceDescriptor fence;
        fence.acquireFenceFd  = fd;
        fence.pReleaseFenceFd = &releaseFd;

        VkVideoEncoderFrameSubmitInfo info = BaseInfo();
        info.pNext    = &fence;
        info.resource = (VkVideoEncoderResource)0xDEADBEEFull;

        const CallResult r = RunWatched(g_encoder.get(), info, fd);
        iterations++;
        if (r.status != VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN) {
            Check(false, "status", "iteration " + I64(f) + " status " +
                                       I64((long long)r.status));
            break;
        }
        if (r.stillOpen) {
            leaked++;
            ::close(fd);  // keep the loop from exhausting the table
        }
        if (r.closes != 1) {
            wrongCloses++;
        }
        DrainCaptures();
    }
    const int fdAfter = OpenFdCount();
    std::printf("    [%s] iterations=%u leaked=%u wrongCloseCount=%u  "
                "/proc/self/fd %d -> %d\n",
                g_case, iterations, leaked, wrongCloses, fdBefore, fdAfter);

    Check(iterations == kRefusalLoopIterations, "the whole loop ran",
          I64(iterations) + " of " + I64(kRefusalLoopIterations));
    Check(leaked == 0, "not one refusal leaked its fd",
          I64(leaked) + " of " + I64(iterations) + " left the fd open");
    Check(wrongCloses == 0, "every refusal closed exactly once",
          I64(wrongCloses) + " of " + I64(iterations) +
              " closed a number of times other than one");
    Check((fdBefore >= 0) && (fdAfter >= 0) && (fdAfter <= fdBefore),
          "no fd growth across the refusal loop",
          "before " + I64(fdBefore) + ", after " + I64(fdAfter));
}

}  // namespace

// Both of case 10 timeline semaphores, on every exit. Two handles and five
// exits is how one of them comes to be leaked on the path nobody reran.
void DestroyCase10Semaphores(VkSemaphore callerWait, VkSemaphore callerSignal)
{
    if (callerWait != VK_NULL_HANDLE) {
        g_fns.DestroySemaphore(g_device, callerWait, nullptr);
    }
    if (callerSignal != VK_NULL_HANDLE) {
        g_fns.DestroySemaphore(g_device, callerSignal, nullptr);
    }
}

//=============================================================================
// Case 10 -- AN ARMED ACQUIRE FD *ALONGSIDE* A CALLER WAIT ARRAY.
//
// WHY THIS CASE EXISTS, and why it is the only one added rather than one per
// -1 site. Every other acquireFenceFd in this tree is -1: six sites in
// encoder-ext-sync and one in encoder-ext-release-fence. For all seven, -1 is
// the RIGHT input and was left alone -- the sync suite runs on a null-backend
// session with no device, so it cannot mint a real sync_fd at all and what it
// pins is the chained-descriptor WALK, not the import; and the release-fence
// loop is measuring the export half, which an armed acquire fence would only
// add noise to (case 8 above already covers armed frames exporting release
// fences, 8 for 8).
//
// One genuine gap survived that audit. The library APPENDS the imported
// acquire semaphore to whatever wait array the frame ended up with
// (vulkan_video_encoder_ext.cpp:6265-6281), and the header promises
// "Supplying an acquireFenceFd and a pWaitSemaphores array together is legal
// and loses neither". The copy loop that carries the caller's existing waits
// across that append (:6269-6272) runs ONLY when a fence is armed, and every
// armed submit in this file passes BaseInfo(), whose waitSemaphoreCount is 0.
// So the loop body has never executed once, anywhere in this tree: the append
// has only ever been tested appending to nothing.
//
// HOW IT IS MADE OBSERVABLE. Two signalled waits prove nothing -- the frame
// completes whether or not the caller's wait was carried across. So the
// caller's wait is a TIMELINE semaphore held at 0 and required at 1, and the
// acquire fence is a real, already-signalled sync_fd. The release fence then
// tells us which of two worlds we are in:
//
//   not signalled while the timeline is at 0  =>  the caller's wait survived
//                                                 the append. ASSERTED.
//   signalled after vkSignalSemaphore(1)      =>  the acquire wait did not
//                                                 deadlock it.  ASSERTED.
//
// WHAT THIS MEASURES:
//
//   armed  (acquireFenceFd = a real sync_fd) : earlyPoll=0   correct
//   control(acquireFenceFd = -1, same frame,
//           same caller timeline at 0)       : earlyPoll=0   correct
//
// The claim HOLDS. This case is a gating assertion, not a known failure.
//
// A HAZARD FOR ANYONE RE-RUNNING THE MUTATION PROOF FOR THIS CASE. The proof
// forces the copy loop at vulkan_video_encoder_ext.cpp:6268 to zero
// iterations, which IS "the caller wait array is dropped when an acquire
// fence is armed", so the mutant runs red as it should.
//
// Reverting that mutant can restore a source file whose mtime is OLDER than
// the object already built from it. make then rebuilds nothing, and every run
// afterwards -- including the ones believed to be on clean source -- re-runs
// the mutant binary. Confirm the object is newer than the source before
// reading any result here as a property of the library, or a stale binary
// will be reported as one.
//
// Settled by measurement, not by argument:
//
//   * The wait array reaches vkQueueSubmit2 intact. Traced with a temporary
//     print at three points -- the ext layer immediately above
//     SubmitExternalFrameCommon, StampExternalFrameInfo, and the
//     VkSubmitInfo2 handed to MultiThreadedQueueSubmit in
//     SubmitStagedInputFrame -- an armed frame carrying one caller TIMELINE
//     wait submits waitSemaphoreInfoCount=2: {callerTimeline, value 1} and
//     {importedAcquire, value 0}, both at TRANSFER. Nothing is dropped,
//     truncated or overwritten anywhere at or below :6333.
//   * Clean source: 63 consecutive runs of this case, earlyPoll=0 every
//     time, 24 of them pinned to a single core to skew the timing.
//   * Rebuilding the mutant reproduces the reported failure exactly, and
//     reproduces a second fingerprint the reported failure also carried: a
//     red run emits NO "asyncAssemblyFence ... is not done after N mSec"
//     warning, because the frame is never parked; a green run emits exactly
//     two, at 100 ms and 200 ms, because it is. Every archived red log has
//     zero of them and every archived green log has two.
//
// So there is no read-before-write race here, and nothing in the library was
// changed to make this green. The case is KEPT, and promoted from WILL_FAIL
// to gating, because the mutation proof shows it bites: it is still the only
// test in this tree that executes that copy loop with anything to copy.
//
// THE SIGNAL DIRECTION IS COVERED HERE TOO, and for the same reason the wait
// direction was uncovered. The release-fence append on the signal side
// (:6316-6331) has the same shape as the acquire append on the wait side --
// an append plus a loop that carries the caller array across it -- and every
// armed submit in this tree passed signalSemaphoreCount == 0, so ITS
// carry-across loop had never executed either. The frame therefore also
// carries a caller TIMELINE SIGNAL, and this case asserts that it is still at
// 0 while the frame is parked and reaches its requested value once the frame
// goes through. A library that discarded the caller signal array when it
// appended the release fence would leave that timeline at 0 forever --
// whoever waits on it waits forever -- and would pass every other assertion
// in this file. Measured: it survives.
//
// LAST, on purpose: it parks a frame on an unsignalled wait for 200 ms, so it
// must not sit ahead of any loop that needs admission slots.
//=============================================================================
void CaseArmedFdAlongsideCallerWaitArray()
{
    g_case = "ArmedFdWithCallerWaitArray";

    VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue  = 0;
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semInfo.pNext = &typeInfo;

    VkSemaphore callerWait = VK_NULL_HANDLE;
    const VkResult semRes =
        g_fns.CreateSemaphore(g_device, &semInfo, nullptr, &callerWait);
    Check(semRes == VK_SUCCESS,
          "a host-signallable TIMELINE semaphore could be created",
          "vkCreateSemaphore returned " + I64((long long)semRes) +
              " -- without one this case cannot hold a caller wait open and "
              "the append below would be untestable");
    if ((semRes != VK_SUCCESS) || (callerWait == VK_NULL_HANDLE)) {
        return;
    }

    // The mirror of |callerWait| on the other side of the frame: a TIMELINE
    // the library must SIGNAL, held at 0 by construction, so that the
    // release-fence append has a caller signal array to append TO.
    VkSemaphore callerSignal = VK_NULL_HANDLE;
    const VkResult sigSemRes =
        g_fns.CreateSemaphore(g_device, &semInfo, nullptr, &callerSignal);
    Check(sigSemRes == VK_SUCCESS,
          "a readable TIMELINE semaphore could be created for the signal side",
          "vkCreateSemaphore returned " + I64((long long)sigSemRes) +
              " -- without one the SIGNAL half of this case cannot be observed");
    if ((sigSemRes != VK_SUCCESS) || (callerSignal == VK_NULL_HANDLE)) {
        DestroyCase10Semaphores(callerWait, callerSignal);
        return;
    }

    const uint64_t kCallerWaitValue = 1;
    const uint64_t kCallerSignalValue = 7;
    int  releaseFd  = 0x5EED;
    bool submitted  = false;
    int  libCloses  = -1;

    for (int attempt = 0; attempt < 64; attempt++) {
        const int fd = MintSyncFd();
        if (fd < 0) {
            Check(false, "mint a real sync_fd", "MintSyncFd returned -1");
            DestroyCase10Semaphores(callerWait, callerSignal);
            return;
        }
        releaseFd = 0x5EED;
        VkVideoEncoderFrameFenceDescriptor fence;
        fence.acquireFenceFd  = fd;
        fence.pReleaseFenceFd = &releaseFd;

        VkVideoEncoderFrameSubmitInfo info = BaseInfo();
        info.pNext                = &fence;
        // THE POINT OF THE CASE: a NON-EMPTY caller wait array, so the append
        // has something to append TO.
        info.waitSemaphoreCount   = 1;
        info.pWaitSemaphores      = &callerWait;
        info.pWaitSemaphoreValues = &kCallerWaitValue;
        // The other half of the point: a NON-EMPTY caller SIGNAL array, so
        // the release-fence append has something to append to as well.
        info.signalSemaphoreCount   = 1;
        info.pSignalSemaphores      = &callerSignal;
        info.pSignalSemaphoreValues = &kCallerSignalValue;

        const CallResult r = RunWatched(g_encoder.get(), info, fd);
        if (r.status == VK_VIDEO_ENCODER_STATUS_NOT_READY) {
            DrainCaptures();
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, nullptr);
            continue;
        }
        libCloses = r.closes;
        Check(r.status == VK_VIDEO_ENCODER_STATUS_SUCCESS,
              "an armed fd together with a caller wait array is accepted",
              "got " + I64((long long)r.status) + ", want SUCCESS");
        submitted = (r.status == VK_VIDEO_ENCODER_STATUS_SUCCESS);
        break;
    }
    if (!submitted) {
        Check(false, "the frame was submitted",
              "never got past NOT_READY, or the submit was refused");
        DestroyCase10Semaphores(callerWait, callerSignal);
        return;
    }

    Check(libCloses == 0,
          "the library did not close what it handed to the driver",
          I64(libCloses) + " close(2) calls on a descriptor "
          "vkImportSemaphoreFdKHR already owns");
    Check(releaseFd >= 0,
          "the input-consuming submit was issued",
          "release fd " + I64(releaseFd));
    if (releaseFd < 0) {
        DestroyCase10Semaphores(callerWait, callerSignal);
        return;
    }

    // (1) The caller's wait SURVIVED the append. The acquire fence is already
    // signalled, so if the caller's timeline had been dropped there would be
    // nothing left to hold this submit back and the release fence would be
    // signalled by now.
    const int earlyPoll = PollSignalled(releaseFd, 200);
    Check(earlyPoll == 0,
          "the caller wait array survived the acquire-fence append",
          "release fence poll returned " + I64(earlyPoll) +
              " while the caller timeline is still at 0 -- 1 means the "
              "input-consuming submit ran anyway, i.e. the caller wait was "
              "lost once an acquire fence was armed. The same frame with "
              "acquireFenceFd = -1 polls 0 here, so the wait itself works; "
              "arming the fence would be what lost it");

    // (1b) The SIGNAL direction, read at the same instant and for the mirror
    // reason. The frame is parked, so nothing in its batch has run and the
    // caller timeline must still be at 0. A non-zero here is the same event
    // the poll above names, seen through the other array.
    uint64_t earlySignalValue = ~(uint64_t)0;
    const VkResult earlyGet = g_fns.GetSemaphoreCounterValue(
        g_device, callerSignal, &earlySignalValue);
    Check((earlyGet == VK_SUCCESS) && (earlySignalValue == 0),
          "the caller signal timeline has not moved while the frame is parked",
          "vkGetSemaphoreCounterValue returned " + I64((long long)earlyGet) +
              " value " + I64((long long)earlySignalValue) + ", want 0");

    // (2) And nothing deadlocked: once the caller's wait is satisfied the
    // frame goes through, which it could not do if the appended acquire
    // semaphore were unsignalled or waited on twice.
    VkSemaphoreSignalInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    signalInfo.semaphore = callerWait;
    signalInfo.value     = kCallerWaitValue;
    const VkResult sigRes = g_fns.SignalSemaphore(g_device, &signalInfo);
    Check(sigRes == VK_SUCCESS, "the caller's timeline could be signalled",
          "vkSignalSemaphore returned " + I64((long long)sigRes));

    const int latePoll = PollSignalled(releaseFd, 10000);
    Check(latePoll == 1,
          "with both waits satisfied the input-consuming submit completes",
          "release fence poll returned " + I64(latePoll) +
              " after the caller's timeline reached " +
              I64((long long)kCallerWaitValue));

    // (3) The caller SIGNAL array survived the release-fence append. That
    // fence and this timeline ride the SAME VkSubmitInfo2, so once the fd is
    // signalled the batch has retired and every signal in it has happened;
    // the bounded retry below is for counter visibility only, never for
    // ordering, and it cannot turn a dropped array into a pass because a
    // dropped array leaves this at 0 for the whole second.
    uint64_t lateSignalValue = 0;
    for (int i = 0; i < 200; i++) {
        if (g_fns.GetSemaphoreCounterValue(g_device, callerSignal,
                                           &lateSignalValue) != VK_SUCCESS) {
            break;
        }
        if (lateSignalValue >= kCallerSignalValue) {
            break;
        }
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, nullptr);
    }
    Check(lateSignalValue == kCallerSignalValue,
          "the caller signal array survived the release-fence append",
          "caller signal timeline reached " + I64((long long)lateSignalValue) +
              ", want " + I64((long long)kCallerSignalValue) +
              " -- a library that discarded the caller signal array when it "
              "appended the release fence leaves this at 0 forever, and "
              "whoever waits on it waits forever");

    std::printf("    [%s] earlyPoll=%d (want 0) latePoll=%d (want 1) "
                "callerSignal early=%llu late=%llu (want 0 then %llu) "
                "libraryCloses=%d\n",
                g_case, earlyPoll, latePoll,
                (unsigned long long)earlySignalValue,
                (unsigned long long)lateSignalValue,
                (unsigned long long)kCallerSignalValue, libCloses);

    ::close(releaseFd);
    DrainCaptures();
    // The frame must be released before the semaphore it waited on is
    // destroyed; the drain above does that via ReleaseEncodedFrame.
    g_encoder->DrainPendingFrames();
    DrainCaptures();
    DestroyCase10Semaphores(callerWait, callerSignal);
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--caller-wait-order") == 0) {
            g_assertCallerWaitOrder = true;
        }
    }
    std::printf("Encoder-ext per-frame ACQUIRE fence fd ownership "
                "(real device)%s\n",
                g_assertCallerWaitOrder
                    ? " -- CALLER-WAIT-ORDER MODE (ordering gate only)"
                    : "");
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
    // No reordering: the input-consuming submit is then issued inline with
    // the frame that produced it, which is what makes the release fd this
    // test mints from available on the same call.
    config.consecutiveBFrames = 0;
    config.idrPeriod          = 30;
    config.frameRateNum       = 30;
    config.frameRateDen       = 1;
    config.deviceId           = -1;  // -1 is auto-select; 0 names index 0
    config.disableFileOutput  = VK_TRUE;

    if (g_encoder->InitializeExt(config) != VK_SUCCESS) {
        std::printf("SKIP: InitializeExt failed -- no encode-capable Vulkan "
                    "device on this host\n");
        return 77;
    }

    VkInstance       instance = g_encoder->GetVkInstance();
    VkDevice         device   = g_encoder->GetVkDevice();
    VkPhysicalDevice phys     = g_encoder->GetVkPhysicalDevice();
    DeviceFns& fns = g_fns;
    g_device = device;
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

    // The counter has to be live before the first assertion depends on it.
    //
    // WHAT THIS PROVES, EXACTLY, AND NO MORE: that the counting wrapper
    // increments and that its RTLD_NEXT forward resolved. The ::close(probe)
    // below is compiled in the SAME translation unit as the definition of
    // close() above it, so the compiler binds it straight to that definition.
    // It therefore CANNOT fail, and it says nothing whatever about whether
    // the encoder archive's call sites bind here. It is a smoke test against
    // a broken dlsym or a miscompiled counter -- which would report 0 closes
    // for everything and read exactly like a total leak.
    //
    // What establishes that the ARCHIVE binds to this definition is the
    // RED/GREEN differential of the suite as a whole: the same source, linked
    // against the unfixed library, reports closes=0 on all six refusals;
    // against the fixed one, closes=1, corroborated by fcntl -> EBADF and a
    // /proc/self/fd delta of -1. A counter the library never reached could
    // not move between those two builds.
    {
        const int probe = ::dup(2);
        if (probe < 0) {
            std::printf("SKIP: dup(2) failed\n");
            return 77;
        }
        g_closeCount.store(0, std::memory_order_relaxed);
        g_watchedFd.store(probe, std::memory_order_relaxed);
        ::close(probe);
        g_watchedFd.store(-1, std::memory_order_relaxed);
        const int seen = g_closeCount.load(std::memory_order_relaxed);
        g_case = "InterposerSelfTest";
        Check(seen == 1, "the close(2) interposer is wired",
              I64(seen) + " closes counted for one close -- every "
              "closed-exactly-once assertion below depends on this");
        if (seen != 1) {
            std::printf("RESULT: FAIL (interposer not wired; the rest would "
                        "be meaningless)\n");
            return 1;
        }
    }

    const int fdAtStart = OpenFdCount();
    std::printf("  /proc/self/fd at start: %d\n", fdAtStart);

    // WHY THE CASES RUN FROM A TABLE NOW: the per-case check LEDGER.
    //
    // "checks: N" was the only summary this file offered, and it is a weak
    // signal in two directions. It moved run to run (fixed above), and --
    // worse -- a case that returned early on a setup failure, or that got
    // dropped from this list, subtracted from it silently while the suite
    // still reported PASS, because returning early fires no Check(). The
    // floor below is per case, so "this case stopped exercising anything"
    // now fails loudly and NAMES the case instead of showing up as a smaller
    // number nobody was tracking.
    //
    // The floors are the assertion counts each case reaches today. They are
    // FLOORS, not equalities: a case may add assertions without anybody
    // having to update a magic number, and only a case that stops asserting
    // what it already asserted trips the audit.
    //
    // ORDER MATTERS, and not for style. A frame with an armed acquire fence
    // DOES retire on this library, which case 8 pins, so the ordering rests on
    // admission slots and not on retirement. What has to hold is:
    //   - the 120-mint bulk loop needs a working submit path for every
    //     iteration, so it runs before anything that could tie up admission
    //     slots;
    //   - the two chain refusals refuse before the frame is admitted, so
    //     neither holds a slot and both belong up with the other refusals;
    //   - nothing in this file may call DrainPendingFrames() before teardown.
    //     That call is terminal for the completion surface, so a drain
    //     anywhere above would starve every later loop on NOT_READY and get
    //     reported as a leak.
    struct CaseEntry {
        const char* name;
        void      (*fn)();
        int         minChecks;
    };
    static const CaseEntry kCaseLedger[] = {
        { "WrongTopLevelSType",     CaseWrongTopLevelSType,                 6 },
        { "NotInitialized",         CaseNotInitialized,                     6 },
        { "ResourceUnknown",        CaseResourceUnknown,                    6 },
        { "UnknownSTypeAheadOfFence",
                                    CaseUnknownSTypeAheadOfFence,           6 },
        { "UnresolvableWaitIdAheadOfFence",
                                    CaseUnresolvableWaitIdAheadOfFence,     6 },
        { "UnresolvableSignalIdAheadOfFence",
                                    CaseUnresolvableSignalIdAheadOfFence,   6 },
        { "CyclicChainRefused",     CaseCyclicChainIsRefused,               6 },
        { "DuplicateAcquireFd",     CaseDuplicateAcquireFdIsRefused,        4 },
        { "RefusalsAtScale",        CaseRefusalsAtScaleDoNotGrowTheFdTable, 4 },
        { "SuccessPathConsumesOnce", CaseSuccessPathConsumesOnce,           5 },
        { "ArmedFramesRepeated",    CaseArmedFramesRepeatEdly,              5 },
        // LAST. It parks a frame on an unsignalled caller wait for 200 ms and
        // ends with a DrainPendingFrames() of its own, so nothing that needs
        // admission slots or the completion surface may follow it.
        { "ArmedFdWithCallerWaitArray",
                                    CaseArmedFdAlongsideCallerWaitArray,
                                    kCallerWaitOrderFloor },
    };

    // --caller-wait-order runs ONLY case 10, so that the ordering claim has
    // a CTest entry that names it. The ledger table above is not walked in
    // this mode, so its floor for that case is asserted directly here: a mode
    // whose one case returned early would otherwise fire no Check() at all
    // and exit 0, which is a gate that cannot fail.
    if (g_assertCallerWaitOrder) {
        const int beforeOrdering = g_checks;
        CaseArmedFdAlongsideCallerWaitArray();
        const int orderingChecks = g_checks - beforeOrdering;
        g_case = "CaseLedger";
        std::printf("  [ledger] %-34s %3d checks (floor %d)\n",
                    "ArmedFdWithCallerWaitArray", orderingChecks,
                    kCallerWaitOrderFloor);
        Check(orderingChecks >= kCallerWaitOrderFloor,
              "the ordering case contributed at least its floor of checks",
              "contributed " + I64(orderingChecks) + ", floor " +
                  I64(kCallerWaitOrderFloor) +
                  " -- a case that returns early fires no Check() and would "
                  "otherwise leave this mode green");
        g_case = "teardown";
        g_encoder->UnregisterImageResource(g_resource);
        fns.DestroyImage(device, input.image, nullptr);
        fns.FreeMemory(device, input.memory, nullptr);
        g_encoder = nullptr;
        std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
        std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
        return (g_failures == 0) ? 0 : 1;
    }

    int subjectChecksBeforeLedger = 0;
    for (const CaseEntry& entry : kCaseLedger) {
        const int before = g_checks;
        entry.fn();
        const int delta = g_checks - before;
        subjectChecksBeforeLedger += delta;
        std::printf("  [ledger] %-34s %3d checks (floor %d)\n",
                    entry.name, delta, entry.minChecks);
        g_case = "CaseLedger";
        Check(delta >= entry.minChecks,
              "the case contributed at least its floor of checks",
              std::string(entry.name) + " contributed " + I64(delta) +
                  ", floor " + I64(entry.minChecks) +
                  " -- a case that returns early fires no Check() and would "
                  "otherwise leave the suite green");
    }
    std::printf("  [ledger] %d subject checks across %d cases, "
                "plus one ledger check per case\n",
                subjectChecksBeforeLedger,
                (int)(sizeof(kCaseLedger) / sizeof(kCaseLedger[0])));

    g_case = "teardown";
    g_encoder->DrainPendingFrames();
    DrainCaptures();
    const int fdAtEnd = OpenFdCount();
    std::printf("  /proc/self/fd at end:   %d\n", fdAtEnd);
    std::printf("  captured frames=%u  bitstream bytes=%llu\n",
                g_captured, (unsigned long long)g_capturedBytes);
    Check(g_captured > 0, "the encode produced frames",
          I64(g_captured) + " captured");
    Check(g_capturedBytes > 0, "the encode produced a bitstream",
          I64((long long)g_capturedBytes) + " bytes");

    // Order matters and the header states it: unregister BEFORE destroying
    // the image, and destroy the image BEFORE releasing the encoder.
    g_encoder->UnregisterImageResource(g_resource);
    fns.DestroyImage(device, input.image, nullptr);
    fns.FreeMemory(device, input.memory, nullptr);
    g_encoder = nullptr;

    std::printf("------------------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
