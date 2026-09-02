/*
 * Copyright 2024-2025 NVIDIA Corporation.
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

// THE COUPLING THIS FILE DEPENDS ON, MADE LOUD. Every Win32 arm below is
// spelled `defined(_WIN32)` while the symbols inside them -- HANDLE,
// VkImportSemaphoreWin32HandleInfoKHR, VK_KHR_EXTERNAL_*_WIN32_EXTENSION_NAME
// -- come from vulkan_win32.h, which is gated on VK_USE_PLATFORM_WIN32_KHR.
// Both builds that compile this file define it whenever the target is Windows
// (the CMake build sets it directly; the GN build gets it from
// third_party/vulkan-headers). That is a real guarantee, not an accident, but
// nothing enforced it -- so a third build could have flipped every Win32 arm
// to a wall of undeclared identifiers. It cannot now.
#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#error "This file spells its Win32 arms `defined(_WIN32)` but uses \
vulkan_win32.h symbols inside them. Define VK_USE_PLATFORM_WIN32_KHR when \
building for Windows, or convert every _WIN32 arm in this file to \
VK_USE_PLATFORM_WIN32_KHR."
#endif

#include <atomic>
#include <iostream>
#include "vulkan_video_encoder_os_event_linux.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <unordered_set>
#include <vector>

#include "vulkan_video_encoder_ext_internal.h"
#include <thread>
#include <mutex>
#include <sstream>

// POSIX, not Linux. <unistd.h> is where close(2) is declared on every POSIX
// target, and the acquire fence this file consumes is a POSIX fd on all of
// them -- gfx::GpuFenceHandle::ScopedPlatformFence is base::ScopedFD under
// BUILDFLAG(IS_POSIX), not under IS_LINUX. MSVC ships no <unistd.h>, which is
// why _WIN32 gets its own arm below rather than an unguarded include.
#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>  // F_DUPFD_CLOEXEC: the BORROW mode's private duplicate
#include <unistd.h>  // close(2): the fd-consumption rule (VkEncConsumeOsHandle)
#endif

#include "vulkan_video_encoder_ext.h"
#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkVideoEncoder/VkEncoderConfigH264.h"
#include "VkVideoEncoder/VkEncoderConfigH265.h"
#include "VkVideoEncoder/VkEncoderConfigAV1.h"
#include "VkVideoEncoder/VkVideoEncoder.h"
// The device-free capture backend below stands in for a real session
// by BEING one of the codec encoders rather than imitating it, so the
// H.264 arm of the mid-stream rate-control refresh is the code a test
// drives, not a second copy of it.
#include "VkVideoEncoder/VkVideoEncoderH264.h"
// YcbcrVkFormatInfo() / GetBitsPerChannel() -- used to derive the input bit depth,
// chroma subsampling and plane count from VkVideoEncoderConfig::inputFormat.
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

//=============================================================================
// VulkanVideoEncoderExtImpl - Concrete implementation of VulkanVideoEncoderExt
//
// Wraps the internal VkVideoEncoder with the public service-oriented API.
// Uses VulkanDeviceContext for Vulkan init and VkVideoEncoder for encoding.
//=============================================================================

class VulkanVideoEncoderExtImpl : public VulkanVideoEncoderExt {
public:
    VulkanVideoEncoderExtImpl()
        : m_vkDevCtx()
        , m_encoderConfig()
        , m_encoder()
        , m_initialized(false)
        , m_framesSubmitted(0)
        , m_rateControlMode(VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR)
    { }

    virtual ~VulkanVideoEncoderExtImpl() {
        // ENFORCED, like the session-serial methods -- but they can
        // return VK_ERROR_NOT_PERMITTED_KHR from inside the completion
        // callback and a destructor cannot. Dropping the last encoder
        // reference inside pfnFrameReady tears this state down under
        // the invoking edge's feet (it still writes its thread-id
        // bookkeeping after the callback returns) and, on a real
        // session, Deinitialize() would join the very delivery thread
        // running the callback. Both failure modes are silent -- a
        // use-after-free with no report, or a self-join deadlock whose
        // stack names nobody -- so the enforcement is the diagnosed
        // abort, the same shape as the noexcept-escape abort on the
        // invocation path. The message goes to stderr; under
        // silenceStdio the abort itself is still the diagnosis.
        if (IsInCompletionCallback()) {
            VkEncErr() << "[EncoderExt] encoder destroyed from inside "
                          "the completion callback: the last reference "
                          "must not be dropped from pfnFrameReady -- "
                          "aborting" << std::endl;
            std::abort();
        }
        // Workers first: Deinitialize() IS the join, and until it returns
        // an assembly worker can still be inside OnBitstreamCaptured --
        // invoking the callback with m_completionUserData and writing to
        // the completion event. The cookie is released AFTER the workers are
        // joined, not before: releasing first inverts the header's promise (the
        // release thunk fires
        // "only after any in-flight completion invocation has returned")
        // for exactly the consumer the release exists for: one that
        // destroys without detaching. Both shipping consumers happen to
        // detach or quiesce before destruction today; the order is fixed
        // so the contract stops depending on that.
        Deinitialize();
        // The cookie goes back after the join: a caller that transferred
        // ownership must get it back even on the path where nothing ever
        // detached, and only once nothing can still be invoking it.
        if (m_completionUserDataRelease != nullptr) {
            PFN_vkVideoEncoderUserDataRelease release =
                m_completionUserDataRelease;
            void* userData = m_completionUserData;
            m_completionUserDataRelease = nullptr;
            m_completionUserData        = nullptr;
            m_completionCallback        = nullptr;
            release(userData);
        }
        // Destroyed last, and here rather than in Deinitialize: the
        // handle's lifetime is the encoder object, not the session. A
        // consumer that obtained it and then saw a Deinitialize must not
        // have it closed underneath it -- the number gets recycled, and
        // the next thing to open a file inherits a waiter. After the join
        // above, nothing can be writing to it either, which the previous
        // order (close before join) did not guarantee.
        // Relaxed is sufficient here and only here: Deinitialize() above is
        // the worker join, so no other thread can be loading this member.
        vkenc::OsCompletionEventDestroy(
            m_completionEventHandle.load(std::memory_order_relaxed));
        m_completionEventHandle.store(vkenc::kOsCompletionEventNone,
                                      std::memory_order_relaxed);
    }

    

    //=========================================================================
    // VulkanVideoEncoder (base interface - file-based, backward compatible)
    //=========================================================================
    VkResult Initialize(VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
                        int argc, const char** argv) override;

    int64_t GetNumberOfFrames() override {
        return m_encoderConfig ? m_encoderConfig->numFrames : 0;
    }

    VkResult EncodeNextFrame(int64_t& frameNumEncoded) override;
    VkResult GetBitstream() override { return VK_SUCCESS; }

    //=========================================================================
    // VulkanVideoEncoderExt (extended interface - external frame input)
    //=========================================================================
    VkResult InitializeExt(const VkVideoEncoderConfig& config) override;
    // Public legacy entry point: a thin wrapper over
    // SubmitExternalFrameCommon (the one submit implementation both public
    // entry points share) with no prepared node, so the per-frame wrap
    // happens inside the encoder exactly as before registration existed.
    VkResult SubmitExternalFrame(
        const VkVideoEncodeInputFrame& frame,
        VkSemaphore* pStagingCompleteSemaphore = nullptr) override;
    VkResult SetCompletionCallback(
        PFN_vkVideoEncoderCompletionCallback callback, void* pUserData,
        PFN_vkVideoEncoderUserDataRelease releaseUserData) override;
    uint64_t GetCompletionCounter() override;
    VkResult GetCompletionInfo(VkVideoEncoderCompletionInfo* pInfo) override;
    VkVideoEncoderStatusCode GetCompletionEventHandle(uint64_t* outHandle) override;
    VkSemaphore GetCompletionSemaphore() const override;
    VkVideoEncoderStatusCode ExportCompletionSemaphoreHandle(
        VkVideoEncoderExternalHandleType handleType,
        uint64_t* outHandle) override;
    VkResult SetFrameDeadline(uint32_t deadlineMs) override;
    VkResult CancelFrame(uint64_t frameId) override;
    VkResult CancelAllPendingFrames(uint32_t* pCancelledCount) override;
    VkResult AbandonAllFrames(uint32_t* pAbandonedCount) override;
    VkResult AcquireNextEncodedFrame(VkVideoEncodeResult& result) override;
    VkResult AcquireEncodedFrame(uint64_t frameId,
                                 VkVideoEncodeResult& result) override;
    VkVideoEncoderFrameState GetFrameStatus(uint64_t frameId) override;
    VkResult GetEncodedFrame(VkVideoEncodeResult& result) override;
    void     ReleaseEncodedFrame(uint64_t frameId) override;
    VkResult Flush() override;
    VkResult DrainPendingFrames() override;
    VkResult Reconfigure(const VkVideoEncoderConfig& config) override;
    VkBool32 SupportsFormat(VkFormat inputFormat) const override;
    // The same question asked of a DECLARED colour model rather than of the
    // session's. A descriptor states the model its samples carry, and the
    // packed 4:4:4 layouts make that statement load-bearing: they ride RGBA
    // format enumerants, so the format alone cannot say which of the two a
    // surface is. VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT declares nothing
    // and falls back to the session's model, which is what the one-argument
    // form above passes.
    VkBool32 SupportsFormat(VkFormat inputFormat,
                            VkVideoEncoderColorModel declaredColorModel) const;
    // Whether THIS session can perform the compute-tier conversion: the
    // filter compiled in and the caller having asked for it. The rung-2
    // half of the adaptation ladder's "query the device first" rule.
    bool ComputeFilterActive() const;
    // Stricter: does this session's filter take THIS DECLARED PAIR --
    // format and colour model -- as its input?
    bool ComputeFilterTakesFormat(VkFormat inputFormat,
                                  VkVideoEncoderColorModel colorModel) const;
    // The colour model to read |inputFormat| under, from this session's point
    // of view: the caller's declaration for the session's OWN input format,
    // and the format's own answer for anything else. A session declares one
    // input; a question about some other format is not covered by that
    // declaration and must not silently borrow it.
    VkVideoEncoderColorModel SessionColorModel(VkFormat inputFormat) const;
    // Whether the DEVICE could STORAGE-READ an image with this descriptor's
    // format and tiling -- the fact the registration gate consults for the
    // single-plane arm of the preprocess filter.
    bool DeviceCanStorageRead(
        const VkVideoEncoderExternalImageDescriptor& desc) const;
    uint32_t GetMaxWidth() const override;
    uint32_t GetMaxHeight() const override;
    VkResult GetRuntimeInfo(VkVideoEncoderRuntimeInfo* outInfo) const override;

    // One predicate, shared by RegisterImageResource and QueryImageSupport.
    // Pure with respect to the handle: it never consumes an fd, so the
    // caller owns that decision and the query can run without one.
    VkVideoEncoderStatusCode ValidateImageDescriptor(
        const VkVideoEncoderExternalImageDescriptor& descriptor) const;
    VkVideoEncoderStatusCode QueryImageSupport(
        const VkVideoEncoderExternalImageDescriptor& descriptor,
        VkVideoEncoderImageSupport* outSupport) override;
    VkVideoEncoderStatusCode RegisterImageResource(
        const VkVideoEncoderExternalImageDescriptor& descriptor,
        uint64_t osHandle,
        VkVideoEncoderResource* outResource,
        VkVideoEncoderStatus* pStatus) override;
    VkVideoEncoderStatusCode UnregisterImageResource(
        VkVideoEncoderResource resource) override;
    VkVideoEncoderStatusCode RegisterSemaphore(
        const VkVideoEncoderSemaphoreDescriptor& descriptor,
        uint64_t osHandle,
        VkVideoEncoderResource* outResource,
        VkVideoEncoderStatus* pStatus) override;
    VkVideoEncoderStatusCode UnregisterSemaphore(
        VkVideoEncoderResource resource) override;
    // VK_NULL_HANDLE for a stale, untagged, or never-registered id.
    VkSemaphore ResolveSemaphore(VkVideoEncoderResource resource);
    VkVideoEncoderStatusCode SubmitRegisteredFrame(
        const VkVideoEncoderFrameSubmitInfo& info,
        VkSemaphore* pStagingCompleteSemaphore) override;
    // Drop a frame's reference on its registration, retiring the
    // registration if an Unregister was deferred waiting for it.
    void ReleaseResourceReference(VkVideoEncoderResource resource);

    VkDevice GetVkDevice() const override { return m_vkDevCtx; }
    VkPhysicalDevice GetVkPhysicalDevice() const override { return m_vkDevCtx.getPhysicalDevice(); }
    VkInstance GetVkInstance() const override { return m_vkDevCtx.getInstance(); }
    PFN_vkGetInstanceProcAddr GetVkGetInstanceProcAddr() const override {
        // VkInterfaceFunctions::GetInstanceProcAddr, dlsym'd from the loader
        // handle this context keeps mapped. Null until InitVulkanDevice runs.
        return m_vkDevCtx.GetInstanceProcAddr;
    }

    // Fault-injection seam (vulkan_video_encoder_ext_internal.h): exactly
    // these functions may reach the impl's private state, and only for
    // objects minted by CreateVulkanVideoEncoderExt.
    friend VkResult VkEncInjectImportContentMeasurement(
        VulkanVideoEncoderExt*, VkVideoEncoderResource, uint32_t, uint32_t,
        uint32_t);
    friend VkResult VkEncInstallNullBackend(VulkanVideoEncoderExt*,
                                            const VkEncNullBackendState*);
    friend VkVideoEncoderStatusCode VkEncProbeResource(VulkanVideoEncoderExt*,
                                                       VkVideoEncoderResource,
                                                       VkEncResourceProbe*);
    friend VkBool32 VkEncSessionInitialized(VulkanVideoEncoderExt*);
    friend void VkEncFireCompletionEdge(VulkanVideoEncoderExt*, uint64_t);
    friend VkResult VkEncApplyAndGetSessionConstQp(VulkanVideoEncoderExt*,
                                                   int32_t*, int32_t*,
                                                   int32_t*);
    friend VkResult VkEncApplyAndGetRateControl(
        VulkanVideoEncoderExt*, VkEncRateControlObservation*);
    friend VkResult VkEncGetRecordedConfig(VulkanVideoEncoderExt*,
                                           VkVideoEncoderConfig*);
    friend VkResult VkEncSeedRecordedConfig(VulkanVideoEncoderExt*,
                                            const VkVideoEncoderConfig*);
    friend VkResult VkEncSetDeviceQpWindow(VulkanVideoEncoderExt*,
                                           int32_t, int32_t);
    friend VkResult VkEncPushCapture(VulkanVideoEncoderExt*, uint64_t,
                                     VkResult);
    friend VkResult VkEncInstallTestSemaphore(VulkanVideoEncoderExt*,
                                              VkSemaphore,
                                              VkVideoEncoderResource*);
    friend VkVideoEncoderStatusCode VkEncUninstallTestSemaphore(
        VulkanVideoEncoderExt*, VkVideoEncoderResource);
    friend VkVideoEncoderStatusCode VkEncProbeLastSubmitSync(
        VulkanVideoEncoderExt*, VkEncSubmitSyncProbe*);

    // Bind this session to the context it was created on. Called ONLY by
    // CreateVulkanVideoEncoderExtOnContext, before the session is visible to
    // anyone else, so there is nothing to synchronise: these are written once
    // and read on the init path.
    //
    // The instance and physical device are RESOLVED BY THE CALLER and cached
    // here as plain values rather than re-read from the context during init.
    // Two reasons, and the first is a hard one: VulkanVideoEncoderContext is
    // an incomplete type at InitVulkanDevice's definition in this translation
    // unit, so the accessors cannot be called there at all. The second is that
    // caching is sound -- a context is immutable after construction, which the
    // header states as a contract and which is what lets unrelated sequences
    // share one context with no locking.
    void SetContext(const VkSharedBaseObj<VulkanVideoEncoderContext>& context,
                    VkInstance                                        instance,
                    VkPhysicalDevice                                  physicalDevice)
    {
        m_context           = context;
        m_contextInstance   = instance;
        m_contextPhysDevice = physicalDevice;
    }

private:
    void Deinitialize();

    // Build EncoderConfig from the structured VkVideoEncoderConfig
    VkResult BuildEncoderConfig(const VkVideoEncoderConfig& extConfig,
                                VkVideoCodecOperationFlagBitsKHR codecOp,
                                VkSharedBaseObj<EncoderConfig>& outConfig);

    // Initialize VulkanDeviceContext with encode queue support
    VkResult InitVulkanDevice(VkVideoCodecOperationFlagBitsKHR codecOp,
                              const VkVideoEncoderConfig& config);

    // Non-null only for a session built by CreateVulkanVideoEncoderExtOnContext.
    //
    // WHAT THIS REFERENCE DOES, AND WHAT IT CANNOT DO. It keeps the CONTEXT
    // OBJECT -- and with it the capability snapshot the caller selected from --
    // alive for as long as the session. It does NOT keep the borrowed
    // VkInstance alive, and no reference here could: in ADOPT the instance
    // belongs to the embedder and the context destroys nothing on release
    // (header context rule 3), and in OWN the library's floor registry already
    // holds the context above zero for the process lifetime (rule 2). An
    // embedder that destroys its VkInstance under a live session has a
    // use-after-free either way; that hazard is the embedder's to avoid, and it
    // is identical on the config path. An earlier version of this comment
    // claimed the reference closed it. It does not.
    //
    // Declared BEFORE |m_vkDevCtx| so it is destroyed AFTER it. Given the
    // above, that ordering is not load-bearing today -- it is kept because it
    // is the order that stays correct if a session ever holds something the
    // context genuinely owns.
    VkSharedBaseObj<VulkanVideoEncoderContext> m_context;
    // The context's instance and the chosen physical device, resolved at
    // creation. See SetContext for why they are cached rather than re-read.
    VkInstance                                 m_contextInstance   = VK_NULL_HANDLE;
    VkPhysicalDevice                           m_contextPhysDevice = VK_NULL_HANDLE;

    VulkanDeviceContext              m_vkDevCtx;
    VkSharedBaseObj<EncoderConfig>   m_encoderConfig;
    VkSharedBaseObj<VkVideoEncoder>  m_encoder;
    // Written at init/teardown, read by the lock-free class-(c) queries.
    std::atomic<bool>                m_initialized;
    // Test seam (VkEncInstallNullBackend, internal header): null in every
    // production session. When set, the session reports initialized with no
    // VkVideoEncoder behind it (until VkEncPushCapture installs its
    // device-free capture source), VK_IMAGE registration skips the
    // device-touching view build, and SubmitExternalFrameCommon terminates
    // at this backend instead of m_encoder.
    // Per-frame acquire fences imported as binary semaphores (design 3.5).
    // The IMPORT is temporary -- SYNC_FD is copy-transference, so the payload
    // dies with the first wait -- but the VkSemaphore OBJECT is the library's
    // and has vkDestroySemaphore's ordinary precondition, so it retires with
    // the frame that waited on it exactly like the release fence below. It
    // destroyed here rather than parked in a session-lifetime vector: with no
    // destroy call anywhere, one driver semaphore leaks per fenced frame for
    // the life of the session.
    VkSemaphore ImportAcquireFenceLocked(int fd);

    // === Per-frame RELEASE fences (design 3.5, the export half) ============
    //
    // The other direction of the same entry point: a library-owned BINARY
    // semaphore appended to the frame's signal list, so the submission that
    // consumes the input image signals it, then exported as a SYNC_FD for
    // the caller to feed into its own end-of-read-access obligation.
    //
    // Binary and per-frame, NOT the registered timeline: registration is
    // timeline-only by design and a binary semaphore cannot be registered
    // once and named repeatedly. These two paths stay separate.
    //
    // Two graveyards, because "may I destroy this VkSemaphore" is exactly
    // "has every batch referring to it completed", and only one of the
    // retirement paths can prove that. |m_releaseFenceRetired| holds
    // semaphores whose frame retired WITH a real capture -- the capture is
    // published only after the encode command buffer's fence wait, and the
    // encode submit waits on the staging submit, so both submissions that
    // could signal a release fence have completed. They are destroyed on the
    // next submit, off the lock. |m_unprovenSemaphores| holds the rest
    // (abandoned, timed out, failed, delivered with a non-SUCCESS status, or
    // still in flight at Deinitialize) plus every imported ACQUIRE semaphore
    // that took one of those same routes. Destroyed behind a device wait-idle
    // -- at teardown, and in-session once the vector crosses
    // kUnprovenSemaphoreBound, because the routes that feed it are all
    // repeatable in-session operations (a deadline drop, a CancelFrame, an
    // AbandonAllFrames, a failed capture) and a graveyard only teardown
    // empties is not a graveyard, it is a leak with a comment.
    std::vector<VkSemaphore>         m_releaseFenceRetired;
    std::vector<VkSemaphore>         m_unprovenSemaphores;
    // Above this many entries the unproven graveyard is swept on the submit
    // path behind a DeviceWaitIdle. Well above any steady-state depth: in a
    // healthy session every acquire and release fence retires with its frame
    // and this vector stays empty, so the sweep is a backstop for the
    // abnormal routes and never a per-frame cost.
    static const size_t              kUnprovenSemaphoreBound = 64;
    // Exportability is a physical-device property: asked once, never
    // assumed -- creating a semaphore with an unsupported export handle
    // type is itself invalid usage, so the query has to precede the create.
    bool                             m_releaseFenceExportProbed = false;
    bool                             m_releaseFenceExportable = false;
    // Create a SYNC_FD-exportable binary semaphore, or VK_NULL_HANDLE when
    // this device/platform cannot provide one. Never touches a lock.
    VkSemaphore CreateReleaseFenceSemaphore();
    // Export |semaphore|'s pending signal as a SYNC_FD. -1 is a legal answer
    // (already signalled, or the driver declined); it is never an error the
    // caller has to handle as one.
    int         ExportReleaseFenceFd(VkSemaphore semaphore);
    // Destroy everything in m_releaseFenceRetired. Takes m_pendingMutex to
    // swap the vector out, then destroys OUTSIDE it -- the same one-sided
    // lock discipline Flush and Deinitialize use for driver calls.
    void        DrainRetiredReleaseFences();
    // Destroy everything in m_unprovenSemaphores when it has crossed
    // kUnprovenSemaphoreBound. Unlike the retired vector these carry no
    // completion proof of their own, so the destroy precondition has to be
    // manufactured -- DeviceWaitIdle, the same instrument Deinitialize uses,
    // for the same reason (a staging batch can ride the TRANSFER queue, which
    // an encode-queue wait does not cover). Takes m_pendingMutex to swap out,
    // waits and destroys outside it.
    void        DrainUnprovenSemaphores();

    const VkEncNullBackendState*     m_nullBackend = nullptr;
    // Observation seam (VkEncProbeLastSubmitSync, internal header): the
    // wait/signal arrays the last submit was handed after the
    // chained-descriptor walk. Written on the NULL-BACKEND arm only, so a
    // production session never touches either member and pays nothing.
    std::mutex                       m_lastSubmitSyncMutex;
    VkEncSubmitSyncProbe             m_lastSubmitSync;
    void RecordSubmitSyncForTest(const VkVideoEncodeInputFrame& frame);

    uint64_t                         m_framesSubmitted;
    // Rate-control mode (VkVideoEncoderConfig::rateControlMode)
    // stashed at InitializeExt time so GetRuntimeInfo() can derive
    // trustedRateController without reaching into EncoderConfig internals.
    // The rate-control MODE is session-fixed; Reconfigure() updates rates only.
    std::atomic<VkVideoEncodeRateControlModeFlagBitsKHR> m_rateControlMode;
    // Capability scalars reported by the lock-free query methods. Snapshotted
    // at init so those methods never dereference m_encoderConfig, which
    // Deinitialize can null concurrently.
    std::atomic<uint32_t>            m_capsMaxWidth{0};
    std::atomic<uint32_t>            m_capsMaxHeight{0};
    // The preprocess compute filter's session state, snapshotted for the same
    // reason and under the same rule as the scalars above: ComputeFilterActive
    // is reached from SupportsFormat, which the header lists as class (c) --
    // callable from ANY thread, taking no lock -- while Deinitialize nulls and
    // then destroys m_encoderConfig under m_pendingMutex. Reading the
    // shared_ptr there is both a use-after-free window and an unsynchronised
    // read of a shared_ptr instance another thread is storing to.
    //
    // The FORMAT is snapshotted too, not just the flag: "this session has a
    // filter" is not the same question as "this session's filter takes THIS
    // format", and answering the second with the first over-promises to a
    // producer that then allocates a pool RegisterImageResource refuses.
    std::atomic<bool>                m_computeFilterActive{false};
    std::atomic<VkFormat>            m_computeFilterInputFormat{VK_FORMAT_UNDEFINED};
    // The session's own input declaration -- the format and the colour model
    // the caller stated for it -- snapshotted for the same lock-free readers.
    std::atomic<VkFormat>            m_sessionInputFormat{VK_FORMAT_UNDEFINED};
    std::atomic<VkVideoEncoderColorModel> m_sessionInputColorModel{
        VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT};
    // What the session was initialized with, so Reconfigure can tell a
    // change it can carry from one it can only pretend to.
    VkVideoEncoderConfig             m_initConfig{};
    std::atomic<uint32_t>            m_capsGranularityW{0};
    std::atomic<uint32_t>            m_capsGranularityH{0};
    // R-5 telemetry: how often import memory-type selection ran WITHOUT its
    // authoritative constraint (query unavailable, or opaque import without
    // the exporter's index), and how often an exporter-supplied index was
    // overridden by the mask. "Zero fallbacks" was the design's named
    // removal condition for the transition quirk; these are what make that
    // number observable.
    std::atomic<uint64_t> m_importMemTypeHeuristicSelections{0};
    std::atomic<uint64_t> m_importMemTypeExporterOverrides{0};
    void SnapshotCaps();

    // Tracking submitted frames for async retrieval
    struct PendingFrame {
        uint64_t frameId = 0;
        uint64_t pts = 0;
        VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> encodeFrameInfo;
        // In-memory bitstream captured for this frame.
        // Populated by GetEncodedFrame() draining
        // VkVideoEncoder::TryPopCapturedBitstream(). Empty until a
        // corresponding capture arrives.
        std::vector<uint8_t> bytes;
        bool isIdr = false;
        uint32_t pictureType = 0;
        // Readiness + per-frame result. hasCapture (not
        // bytes-non-empty) is the readiness signal so both failed frames
        // (empty bytes + error status) and legitimate 0-byte drop-frames
        // are DELIVERED instead of wedging the FIFO forever.
        bool hasCapture = false;
        VkResult status = VK_SUCCESS;
        // Retrieval state: |acquired| marks delivery (pBitstreamData stays
        // valid until ReleaseEncodedFrame); |timedOut| marks a
        // deadline-synthesized drop whose late capture, if one ever
        // arrives, must be discarded; |submitTime| anchors the deadline.
        bool acquired = false;
        bool timedOut = false;
        // The registration this frame was submitted against, so its
        // reference can be dropped when the frame leaves the queue.
        VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        // Unique per reservation. Frame ids are consumer-chosen and may
        // recur while an earlier frame carrying the same id is still
        // resident, so commit and rollback address a record by this and
        // never by frameId -- which would otherwise let a submit roll back
        // somebody else's frame.
        uint64_t admissionToken = 0;
        // False between reservation and the submit returning. The entry is
        // in the queue from the reservation onwards so an early worker
        // capture has somewhere to land; what only the submit knows is
        // filled at commit.
        bool admitted = false;
        std::chrono::steady_clock::time_point submitTime;
        // The per-frame release fence's semaphore, if this frame asked for
        // one. The SYNC_FD handed to the caller was exported from it and is
        // an independent kernel object from that moment on, so this handle
        // exists only to be destroyed once the submissions referring to it
        // have completed -- see the two graveyards above.
        VkSemaphore releaseFenceSemaphore = VK_NULL_HANDLE;
        // The acquire half of the same entry point: the binary semaphores
        // this frame's imported acquire fds were imported into. Owned here
        // for the same reason and disposed on the same proof -- the
        // submission that WAITS on them is the submission that signals the
        // release fence, so the two have an identical destroy precondition.
        std::vector<VkSemaphore> acquireFenceSemaphores;
    };
    std::deque<PendingFrame>  m_pendingFrames;
    // Monotonic, never reused, guarded by m_pendingMutex. Zero is "no
    // reservation", which is what a rollback for a refused reservation
    // carries.
    uint64_t                  m_nextAdmissionToken = 1;
    // Completion order (design 3.4). Frame ids in the order their
    // outcomes became deliverable -- a real capture, a deadline drop, or a
    // cancellation -- which is NOT submit order. AcquireNextEncodedFrame
    // drains from the front, so a frame that never completes cannot strand
    // the frames behind it. Entries are skipped lazily when the frame was
    // taken first by the keyed AcquireEncodedFrame, and pruned on release.
    std::deque<uint64_t>      m_readyOrder;
    // Frame ids ReleaseEncodedFrame or AbandonAllFrames erased while they
    // were still PENDING (no capture, no deadline drop, no cancellation):
    // their completion records are still in flight and will pop with no
    // entry to match. DrainCapturesLocked consumes one record per such pop
    // and discards it silently; an unmatched pop with no record here is
    // the late class m_lateCaptures counts. Guarded by m_pendingMutex,
    // like the queues above. Self-draining in steady state (one insert,
    // one pop); cleared where its pops can no longer arrive -- Flush()
    // where m_encoder is dropped, and Deinitialize() with the pending set.
    // A multiset because frame ids are consumer-chosen: an id released
    // while pending may be submitted and released again before its first
    // capture pops.
    std::unordered_multiset<uint64_t> m_releasedWhilePending;

    // Registered semaphores. Separate from the image registry, and ids carry
    // kSemaphoreTag so an image id used where a semaphore is expected is
    // rejected rather than resolved against the wrong table -- that mistake
    // would produce a wait on an unrelated object, which presents as a hang
    // rather than as an error.
    struct RegisteredSemaphore {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        uint32_t    generation = 1;
        bool        live = false;
    };
    static constexpr uint64_t kSemaphoreTag = 1ull << 63;
    std::mutex                       m_semaphoreMutex;
    std::deque<RegisteredSemaphore>  m_semaphores;
    // mutable: GetCompletionSemaphore() is const but class (c), and the
    // state it reads (m_encoder) is cleared under this lock by Flush()
    // and teardown -- a const method that skipped the lock would race
    // them (the defect this comment replaces).
    mutable std::mutex        m_pendingMutex;

    // Completion deadline (ns; clamped at InitializeExt) + the two
    // observability counters that make a deadline/fence-cap collision or a
    // stalling pipeline visible instead of silent.
    uint64_t m_frameTimeoutNs = 8000000000ull;
    uint64_t m_framesTimedOut = 0;
    uint64_t m_lateCaptures   = 0;

    void OnBitstreamCaptured(uint64_t frameId);
    bool IsInCompletionCallback() const {
        return m_callbackThreadId.load(std::memory_order_relaxed) ==
               std::this_thread::get_id();
    }
    void CancelFrameLocked(PendingFrame& frame);

    // M6 completion currency. Counter is the coalescing-safe drain target.
    // The callback pointer pair is guarded by m_pendingMutex and is only
    // READ FOR INVOCATION while m_callbackMutex is held; invocations are
    // serialized by m_callbackMutex, with the invoking thread recorded so
    // session-serial methods can reject re-entry from inside the callback.
    // (Captures are already serialized by the core encoder's assembly
    // ordering lock, so concurrent invocation is not reachable today;
    // m_callbackMutex keeps the contract true independently of that.)
    // SetCompletionCallback takes BOTH locks (callback -> pending, the
    // OnBitstreamCaptured order), making it a quiesce point: when it
    // returns, no invocation of the previously installed callback is in
    // flight or can start -- the caller may then destroy the old pUserData.
    PFN_vkVideoEncoderCompletionCallback m_completionCallback = nullptr;
    void*                                m_completionUserData = nullptr;
    std::mutex                           m_callbackMutex;
    std::atomic<std::thread::id>         m_callbackThreadId{};
    std::atomic<uint64_t>                m_completionCounter{0};

    // Non-null when the caller transferred ownership of m_completionUserData
    // to us. Invoked exactly once, when the cookie is displaced or the
    // encoder is destroyed.
    PFN_vkVideoEncoderUserDataRelease     m_completionUserDataRelease{nullptr};

    // OS-handle completion currency (M7). kOsCompletionEventNone until a
    // caller asks for it, so a consumer that never wants one pays nothing.
    // Atomic because creation (under m_pendingMutex, published with a
    // release store) races the capture-path signal, which is a bare acquire
    // load plus write() and must not take a lock on the capture path; the
    // pairing makes the eventfd's creation happen-before any signal
    // through it.
    std::atomic<uint64_t>                m_completionEventHandle{
        vkenc::kOsCompletionEventNone};
    uint64_t                             m_framesCancelled = 0;

    // Diagnosability that survives silenceStdio (A10.2): the stderr
    // misuse lines are also recorded here -- count plus most recent
    // text -- and read back through the GetCompletionInfo pNext chain.
    // Guarded by m_pendingMutex, which every recording site already
    // holds.
    uint64_t m_diagnosticCount = 0;
    char     m_lastDiagnostic[VK_VIDEO_ENCODER_MAX_DIAGNOSTIC_CHARS] = {};

    // The most recent verdict from a registration the dma-buf
    // import-ordinal guard EVALUATED, readable through the
    // GetCompletionInfo pNext chain (VkVideoEncoderImportGuardInfo). A
    // registration the guard does not apply to leaves it UNCHANGED, so a
    // VK_IMAGE registration cannot erase the COMPLETE a dma-buf one
    // established. Guarded by m_pendingMutex, like the diagnostic pair
    // above and for the same reason: that is the lock GetCompletionInfo
    // reads under.
    VkEncImportOrdinalGuardReport m_importGuardReport{};

    // This session's silence request, held for its whole life -- see
    // VkEncoderStdioLatch.h. Declared before the members whose teardown can
    // still print, so it is destroyed after them.
    VkEncoderStdioSilenceScope m_stdioSilence;

    // --- Terminal shutdown ---
    //
    // Serializes Flush() against itself so a repeated Finish never drains
    // twice or restarts workers, and guards every field of m_shutdown. Never
    // taken while m_pendingMutex or m_callbackMutex is held: Flush joins
    // library threads, and those threads take both.
    mutable std::mutex         m_shutdownMutex;
    VkVideoEncoderShutdownInfo m_shutdown{
        VK_VIDEO_ENCODER_SHUTDOWN_UNPROVEN,
        VK_SUCCESS,
        VK_NOT_READY,
        VK_FALSE,
        VK_FALSE,
        VK_FALSE,
        VK_FALSE};

    // Acceptance stops ONCE, before the joins, and stays stopped even if the
    // shutdown ends Unproven and keeps the encoder alive. Checked without
    // m_shutdownMutex because the submit path must not serialize behind a
    // shutdown that is joining threads.
    std::atomic<bool> m_shutdownStarted{false};

    // --- Handle exchange ---
    struct RegisteredImage {
        VkImage        image      = VK_NULL_HANDLE;
        VkDeviceMemory memory     = VK_NULL_HANDLE;
        // Bumped on every retirement, so a stale id names a generation that
        // no longer exists and is rejected instead of matching a recycled
        // slot. This is the whole reason ids are not pointers.
        uint32_t       generation = 1;
        // Frames submitted against this registration that the GPU may still
        // be reading. Retirement waits for zero: refcounted, never
        // timeline-driven, because a timeline eviction either stalls on a
        // device wait or frees an image mid-read.
        uint32_t       inFlight   = 0;
        bool           live       = false;
        bool           retired    = false;
        // False for VK_IMAGE registrations: the caller's image, not ours.
        bool           ownsImage  = false;
        // Kept so submit can apply the residency rule: an EXPLICIT
        // declaration is honoured on ANY handle type; AUTO (and a
        // zero-initialised field) is derived as FOREIGN for an import.
        // The rule is stated on the field it describes, not inferred from the
        // handle type.
        VkVideoEncoderExternalHandleType handleType =
            VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_NONE;
        VkVideoEncoderInputResidency residency =
            VK_VIDEO_ENCODER_INPUT_RESIDENCY_AUTO;
        VkFormat       format     = VK_FORMAT_UNDEFINED;
        uint32_t       width      = 0;
        uint32_t       height     = 0;
        VkImageLayout  defaultLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageTiling  tiling = VK_IMAGE_TILING_OPTIMAL;
        // Input routing decided once, at registration, from the SAME
        // predicate that routes the registered submit (encodeCapable below)
        // -- so path and routing can never disagree. All three values are
        // produced: FILTER is assigned when the registration needs the
        // preprocess compute filter and this session can actually run it,
        // and the submit path reads this field back to route the frame.
        VkVideoEncoderExternalInputPath inputPath =
            VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT;
        // Reserved scratch slot for the A2 filter path: the library-owned
        // OPTIMAL image the compute filter would write and the encode would
        // read. Never populated today; destruction is
        // wired in DestroyResourceLocked so a future population cannot leak.
        VkImage        filterScratchImage  = VK_NULL_HANDLE;
        VkDeviceMemory filterScratchMemory = VK_NULL_HANDLE;
        VkImageView    filterScratchView   = VK_NULL_HANDLE;
        // The import's / caller's true usage. For an import this is the
        // usage the image was created with here (never fabricated); for a
        // VK_IMAGE registration it is the caller's declaration, 0 = unknown
        // (the view then assumes the legacy set -- once, at registration).
        VkImageUsageFlags imageUsage = 0;
        // Routing predicate, computed once at registration: encodable
        // format, non-LINEAR tiling, and the image actually carries
        // VIDEO_ENCODE_SRC usage. Replaces the per-frame tiling check on
        // the registered submit path.
        bool           encodeCapable = false;
        // Whether this registration's view carries the per-plane STORAGE
        // views the preprocess compute filter binds. Computed once, from the
        // DESCRIPTOR (MUTABLE_FORMAT + STORAGE + a multi-planar format) and
        // then confirmed against the view that was actually built, so a
        // driver that declined a plane view cannot leave this claiming one
        // exists. Distinct from |encodeCapable|: this registration's image is
        // the filter's SOURCE, and only the filter's OUTPUT is an encode
        // source (PROCESSING_GRAPHS section 3.1).
        bool           planeStorageViews = false;
        // The SINGLE-PLANE parallel of the field above, and a different
        // question about the same wrapper: not "how many per-plane views
        // were built" but "is there a combined view, and may it be bound as
        // a STORAGE_IMAGE". The compute filter's RGBA arm binds exactly one
        // VK_DESCRIPTOR_TYPE_STORAGE_IMAGE over that combined view
        // (VulkanFilterYuvCompute: m_inputImageAspects = COLOR_BIT, aspect 0
        // bound through GetImageView()), so a slot claiming this must have
        // one. Computed the same way |planeStorageViews| is -- from the view
        // that was actually BUILT, never from the descriptor's request --
        // because those two can disagree and only the built one is what the
        // filter will bind.
        bool           storageReadView = false;
        // The allocation size the import actually used, recorded so the
        // refcounted wrapper's bookkeeping matches the real allocation.
        VkDeviceSize   importedAllocSize = 0;
        // Created ONCE at registration, so that a submit performs no
        // allocation at all. The node is shared by every frame
        // submitted against this registration -- safe because the encode
        // path only reads it (GetPictureResourceInfo / GetImageView are the
        // only consumers; SetNewLayout has no call site in the encoder
        // libs). A frame in flight holds |node| via VkSharedBaseObj, so
        // retirement can drop these refs without freeing memory the GPU is
        // still reading: the wrapper's refcount is the final arbiter.
        VkSharedBaseObj<VkImageResourceView>      imageView;
        VkSharedBaseObj<VulkanVideoImagePoolNode> node;
    };
    std::vector<RegisteredImage> m_resources;
    std::mutex                   m_resourceMutex;

    static VkVideoEncoderResource MakeResourceId(size_t index,
                                                 uint32_t generation) {
        return ((VkVideoEncoderResource)generation << 32) |
               (VkVideoEncoderResource)(index + 1);
    }
    static size_t   ResourceIndex(VkVideoEncoderResource r) {
        return (size_t)((r & 0xFFFFFFFFull) - 1);
    }
    static uint32_t ResourceGeneration(VkVideoEncoderResource r) {
        return (uint32_t)(r >> 32);
    }
    // Caller holds m_resourceMutex.
    RegisteredImage* LookupResourceLocked(VkVideoEncoderResource resource);
    void DestroyResourceLocked(RegisteredImage& slot);
    VkVideoEncoderStatusCode ImportImageLocked(
        const VkVideoEncoderExternalImageDescriptor& desc,
        uint64_t osHandle, RegisteredImage& slot);
    // Fold the calling thread's import-guard verdict into the session
    // snapshot, and record an INCOMPLETE one in the diagnostic channel.
    // MUST NOT be called with m_resourceMutex held -- see the definition.
    void PublishImportGuardVerdict();
    // The dma-buf import CONTENT probe. Owned HERE and injected into the
    // encoder, not the other way round: a null-backend session has no
    // encoder object, and arming plus reporting must still work there or the
    // carrier is provable only on hardware. Created lazily by the first
    // registration that asks for it, so a caller that never chains
    // VkVideoEncoderImportContentInfo pays nothing -- not even the object.
    VkSharedBaseObj<VkVideoEncoderContentProbe> m_contentProbe;
    // Both MUST NOT be called with m_resourceMutex held: they take
    // m_pendingMutex to reach m_encoder, and this file's only established
    // order is pending -> resource. Both callers run them from a scope guard
    // declared ahead of their m_resourceMutex lock guard.
    VkVideoEncoderImportContentState ArmImportContentProbe(
        VkVideoEncoderResource resource,
        VkVideoEncoderContentProbe::CaptureSite captureSite);
    void ForgetImportContentProbe(VkVideoEncoderResource resource);
    // Build the once-per-registration wrapper + combined view + pool node
    // for |slot| (see the definition for the full contract). Caller holds
    // m_resourceMutex.
    VkVideoEncoderStatusCode BuildRegisteredViewLocked(
        const VkVideoEncoderExternalImageDescriptor& desc,
        RegisteredImage& slot);
    // Device's DRM format modifiers + their 32-bit tiling features for
    // |format| (two-call vkGetPhysicalDeviceFormatProperties2 with
    // VkDrmFormatModifierPropertiesListEXT chained). Empty when the
    // session is not initialized or the modifier extension is unavailable.
    std::vector<VkDrmFormatModifierPropertiesEXT> EnumerateDrmModifiers(
        VkFormat format) const;
    // Would an image with |desc|'s shape register if it were DRM-tiled
    // with |modifier| and imported from |desc|'s handle type?
    // vkGetPhysicalDeviceImageFormatProperties2 with the modifier,
    // external-handle and (under MUTABLE_FORMAT) format-list inputs
    // mirroring ImportImageLocked's vkCreateImage, accepted only when the
    // handle type is IMPORTABLE and every image-creation limit the query
    // returns admits the descriptor (VkEncDescriptorWithinCreationLimits)
    // -- the limits that create call's validity is defined against.
    // Mirrored inputs alone are not agreement: the create call is bound by
    // the query's OUTPUTS too, and an acceptance that reads fewer of them
    // admits a descriptor the import must then refuse. What the query
    // cannot see -- the explicit plane layouts, allocation failure -- stays
    // the import's to judge, and a refusal there is a genuine
    // IMPORT_FAILED, not a misclassified MODIFIER_UNSUPPORTED.
    bool ModifierWouldRegister(
        const VkVideoEncoderExternalImageDescriptor& desc,
        uint64_t modifier) const;
    // Fill a chained VkVideoEncoderImageSupportDetails. Best-effort: leaves
    // the zeroed defaults wherever the answer is unknowable.
    void FillImageSupportDetails(
        const VkVideoEncoderExternalImageDescriptor& desc,
        VkVideoEncoderImageSupportDetails* details) const;
    // The one submit implementation behind both public entry points. With
    // |preparedNode| == nullptr this is the legacy arm, byte-for-byte: the
    // per-frame wrap happens inside SetExternalInputFrame. With a node it
    // is the registered arm: the wrap already happened at registration and
    // this creates no Vulkan object for the input.
    //
    // |releaseFenceSemaphore| (optional) is the caller's per-frame release
    // fence semaphore, already appended to |frame|'s signal list by the
    // caller of this function. This function owns exporting it, because the
    // only place the answer to "was the input-consuming submit issued?" is
    // observable is right here, on the frame-info node, immediately after
    // the Set*Frame call returns. |pReleaseFenceFd| receives the exported fd
    // or -1; the semaphore itself always ends up owned by the PendingFrame.
    VkResult SubmitExternalFrameCommon(
        const VkVideoEncodeInputFrame& frame,
        VkSharedBaseObj<VulkanVideoImagePoolNode>* preparedNode,
        bool encodeCapable,
        // The registration's resolved ladder rung for the frames
        // |encodeCapable| refuses. Passed down rather than re-derived in the
        // encoder core, so the path this registration REPORTS through
        // VK_VIDEO_EXTERNAL_INPUT_PATH_* and the path its frames actually
        // take are one decision, made once.
        bool routeViaFilter,
        VkVideoEncoderResource resource,
        VkSemaphore* pStagingCompleteSemaphore,
        VkSemaphore releaseFenceSemaphore = VK_NULL_HANDLE,
        int* pReleaseFenceFd = nullptr,
        const std::vector<VkSemaphore>* acquireFenceSemaphores = nullptr,
        // Did |frame.currentLayout| come from the FRAME (explicit) or from
        // the registration's defaultLayout standing in for the UNDEFINED
        // sentinel? Only SubmitRegisteredFrame, which applies that
        // sentinel, can answer, and the encoder core needs the answer to
        // decide whether its own recorded residual layout may supersede
        // the declaration. Defaults false, which is both correct and
        // inert for the LEGACY lane: it has no registration default, and
        // its per-frame node never carries a residual.
        bool srcLayoutIsExplicit = false);
    // Create the PendingFrame entry for a successfully submitted frame,
    // handing it ownership of |resource|'s in-flight reference at creation
    // (NULL for the legacy arm). Takes m_pendingMutex.
    void EnqueuePendingFrame(
        const VkVideoEncodeInputFrame& frame,
        VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo>&
            encodeFrameInfo,
        VkVideoEncoderResource resource,
        VkSemaphore releaseFenceSemaphore = VK_NULL_HANDLE,
        const std::vector<VkSemaphore>* acquireFenceSemaphores = nullptr);

    // Reserve/commit/rollback around the core submission.
    //
    // Reserve inserts the entry with everything known before the encoder is
    // touched, and returns its admission token. It takes and RELEASES
    // m_pendingMutex: no core or queue operation may run under that lock,
    // because the assembly workers take it on the capture path.
    uint64_t ReservePendingFrame(
        const VkVideoEncodeInputFrame& frame,
        VkVideoEncoderResource resource,
        VkSemaphore releaseFenceSemaphore,
        const std::vector<VkSemaphore>* acquireFenceSemaphores);
    // Finish admission for a submit that succeeded: attach the frame info
    // the submit produced and count the frame once.
    void CommitPendingFrame(
        uint64_t admissionToken,
        VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo>&
            encodeFrameInfo);
    // Undo a reservation whose submit was refused. Removes the entry only
    // when no capture arrived for it; a reservation that DID capture
    // describes work that landed, and its resources stay owned by the
    // pending record rather than being torn down under in-flight use.
    void RollbackPendingFrame(
        uint64_t admissionToken,
        VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo>&
            encodeFrameInfo);

    void DrainCapturesLocked();
    void FillResultLocked(PendingFrame& frame, VkVideoEncodeResult& result);
    bool SynthesizeTimeoutLocked(PendingFrame& frame);
    void MarkFrameReadyLocked(PendingFrame& frame);
    bool TryPopReadyLocked(VkVideoEncodeResult& result);
};

//=============================================================================
// Map VkVideoEncoderConfig codec field to VkVideoCodecOperationFlagBitsKHR
//=============================================================================
static VkVideoCodecOperationFlagBitsKHR MapCodecOperation(
    VkVideoCodecOperationFlagBitsKHR codec)
{
    switch ((uint32_t)codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            return codec;
        default:
            return VK_VIDEO_CODEC_OPERATION_NONE_KHR;
    }
}

//=============================================================================
// Build EncoderConfig from structured config
//=============================================================================
// Size guard for the config binder below. This does NOT pin an ABI -- the
// struct is versioned by sType and consumers are rebuilt against the header.
// It exists so that ADDING A FIELD cannot silently do nothing: the binder
// copies fields one at a time, a new field that nobody wired up simply never
// reaches the encoder, and that failure is invisible at runtime. Tripping this
// assert means: bind the new field here, then update the number.
// The public profile constants ARE the codec standard's numbers, which is
// what lets the binder below cast rather than translate. The std enums carry
// the same numbers, so this pins the two together at compile time instead of
// leaving a table to drift.
static_assert((uint32_t)VK_VIDEO_ENCODER_PROFILE_H264_BASELINE ==
                  (uint32_t)STD_VIDEO_H264_PROFILE_IDC_BASELINE &&
              (uint32_t)VK_VIDEO_ENCODER_PROFILE_H264_MAIN ==
                  (uint32_t)STD_VIDEO_H264_PROFILE_IDC_MAIN &&
              (uint32_t)VK_VIDEO_ENCODER_PROFILE_H264_HIGH ==
                  (uint32_t)STD_VIDEO_H264_PROFILE_IDC_HIGH,
              "H.264 profile constants must be profile_idc");
static_assert((uint32_t)VK_VIDEO_ENCODER_PROFILE_H265_MAIN ==
                  (uint32_t)STD_VIDEO_H265_PROFILE_IDC_MAIN &&
              (uint32_t)VK_VIDEO_ENCODER_PROFILE_H265_MAIN10 ==
                  (uint32_t)STD_VIDEO_H265_PROFILE_IDC_MAIN_10,
              "H.265 profile constants must be general_profile_idc");
static_assert((uint32_t)VK_VIDEO_ENCODER_PROFILE_AV1_MAIN ==
                  (uint32_t)STD_VIDEO_AV1_PROFILE_MAIN,
              "AV1 profile constants must be seq_profile");

#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
static_assert(sizeof(VkVideoEncoderConfig) == 208,
              "VkVideoEncoderConfig changed size -- a field was added, removed "
              "or reordered. Bind it in BuildEncoderConfig before updating "
              "this number.");
#endif  // 64-bit: sizeof moves with pointer width


// Declared here rather than in the internal header: its signature names
// EncoderConfig, and that header deliberately stays clear of the library's
// private types so a test can include it without them.
// |requestedEncodeBitDepth| is the encode side stated rather than derived; see
// the internal header's note on VkEncBuildAndProbeConfig for why anything
// needs to state it. Zero is the ordinary path.
VkResult VkEncBuildEncoderConfig(const VkVideoEncoderConfig& extConfig,
                                 VkVideoCodecOperationFlagBitsKHR codecOp,
                                 VkSharedBaseObj<EncoderConfig>& outConfig,
                                 uint32_t requestedEncodeBitDepth = 0);

// The quantizer range |codecOp| admits, in the units the caller states
// constQpI/P/B in.
//
// CODEC-DEPENDENT BECAUSE THE UNIT IS. H.264 and H.265 carry a QP on 0..51.
// AV1 has no QP at all: it carries a quantizer INDEX on 0..255. 52 is a
// legal AV1 quantizer index and an illegal H.26x QP, so one range applied to
// both would either refuse three quarters of the AV1 scale or admit an H.26x
// value the codec has no syntax for.
static int32_t VkEncMaxConstQpForCodec(VkVideoCodecOperationFlagBitsKHR codecOp)
{
    return (codecOp == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) ? 255 : 51;
}

// Refuse a constant quantizer the codec cannot express, instead of letting it
// wrap.
//
// WHY THE REFUSAL IS HERE AND NOT WHERE THE VALUE BREAKS. Nothing between
// this boundary and the bitstream narrows it: the ext fields are int32_t,
// ConstQpSettings holds uint32_t (VkVideoEncoderDef.h), and the single
// narrowing is the (uint8_t) cast in VkVideoEncoderAV1::EncodeFrame that
// writes pictureInfo.constantQIndex and, from it, stdQuantInfo.base_q_idx.
// So an out-of-range value was refused nowhere -- it was TRUNCATED there,
// modulo 256, with VK_SUCCESS answered to the caller and no diagnostic
// anywhere: constQpI 300 encoded at quantizer index 44. A value silently
// accepted and altered is the same defect as a VK_SUCCESS that changes
// nothing, and it is worse for being invisible at the call that caused it.
//
// NEGATIVE IS NOT OUT OF RANGE. It is this API's spelling of "this config
// names no quantizer", read that way on both entry paths, so it is left to
// them; only an upper bound is enforced here.
//
// THE DEVICE'S OWN QUANTIZER WINDOW IS NOT CONSULTED, and reusing the one
// this library already records would not have covered the defect. That
// window (VkVideoEncoder::m_deviceQpWindowMin/Max, checked in
// RequestRateControlUpdate) is written only by the H.264 and H.265 arms at
// codec init, from h26xEncodeCapabilities.minQp/maxQp. The AV1 arm never
// writes it, so it reads 0 and the guard keyed on it is inert on exactly the
// codec whose scale wraps. AV1 device limits do exist, but in the other unit
// and on another object (EncoderConfigAV1::minQIndex/maxQIndex). This is the
// SYNTACTIC range, refused device-free; the device window stays the later
// and separate gate it already was.
static VkResult VkEncValidateConstQpRange(
    int32_t constQpI, int32_t constQpP, int32_t constQpB,
    VkVideoCodecOperationFlagBitsKHR codecOp, const char* where)
{
    const int32_t maxQuantizer = VkEncMaxConstQpForCodec(codecOp);
    const struct {
        const char* name;
        int32_t     value;
    } named[] = {
        {"constQpI", constQpI},
        {"constQpP", constQpP},
        {"constQpB", constQpB},
    };
    for (const auto& quantizer : named) {
        if (quantizer.value > maxQuantizer) {
            VkEncErr() << "[EncoderExt] " << where << quantizer.name << " "
                       << quantizer.value << " is outside the "
                       << ((codecOp ==
                            VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR)
                               ? "AV1 quantizer-index range 0..255"
                               : "H.26x QP range 0..51")
                       << ". The unit is the codec's own, and the value is "
                          "refused rather than truncated." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return VK_SUCCESS;
}

VkResult VulkanVideoEncoderExtImpl::BuildEncoderConfig(
    const VkVideoEncoderConfig& extConfig,
    VkVideoCodecOperationFlagBitsKHR codecOp,
    VkSharedBaseObj<EncoderConfig>& outConfig)
{
    return VkEncBuildEncoderConfig(extConfig, codecOp, outConfig);
}

// Section 4.2 layer 3: run the binder and flatten what it produced. Kept
// beside the binder so a new BOUND field is projected here in the same edit
// that binds it, and the conformance test then has something to assert on.
VkResult VkEncBuildAndProbeConfig(const VkVideoEncoderConfig& extConfig,
                                  VkVideoCodecOperationFlagBitsKHR codecOp,
                                  VkEncBoundConfigProbe* outProbe,
                                  uint32_t requestedEncodeBitDepth)
{
    if (outProbe == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkSharedBaseObj<EncoderConfig> cfg;
    const VkResult result = VkEncBuildEncoderConfig(extConfig, codecOp, cfg,
                                                    requestedEncodeBitDepth);
    if ((result != VK_SUCCESS) || !cfg) {
        return (result != VK_SUCCESS) ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    *outProbe = {};
    outProbe->encodeWidth   = cfg->encodeWidth;
    outProbe->encodeHeight  = cfg->encodeHeight;
    outProbe->inputWidth    = cfg->input.width;
    outProbe->inputHeight   = cfg->input.height;
    outProbe->inputBpp      = cfg->input.bpp;
    outProbe->rateControlMode = (uint32_t)cfg->rateControlMode;
    outProbe->averageBitrate  = (uint32_t)cfg->averageBitrate;
    outProbe->maxBitrate      = (uint32_t)cfg->maxBitrate;
    outProbe->vbvBufferSize   = (uint32_t)cfg->vbvBufferSize;
    outProbe->constQpIntra    = (uint32_t)cfg->constQp.qpIntra;
    outProbe->constQpInterP   = (uint32_t)cfg->constQp.qpInterP;
    outProbe->constQpInterB   = (uint32_t)cfg->constQp.qpInterB;
    outProbe->minQp           = cfg->minQp;
    outProbe->maxQp           = cfg->maxQp;
    outProbe->minQpSet        = cfg->minQpSet ? 1u : 0u;
    outProbe->maxQpSet        = cfg->maxQpSet ? 1u : 0u;
    outProbe->constQpSet      = cfg->constQpSet ? 1u : 0u;
    outProbe->gopFrameCount      = cfg->gopStructure.GetGopFrameCount();
    outProbe->idrPeriod          = (uint32_t)cfg->gopStructure.GetIdrPeriod();
    outProbe->consecutiveBFrames =
        cfg->gopStructure.GetConsecutiveBFrameCount();
    outProbe->closedGop          = cfg->gopStructure.IsClosedGop() ? 1u : 0u;
    outProbe->frameRateNumerator   = cfg->frameRateNumerator;
    outProbe->frameRateDenominator = cfg->frameRateDenominator;
    outProbe->qualityLevel         = (uint32_t)cfg->qualityLevel;
    outProbe->tuningMode           = (uint32_t)cfg->tuningMode;
    outProbe->colourPrimaries         = cfg->colour_primaries;
    outProbe->transferCharacteristics = cfg->transfer_characteristics;
    outProbe->matrixCoefficients      = cfg->matrix_coefficients;
    outProbe->videoFullRangeFlag      = cfg->video_full_range_flag;
    outProbe->colorDescriptionPresent = cfg->color_description_present_flag;
    outProbe->videoSignalTypePresent  = cfg->video_signal_type_present_flag;
    outProbe->chromaLocInfoPresent    = cfg->chroma_loc_info_present_flag;
    outProbe->chromaSampleLocType     = cfg->chroma_sample_loc_type;
    outProbe->inputColourChainPresent = cfg->inputColourChainPresent;
    outProbe->inputColourPrimaries    = cfg->inputColourPrimaries;
    outProbe->inputTransferCharacteristics =
        cfg->inputTransferCharacteristics;
    outProbe->inputMatrixCoefficients = cfg->inputMatrixCoefficients;
    outProbe->inputRange              = cfg->inputRange;
    outProbe->hdrMasteringPresent     = cfg->hdrMetadata.masteringDisplayPresent;
    outProbe->hdrContentLightPresent  = cfg->hdrMetadata.contentLightLevelPresent;
    outProbe->hdrMaxDisplayMasteringLuminance =
        cfg->hdrMetadata.maxDisplayMasteringLuminance;
    outProbe->hdrMinDisplayMasteringLuminance =
        cfg->hdrMetadata.minDisplayMasteringLuminance;
    outProbe->hdrMaxContentLightLevel = cfg->hdrMetadata.maxContentLightLevel;
    outProbe->hdrMaxFrameAverageLightLevel =
        cfg->hdrMetadata.maxFrameAverageLightLevel;
    outProbe->hdrGreenPrimaryX        = cfg->hdrMetadata.displayPrimaryX[0];
    outProbe->hdrGreenPrimaryY        = cfg->hdrMetadata.displayPrimaryY[0];
    outProbe->verbose           = cfg->verbose ? 1u : 0u;
    outProbe->validate          = cfg->validate ? 1u : 0u;
    outProbe->disableFileOutput = cfg->disableFileOutput ? 1u : 0u;
    // Not a public config field: it is where the library's own
    // preprocess-conversion decision lands. Read through the compile-safe
    // accessor so the projection exists under both build gates -- and so a
    // build without the filter honestly reports 0 for an input that would
    // have needed one, which is the same build whose binder refuses that
    // input outright.
    outProbe->preprocessComputeFilter =
        cfg->IsPreprocessComputeFilterEnabled() ? 1u : 0u;
    // The only observable trace of inputFormat's plane layout: EncoderConfig
    // derives input.vkFormat from this rather than storing the format.
    outProbe->inputNumPlanes = cfg->input.numPlanes;
    // The other half of that derivation, and what the encode profile is
    // picked from: a 4:4:4 input must have moved this off the 4:2:0 default.
    outProbe->inputChromaSubsampling =
        (uint32_t)cfg->input.chromaSubsampling;
    outProbe->inputVkFormat = (uint32_t)cfg->input.vkFormat;
    outProbe->encodeChromaSubsampling =
        (uint32_t)cfg->encodeChromaSubsampling;
    outProbe->encodeBitDepthLuma   = cfg->encodeBitDepthLuma;
    outProbe->encodeBitDepthChroma = cfg->encodeBitDepthChroma;
    // Per arm through virtual dispatch: the profile the session-creation
    // path consumes (EncoderConfig::InitVideoProfile reads GetCodecProfile).
    // Never INVALID here -- the binder either wrote the caller's profile or
    // InitProfileLevel derived one inside InitializeParameters above.
    outProbe->codecProfile      = cfg->GetCodecProfile();

    // Per-codec-arm projections. Everything here is device-free: the H.26x
    // GetRateControlParameters overloads and the AV1 InitSequenceHeader read
    // only config state the binder + FinalizeConfig already produced, so the
    // conformance test can assert per-arm EFFECT, not just per-field storage.
    switch ((uint32_t)codecOp) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR: {
            EncoderConfigH264* h264 = cfg->GetEncoderConfigh264();
            if (h264 != nullptr) {
                VkVideoEncodeRateControlInfoKHR rcInfo{};
                VkVideoEncodeRateControlLayerInfoKHR rcLayer{};
                VkVideoEncodeH264RateControlInfoKHR rcInfoH264{};
                VkVideoEncodeH264RateControlLayerInfoKHR rcLayerH264{};
                h264->GetRateControlParameters(&rcInfo, &rcLayer,
                                               &rcInfoH264, &rcLayerH264);
                outProbe->rcUseMinQp = (rcLayerH264.useMinQp == VK_TRUE) ? 1u : 0u;
                outProbe->rcUseMaxQp = (rcLayerH264.useMaxQp == VK_TRUE) ? 1u : 0u;
                outProbe->rcMinQpI   = rcLayerH264.minQp.qpI;
                outProbe->rcMaxQpI   = rcLayerH264.maxQp.qpI;

                StdVideoH264SequenceParameterSetVui vui{};
                StdVideoH264HrdParameters hrd{};
                h264->InitVuiParameters(&vui, &hrd);
                outProbe->vuiChromaLocInfoPresent =
                    vui.flags.chroma_loc_info_present_flag;
                outProbe->vuiChromaSampleLocTypeTop =
                    vui.chroma_sample_loc_type_top_field;
                outProbe->vuiChromaSampleLocTypeBottom =
                    vui.chroma_sample_loc_type_bottom_field;
            }
        } break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR: {
            EncoderConfigH265* h265 = cfg->GetEncoderConfigh265();
            if (h265 != nullptr) {
                VkVideoEncodeRateControlInfoKHR rcInfo{};
                VkVideoEncodeRateControlLayerInfoKHR rcLayer{};
                VkVideoEncodeH265RateControlInfoKHR rcInfoH265{};
                VkVideoEncodeH265RateControlLayerInfoKHR rcLayerH265{};
                h265->GetRateControlParameters(&rcInfo, &rcLayer,
                                               &rcInfoH265, &rcLayerH265);
                outProbe->rcUseMinQp = (rcLayerH265.useMinQp == VK_TRUE) ? 1u : 0u;
                outProbe->rcUseMaxQp = (rcLayerH265.useMaxQp == VK_TRUE) ? 1u : 0u;
                outProbe->rcMinQpI   = rcLayerH265.minQp.qpI;
                outProbe->rcMaxQpI   = rcLayerH265.maxQp.qpI;
                outProbe->h265CpbVclFactor = h265->GetCpbVclFactor();
                outProbe->h265LevelIdc     = (uint32_t)h265->levelIdc;
                outProbe->h265GeneralTierFlag = h265->general_tier_flag;

                StdVideoH265SequenceParameterSetVui vui{};
                StdVideoH265HrdParameters hrd{};
                StdVideoH265SubLayerHrdParameters subHrd{};
                h265->InitVuiParameters(&vui, &hrd, &subHrd);
                outProbe->vuiChromaLocInfoPresent =
                    vui.flags.chroma_loc_info_present_flag;
                outProbe->vuiChromaSampleLocTypeTop =
                    vui.chroma_sample_loc_type_top_field;
                outProbe->vuiChromaSampleLocTypeBottom =
                    vui.chroma_sample_loc_type_bottom_field;
            }
        } break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR: {
            EncoderConfigAV1* av1 = cfg->GetEncoderConfigAV1();
            if (av1 != nullptr) {
                StdVideoAV1SequenceHeader seqHdr{};
                StdVideoEncodeAV1OperatingPointInfo opInfo{};
                av1->InitSequenceHeader(&seqHdr, &opInfo);
                outProbe->av1ColorConfigPresent =
                    (seqHdr.pColorConfig != nullptr) ? 1u : 0u;
                if (seqHdr.pColorConfig != nullptr) {
                    outProbe->av1ColorDescriptionPresent =
                        seqHdr.pColorConfig->flags.color_description_present_flag;
                    outProbe->av1ColorPrimaries =
                        (uint32_t)seqHdr.pColorConfig->color_primaries;
                    outProbe->av1TransferCharacteristics =
                        (uint32_t)seqHdr.pColorConfig->transfer_characteristics;
                    outProbe->av1MatrixCoefficients =
                        (uint32_t)seqHdr.pColorConfig->matrix_coefficients;
                    outProbe->av1ColorRange =
                        seqHdr.pColorConfig->flags.color_range;
                    outProbe->av1BitDepth = seqHdr.pColorConfig->BitDepth;
                    outProbe->av1ChromaSamplePosition =
                        (uint32_t)seqHdr.pColorConfig->chroma_sample_position;
                    outProbe->av1SubsamplingX =
                        seqHdr.pColorConfig->subsampling_x;
                    outProbe->av1SubsamplingY =
                        seqHdr.pColorConfig->subsampling_y;
                }
            }
        } break;
        default:
            break;
    }
    return VK_SUCCESS;
}

// The chroma subsamplings and the maximum component bit depth a codec profile
// admits, per the codec standard.
//
// THE STANDARD'S RULE AND ONLY THE STANDARD'S. Not this device's: a device may
// refuse H.264 High 4:4:4 Predictive above 8 bits while H.264 Table A-1 admits
// up to 14, and which of the two a caller has hit is answered by a device query
// and not from here. Not this library's binding set either -- but every row is
// now reachable through it: the guard below binds 66, 77, 100, 110, 122 and 244
// for H.264, 1, 2, 3, 4 and 9 for H.265, and 0, 1 and 2 for AV1, which is every
// number this table states.
//
// TWO CALLERS, ONE TABLE, and that is why it is a table rather than a pair of
// literals at the two sites. The explicit-profile guard below refuses a named
// profile that cannot carry the declared input; VkEncQueryInputFormatSupport
// answers the same question before a session exists. Stated separately the two
// could drift, and the shape of that drift is a query that promises what
// InitializeExt then refuses -- the accepted-then-refused failure the input
// taxonomy exists to prevent.
//
// SUBSAMPLING IS STATED OVER {4:2:0, 4:2:2, 4:4:4} AND NOTHING ELSE.
// Monochrome is a chroma_format_idc that H.264 100/110/122/244 and H.265 4
// all admit, and its absence is deliberate rather than an oversight:
// EncoderConfig::input.chromaSubsampling is DERIVED from the input VkFormat
// further down this file and that derivation has no monochrome arm, so a
// monochrome bit here would be a claim nothing can put to it.
struct VkEncProfileInputLimits {
    uint32_t                         maxBpp;
    VkVideoChromaSubsamplingFlagsKHR subsamplings;
};

// False when |profile| is not a number this table states for |codec|. That is
// NOT "the standard does not define it": it means nothing about the number
// should be inferred from here, and each caller decides what the silence
// means -- the guard falls through to its own bindability refusal, and the
// point query reports that it cannot answer.
static bool VkEncGetProfileInputLimits(VkVideoCodecOperationFlagBitsKHR codec,
                                       uint32_t                        profile,
                                       VkEncProfileInputLimits&        out)
{
    const VkVideoChromaSubsamplingFlagsKHR only420 =
        VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    const VkVideoChromaSubsamplingFlagsKHR upTo422 =
        only420 | VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR;
    const VkVideoChromaSubsamplingFlagsKHR upTo444 =
        upTo422 | VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;

    switch ((uint32_t)codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            // ITU-T H.264 Annex A, Table A-1.
            switch (profile) {
                case STD_VIDEO_H264_PROFILE_IDC_BASELINE:
                case STD_VIDEO_H264_PROFILE_IDC_MAIN:
                case STD_VIDEO_H264_PROFILE_IDC_HIGH:
                    out.maxBpp = 8;  out.subsamplings = only420; return true;
                case STD_VIDEO_H264_PROFILE_IDC_HIGH_10:
                    out.maxBpp = 10; out.subsamplings = only420; return true;
                case STD_VIDEO_H264_PROFILE_IDC_HIGH_422:
                    out.maxBpp = 10; out.subsamplings = upTo422; return true;
                case STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE:
                    out.maxBpp = 14; out.subsamplings = upTo444; return true;
                default:
                    return false;
            }
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            // ITU-T H.265 Annex A.3.
            switch (profile) {
                case STD_VIDEO_H265_PROFILE_IDC_MAIN:
                case STD_VIDEO_H265_PROFILE_IDC_MAIN_STILL_PICTURE:
                    out.maxBpp = 8;  out.subsamplings = only420; return true;
                case STD_VIDEO_H265_PROFILE_IDC_MAIN_10:
                    out.maxBpp = 10; out.subsamplings = only420; return true;
                case STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS:
                case STD_VIDEO_H265_PROFILE_IDC_SCC_EXTENSIONS:
                    out.maxBpp = 16; out.subsamplings = upTo444; return true;
                default:
                    return false;
            }
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            // AV1 6.4.1 and A.2. Main is 4:2:0 at 8 or 10 bits, High is 4:4:4
            // at the same two, and Professional is the one that reaches 12.
            switch (profile) {
                case STD_VIDEO_AV1_PROFILE_MAIN:
                    out.maxBpp = 10; out.subsamplings = only420; return true;
                case STD_VIDEO_AV1_PROFILE_HIGH:
                    out.maxBpp = 10;
                    out.subsamplings = VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
                    return true;
                case STD_VIDEO_AV1_PROFILE_PROFESSIONAL:
                    out.maxBpp = 12; out.subsamplings = upTo444; return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

// The subsampling a refusal names, so a caller reads back what it declared
// rather than a flag value. The three the input derivation can produce, and a
// fallback that derivation cannot reach.
static const char* VkEncChromaSubsamplingName(
    VkVideoChromaSubsamplingFlagBitsKHR subsampling)
{
    switch ((uint32_t)subsampling) {
        case VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR: return "4:2:0";
        case VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR: return "4:2:2";
        case VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR: return "4:4:4";
        // Unreachable from the input derivation, which produces only the three
        // above -- and named rather than left as a pronoun because this
        // function has two callers now and one of them prints it beside the
        // depth, where "its" would read as a missing word rather than a
        // fallback.
        default: return "an unnamed chroma subsampling";
    }
}

// VK_SUCCESS when |profile| can carry an input at |bpp| bits and
// |subsampling|, and otherwise the refusal, having already reported WHICH
// term of the standard's rule it failed and what to do instead.
//
// CALLED ONLY FROM THE ARMS THAT CAN BIND THE NUMBER, and the ordering is the
// point. A profile this library cannot bind at all -- AV1 High (1), say --
// must be refused as unbindable, because that is the caller's actual problem;
// telling it instead that AV1 High does not admit 4:2:0 is true, and useless,
// since no input format would make the request succeed. So bindability is
// settled first and this runs inside the arms that survived it.
//
// |bpp| is taken as uint32_t deliberately: EncoderConfig::input.bpp is a
// uint8_t and streams as a CHARACTER, which silently emptied the number out
// of this diagnostic when it was written against the field's own type.
static VkResult VkEncRefuseIfProfileCannotCarryInput(
    VkVideoCodecOperationFlagBitsKHR    codec,
    uint32_t                            profile,
    uint32_t                            bpp,
    VkVideoChromaSubsamplingFlagBitsKHR subsampling)
{
    VkEncProfileInputLimits limits = {};
    if (!VkEncGetProfileInputLimits(codec, profile, limits)) {
        // The table states nothing about this number. It is not this
        // function's place to invent a constraint, and the arm that called it
        // has already decided the number is bindable.
        return VK_SUCCESS;
    }
    if (bpp > limits.maxBpp) {
        VkEncErr() << "[EncoderExt] profile " << profile
                   << " does not admit " << bpp
                   << "-bit input: the codec standard gives it at most "
                   << limits.maxBpp
                   << " bits per component. Submit frames at a depth it "
                      "admits, or use VK_VIDEO_ENCODER_PROFILE_DEFAULT, which "
                      "derives the profile from the input."
                   << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if ((limits.subsamplings & subsampling) == 0) {
        VkEncErr() << "[EncoderExt] profile " << profile
                   << " does not admit "
                   << VkEncChromaSubsamplingName(subsampling)
                   << " input: the codec standard gives it"
                   << (((limits.subsamplings &
                         VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR) != 0)
                           ? " 4:2:0" : "")
                   << (((limits.subsamplings &
                         VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR) != 0)
                           ? " 4:2:2" : "")
                   << (((limits.subsamplings &
                         VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR) != 0)
                           ? " 4:4:4" : "")
                   << " only. The subsampling is read off the input format, so "
                      "submit frames at an admitted one, or use "
                      "VK_VIDEO_ENCODER_PROFILE_DEFAULT, which derives the "
                      "profile from the input's own subsampling -- 4:2:2 "
                      "derives H.264 High 4:2:2 (122) and 4:4:4 derives H.264 "
                      "High 4:4:4 Predictive (244) or H.265 Range Extensions "
                      "(4)."
                   << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

// Free function: reads only its arguments, so a test can drive it with no
// device and no encoder -- VkEncBuildAndProbeConfig above is that entry.
// The layer-3 binder suite (VulkanVideoEncoderConfigBinderTest in Chromium's
// vulkan_video_encode_accelerator_unittest.cc) drives it per codec arm and
// asserts effect-or-explicit-rejection for every BOUND field in the section
// 4.2 field table except outputPath, which has no probe projection: it
// binds as a conditional file-open (see the disableFileOutput interplay
// below) and no test exercises it -- the Chromium consumer nulls the field
// by design.
VkResult VkEncBuildEncoderConfig(
    const VkVideoEncoderConfig& extConfig,
    VkVideoCodecOperationFlagBitsKHR codecOp,
    VkSharedBaseObj<EncoderConfig>& outConfig,
    uint32_t requestedEncodeBitDepth)
{
    // Direct binder: create the codec-typed config, assign the fields on it,
    // then run the same derived tail (FinalizeConfig) + InitializeParameters
    // the ParseArguments pipeline runs. No argv round-trip -- a field no
    // longer needs a CLI flag to exist, so nothing here can be silently
    // dropped by the parser.
    VkResult result = EncoderConfig::CreateCodecConfigDirect(codecOp, outConfig);
    if ((result != VK_SUCCESS) || !outConfig) {
        return (result != VK_SUCCESS) ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    EncoderConfig* cfg = outConfig.get();

    // THE ENCODE DEPTH, WHEN IT IS STATED RATHER THAN DERIVED. Written here,
    // before InitializeParameters, because that is where its zero-means-unset
    // guard reads it: set, the derivation from input.bpp does not run and the
    // encode side is the request. Zero leaves the derivation in charge, which
    // is every caller but the internal probe.
    if (requestedEncodeBitDepth != 0) {
        cfg->encodeBitDepthLuma = (uint8_t)requestedEncodeBitDepth;
    }

    // A2 clause 2: reject an unencodable input format at INIT, not only
    // per frame. SubmitExternalFrame already rejects it -- but only after the
    // producer has allocated an entire frame pool in a format this encoder
    // will never take. Init is the last point at which it can still choose
    // differently, which is why the finding asked for both.
    //
    // The colour-model DECLARATION is judged first, and separately, because
    // the two refusals are about different fields. VkEncResolveColorModel
    // answers VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT for a
    // (format, colorModel) pair that cannot be reconciled -- RGB declared
    // over a Y'CbCr format, Y'CbCr declared over an RGBA layout that carries
    // no packed 4:4:4 reading, or a value the enumeration does not define --
    // and there is no route to choose from an answer that means "the caller
    // stated two things that cannot both be true". This is the same
    // predicate, and the same refusal, the registration gate applies to a
    // descriptor, so one contradiction is answered once.
    //
    // Separated from the format refusal below because the FORMAT in such a
    // pair is usually the half that is not wrong: NV12 declared RGB reaches
    // here and NV12 is directly encodable, and the format message would
    // answer it by naming the submitted format among the ones it accepts.
    // The field the caller has to change is inputColorModel, and the
    // refusal says so.
    //
    // FROM_FORMAT -- what a zero-initialised config declares, and the
    // ordinary case -- reads the model off the format and passes wherever
    // the format does, as does a declaration that agrees with its format.
    if (VkEncResolveColorModel(extConfig.inputFormat,
                               extConfig.inputColorModel) ==
        VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) {
        VkEncErr() << "[EncoderExt] declared inputColorModel "
                   << (uint32_t)extConfig.inputColorModel
                   << " contradicts inputFormat "
                   << (uint32_t)extConfig.inputFormat
                   << "; the two cannot both be true and nothing here can "
                      "know which was meant. Declare the colour model the "
                      "format carries, or leave the field "
                      "VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT to read it "
                      "off the format." << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkEncInputFormatClass inputFormatClass =
        VkEncClassifyInput(extConfig.inputFormat, extConfig.inputColorModel);
    if (inputFormatClass == VK_ENC_INPUT_FORMAT_UNSUPPORTED) {
        // THE SET IS WALKED, NOT NAMED. A sentence listing the accepted
        // formats is a third statement of the routable set, beside the
        // classifier and the enumeration, and it is the one nothing tests:
        // this message named "NV12, P010, NV24 and S410 ... P012, I420 and
        // its 10/12-bit siblings" and was already narrower than the set the
        // classifier answered for. Printing the derived list cannot go stale.
        uint32_t routableCount = 0;
        const VkFormat* const routable =
            VkEncRoutableInputFormats(routableCount);
        std::ostringstream routableText;
        for (uint32_t i = 0; i < routableCount; i++) {
            routableText << ((i == 0) ? "" : ", ") << (uint32_t)routable[i];
        }
        VkEncErr() << "[EncoderExt] inputFormat "
                   << (uint32_t)extConfig.inputFormat
                   << " is not encodable. The VkFormat values this library "
                      "routes, directly or through the preprocess compute "
                      "filter, are: "
                   << routableText.str()
                   << ". Two more are reached only by DECLARING "
                      "VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR over an RGBA "
                      "enumerant -- the packed 4:4:4 layouts AYUV "
                      "(R8G8B8A8_UNORM) and Y410 (A2B10G10R10_UNORM_PACK32) "
                      "-- so they are not on that list and their absence "
                      "from it is not a refusal. Convert before submitting. "
                      "Note that Y416 (R16G16B16A16_UNORM) is not taken: 16 "
                      "bits per component is not an encode component bit "
                      "depth. Note also that the _SRGB "
                      "spellings are deliberately NOT accepted -- the filter "
                      "binds its RGBA input as a storage image, and no _SRGB "
                      "format carries VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, so "
                      "an sRGB view can never be that descriptor; submit the "
                      "_UNORM spelling of the same format instead."
                   << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // THE FILTER DECISION, AND IT IS THE LIBRARY'S. A format that is
    // encodable only THROUGH the preprocess compute filter gets the filter; a
    // directly encodable one does not. Nothing the caller sets takes part:
    // the class is a pure function of inputFormat, which is why this is
    // decided here, in a binder with no device, and is answerable on the
    // query surface before a producer allocates a frame pool.
    const bool needsPreprocessFilter =
        (inputFormatClass == VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER);

    // Refusing rather than dropping the conversion: without the filter the
    // frames that needed converting would fall to the staging copy, and for a
    // plane-count or colour-model mismatch that copy does not encode slowly,
    // it encodes wrongly -- from a three-plane source it hangs the GPU. So
    // the answer to an input that needs converting is a conversion or an
    // error, never a quiet no.
    //
    // This is the half a device-free binder CAN check: that the filter is
    // compiled into this build. The session-level half -- that the device has
    // a compute queue to run it on, which under a caller-supplied VkDevice
    // cannot be assumed -- is checked in InitializeExt, after the device
    // exists. Both halves must hold.
#ifndef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    if (needsPreprocessFilter) {
        VkEncErr() << "[EncoderExt] inputFormat "
                   << (uint32_t)extConfig.inputFormat
                   << " is encodable only through the preprocess compute "
                      "filter, which is not compiled into this build "
                      "(VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED is "
                      "undefined; the CMake option is "
                      "BUILD_ENCODER_COMPUTE_FILTER). Convert before "
                      "submitting, or submit a semi-planar 4:2:0 format."
                   << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
#endif

    // Resolution.
    cfg->input.width  = extConfig.inputWidth;
    cfg->input.height = extConfig.inputHeight;
    cfg->encodeWidth  = extConfig.encodeWidth;
    cfg->encodeHeight = extConfig.encodeHeight;

    // Input geometry: derive the bit depth, chroma subsampling AND plane count
    // from the input VkFormat.
    //
    // This is the only consumer of VkVideoEncoderConfig::inputFormat. Deriving
    // the bit depth alone and leaving chromaSubsampling and numPlanes at their
    // 4:2:0 defaults pins every caller of this library to 4:2:0 whatever format
    // it asked for: a 4:4:4 or 4:2:2 request then encodes as 4:2:0 and reports
    // success. CodecGetVkFormat covers 4:2:2 and 4:4:4 at 8, 10 and 12 bits, so
    // the encode side is already generic; only the derivation is needed here.
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(extConfig.inputFormat);
    if (mpInfo != nullptr) {
        const uint32_t bpp = GetBitsPerChannel(mpInfo->planesLayout);
        if (bpp > 8) {
            cfg->input.bpp = bpp;
        }

        // secondaryPlaneSubsampledX/Y are 1-bit flags: 0 = full rate, 1 = halved.
        //   4:2:0 -> X=1, Y=1     4:2:2 -> X=1, Y=0     4:4:4 -> X=0, Y=0
        const bool subX = (mpInfo->planesLayout.secondaryPlaneSubsampledX != 0);
        const bool subY = (mpInfo->planesLayout.secondaryPlaneSubsampledY != 0);
        cfg->input.chromaSubsampling =
            (!subX && !subY) ? VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR :
            (subX && !subY)  ? VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR :
                               VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;

        // numberOfExtraPlanes: 1 for semi-planar (2-plane), 2 for 3-plane planar.
        const uint32_t numPlanes = mpInfo->planesLayout.numberOfExtraPlanes + 1u;
        if ((numPlanes == 2) || (numPlanes == 3)) {
            cfg->input.numPlanes = numPlanes;
        }
    }
    // else: not a Y'CbCr VkFormat. Either RGB input destined for the
    // RGBA->Y'CbCr preprocess filter, or one of the packed 4:4:4 aliases (AYUV
    // on R8G8B8A8_UNORM, Y410 on A2B10G10R10_UNORM_PACK32) which have no Y'CbCr
    // VkFormat of their own. The two are indistinguishable from the VkFormat
    // alone, so the input-format taxonomy is what separates them: the two arms
    // immediately below read it. A format the taxonomy does not place keeps the
    // default geometry rather than a guess.

    // RGBA is the one input family whose format cannot be RECONSTRUCTED from
    // (subsampling, bit depth, plane count) -- CodecGetVkFormat() only spells
    // Y'CbCr. So for RGBA the format has to be carried through literally, and
    // input.colorSpace is what tells VerifyInputs() to carry rather than
    // re-derive it (and to lay the image out as one 4-byte-per-pixel plane).
    // Writing vkFormat unconditionally would be pointless for the Y'CbCr families --
    // VerifyInputs() overwrites it there by design, which is how a semi-planar
    // session still gets the format its subsampling and bit depth imply. The
    // plane count is set here for the same reason: the derivation above speaks
    // only for Y'CbCr formats, and the compute filter is built from this field.
    //
    // The pair is known resolvable by the time it reaches here -- an
    // unresolvable one was refused at the top of this function -- so this
    // reads as the two-way choice it is written as, and not as a fall to
    // Y'CbCr for an answer that meant neither.
    cfg->input.colorSpace =
        (VkEncResolveColorModel(extConfig.inputFormat,
                                extConfig.inputColorModel) ==
         VK_VIDEO_ENCODER_COLOR_MODEL_RGB)
            ? VkEncColorSpace::kRGB
            : VkEncColorSpace::kYCbCr;
    // A caller-supplied image carries its samples where its VkFormat says they
    // are, so the alignment is declared here rather than detected.
    // DetectInputMsbShift exists to sniff an input FILE's content; with no file
    // to read it returns the documented default, which claims an LSB-aligned
    // source and makes the preprocess filter scale every sample by 2^msbShift.
    // The X6/X4-packed formats this interface accepts hold their samples in the
    // high bits, and msbShift == 0 is how that is spelled.
    cfg->input.msbShift = 0;

    if (cfg->input.colorSpace == VkEncColorSpace::kRGB) {
        cfg->input.numPlanes = VkEncInputFormatPlaneCount(extConfig.inputFormat);
        cfg->input.vkFormat  = extConfig.inputFormat;
    } else if (mpInfo == nullptr) {
        // A Y'CbCr input the multi-planar table does not place is a packed
        // 4:4:4 alias declared as such -- nothing else resolves to Y'CbCr and
        // survives the class gate above. Its geometry has to be written here
        // for the same reason the RGBA arm's does: the derivation above
        // speaks only for formats that table holds, and left alone the input
        // would keep EncoderConfig's 3-plane 4:2:0 default and the session
        // would be configured as I420 while the caller declared AYUV.
        //
        // vkFormat is DELIBERATELY not written, unlike the RGBA arm.
        // VerifyInputs() reconstructs it from exactly these three values --
        // CodecGetVkFormat(4:4:4, bitDepth, PLANE_LAYOUT_PACKED_1) spells
        // AYUV at 8 bits and Y410 at 10 -- so writing it would be a second
        // statement of the same fact, and the round trip is what makes the
        // geometry below sufficient rather than merely plausible.
        const VkPackedYcbcrFormatDesc* packed =
            PackedYcbcrFormatDesc(extConfig.inputFormat);
        if (packed != nullptr) {
            cfg->input.bpp = packed->bitDepth;
            cfg->input.chromaSubsampling =
                VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
            cfg->input.numPlanes =
                VkEncInputFormatPlaneCount(extConfig.inputFormat);
        }
    }

    // ---- Tuning / rate control ----
    switch (extConfig.tuningMode) {
        case VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR:
        case VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR:
        case VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR:
        case VK_VIDEO_ENCODE_TUNING_MODE_LOSSLESS_KHR:
            cfg->tuningMode = extConfig.tuningMode;
            break;
        case VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR:
            break;  // the caller explicitly wants the codec default
        default:
            // Reject rather than silently substituting the codec default: a
            // caller that miscomputed this value would otherwise ship with
            // tuning it never selected and no diagnostic anywhere.
            VkEncErr() << "[EncoderExt] unrecognized tuningMode "
                       << (uint32_t)extConfig.tuningMode << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    // THE CONSTANT QUANTIZERS, RANGE-CHECKED BEFORE ANYTHING READS THEM.
    //
    // Checked on every session and not only a constant-QP one. The three
    // fields are the caller's statement in the codec's own units whatever
    // the rate-control mode; Reconfigure records and applies them whatever
    // the mode; and a rule that held only under DISABLED would be a second
    // contract for the same three fields, which the header would then have
    // to state twice.
    result = VkEncValidateConstQpRange(extConfig.constQpI, extConfig.constQpP,
                                       extConfig.constQpB, codecOp, "");
    if (result != VK_SUCCESS) {
        return result;
    }

    // Constant-QP (DISABLED) when requested explicitly or when lossless.
    const bool lossless   =
        (extConfig.tuningMode == VK_VIDEO_ENCODE_TUNING_MODE_LOSSLESS_KHR);
    const bool constantQp =
        (extConfig.rateControlMode ==
         VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) ||
        lossless;
    if (constantQp) {
        cfg->rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
        // THE DEFAULT IS CODEC-DEPENDENT because the UNIT is. For H.264/H.265
        // this field is a QP on 0..51 and 26 is mid-range. AV1 has no QP: the
        // value lands in base_q_idx verbatim -- VkVideoEncoderAV1::EncodeFrame
        // writes pictureInfo.constantQIndex from the resolved constQp and
        // stdQuantInfo.base_q_idx from that -- on a 0..255 quantizer-index
        // scale, where 26 is libaom quantizer 7 of 63: near-lossless, and a
        // bitrate to match.
        //
        // CITED BY FUNCTION AND SYMBOL, not by line. Line references here drift --
        // onto picOrderCntVal arithmetic and srcPictureResource setup, in the two
        // cases this paragraph would otherwise carry -- so a reader who follows one
        // finds no quantizer at all and concludes the value never reaches the
        // bitstream. A symbol survives the edits a line number does not.
        //
        // So when the caller specified NOTHING for an AV1 session, leave the
        // qindices unset rather than inventing one here.
        // EncoderConfigAV1::InitDeviceCapabilities then substitutes the
        // driver's own preferredConstantQIndex triple -- the only AV1-aware
        // default available at this layer. That substitution is guarded on
        // constQpSet, which is precisely what this arm must NOT set.
        //
        // Every other case keeps the established semantics: an explicit
        // quantizer is RESOLVED AND CARRIED including 0, lossless resolves to
        // 0, P inherits I, B inherits P, and constQpSet marks the result
        // fully resolved so the substitution above stays out.
        //
        // THAT IS A LIBRARY GUARANTEE AND IT STOPS AT THE DRIVER. What this
        // arm promises is that the quantizer the caller named is the one the
        // library resolves, records and hands down -- NOT that it is the one
        // the bitstream comes back carrying. On AV1 with driver 620.18 the
        // two part company at exactly one value: an explicit 0 reads back
        // base_q_idx 114 on KEY and 131 on INTER, which is the same pair a
        // session that named nothing at all receives. The driver is treating
        // base_q_idx 0 as unspecified and substituting its own preference; 1
        // is honoured exactly.
        //
        // 0 IS NEITHER NORMALISED NOR REFUSED HERE. It is a legal AV1
        // quantizer index -- the lossless one -- it is what the LOSSLESS
        // tuning mode resolves to a few lines below, and a consumer already
        // asserts it survives this binder unchanged. Rewriting it to suit a
        // driver that does not honour it would be the silent alteration the
        // range check above exists to remove, and would ratify the driver
        // behaviour in the library's own contract. The consequence worth
        // knowing is that AV1 lossless requested this way does not come back
        // lossless on that driver. That is a driver-side deviation, and not
        // one this layer can correct by sending a value other than the one it
        // was given.
        const bool isAv1 =
            (extConfig.codec == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR);
        const bool av1QIndexUnspecified =
            isAv1 && !lossless && (extConfig.constQpI < 0) &&
            (extConfig.constQpP < 0) && (extConfig.constQpB < 0);
        if (av1QIndexUnspecified) {
            cfg->constQp.qpIntra  = 0;
            cfg->constQp.qpInterP = 0;
            cfg->constQp.qpInterB = 0;
            cfg->constQpSet = 0;
        } else {
            const int32_t qpI = (extConfig.constQpI >= 0) ? extConfig.constQpI : (lossless ? 0 : 26);
            const int32_t qpP = (extConfig.constQpP >= 0) ? extConfig.constQpP : qpI;
            const int32_t qpB = (extConfig.constQpB >= 0) ? extConfig.constQpB : qpP;
            cfg->constQp.qpIntra  = (uint32_t)qpI;
            cfg->constQp.qpInterP = (uint32_t)qpP;
            cfg->constQp.qpInterB = (uint32_t)qpB;
            cfg->constQpSet = 1;
        }
    } else {
        if ((extConfig.rateControlMode ==
             VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR) ||
            (extConfig.rateControlMode ==
             VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR)) {
            cfg->rateControlMode = extConfig.rateControlMode;
        } else if (extConfig.rateControlMode !=
                   VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR) {
            // Anything else is a caller error. Value 3 earns its own message:
            // it was this API's VBR before the mode field was aligned onto
            // the Vulkan bit values, so a consumer still passing its old
            // constant would otherwise encode at the codec default rate
            // control and look merely mistuned rather than misconfigured.
            if (extConfig.rateControlMode == 3u) {
                VkEncErr() << "[EncoderExt] rateControlMode 3 is the "
                              "VBR value this library does not use; VBR is "
                           << (uint32_t)VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR
                           << std::endl;
            } else {
                VkEncErr() << "[EncoderExt] unrecognized rateControlMode "
                           << (uint32_t)extConfig.rateControlMode << std::endl;
            }
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (extConfig.averageBitrate > 0) {
            cfg->averageBitrate = extConfig.averageBitrate;
        }
        if (extConfig.maxBitrate > 0) {
            cfg->maxBitrate = extConfig.maxBitrate;
        }
    }
    if (extConfig.vbvBufferSize > 0) {
        // Feeds H.264/H.265 level selection and the HRD/CPB parameters.
        cfg->vbvBufferSize = extConfig.vbvBufferSize;
    }
    if ((extConfig.minQp != 0) || (extConfig.maxQp != 0)) {
        if (codecOp == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) {
            // A1 forward-or-reject: these fields are H.26x-unit QP clamps
            // (0..51). AV1 rate control is quantizer-index based (0..255)
            // and the AV1 config consumes no QP-unit clamp, so a non-zero
            // value here was accepted-and-ignored on every AV1 session.
            // Reject loudly rather than accept and ignore; qIndex clamp
            // fields are a versioned addition for when a consumer needs
            // them.
            VkEncErr() << "[EncoderExt] minQp/maxQp are H.26x-unit QP "
                          "clamps (0..51); AV1 rate control uses quantizer "
                          "indices (0..255) and this config version has no "
                          "qIndex clamp fields. Leave both 0 on AV1 "
                          "sessions." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // Syntactic H.26x range; the device's supported QP window is
        // narrower on some implementations and is checked with the
        // capabilities at device init.
        if ((extConfig.minQp < 0) || (extConfig.minQp > 51) ||
            (extConfig.maxQp < 0) || (extConfig.maxQp > 51)) {
            VkEncErr() << "[EncoderExt] minQp/maxQp outside the H.26x QP "
                          "range 0..51 (minQp=" << extConfig.minQp
                       << ", maxQp=" << extConfig.maxQp << ")" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if ((extConfig.minQp > 0) && (extConfig.maxQp > 0) &&
            (extConfig.minQp > extConfig.maxQp)) {
            VkEncErr() << "[EncoderExt] minQp " << extConfig.minQp
                       << " > maxQp " << extConfig.maxQp
                       << " -- inverted clamp window" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    // 0 keeps a field unset: no clamp reaches the driver. An explicit
    // minQp=0 collapses onto unset BY DESIGN (QP 0 is the codec floor, so
    // the two admit the same QP range); maxQp=0 (force QP 0) is not
    // expressible -- see the header note on these fields.
    if (extConfig.minQp > 0) {
        cfg->minQp    = extConfig.minQp;
        cfg->minQpSet = 1;
    }
    if (extConfig.maxQp > 0) {
        cfg->maxQp    = extConfig.maxQp;
        cfg->maxQpSet = 1;
    }

    // ---- GOP ----
    if (extConfig.gopLength > 0) {
        // Pass through at full width. SetGopFrameCount takes uint32_t and
        // m_gopFrameCount is uint32_t -- only the constructor's default
        // parameter is 8-bit, so an earlier clamp to 255 here rested on a
        // false premise and silently rewrote long GOPs: gopLength was
        // clamped while idrPeriod was not, and GetPositionInGOP's
        // `positionInInputOrder % m_gopFrameCount` then emitted a non-IDR
        // intra every 255 frames that no caller had asked for.
        cfg->gopStructure.SetGopFrameCount(extConfig.gopLength);
    }
    // Communicate the B-frame count UNCONDITIONALLY: the gopStructure
    // defaults to 2 consecutive B-frames, and omitting the assignment when
    // the caller requests 0 would silently encode B-frames against the
    // caller's intent (0 == IPPP). When B-frames ARE requested, encode
    // submissions reorder relative to input order; the producer's
    // input-release timeline stays correct because the encoder signals
    // releases at queue flush points. The producer's frame pool must be
    // deeper than one mini-GOP (B count + 1).
    if (extConfig.consecutiveBFrames ==
        VK_VIDEO_ENCODER_B_FRAMES_DRIVER_PREFERRED) {
        // Leave the library's sentinel in place: InitDeviceCapabilities then
        // adopts the driver's preferred count. This is the capability the
        // upstream argv path gets by omitting the flag; here it is asked for
        // explicitly instead of inferred from an unset field.
    } else if (extConfig.consecutiveBFrames >=
               (uint32_t)EncoderConfig::CONSECUTIVE_B_FRAME_COUNT_MAX_VALUE) {
        // 255 IS the sentinel. The previous clamp turned any larger request
        // into it, so asking for 300 B-frames silently selected
        // driver-preferred -- a different feature, chosen by accident.
        VkEncErr() << "[EncoderExt] consecutiveBFrames "
                   << extConfig.consecutiveBFrames
                   << " is out of range (0.."
                   << (uint32_t)EncoderConfig::CONSECUTIVE_B_FRAME_COUNT_MAX_VALUE - 1
                   << ", or VK_VIDEO_ENCODER_B_FRAMES_DRIVER_PREFERRED)"
                   << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    } else {
        // AV1 CAPTURE CANNOT REORDER IN THIS RELEASE.
        //
        // Reordering AV1 emits show-existing-frame headers, and the temporal
        // unit a capture consumer receives is assembled by the file writer,
        // not by the capture path. Capturing a reordered AV1 stream therefore
        // hands back temporal units missing those headers, whose frame
        // identity no consumer can reconstruct. Refuse the mode instead of
        // producing a stream that decodes to the wrong pictures.
        //
        // File output keeps B-frames, and capture keeps B=0, which is what
        // Chromium uses. The driver-preferred sentinel is not decided here --
        // it resolves in InitDeviceCapabilities -- so the definitive refusal
        // is repeated in the core once the effective count is known.
        if ((codecOp == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) &&
            (extConfig.disableFileOutput == VK_TRUE) &&
            (extConfig.consecutiveBFrames > 0)) {
            VkEncErr() << "[EncoderExt] AV1 in-memory capture does not support "
                          "B-frames in this release (consecutiveBFrames="
                       << extConfig.consecutiveBFrames
                       << "); use file output, or request 0" << std::endl;
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        // 0 binds as 0: no B-frames, which is what both this header and the
        // reference renderer's config document, and what the Chromium frame
        // tracking requires.
        cfg->gopStructure.SetConsecutiveBFrameCount(
            (uint8_t)extConfig.consecutiveBFrames);
    }
    if (extConfig.idrPeriod > 0) {
        // 0 keeps the sentinel default so InitDeviceCapabilities can apply
        // the driver's preferredIdrPeriod.
        cfg->gopStructure.SetIdrPeriod((int32_t)extConfig.idrPeriod);
    }
    if (extConfig.closedGop == VK_TRUE) {
        cfg->gopStructure.SetClosedGop();
    }

    // ---- Frame rate ----
    // Guarded so zero/unset fields keep the codec-config default (30000/1001)
    // instead of poisoning rate control, VUI timing and the AV1 timebase.
    if ((extConfig.frameRateNum > 0) && (extConfig.frameRateDen > 0)) {
        cfg->frameRateNumerator   = extConfig.frameRateNum;
        cfg->frameRateDenominator = extConfig.frameRateDen;
    }

    // ---- Transfer function ----
    //
    // This library converts the colour MODEL and implements no transfer
    // function, so the transfer function the input arrives in and the one the
    // bitstream advertises have to be the same one. A DECLARED mismatch is
    // refused here rather than converted: an unapplied transfer function
    // produces pixels that are close enough to look plausible and wrong
    // everywhere.
    //
    // 0 on inputTransferCharacteristics declares nothing and asserts nothing;
    // the input is taken to be in transferCharacteristics already.
    if ((extConfig.inputTransferCharacteristics != 0) &&
        (extConfig.inputTransferCharacteristics !=
         extConfig.transferCharacteristics)) {
        VkEncErr() << "[EncoderExt] inputTransferCharacteristics "
                   << (uint32_t)extConfig.inputTransferCharacteristics
                   << " differs from transferCharacteristics "
                   << (uint32_t)extConfig.transferCharacteristics
                   << ". This encoder applies no transfer function, so the "
                      "submitted frames and the encoded bitstream must "
                      "declare the same one. Convert before submitting, or "
                      "declare the transfer function the input carries."
                   << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // ---- Colour description (VUI) ----
    //
    // PER FIELD, NOT ALL-OR-NOTHING. An all-or-nothing gate here would read
    //
    //     if (colourPrimaries || transferCharacteristics ||
    //         matrixCoefficients || videoFullRange)
    //
    // and then copied ALL FOUR values. So a caller that supplied only
    // transferCharacteristics = 16 (PQ) -- which is exactly what an HDR
    // caller supplies, and often the only thing it knows -- also emitted
    // colour_primaries = 0 (Reserved) and matrix_coefficients = 0
    // (Identity/GBR, i.e. "these samples are RGB"). Two fabricated
    // declarations, both wrong, both on the HDR path, from one honest one.
    //
    // 0 IS "NOT SUPPLIED" ON THIS SURFACE, and that is now a stated property
    // rather than an accident of the gate. It costs the ability to REQUEST
    // code point 0: Identity/GBR primaries/matrix cannot be asked for
    // through these fields. That is deliberate and cheap here -- this
    // encoder converts to YCbCr and has no Identity path to offer -- and if
    // it ever needs to be requestable it takes a new chained struct, per the
    // versioning rules at the top of the public header.
    //
    // WHAT AN UNSUPPLIED FIELD BECOMES: 2, "Unspecified" in ISO/IEC 23091-4,
    // which H.264, H.265 and AV1 all share. It is the code point that MEANS
    // "not stated", so a partially-supplied declaration stays truthful in
    // every field instead of asserting Reserved/Identity by omission.
    //
    // video_format 5 = "unspecified" (Rec. ITU-T H.264 Table E-2), the
    // correct value when the caller communicates colorimetry only.
    const bool anyColourIdcSupplied = (extConfig.colourPrimaries != 0) ||
                                      (extConfig.transferCharacteristics != 0) ||
                                      (extConfig.matrixCoefficients != 0);
    const uint8_t kColourIdcUnspecified = 2;
    if (anyColourIdcSupplied || (extConfig.videoFullRange == VK_TRUE)) {
        // video_signal_type carries video_format and the range flag; it
        // travels alone when only the range was declared.
        cfg->video_format                   = 5;
        cfg->video_signal_type_present_flag = 1;
        cfg->video_full_range_flag = (extConfig.videoFullRange == VK_TRUE) ? 1 : 0;
    }
    if (anyColourIdcSupplied) {
        // Raise the colour description only when there IS one, and fill each
        // of its three fields from the caller or from Unspecified --
        // independently. Taking all three verbatim at 0 emits Reserved
        // primaries/transfer and matrix 0 (the samples are RGB), which is why each
        // falls back to Unspecified on its own.
        cfg->colour_primaries = (extConfig.colourPrimaries != 0)
                                    ? extConfig.colourPrimaries
                                    : kColourIdcUnspecified;
        cfg->transfer_characteristics =
            (extConfig.transferCharacteristics != 0)
                ? extConfig.transferCharacteristics
                : kColourIdcUnspecified;
        cfg->matrix_coefficients = (extConfig.matrixCoefficients != 0)
                                       ? extConfig.matrixCoefficients
                                       : kColourIdcUnspecified;
        cfg->color_description_present_flag = 1;
    }

    // ---- Quality / profile ----
    if (extConfig.qualityLevel > 0) {
        cfg->qualityLevel = extConfig.qualityLevel;
    }
    // ---- Profile ----
    //
    // The value IS the codec standard's own profile number, read against
    // |codecOp|. A validating consumer expects the exact requested profile in
    // the bitstream and rejects on a mismatch, so a number this library cannot
    // bind is refused with the reason rather than ignored: an ignored profile
    // request produces a bitstream describing something the caller did not ask
    // for.
    //
    // Which profiles admit which input BIT DEPTHS AND WHICH CHROMA
    // SUBSAMPLINGS is the standard's rule and is enforced here, both terms of
    // it. Honouring an 8-bit-only profile over deeper input would emit an
    // out-of-spec bitstream, and quietly substituting a deeper profile would
    // be the same ignored request in the other direction. The subsampling term
    // is that same argument on the other axis: H.264 High is 4:2:0 only, so
    // binding it over 4:4:4 input would declare 4:2:0 while carrying 4:4:4 --
    // and it would do so by OVERRIDING a derivation that reads the input's own
    // subsampling and would have chosen High 4:4:4 Predictive (244).
    //
    // Both terms are read off cfg->input, which the input-geometry derivation
    // above has already written from the caller's inputFormat, so this runs
    // after it and not before.
    //
    // VK_VIDEO_ENCODER_PROFILE_DEFAULT binds nothing and leaves the codec
    // config's own derivation in effect, which reads the input depth. On AV1
    // it is also seq_profile 0; see the profile constants in the public
    // header for what that overlap does and does not cost.
    if (extConfig.profile != VK_VIDEO_ENCODER_PROFILE_DEFAULT) {
        // TWO QUESTIONS, IN THIS ORDER. First, is the number one this library
        // can bind for this codec? Then, and only for the numbers that
        // survive, does the standard let that profile carry the declared
        // input? Reversing them answers the second question about a profile
        // the caller can never have, which reads as advice to change the
        // input format when no input format would help.
        const char* unbindable = nullptr;
        switch ((uint32_t)codecOp) {
            case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR: {
                EncoderConfigH264* h264 = static_cast<EncoderConfigH264*>(cfg);
                switch (extConfig.profile) {
                    case VK_VIDEO_ENCODER_PROFILE_H264_BASELINE:
                    case VK_VIDEO_ENCODER_PROFILE_H264_MAIN:
                    case VK_VIDEO_ENCODER_PROFILE_H264_HIGH:
                    case STD_VIDEO_H264_PROFILE_IDC_HIGH_10:
                    case STD_VIDEO_H264_PROFILE_IDC_HIGH_422:
                    case STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE: {
                        // ONE ARM, because the work is identical: the
                        // standard's own limits decide what each number can
                        // carry, and the table beside them states all six.
                        // BOTH terms are checked -- the depth check alone let
                        // an explicit High (100) bind over 4:4:4 input and
                        // override the derivation that would have chosen High
                        // 4:4:4 Predictive (244).
                        //
                        // 110, 122 and 244 are here because the DEFAULT
                        // derivation already selects them from the input's own
                        // depth and subsampling and this library emits those
                        // streams; refusing the same numbers when a caller
                        // names them was this library's rule and not the
                        // standard's. What the DEVICE can encode is a separate
                        // question, asked where the session is created and
                        // answerable beforehand through the input-format query.
                        const VkResult admits =
                            VkEncRefuseIfProfileCannotCarryInput(
                                codecOp, extConfig.profile, cfg->input.bpp,
                                cfg->input.chromaSubsampling);
                        if (admits != VK_SUCCESS) {
                            return admits;
                        }
                        h264->profileIdc =
                            (StdVideoH264ProfileIdc)extConfig.profile;
                    } break;
                    default:
                        unbindable = "H.264 profile_idc; this library binds "
                                     "66 (Baseline), 77 (Main), 100 (High), "
                                     "110 (High 10), 122 (High 4:2:2) and "
                                     "244 (High 4:4:4 Predictive)";
                        break;
                }
            } break;
            case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR: {
                EncoderConfigH265* h265 = static_cast<EncoderConfigH265*>(cfg);
                switch (extConfig.profile) {
                    case VK_VIDEO_ENCODER_PROFILE_H265_MAIN:
                    case VK_VIDEO_ENCODER_PROFILE_H265_MAIN10:
                    case STD_VIDEO_H265_PROFILE_IDC_MAIN_STILL_PICTURE:
                    case STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS:
                    case STD_VIDEO_H265_PROFILE_IDC_SCC_EXTENSIONS: {
                        // Main is 8-bit 4:2:0 (H.265 A.3.2), Main 10 is 8/10
                        // bit 4:2:0 (A.3.3), Main Still Picture is Main's
                        // single-picture form, and Range Extensions and SCC
                        // Extensions reach 4:2:2, 4:4:4 and sixteen bits
                        // (A.3.5, A.3.7). The table states all five and the
                        // guard reads it, so the arms are one.
                        //
                        // 4 is here because the DEFAULT derivation reaches it
                        // from 4:4:4 or 12-bit input already; naming it was
                        // refused only because this switch did not list it.
                        const VkResult admits =
                            VkEncRefuseIfProfileCannotCarryInput(
                                codecOp, extConfig.profile, cfg->input.bpp,
                                cfg->input.chromaSubsampling);
                        if (admits != VK_SUCCESS) {
                            return admits;
                        }
                        h265->profile =
                            (StdVideoH265ProfileIdc)extConfig.profile;
                    } break;
                    default:
                        unbindable = "H.265 general_profile_idc; this library "
                                     "binds 1 (Main), 2 (Main 10), 3 (Main "
                                     "Still Picture), 4 (Range Extensions) "
                                     "and 9 (SCC Extensions)";
                        break;
                }
            } break;
            case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR: {
                EncoderConfigAV1* av1 = cfg->GetEncoderConfigAV1();
                switch (extConfig.profile) {
                    // seq_profile 0 is Main AND is
                    // VK_VIDEO_ENCODER_PROFILE_DEFAULT, so it never reaches
                    // this switch: the DEFAULT test above took it, and the
                    // codec config's own derivation is in effect for it.
                    case STD_VIDEO_AV1_PROFILE_HIGH:
                    case STD_VIDEO_AV1_PROFILE_PROFESSIONAL: {
                        // High is 4:4:4 at 8 or 10 bits and Professional
                        // reaches 4:2:2 and twelve (AV1 6.4.1, A.2), which is
                        // exactly what InitProfileLevel derives from the input
                        // when nothing is named. The library emitted those
                        // seq_profiles already; only naming one was refused.
                        const VkResult admits =
                            VkEncRefuseIfProfileCannotCarryInput(
                                codecOp, extConfig.profile, cfg->input.bpp,
                                cfg->input.chromaSubsampling);
                        if (admits != VK_SUCCESS) {
                            return admits;
                        }
                        av1->profile = (StdVideoAV1Profile)extConfig.profile;
                    } break;
                    default:
                        unbindable = "AV1 seq_profile; this library binds 0 "
                                     "(Main), which is also "
                                     "VK_VIDEO_ENCODER_PROFILE_DEFAULT, 1 "
                                     "(High) and 2 (Professional)";
                        break;
                }
            } break;
            default:
                unbindable = "the selected codec";
                break;
        }
        if (unbindable != nullptr) {
            VkEncErr() << "[EncoderExt] profile " << extConfig.profile
                       << " is not a value this library can bind as "
                       << unbindable
                       << ". Use VK_VIDEO_ENCODER_PROFILE_DEFAULT to let the "
                          "library derive the profile from the input."
                       << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    // ---- Session / runtime knobs ----
    cfg->numFrames = 1000000;        // streaming mode (not UINT32_MAX -- some
                                     // code paths overflow on it)
    cfg->repeatInputFrames = true;   // external input: also gates skipping the
                                     // host-visible linear staging pool
    cfg->verbose  = (extConfig.verbose == VK_TRUE) ? 1 : 0;
    cfg->validate = (extConfig.validate == VK_TRUE) ? 1 : 0;
    cfg->disableFileOutput = (extConfig.disableFileOutput == VK_TRUE) ? 1 : 0;
    // Open the output file ONLY when file output is actually wanted. (The
    // argv path opened it during parsing even under --disableFileOutput --
    // a latent 0-byte-artifact bug, fixed in passing here.)
    if (!cfg->disableFileOutput &&
        (extConfig.outputPath != nullptr) && (extConfig.outputPath[0] != '\0')) {
        const size_t fileSize = cfg->outputFileHandler.SetFileName(extConfig.outputPath);
        if ((int64_t)fileSize <= 0) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    // The completion edge is raised on the threaded assembly path
    // (AssemblyWorkerThread -> WriteBitstreamToFile -> PushCapturedBitstream).
    // The synchronous AssembleBitstreamData path predates the edge and does
    // not publish it. asyncAssembly already defaults to true and no ext
    // config field can clear it; this pin turns that implicit dependency
    // into a stated invariant so a future default change cannot silently
    // kill every completion edge of every ext consumer.
    //
    // The pin is necessary and was never sufficient: it fixes the config,
    // and m_asyncAssemblyEnabled is RUNTIME state that any drain used to
    // clear for good. DrainPendingFrames() -> DrainAndRestartThreads() is
    // what keeps the runtime state in agreement with this line, and
    // ProcessOrderedFrames now refuses the synchronous fallback outright
    // while a completion subscriber is registered, so a third way of
    // reaching it would be an error and not another silent stall.
    cfg->asyncAssembly = 1;
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    // Written unconditionally, both ways -- and that is the point, not a
    // style preference. EncoderConfig defaults this member to TRUE
    // (VkEncoderConfig.h), so writing it only in the true case would leave a
    // directly encodable session with the filter CREATED: InitEncoder swaps
    // m_inputCommandBufferPool for the filter's compute-family pool and every
    // staged frame moves to the COMPUTE queue -- a queue and a pipeline that
    // session has no use for.
    //
    // WHICH conversion to run is a separate question, and a device-dependent
    // one: VkVideoEncoder::InitEncoder derives the filter type from the input
    // and encode-source formats. The binder has no device and decides only
    // WHETHER.
    cfg->enablePreprocessComputeFilter = needsPreprocessFilter ? 1 : 0;
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    // ---- HDR10 static metadata (pNext-chained) ----
    //
    // THE BINDER WALKS THE CHAIN, not InitializeExt, and that is the whole
    // reason this is assertable without a device: VkEncBuildAndProbeConfig
    // calls this function and nothing else, so a device-free test can drive
    // the chain end to end. InitializeExt's own walk ACCEPTS this sType and
    // consumes nothing, with a comment saying so -- the two walks must not
    // both try to own it.
    //
    // An unknown sType anywhere in the chain is still rejected, by that other
    // walk. This one only looks for the link it owns and ignores the rest,
    // because rejecting here would duplicate a gate whose complete list lives
    // there.
    for (const void* link = extConfig.pNext; link != nullptr;) {
        const auto* base =
            reinterpret_cast<const VkVideoEncoderHdrMetadataInfo*>(link);
        if (base->sType == VK_VIDEO_ENCODER_STRUCTURE_TYPE_HDR_METADATA_INFO) {
            // H.264 HAS NO SUCH SEI. There is no standard H.264
            // mastering-display or content-light message, so an H.264 session
            // could only accept this and drop it -- the accepted-and-ignored
            // class this API refuses everywhere else. Say no, with the reason.
            if (codecOp == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) {
                VkEncErr() << "[EncoderExt] HDR10 static metadata was chained "
                              "onto an H.264 session. H.264 defines no "
                              "mastering-display or content-light-level SEI, "
                              "so this metadata cannot be carried; select "
                              "H.265 or AV1, or drop the chain."
                           << std::endl;
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            EncoderHdrStaticMetadata& md = cfg->hdrMetadata;
            md.masteringDisplayPresent =
                (base->masteringDisplayPresent == VK_TRUE) ? 1u : 0u;
            md.contentLightLevelPresent =
                (base->contentLightLevelPresent == VK_TRUE) ? 1u : 0u;
            for (int c = 0; c < 3; c++) {
                md.displayPrimaryX[c] = base->displayPrimaryX[c];
                md.displayPrimaryY[c] = base->displayPrimaryY[c];
            }
            md.whitePointX = base->whitePointX;
            md.whitePointY = base->whitePointY;
            md.maxDisplayMasteringLuminance =
                base->maxDisplayMasteringLuminance;
            md.minDisplayMasteringLuminance =
                base->minDisplayMasteringLuminance;
            md.maxContentLightLevel      = base->maxContentLightLevel;
            md.maxFrameAverageLightLevel = base->maxFrameAverageLightLevel;
        } else if (base->sType ==
                   VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_COLOUR_INFO) {
            const auto* inputColour =
                reinterpret_cast<const VkVideoEncoderInputColourInfo*>(link);
            if (inputColour->reserved != 0) {
                VkEncErr() << "[EncoderExt] VkVideoEncoderInputColourInfo::"
                              "reserved must be 0."
                           << std::endl;
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            // THE CALLER DECLARES WHAT ITS BUFFER IS. Recorded here, and the
            // presence flag with it -- 0 is UNDECLARED on every axis, so no
            // value field can tell "absent" from "present and zero".
            cfg->inputColourChainPresent      = 1;
            cfg->inputColourPrimaries         = inputColour->inputColourPrimaries;
            cfg->inputTransferCharacteristics =
                inputColour->inputTransferCharacteristics;
            cfg->inputMatrixCoefficients      = inputColour->inputMatrixCoefficients;
            cfg->inputRange                   = (uint8_t)inputColour->inputRange;
        }
        link = base->pNext;
    }

    // ---- What the input declares against what the bitstream declares ----
    //
    // REFUSED ON THE AXES THIS LIBRARY CANNOT CONVERT, which is the rule
    // inputTransferCharacteristics already applies to the transfer axis
    // (above): the filter performs no primaries conversion and applies no
    // transfer function, so an input and an output that name different
    // primaries, or different matrices, describe a conversion that does not
    // happen. Accepting the pair would produce pixels that are close enough to
    // look plausible and wrong everywhere.
    //
    // ONLY WHEN BOTH SIDES ARE DECLARED, AND BOTH SIDES ARE READ FROM THE
    // CALLER. The comparison is against extConfig and not against cfg on
    // purpose: the binder above rewrites an unsupplied output field to 2
    // (Unspecified) so a partial declaration stays truthful, and comparing
    // against that substitution would read the LIBRARY's fill as a caller
    // declaration and refuse a config the caller never contradicted.
    //
    // 0 IS UNDECLARED AND 2 IS "UNSPECIFIED" -- neither asserts anything a
    // declaration on the other side can contradict, so both are skipped.
    //
    // RANGE IS NOT ON THIS LIST because its rule is not this rule. The three
    // axes here are compared on every lane and applied on none. The range is
    // APPLIED on the Y'CbCr lane -- where the library converts nothing, so
    // the input's range is the stream's -- and compared on the RGB lane,
    // where the filter produces it. Both halves are below, after the axes
    // that have one rule for both lanes.
    if (cfg->inputColourChainPresent != 0) {
        struct InputColourAxis {
            uint8_t     input;
            uint8_t     output;
            const char* name;
            const char* why;
        };
        const InputColourAxis axes[] = {
            { cfg->inputColourPrimaries, extConfig.colourPrimaries,
              "colour primaries",
              "this library performs no primaries conversion, so the input's "
              "primaries and the bitstream's are necessarily the same" },
            { cfg->inputMatrixCoefficients, extConfig.matrixCoefficients,
              "matrix coefficients",
              "the only matrix this library applies is the RGBA->Y'CbCr "
              "filter's, and it produces the matrix the bitstream declares" },
            { cfg->inputTransferCharacteristics,
              extConfig.transferCharacteristics,
              "transfer characteristics",
              "this library applies no transfer function: the code values it "
              "writes are the code values it read" },
        };
        for (const InputColourAxis& axis : axes) {
            if ((axis.input == 0) || (axis.input == 2) ||
                (axis.output == 0) || (axis.output == 2) ||
                (axis.input == axis.output)) {
                continue;
            }
            VkEncErr() << "[EncoderExt] the input declares "
                       << axis.name << " " << (uint32_t)axis.input
                       << " and the bitstream declares "
                       << (uint32_t)axis.output
                       << ". They must agree: " << axis.why
                       << ". Declare the same value on both, or leave the "
                          "input side 0, which asserts nothing."
                       << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // The chained transfer value and the flat config field are the same
        // declaration spelled twice; supplying both is allowed and they must
        // agree, because a caller that contradicts itself has not said what it
        // means.
        if ((extConfig.inputTransferCharacteristics != 0) &&
            (cfg->inputTransferCharacteristics != 0) &&
            (extConfig.inputTransferCharacteristics !=
             cfg->inputTransferCharacteristics)) {
            VkEncErr() << "[EncoderExt] inputTransferCharacteristics was "
                          "declared as "
                       << (uint32_t)extConfig.inputTransferCharacteristics
                       << " on the config and as "
                       << (uint32_t)cfg->inputTransferCharacteristics
                       << " on the chained VkVideoEncoderInputColourInfo. "
                          "They are the same declaration and must agree."
                       << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    // ---- The declared input RANGE ----
    //
    // ONE VARIABLE, TWO CONSUMERS. EncoderConfig::video_full_range_flag is
    // both the VUI bit the bitstream carries and, through
    // VkVideoEncoder::InitEncoder, the VkSamplerYcbcrRange the preprocess
    // filter is built with. They are the same decision and are deliberately
    // not two fields; what a declaration does is decide which value that one
    // variable takes on the lane where nothing else can.
    //
    // THE Y'CbCr LANE APPLIES NO RANGE MAPPING, so the declaration is
    // APPLIED. The samples that arrive are the samples that are coded: there
    // is no scaler between them, and the Y'CbCr copy filter takes its output
    // range from its input range, so a copy stays a copy whatever this flag
    // says. The input's range therefore IS the stream's range, and a caller
    // that states it is stating a fact about the bitstream. Writing it is the
    // only way that fact can survive; leaving it to videoFullRange alone
    // means a producer of full-range Y'CbCr has to know that a zero it never
    // wrote is an assertion, which is the shape of the bug this declaration
    // exists to end.
    //
    // AND IT IS SIGNALLED, not merely recorded. video_full_range_flag is
    // nested inside video_signal_type_present_flag, and an absent
    // video_signal_type is inferred as studio swing by H.264 E.2.1 and H.265
    // E.3.1 while decoders report it as unknown at their API boundary. A
    // caller that declared a range and got silence would be exactly as badly
    // served as one that declared nothing, so a declaration raises the
    // presence flag, with video_format 5 (Unspecified) beside it -- the same
    // value the colour binder above writes when a caller communicates
    // colorimetry only.
    //
    // THE RGB LANE APPLIES ONE, so the declaration is COMPARED. There the
    // filter PRODUCES the Y'CbCr range from this same flag, and the caller's
    // declaration is about its RGB buffer: two different quantities, and a
    // declaration about the first must not silently retarget the second. The
    // filter reads its RGB over the full range and performs no input
    // expansion, so an RGB input declared LIMITED describes a conversion that
    // does not happen -- refused with the reason, exactly as the primaries,
    // matrix and transfer axes are. An RGB input declared FULL agrees with
    // what the filter reads and says nothing about the output, which stays
    // the bitstream request's to state.
    //
    // ONLY THE RAISED DIRECTION OF videoFullRange CAN BE CONTRADICTED. It is
    // a VkBool32 with no undeclared state, so VK_FALSE is indistinguishable
    // from silence and reading it as a positive claim of limited range would
    // refuse every caller that simply did not fill it in. VK_TRUE against a
    // LIMITED input is a real contradiction and is refused.
    if ((cfg->inputColourChainPresent != 0) && (cfg->inputRange != 0)) {
        const bool inputIsFullRange =
            (cfg->inputRange == (uint8_t)VK_VIDEO_ENCODER_RANGE_FULL);
        if (cfg->input.colorSpace == VkEncColorSpace::kRGB) {
            if (!inputIsFullRange) {
                VkEncErr()
                    << "[EncoderExt] the input declares limited-range RGB, "
                       "which this library cannot read: the RGBA->Y'CbCr "
                       "filter samples its input over the full range and "
                       "performs no input expansion, so the conversion the "
                       "declaration describes does not happen. Supply "
                       "full-range RGB and declare it, or leave inputRange "
                       "UNDECLARED, which asserts nothing. The range the "
                       "BITSTREAM carries is stated on "
                       "VkVideoEncoderConfig::videoFullRange and is a "
                       "separate question."
                    << std::endl;
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        } else {
            if ((extConfig.videoFullRange == VK_TRUE) && !inputIsFullRange) {
                VkEncErr()
                    << "[EncoderExt] the input declares limited range and the "
                       "bitstream is requested as full range. This library "
                       "applies no range scaling to Y'CbCr input, so the two "
                       "cannot differ: the samples that arrive are the "
                       "samples that are coded. Declare the same range on "
                       "both, or leave inputRange UNDECLARED."
                    << std::endl;
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            cfg->video_format                   = 5;
            cfg->video_signal_type_present_flag = 1;
            cfg->video_full_range_flag          = inputIsFullRange ? 1 : 0;
        }
    }

    // ---- The RGBA->YCbCr matrix contract, settled WITHOUT A DEVICE ----
    //
    // An RGBA input necessarily builds the RGBA2YCBCR filter -- every encode
    // source this library can select is YCbCr, so VkEncDeriveFilterType has
    // exactly one answer for a non-YCbCr input -- which means the matrix
    // question can be settled here, before an instance or a device exists,
    // instead of only at InitEncoder. That matters twice: the caller hears
    // "no" before it allocates a frame pool, and the refusal is reachable
    // from a device-free test through VkEncBuildAndProbeConfig.
    //
    // It is the SAME function InitEncoder calls, and it is idempotent, so the
    // later call is a no-op and the two layers cannot answer differently.
    if ((cfg->input.colorSpace == VkEncColorSpace::kRGB) &&
        cfg->IsPreprocessComputeFilterEnabled()) {
        VkSamplerYcbcrModelConversion filterModel =
            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
        if (!cfg->ResolveRgbToYcbcrMatrix(&filterModel)) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        cfg->ApplyPreprocessFilterChromaSiting();
    }

    // DEBUG: enable the encoder's built-in input-vs-reconstructed PSNR so the
    // encode itself can be judged independent of the decode roundtrip.
    if (getenv("VKENC_DEBUG_PSNR")) {
        cfg->enablePsnrMetrics = 1;
    }

    // ---- Derived tail + codec parameter init (same as the parse path) ----
    if (cfg->FinalizeConfig() != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult paramsResult = outConfig->InitializeParameters();
    if (paramsResult != VK_SUCCESS) {
        return paramsResult;
    }

    // THE TWO DERIVATIONS OF THE INPUT'S IDENTITY MUST AGREE, AND THIS IS
    // WHERE THAT IS SAID.
    //
    // This function derives (chroma subsampling, bit depth, plane count) FROM
    // the caller's VkFormat, above. EncoderInputImageParameters::VerifyInputs()
    // -- which InitializeParameters just ran -- reconstructs a VkFormat FROM
    // those same three. Two derivations of one quantity in opposite directions,
    // and nothing has ever said they had to agree: whatever the second one
    // produced is what the session was configured with, and what the compute
    // filter was built from.
    //
    // THE REVERSE ONE CANNOT SIMPLY BE DELETED, which is why this is an
    // assertion. The packed-alias arm above leaves input.vkFormat unwritten ON
    // PURPOSE so the reconstruction supplies it: CodecGetVkFormat(4:4:4, depth,
    // PLANE_LAYOUT_PACKED_1) spells AYUV at eight bits and Y410 at ten, and
    // that is the only route by which either format is nameable. Removing the
    // reverse derivation removes two capabilities.
    //
    // IT COVERS BOTH LANES WITH ONE COMPARISON. On the RGBA lane VerifyInputs
    // carries the caller's format through rather than reconstructing it --
    // CodecGetVkFormat spells no RGB layout -- so the equality asserted here is
    // a carry-through there and a round trip on the Y'CbCr lanes. The
    // proposition is the same either way: the config's idea of the input format
    // is the caller's.
    //
    // A DISAGREEMENT IS A REFUSAL AND NOT A REPAIR. Overwriting one side with
    // the other would pick a winner between two derivations without knowing
    // which was wrong, and the wrong choice is a session configured for a
    // picture the caller is not sending -- which is exactly the failure that
    // is invisible until the filter is built from it.
    if (cfg->input.vkFormat != extConfig.inputFormat) {
        VkEncErr() << "[EncoderExt] internal inconsistency: inputFormat "
                   << (uint32_t)extConfig.inputFormat
                   << " was bound as (chroma subsampling "
                   << (uint32_t)cfg->input.chromaSubsampling << ", "
                   << (uint32_t)cfg->input.bpp << "-bit, "
                   << cfg->input.numPlanes
                   << "-plane), and that geometry describes format "
                   << (uint32_t)cfg->input.vkFormat
                   << " instead. The session would be configured for a picture "
                      "the caller is not sending, so it is refused rather than "
                      "reconciled." << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

// Byte-exact projection of the HDR10 payload builders -- see the declaration
// in the internal header for why it exists. A thin translation from the
// public struct to the library's POD and back out as bytes: no config, no
// device, no session.
uint32_t VkEncBuildHdrMetadataPayload(const VkVideoEncoderHdrMetadataInfo* info,
                                      VkVideoCodecOperationFlagBitsKHR codecOp,
                                      uint8_t* out, uint32_t capacity)
{
    if ((info == nullptr) || (out == nullptr)) {
        return 0;
    }
    EncoderHdrStaticMetadata md;
    md.masteringDisplayPresent =
        (info->masteringDisplayPresent == VK_TRUE) ? 1u : 0u;
    md.contentLightLevelPresent =
        (info->contentLightLevelPresent == VK_TRUE) ? 1u : 0u;
    for (int c = 0; c < 3; c++) {
        md.displayPrimaryX[c] = info->displayPrimaryX[c];
        md.displayPrimaryY[c] = info->displayPrimaryY[c];
    }
    md.whitePointX = info->whitePointX;
    md.whitePointY = info->whitePointY;
    md.maxDisplayMasteringLuminance = info->maxDisplayMasteringLuminance;
    md.minDisplayMasteringLuminance = info->minDisplayMasteringLuminance;
    md.maxContentLightLevel         = info->maxContentLightLevel;
    md.maxFrameAverageLightLevel    = info->maxFrameAverageLightLevel;

    bool truncated = false;
    size_t written = 0;
    switch ((uint32_t)codecOp) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            written = VkEncBuildH265HdrSeiNal(md, out, capacity, &truncated);
            break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            written = VkEncBuildAv1HdrMetadataObus(md, out, capacity,
                                                   &truncated);
            break;
        default:
            // H.264 carries neither payload; the binder refuses the chain.
            return 0;
    }
    return truncated ? 0u : (uint32_t)written;
}

//=============================================================================
// Initialize VulkanDeviceContext
//=============================================================================
VkResult VulkanVideoEncoderExtImpl::InitVulkanDevice(
    VkVideoCodecOperationFlagBitsKHR codecOp,
    const VkVideoEncoderConfig& config)
{
    // Imported-device contract: providing externalDevice without
    // externalPhysicalDevice is illegal -- the Vulkan API offers no
    // way to recover the physical device from a logical device
    // handle, so the library cannot pick the right physical device
    // to query format / capability properties on. Both must be
    // provided together, or both left VK_NULL_HANDLE.
    if (config.externalDevice != VK_NULL_HANDLE &&
        config.externalPhysicalDevice == VK_NULL_HANDLE) {
        VkEncErr() << "[EncoderExt] externalDevice supplied "
                  << "without externalPhysicalDevice; this "
                  << "combination is illegal. Provide both, or set "
                  << "externalDevice = VK_NULL_HANDLE to let the "
                  << "library pick a device." << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    static const char* const requiredInstanceLayers[] = {
        "VK_LAYER_KHRONOS_validation",
        nullptr
    };

    static const char* const requiredInstanceExtensions[] = {
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
        nullptr
    };

    static const char* const requiredDeviceExtension[] = {
#if defined(__linux) || defined(__linux__) || defined(linux)
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
#elif defined(_WIN32)
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
#endif
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        nullptr
    };

    // DEPENDENCY EXTENSIONS -- DO NOT DELETE AS UNUSED.
    //
    // Three of the names below are here purely to satisfy
    // VUID-vkCreateDevice-ppEnabledExtensionNames-01387: "All required device
    // extensions for each extension in the VkDeviceCreateInfo::
    // ppEnabledExtensionNames list must also be present in that list." Nothing
    // in this library calls their entry points directly, so a reader sweeping
    // for dead names will be tempted to remove them. Each is marked below with
    // the entry that requires it; removing one re-opens 01387 for its parent.
    //
    // Why this was invisible for so long: all three were core-promoted, at
    // Vulkan 1.2 / 1.2 / 1.3 respectively, and validation treats a dependency
    // as satisfied by core once the INSTANCE apiVersion reaches the promoting
    // version. Every in-tree sample creates its instance at
    // VK_HEADER_VERSION_COMPLETE, so the VUID never fires there. Chromium does
    // not: gpu/vulkan/vulkan_function_pointers.h pins
    // kVulkanRequiredApiVersion = VK_API_VERSION_1_1, and the ADOPT path
    // borrows that embedder instance -- below every promotion -- so on Chromium
    // all three fire on the encode session's vkCreateDevice.
    //
    // OPTIONAL is the correct list. AddOptDeviceExtensions() only queues the
    // name; HasAllDeviceExtensions() (common/libs/VkCodecUtils/
    // VulkanDeviceContext.cpp) promotes it to required ONLY if the physical
    // device enumerates it, and otherwise emits a warning. So a device that
    // lacks one of these degrades with a warning rather than failing device
    // creation. Caveat for the next editor: that same filter means a device
    // exposing a parent but not its dependency would drop the dependency and
    // re-open the VUID -- the pairing below is a convention, not an invariant
    // the list machinery enforces.
    static const char* const optionalDeviceExtension[] = {
#if defined(__linux) || defined(__linux__) || defined(linux)
        // Used at runtime by the dma-buf import path (VkImageResource DRM
        // format-modifier queries, FOREIGN queue-family transfers, dma-buf
        // memory import) but previously never requested -- device creation
        // relied on drivers tolerating un-enabled extensions.
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        // 01387 dependency of VK_EXT_image_drm_format_modifier, above.
        // Core in 1.2; needed explicitly on a 1.1 instance (Chromium).
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
#endif
        VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        // 01387 dependency of VK_EXT_descriptor_buffer, above.
        // Core in 1.2; needed explicitly on a 1.1 instance (Chromium).
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUANTIZATION_MAP_EXTENSION_NAME,
        // 01387 dependency of VK_KHR_video_encode_quantization_map, above.
        // Core in 1.3; needed explicitly on a 1.1 instance (Chromium).
        VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_INTRA_REFRESH_EXTENSION_NAME,
        nullptr
    };

    if (config.validate) {
        m_vkDevCtx.AddReqInstanceLayers(requiredInstanceLayers);
        m_vkDevCtx.AddReqInstanceExtensions(requiredInstanceExtensions);
    }

    m_vkDevCtx.AddReqDeviceExtensions(requiredDeviceExtension);
    m_vkDevCtx.AddOptDeviceExtensions(optionalDeviceExtension);

    // Design 3.1 rule 1, and set BEFORE the dlopen InitVulkanDevice performs,
    // because a bring-up that fails AFTER LoadVk still owns the loader handle
    // and still runs the destructor.
    //
    // InitVulkanDevice() calls LoadVk() unconditionally at its top -- above
    // the imported-instance branch -- so this session dlopen()s libvulkan in
    // every mode exactly as it does when it creates its own instance. Without
    // the retain, ~VulkanDeviceContext dlclose()s it once per session.
    // Measured, not argued: a probe reading /proc/self/maps around one
    // create/destroy reports libvulkan.so.1 mapped 0 -> 1 -> 0. Whenever this
    // session holds the last reference the shared object is UNMAPPED, and any
    // embedder function-pointer table bound to it -- Chromium's
    // gpu::VulkanFunctionPointers are bound to exactly this library -- is left
    // holding pointers into an unmapped object.
    //
    // That it has not crashed yet is ordering, not contract: something else
    // usually holds a reference first (the capability enumerator building its
    // process-floor OWN context, or the embedder initialising its own loader).
    // VulkanVideoEncoderContext::Build has retained since it existed; this was
    // the one VulkanDeviceContext in an embedded process that did not, which
    // is the case VulkanDeviceContext.h's RetainLoaderHandle rule names.
    m_vkDevCtx.RetainLoaderHandle();

    // Where the borrowed instance and physical device come from. A session
    // created on a context takes both from the context; one created by the
    // plain factory takes them from the config, as it always did.
    //
    // The two are the SAME borrowing expressed twice, so a session that has a
    // context and a config that also names handles is refused rather than
    // resolved. Picking a winner is what would hurt: a stale config field
    // outranking the context would send this session to a different physical
    // device than the one whose capability snapshot the caller read, and
    // nothing downstream would report the substitution.
    VkInstance       borrowedInstance   = config.externalInstance;
    VkPhysicalDevice borrowedPhysDevice = config.externalPhysicalDevice;
    if (m_context) {
        if ((config.externalInstance != VK_NULL_HANDLE) ||
            (config.externalPhysicalDevice != VK_NULL_HANDLE)) {
            VkEncErr() << "[EncoderExt] config.externalInstance / "
                          "externalPhysicalDevice set on a session created "
                          "with CreateVulkanVideoEncoderExtOnContext. The "
                          "context already supplies both; remove them from "
                          "the config." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (config.externalDevice != VK_NULL_HANDLE) {
            // A context creates no VkDevice and a session built on one
            // creates its own; a caller-supplied device has nowhere to land.
            VkEncErr() << "[EncoderExt] config.externalDevice set on a "
                          "session created with "
                          "CreateVulkanVideoEncoderExtOnContext. A context "
                          "creates no VkDevice, so a caller-supplied one is "
                          "not accepted on this path." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // deviceId and gpuUUID are device SELECTORS, and the context already
        // selected. Left un-refused they still reach InitPhysicalDevice as
        // filters over the single-entry candidate list: a mismatch fails with
        // a bare "InitPhysicalDevice failed: <code>" the caller cannot tell
        // from a hardware problem, and a match is honoured redundantly. Refuse
        // both, for the reason CreateVulkanVideoEncoderContext gives when it
        // refuses a non-zero gpuUUID in ADOPT -- the device is already chosen,
        // so honouring one of the two would be a guess.
        static const uint8_t kZeroUUID[VK_UUID_SIZE] = {};
        if (config.deviceId != -1) {
            VkEncErr() << "[EncoderExt] config.deviceId (" << config.deviceId
                       << ") set on a session created with "
                          "CreateVulkanVideoEncoderExtOnContext. The context's "
                          "deviceIndex already selects the device; leave "
                          "deviceId at -1." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (memcmp(config.gpuUUID, kZeroUUID, VK_UUID_SIZE) != 0) {
            VkEncErr() << "[EncoderExt] config.gpuUUID set on a session "
                          "created with CreateVulkanVideoEncoderExtOnContext. "
                          "The context's deviceIndex already selects the "
                          "device." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        borrowedInstance   = m_contextInstance;
        borrowedPhysDevice = m_contextPhysDevice;
    }

    VkResult result = m_vkDevCtx.InitVulkanDevice("VulkanVideoEncoderExt",
                                                    borrowedInstance,
                                                    config.verbose);
    if (result != VK_SUCCESS) {
        VkEncErr() << "[EncoderExt] InitVulkanDevice failed: " << result << std::endl;
        return result;
    }

    result = m_vkDevCtx.InitDebugReport(config.validate, config.verbose && config.validate);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkQueueFlags requestVideoEncodeQueueMask = VK_QUEUE_VIDEO_ENCODE_BIT_KHR;

    // Always request compute queue — the encoder's VkVideoEncoder::CreateVideoEncoder
    // internally creates a VulkanFilter for input format conversion which asserts
    // m_queue != VK_NULL_HANDLE. This matches the sample app's pattern when
    // selectVideoWithComputeQueue or enablePreprocessComputeFilter is set.
    VkQueueFlags requestVideoComputeQueueMask = VK_QUEUE_COMPUTE_BIT;

    // Use UUID if provided, otherwise auto-select.
    // DeviceUuidUtils default ctor sets m_deviceUuidIsValid=0 → no UUID filtering.
    // MUST NOT pass nullptr — DeviceUuidUtils(const uint8_t*) does memcpy from pointer!
    vk::DeviceUuidUtils deviceUuid;
    uint8_t zeroUUID[VK_UUID_SIZE] = {};
    if (memcmp(config.gpuUUID, zeroUUID, VK_UUID_SIZE) != 0) {
        deviceUuid = vk::DeviceUuidUtils(config.gpuUUID);
    }

    result = m_vkDevCtx.InitPhysicalDevice(
        config.deviceId, deviceUuid,
        (requestVideoComputeQueueMask | requestVideoEncodeQueueMask | VK_QUEUE_TRANSFER_BIT),
        nullptr, 0,
        VK_VIDEO_CODEC_OPERATION_NONE_KHR,
        requestVideoEncodeQueueMask,
        codecOp,
        borrowedPhysDevice);  // borrowed physical device: context or config
    if (result != VK_SUCCESS) {
        VkEncErr() << "[EncoderExt] InitPhysicalDevice failed: " << result << std::endl;
        return result;
    }

    // The per-codec encode extension is requested as OPTIONAL, so a device that
    // lacks it, should't select it.
    {
        const char* requiredExt = nullptr;
        switch (codecOp) {
            case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
                requiredExt = VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME; break;
            case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
                requiredExt = VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME; break;
            case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
                requiredExt = VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME; break;
            default: break;
        }
        if (requiredExt != nullptr) {
            uint32_t extCount = 0;
            m_vkDevCtx.EnumerateDeviceExtensionProperties(m_vkDevCtx.getPhysicalDevice(),
                                                          nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> exts(extCount);
            if (extCount) {
                m_vkDevCtx.EnumerateDeviceExtensionProperties(m_vkDevCtx.getPhysicalDevice(),
                                                              nullptr, &extCount, exts.data());
            }
            bool found = false;
            for (const auto& e : exts) {
                if (strcmp(e.extensionName, requiredExt) == 0) { found = true; break; }
            }
            if (!found) {
                VkEncErr() << "[EncoderExt] ERROR: this device does not support "
                          << requiredExt << "; cannot encode the requested codec"
                          << std::endl;
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
        }
    }

    // For an imported VkDevice, bind the queue families the CALLER
    // created queues for instead of the probe's picks. The probe above only
    // inspects VkPhysicalDevice properties; it cannot know which families the
    // caller's vkCreateDevice actually requested queues on, and
    // vkGetDeviceQueue on a family the device was not created with is
    // undefined behavior. UINT32_MAX fields keep the probed family
    // (default behavior / library-owned-device path). Must run BEFORE the
    // needTransferQueue derivation below -- the override refreshes
    // GetVideoEncodeQueueFlag() to the real flags of the caller's encode
    // family. NOTE: the caller provides no transfer-family index; if its
    // encode family lacks TRANSFER, the probed transfer family is still used
    // (known limitation -- NVIDIA encode families advertise TRANSFER,
    // so an imported-device path never takes that branch).
    if (config.externalDevice != VK_NULL_HANDLE) {
        result = m_vkDevCtx.OverrideImportedQueueFamilies(
            config.externalEncodeQueueFamilyIndex,
            codecOp,
            config.externalComputeQueueFamilyIndex);
        if (result != VK_SUCCESS) {
            VkEncErr() << "[EncoderExt] OverrideImportedQueueFamilies "
                       << "failed: " << result << std::endl;
            return result;
        }
    }

    bool needTransferQueue = ((m_vkDevCtx.GetVideoEncodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) == 0);
    // Always request compute queue — VkVideoEncoder internally creates
    // VulkanFilter for input format conversion, which asserts m_queue != NULL.
    bool needComputeQueue = true;

    result = m_vkDevCtx.CreateVulkanDevice(
        0,            // numDecodeQueues
        1,            // numEncodeQueues
        codecOp,
        needTransferQueue,
        false,        // createGraphicsQueue
        false,        // createDisplayQueue
        needComputeQueue,
        config.externalDevice);  // caller-supplied logical device
    if (result != VK_SUCCESS) {
        VkEncErr() << "[EncoderExt] CreateVulkanDevice failed: " << result << std::endl;
        return result;
    }

    return VK_SUCCESS;
}

//=============================================================================
// VulkanVideoEncoder base interface (file-based, for backward compatibility)
//=============================================================================

VkResult VulkanVideoEncoderExtImpl::Initialize(
    VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
    int argc, const char** argv)
{
    // Delegate to the file-based path
    VkResult result = EncoderConfig::CreateCodecConfig(argc, argv, m_encoderConfig);
    if (result != VK_SUCCESS) return result;

    result = InitVulkanDevice(videoCodecOperation,
        VkVideoEncoderConfig{
            .codec = videoCodecOperation,
            .encodeWidth = m_encoderConfig->encodeWidth,
            .encodeHeight = m_encoderConfig->encodeHeight,
            .verbose = m_encoderConfig->verbose ? VK_TRUE : VK_FALSE,
            .validate = m_encoderConfig->validate ? VK_TRUE : VK_FALSE,
        });
    if (result != VK_SUCCESS) return result;

    result = VkVideoEncoder::CreateVideoEncoder(&m_vkDevCtx, m_encoderConfig, m_encoder);
    if (result != VK_SUCCESS) return result;

    SnapshotCaps();
    m_initialized = true;
    // Currency 3 is created up front, on the session-serial thread, so the
    // submit path can read the handle without a lock. Creation failure
    // degrades the currency (GetCompletionSemaphore stays VK_NULL_HANDLE);
    // it does not fail the session: no consumer requires the semaphore.
    if (m_encoder->CreateCompletionTimelineSemaphore() != VK_SUCCESS) {
        VkEncErr() << "[EncoderExt] completion timeline semaphore creation "
                      "failed; the GPU-ordering currency is unavailable "
                      "this session" << std::endl;
    }
    return VK_SUCCESS;
}

VkResult VulkanVideoEncoderExtImpl::EncodeNextFrame(int64_t& frameNumEncoded)
{
    if (!m_initialized || !m_encoder) return VK_ERROR_NOT_PERMITTED_KHR;

    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> encodeFrameInfo;
    m_encoder->GetAvailablePoolNode(encodeFrameInfo);
    if (!encodeFrameInfo) return VK_ERROR_OUT_OF_POOL_MEMORY;

    VkResult result = m_encoder->LoadNextFrame(encodeFrameInfo);
    if (result != VK_SUCCESS) return result;

    frameNumEncoded = encodeFrameInfo->frameInputOrderNum;
    return VK_SUCCESS;
}

//=============================================================================
// Layout pins for the public structs. (VkVideoEncoderConfig has its own pin
// beside the config binder, where the number carries a second meaning:
// binder completeness.)
//
// WHAT A PIN IS. Each number is the size this build computes for one public
// struct, written down. An in-place field append changes the layout under an
// UNCHANGED sType, so a consumer built against the older header still passes
// the structure-type gate and is then read with a shifted layout, silently --
// no diagnostic at either end. The pin turns that into a build break in this
// file.
//
// THE RULE THE PINS ENFORCE. The only legal way to extend a struct in this
// API is a new pNext-chained struct with a new sType. Appending a field to an
// existing struct is an ABI break and requires a new sType. sType values are
// numbered from a private base and are never reused or renumbered, and the
// library refuses an sType it does not know rather than guessing -- which is
// what turns version skew into an error at the boundary instead of silent
// misbehaviour.
//
// WHAT TRIPPING ONE MEANS. A pin trips only once a public struct has already
// changed shape, so the assert is not a step in a procedure to be worked
// through: it is where the change stops and its author has to decide what
// this API does about an ABI break. What defeats the pin is answering it
// without that decision -- relaxing the assert, or leaving a number stale. A
// size nobody had to think about records nothing.
//=============================================================================
// PINNED ON 64-BIT ONLY (LP64/LLP64). Every pin is a byte size or a byte
// offset, and both move with pointer width: a 32-bit build puts pNext at 4
// rather than 8 and shortens every struct carrying a pointer or a handle.
// Re-deriving a second set of numbers for -m32 would double the maintenance
// and buy nothing -- a field appended in place changes the 64-bit layout too,
// so the gate catches it either way, and the embedding target is 64-bit.
//
// THE CONDITION IS ON EACH MACRO, never on a region of this file. A region
// has to be re-closed every time a pin is added past its end, and a pin
// appended after that close is silently unguarded -- which is exactly the
// defect this arrangement removes: a macro that expands to nothing cannot
// drift, wherever a later pin lands.
#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
#define VK_ENC_PIN_LAYOUT(T, N)                                             \
    static_assert(sizeof(T) == (N),                                         \
                  #T " changed size -- extend it via a pNext-chained "      \
                     "struct with a new sType, never an in-place append")
#else
#define VK_ENC_PIN_LAYOUT(T, N)
#endif
VK_ENC_PIN_LAYOUT(VkVideoEncoderFrameDeadlineInfo, 24);
VK_ENC_PIN_LAYOUT(VkVideoEncoderValidationInfo, 24);
VK_ENC_PIN_LAYOUT(VkVideoEncoderHdrMetadataInfo, 56);
VK_ENC_PIN_LAYOUT(VkVideoEncoderInputColourInfo, 24);
VK_ENC_PIN_LAYOUT(VkVideoEncoderFrameFenceDescriptor, 32);
VK_ENC_PIN_LAYOUT(VkVideoEncoderPlaneLayout, 40);
VK_ENC_PIN_LAYOUT(VkVideoEncoderExternalImageDescriptor, 336);
VK_ENC_PIN_LAYOUT(VkVideoEncoderFrameSubmitInfo, 104);
VK_ENC_PIN_LAYOUT(VkVideoEncoderCompletionInfo, 64);
VK_ENC_PIN_LAYOUT(VkVideoEncoderDiagnosticInfo, 216);
VK_ENC_PIN_LAYOUT(VkVideoEncoderFilterInfo, 40);
VK_ENC_PIN_LAYOUT(VkVideoEncoderInputResidencyInfo, 32);
VK_ENC_PIN_LAYOUT(VkVideoEncoderStagedSubmitInfo, 24);
VK_ENC_PIN_LAYOUT(VkVideoEncodeInputFrame, 128);
VK_ENC_PIN_LAYOUT(VkVideoEncodeResult, 72);
VK_ENC_PIN_LAYOUT(VkVideoEncoderRuntimeInfo, 120);
VK_ENC_PIN_LAYOUT(VkVideoEncoderSemaphoreDescriptor, 32);
VK_ENC_PIN_LAYOUT(VkVideoEncoderFrameSyncDescriptor, 64);
VK_ENC_PIN_LAYOUT(VkVideoEncoderImageSupport, 24);
VK_ENC_PIN_LAYOUT(VkVideoEncoderImageSupportDetails, 280);
VK_ENC_PIN_LAYOUT(VkVideoEncoderStatus, 24);
VK_ENC_PIN_LAYOUT(VkVideoEncoderImportGuardInfo, 40);
VK_ENC_PIN_LAYOUT(VkVideoEncoderImportContentInfo, 56);
VK_ENC_PIN_LAYOUT(VkVideoEncoderCapabilities, 88);
VK_ENC_PIN_LAYOUT(VkVideoEncoderInputFormatProperties, 12);
VK_ENC_PIN_LAYOUT(VkVideoEncoderContextCreateInfo, 64);
VK_ENC_PIN_LAYOUT(VkVideoEncoderDeviceIdentity, 312);
#undef VK_ENC_PIN_LAYOUT

// The sizeof pins cannot catch a reorder that keeps the size constant --
// swapping pNext with a payload field, say -- yet every pNext walk in this
// file reads a link's {sType, pNext} through a SIBLING struct type before
// re-casting ("Every public struct opens with {sType, pNext}"). That walk
// is sound only while the prefix sits at the same offsets in every
// chainable struct, so pin the prefix too: a reorder becomes a build break
// here instead of a payload value dereferenced as a pointer. The 8 is as
// 64-bit-specific as the sizeof numbers above. VkVideoEncoderPlaneLayout and
// VkVideoEncoderInputFormatProperties are deliberately absent: both are
// pointer-free PODs with no {sType, pNext} prefix, and neither rides a chain.
// Those two are the only absences -- every other struct the sizeof pins above
// cover is pinned here, which is what makes this list readable as a set.
#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
#define VK_ENC_PIN_CHAIN_PREFIX(T)                                          \
    static_assert((offsetof(T, sType) == 0) && (offsetof(T, pNext) == 8),   \
                  #T " moved its {sType, pNext} prefix -- every pNext walk" \
                     " reads links through a sibling struct type and"       \
                     " depends on this layout")
#else
#define VK_ENC_PIN_CHAIN_PREFIX(T)
#endif
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderConfig);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderFrameDeadlineInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderValidationInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderHdrMetadataInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderInputColourInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderFrameFenceDescriptor);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderExternalImageDescriptor);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderFrameSubmitInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderCompletionInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderDiagnosticInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderFilterInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderInputResidencyInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderStagedSubmitInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncodeInputFrame);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncodeResult);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderRuntimeInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderSemaphoreDescriptor);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderFrameSyncDescriptor);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderImageSupport);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderImageSupportDetails);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderStatus);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderImportGuardInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderImportContentInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderCapabilities);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderContextCreateInfo);
VK_ENC_PIN_CHAIN_PREFIX(VkVideoEncoderDeviceIdentity);
#undef VK_ENC_PIN_CHAIN_PREFIX

// Neither pin above can see the INTERIOR of a struct. A member removed from
// a run of members that ends where a coarser alignment begins -- the next
// member's, or the struct's own round-up at the end -- need not shrink the
// struct at all: the compiler reclaims exactly the bytes that went as
// padding at the close of the run, so sizeof is unchanged, the prefix is
// unchanged, and every member between the removal and that boundary shifts
// down under an UNCHANGED sType. It holds at every width. A byte-wide
// member in a run closing at a 4-aligned one, a 4-byte member in a run
// closing at an 8-aligned one, and a trailing bool in a run that reaches
// only the struct's own tail padding are the same event. A consumer that
// never named the removed field gets no compile error, and there is no
// diagnostic at either end -- the same silence the sizeof pins exist to
// break, one level further in.
//
// Worse than a lost field, where the survivors are the same width as the
// member that went: the consumer does not read a field that is missing, it
// reads a DIFFERENT field's value under the name it asked for, and reports it
// as the answer. A run of same-width members closing at a padding boundary
// is where that happens.
//
// So, in each public struct that HAS such a run, pin the member that CLOSES
// it -- the one immediately before the next member of coarser alignment, and
// the last member of the struct. That member is the one every removal within
// the run displaces, whichever member the removal takes and whether or not
// the run carries a hole today; removing the pinned member itself instead
// deletes a name this file asserts on. One number per run therefore covers
// every REMOVAL inside the run: it either moves that number or fails to
// compile here.
//
// IT DOES NOT COVER EVERY REORDER, which is worth stating because a reorder
// produces the same misread this whole block exists to stop. Transposing two
// members of EQUAL WIDTH changes no size, no chain prefix and no offset but
// those two, so a closing-member pin sees it only when the pinned member is
// one of the pair. In a run of two members it always is. In a longer run of
// equal-width members it need not be, and the two public structs that are
// one such run end to end -- VkVideoEncoderPlaneLayout and
// VkVideoEncoderInputFormatProperties -- are pinned member by member at the
// end of this block for that reason.
//
// INSERTION IS THE SAME HAZARD AND IS WHY THE COVERAGE BELOW IS EXHAUSTIVE.
// A run that ends in padding swallows a field small enough to fit there: the
// size pin above does not move, the members past the padding do not move, and
// the members of the run between the insertion and that padding all slide.
// VkVideoEncoderConfig carries such a run in the shipped layout -- its
// byte-wide colour description ends one byte short of the 4-aligned member
// behind it -- and a consumer built against the older header would go on
// writing a field some bytes from where the library reads it, with nothing
// here objecting. Every run that can absorb a field therefore carries the pin
// that closes it, derived from measured offsets rather than from reading the
// declarations.
//
// THESE PINS ARE THE ONLY LAYOUT ENFORCEMENT THIS API HAS.
// VK_VIDEO_ENCODER_EXT_API_VERSION is 1 and stays 1 -- it is not bumped for
// layout or vtable changes -- so there is no version a consumer can compare
// to discover that a struct moved underneath it. Nothing else in the build
// notices. A pin removed as redundant is enforcement deleted, not tidied.
//
// THE ONE CASE THESE DO NOT COVER, stated so it is not mistaken for one they
// do: a field dropped INTO a padding hole, displacing nothing. The four bytes
// between sType and the 8-aligned pNext are such a hole in every chainable
// struct here, and so are the bytes after the closing member of a trailing
// run. A field placed there moves no member and changes no size, so nothing
// fires -- not these pins, not the size pin, not the {sType, pNext} prefix
// pin. That was measured rather than assumed. Nothing existing is MISREAD in
// that case: every offset a consumer already knows is still correct. What it
// costs is that the library reads bytes an older consumer never wrote, which
// is the field table's business rather than this one's.
//
// Deliberately not every member, and deliberately not every struct. A struct
// whose every removal changes its size is already answered by the size pin
// above and takes nothing here -- a pin that cannot fail independently of
// the two above records nothing. What this one is for is that a change
// landing in padding trips something, not that every field is frozen.
//
// VkVideoEncoderExternalImageDescriptor carries the most of them because it
// is the one pointer-free POD in this API that IS the IPC payload: a
// producer in another process fills it by field copy, so its member offsets
// are the wire format rather than a property of this build. It is also long
// enough that a change to one end of it is read nowhere near the other.
#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
#define VK_ENC_PIN_MEMBER(T, m, N)                                          \
    static_assert(offsetof(T, m) == (N),                                    \
                  #T "::" #m " moved -- a member removed or reordered into" \
                     " padding leaves sizeof unchanged and shifts the rest" \
                     " of its run under an unchanged sType")
#else
#define VK_ENC_PIN_MEMBER(T, m, N)
#endif
VK_ENC_PIN_MEMBER(VkVideoEncoderExternalImageDescriptor,
                  hasDrmFormatModifier, 64);
VK_ENC_PIN_MEMBER(VkVideoEncoderExternalImageDescriptor,
                  planeCount, 80);
VK_ENC_PIN_MEMBER(VkVideoEncoderExternalImageDescriptor,
                  residency, 312);
VK_ENC_PIN_MEMBER(VkVideoEncoderExternalImageDescriptor,
                  colorModel, 332);
// VkVideoEncoderCapabilities has two such runs: the level, DPB and quality
// scalars closing at the 8-aligned maxBitrate, and the four trailing
// availability flags, which live entirely in the struct's tail padding.
VK_ENC_PIN_MEMBER(VkVideoEncoderCapabilities,
                  maxQualityLevels, 36);
VK_ENC_PIN_MEMBER(VkVideoEncoderCapabilities,
                  supportsResizeWithoutIdr, 83);
// VkVideoEncoderImageSupport is the smallest case of the same shape: its two
// payload members are one run, and both of them sit in the round-up to the
// alignment the pNext pointer imposes.
VK_ENC_PIN_MEMBER(VkVideoEncoderImageSupport,
                  status, 20);
// VkVideoEncoderStagedSubmitInfo has that shape too: the submitted queue
// flag and the family index it resolves to are one run, sitting entirely in
// the round-up the pNext pointer imposes, so dropping either leaves the size
// at 24 and slides the survivor onto the other one's offset.
VK_ENC_PIN_MEMBER(VkVideoEncoderStagedSubmitInfo,
                  queueFamilyIndex, 20);
// The narrow-integer runs of the HDR10 metadata: the two white-point
// chromaticity coordinates, and the two content light levels that close the
// struct.
VK_ENC_PIN_MEMBER(VkVideoEncoderHdrMetadataInfo,
                  whitePointY, 34);
VK_ENC_PIN_MEMBER(VkVideoEncoderHdrMetadataInfo,
                  maxFrameAverageLightLevel, 50);
// VkVideoEncoderConfig has three: the byte-wide colour description, the
// diagnostic switches closing at the 8-aligned external-handle block, and
// the two external queue family indices that close the struct. Its size pin
// lives beside the config binder, where it also serves as the
// binder-completeness check, and cannot see any of these.
VK_ENC_PIN_MEMBER(VkVideoEncoderConfig,
                  matrixCoefficients, 118);
VK_ENC_PIN_MEMBER(VkVideoEncoderConfig,
                  silenceStdio, 172);
VK_ENC_PIN_MEMBER(VkVideoEncoderConfig,
                  externalComputeQueueFamilyIndex, 204);
// The per-frame scalars between the presentation timestamp and the wait
// semaphore array.
VK_ENC_PIN_MEMBER(VkVideoEncodeInputFrame,
                  waitSemaphoreCount, 84);
// The two PCI identifiers that close the device identity, after the fixed
// UUID and name arrays.
VK_ENC_PIN_MEMBER(VkVideoEncoderDeviceIdentity,
                  deviceID, 308);
// VkVideoEncoderValidationInfo: the flag word closes the struct on its own,
// in the round-up behind the single pNext pointer.
VK_ENC_PIN_MEMBER(VkVideoEncoderValidationInfo,
                  flags, 16);
// VkVideoEncoderFrameFenceDescriptor: the acquire fd closes the 4-byte run
// before the 8-aligned release pointer.
VK_ENC_PIN_MEMBER(VkVideoEncoderFrameFenceDescriptor,
                  acquireFenceFd, 16);
// VkVideoEncoderFrameSubmitInfo: two counts, each closing a run before an
// 8-aligned array pointer.
VK_ENC_PIN_MEMBER(VkVideoEncoderFrameSubmitInfo,
                  waitSemaphoreCount, 56);
VK_ENC_PIN_MEMBER(VkVideoEncoderFrameSubmitInfo,
                  signalSemaphoreCount, 80);
// VkVideoEncoderCompletionInfo: the acquired-frame count closes the struct
VK_ENC_PIN_MEMBER(VkVideoEncoderCompletionInfo,
                  framesAcquired, 56);
// VkVideoEncodeInputFrame: the declared layout closes the run before the
// 8-aligned handle block; the
// signal count closes the run before its array pointer
VK_ENC_PIN_MEMBER(VkVideoEncodeInputFrame,
                  currentLayout, 40);
VK_ENC_PIN_MEMBER(VkVideoEncodeInputFrame,
                  signalSemaphoreCount, 104);
// VkVideoEncodeResult: the status closes the struct
VK_ENC_PIN_MEMBER(VkVideoEncodeResult,
                  status, 64);
// VkVideoEncoderRuntimeInfo: the trailing simulcast flag closes the struct
VK_ENC_PIN_MEMBER(VkVideoEncoderRuntimeInfo,
                  applyAlignmentToAllSimulcastLayers, 112);
// VkVideoEncoderSemaphoreDescriptor: the ownership mode closes the struct
VK_ENC_PIN_MEMBER(VkVideoEncoderSemaphoreDescriptor,
                  ownership, 24);
// VkVideoEncoderFrameSyncDescriptor: two counts, each closing a run before
// an 8-aligned array pointer.
VK_ENC_PIN_MEMBER(VkVideoEncoderFrameSyncDescriptor,
                  waitCount, 16);
VK_ENC_PIN_MEMBER(VkVideoEncoderFrameSyncDescriptor,
                  signalCount, 40);
// VkVideoEncoderImageSupportDetails: the modifier count closes the run
// before the 8-aligned modifier array.
VK_ENC_PIN_MEMBER(VkVideoEncoderImageSupportDetails,
                  directModifierCount, 16);
// VkVideoEncoderStatus: the consumed-handle flag closes the struct
VK_ENC_PIN_MEMBER(VkVideoEncoderStatus,
                  handlesConsumed, 16);
// VkVideoEncoderImportGuardInfo: the errno closes the struct
VK_ENC_PIN_MEMBER(VkVideoEncoderImportGuardInfo,
                  failureErrno, 32);
// VkVideoEncoderContextCreateInfo: the mode closes the run before the
// 8-aligned Vulkan handles; silenceStdio
// closes the struct
VK_ENC_PIN_MEMBER(VkVideoEncoderContextCreateInfo,
                  mode, 16);
VK_ENC_PIN_MEMBER(VkVideoEncoderContextCreateInfo,
                  silenceStdio, 56);
// VkVideoEncoderConfig: the GPU UUID closes the run that carries the colour
// description, the
// input transfer function and the device selector -- the run a 4-byte
// insertion slides whole
VK_ENC_PIN_MEMBER(VkVideoEncoderConfig,
                  gpuUUID, 132);
#undef VK_ENC_PIN_MEMBER

// THE TWO STRUCTS A CLOSING-MEMBER PIN CANNOT SPEAK FOR, pinned member by
// member instead. VkVideoEncoderPlaneLayout is five uint64_t and
// VkVideoEncoderInputFormatProperties opens with two VkFormat, so each is one
// run of equal-width members end to end: transposing a pair inside either
// leaves the size, every other member's offset and every other assertion in
// this file untouched, and the consumer reads one member's value under the
// other's name. Neither carries {sType, pNext} and neither rides a chain, so
// the prefix pins have nothing to say about them either.
//
// The FIRST member of each is deliberately left unpinned, so no assertion
// here is offsetof(T, m) == 0. Nothing is lost: a transposition moves two
// members, at most one of which can be the first, so the other one's offset
// has to move and is pinned below.
#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
#define VK_ENC_PIN_ORDER(T, m, N)                                           \
    static_assert(offsetof(T, m) == (N),                                    \
                  #T "::" #m " moved -- every member of this structure is"  \
                     " the same width, so a transposition changes no size"  \
                     " and no other offset, and is read as the wrong"       \
                     " field's value")
#else
#define VK_ENC_PIN_ORDER(T, m, N)
#endif
VK_ENC_PIN_ORDER(VkVideoEncoderPlaneLayout, size, 8);
VK_ENC_PIN_ORDER(VkVideoEncoderPlaneLayout, rowPitch, 16);
VK_ENC_PIN_ORDER(VkVideoEncoderPlaneLayout, arrayPitch, 24);
VK_ENC_PIN_ORDER(VkVideoEncoderPlaneLayout, depthPitch, 32);
VK_ENC_PIN_ORDER(VkVideoEncoderInputFormatProperties, encodeFormat, 4);
VK_ENC_PIN_ORDER(VkVideoEncoderInputFormatProperties, optimality, 8);
#undef VK_ENC_PIN_ORDER

//=============================================================================
// VkVideoEncoderContentProbe::State is a PRIVATE MIRROR of
// VkVideoEncoderImportContentState. The probe lives in the encoder library,
// which sits below this file and must not include the ext header, so the two
// enums are declared independently -- and independently declared enums drift.
// These pin the mapping to the identity function, which is what
// VkEncMapContentState() below relies on, and pin the one shared numeric
// constant of the predicate. Tripping one means the mirror moved: fix the
// mirror, do not "fix" the assert.
//=============================================================================
static_assert((int)VkVideoEncoderContentProbe::STATE_NOT_EVALUATED ==
                  (int)VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED &&
              (int)VkVideoEncoderContentProbe::STATE_NOT_APPLICABLE ==
                  (int)VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_APPLICABLE &&
              (int)VkVideoEncoderContentProbe::STATE_ARMED ==
                  (int)VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_ARMED &&
              (int)VkVideoEncoderContentProbe::STATE_CLEAN ==
                  (int)VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_CLEAN &&
              (int)VkVideoEncoderContentProbe::STATE_DAMAGED_CHROMA ==
                  (int)VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_DAMAGED_CHROMA &&
              (int)VkVideoEncoderContentProbe::STATE_DAMAGED_ALL ==
                  (int)VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_DAMAGED_ALL,
              "VkVideoEncoderContentProbe::State drifted from "
              "VkVideoEncoderImportContentState");
static_assert(VkVideoEncoderContentProbe::kDeadPlaneMeanQ8 ==
                  VK_VIDEO_ENCODER_IMPORT_CONTENT_DEAD_PLANE_MEAN_Q8,
              "the scorer's dead-plane threshold and the one the public "
              "header documents have diverged");

static inline VkVideoEncoderImportContentState VkEncMapContentState(
    VkVideoEncoderContentProbe::State state)
{
    return (VkVideoEncoderImportContentState)state;
}

//=============================================================================
// VulkanVideoEncoderExt interface (external frame input)
//=============================================================================

enum VkEncDeviceFormatVerdict {
    VK_ENC_DEVICE_FORMAT_ACCEPTED = 0,
    // The bit depth is not a VkVideoComponentBitDepthFlagBitsKHR, so no video
    // profile can be spelled for it at all. Device-free, and reached before
    // the device is asked anything.
    VK_ENC_DEVICE_FORMAT_DEPTH_NOT_ENCODABLE,
    // The device exposes no encode capability at this (codec, profile,
    // subsampling, depth). This is the 4:2:2 and 12-bit answer on both
    // measured architectures.
    VK_ENC_DEVICE_FORMAT_PROFILE_ABSENT,
    // A conversion is needed and no target exists that this device would take.
    VK_ENC_DEVICE_FORMAT_NO_CONVERSION_TARGET,
    // The device has the profile and does not list the format the encoder
    // would be handed as an encode source for it.
    VK_ENC_DEVICE_FORMAT_NOT_AN_ENCODE_SOURCE,
};

// The reason, in the words a caller can act on. Kept beside the enum so a new
// verdict cannot be added without a sentence.
static const char* VkEncDeviceFormatReason(VkEncDeviceFormatVerdict verdict)
{
    switch (verdict) {
        case VK_ENC_DEVICE_FORMAT_DEPTH_NOT_ENCODABLE:
            return "that component bit depth is not a video encode bit depth, "
                   "so no profile can carry it on any device";
        case VK_ENC_DEVICE_FORMAT_PROFILE_ABSENT:
            return "this device exposes no encode capability at that chroma "
                   "subsampling and bit depth";
        case VK_ENC_DEVICE_FORMAT_NO_CONVERSION_TARGET:
            return "the conversion this input needs has no output format this "
                   "device accepts as an encode source";
        case VK_ENC_DEVICE_FORMAT_NOT_AN_ENCODE_SOURCE:
            return "this device has that profile and does not list the format "
                   "the encoder would be handed as an encode source for it";
        case VK_ENC_DEVICE_FORMAT_ACCEPTED:
        default:
            return "accepted";
    }
}

// DECLARED HERE, DEFINED BESIDE THE DEVICE QUERY IT CALLS. The vocabulary
// above needs nothing, so it lives where its first reader is; the resolver
// needs VkEncQueryDeviceEncodeSrcFormats and the routable-format helpers, and
// lives with those.
static VkEncDeviceFormatVerdict VkEncResolveDeviceEncodeFormat(
    const VulkanDeviceContext&       devCtx,
    VkPhysicalDevice                 physDevice,
    VkVideoCodecOperationFlagBitsKHR codec,
    uint32_t                         codecProfile,
    uint32_t                         chromaSubsampling,
    uint32_t                         bitDepth,
    VkFormat                         inputFormat,
    bool                             viaFilter,
    VkFormat&                        outEncodeFormat);

VkResult VulkanVideoEncoderExtImpl::InitializeExt(const VkVideoEncoderConfig& config)
{
    // Structure-type gate: reject version skew loudly instead of reading a
    // differently-laid-out struct.
    if (config.sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // Walk the extension chain. Known links are consumed; anything else is
    // rejected rather than ignored -- an extension the library does not
    // understand means the caller asked for something it is not getting.
    uint64_t requestedFrameTimeoutNs = 0;
    VkVideoEncoderValidationFlags validationFlags = 0;
    for (const void* link = config.pNext; link != nullptr;) {
        // Every public struct opens with {sType, pNext}; read those two
        // fields, then re-cast once the type is known.
        const auto* base =
            reinterpret_cast<const VkVideoEncoderFrameDeadlineInfo*>(link);
        switch (base->sType) {
            case VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_DEADLINE_INFO:
                requestedFrameTimeoutNs = base->frameCompletionTimeoutNs;
                break;
            case VK_VIDEO_ENCODER_STRUCTURE_TYPE_HDR_METADATA_INFO:
            case VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_COLOUR_INFO:
                // Recognized here so the gate below does not reject it, and
                // CONSUMED NOWHERE IN THIS FUNCTION: VkEncBuildEncoderConfig
                // walks the same chain and binds it. Two walks, one owner --
                // and the binder is the owner because it is the half a
                // device-free test can drive.
                break;
            case VK_VIDEO_ENCODER_STRUCTURE_TYPE_VALIDATION_INFO: {
                const auto* validation =
                    reinterpret_cast<const VkVideoEncoderValidationInfo*>(
                        link);
                if ((validation->flags &
                     ~VK_VIDEO_ENCODER_VALIDATE_EXTENSIONS_BIT) != 0) {
                    // A validation this build does not know was asked for
                    // and will not run; that must be loud, not silent.
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                validationFlags = validation->flags;
                break;
            }
            default:
                return VK_ERROR_INITIALIZATION_FAILED;
        }
        link = base->pNext;
    }
    // Session-serial: not callable from inside the completion callback. The
    // init path reaches WaitForThreadsToComplete, which would join the very
    // thread invoking the callback -- a hang where the header promises an
    // error code.
    if (IsInCompletionCallback()) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }

    // Latch the process-wide stdio-silence gate from the config BEFORE any
    // output is produced, so no gated VkEncOut() / VkEncErr() line escapes
    // ahead of it. Default VK_FALSE preserves behavior.
    //
    // The ordering requirement stands on the gated streams alone: the latch
    // must be installed before anything can write to them. BuildEncoderConfig
    // prints nothing at all, and what ParseArguments prints is raw printf /
    // fprintf that the latch does not intercept in any case.
    // The session's own request, held for as long as the session is. The
    // assignment this replaces made the last initializer the only one whose
    // choice survived; a token means a second session cannot un-silence the
    // first, and this session's silence ends when it does.
    m_stdioSilence = VkEncoderStdioSilenceScope(config.silenceStdio == VK_TRUE);

    VkVideoCodecOperationFlagBitsKHR codecOp = MapCodecOperation(config.codec);
    if (codecOp == VK_VIDEO_CODEC_OPERATION_NONE_KHR) {
        VkEncErr() << "[EncoderExt] Unsupported codec: " << (uint32_t)config.codec << std::endl;
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    // Build EncoderConfig from structured config
    VkResult result = BuildEncoderConfig(config, codecOp, m_encoderConfig);
    if (result != VK_SUCCESS) {
        VkEncErr() << "[EncoderExt] BuildEncoderConfig failed: " << result << std::endl;
        return result;
    }

    // Initialize Vulkan device with encode queue
    result = InitVulkanDevice(codecOp, config);
    if (result != VK_SUCCESS) {
        return result;
    }

    if ((validationFlags & VK_VIDEO_ENCODER_VALIDATE_EXTENSIONS_BIT) != 0) {
        // Everything in this list is an extension the ext import surface
        // actually calls into: VkImportMemoryFdInfoKHR
        // (external_memory_fd), the DMA_BUF handle type
        // (external_memory_dma_buf), DRM-tiled import
        // (image_drm_format_modifier), the FOREIGN acquire
        // (queue_family_foreign), and vkImportSemaphoreFdKHR
        // (external_semaphore_fd). VK_KHR_timeline_semaphore is
        // deliberately absent: core-promoted in 1.2, so checking the
        // extension STRING would false-fail a driver that stopped
        // advertising it.
        static const char* const kImportPathExtensions[] = {
#if defined(__linux) || defined(__linux__) || defined(linux)
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
#elif defined(_WIN32)
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#endif
        };
        bool anyMissing = false;
        for (const char* name : kImportPathExtensions) {
            if (m_vkDevCtx.FindRequiredDeviceExtension(name) == nullptr) {
                anyMissing = true;
                VkEncErr() << "[EncoderExt] VALIDATE_EXTENSIONS: the "
                              "session device lacks " << name
                           << ", which the external-input import path uses"
                           << std::endl;
            }
        }
        if (anyMissing) {
            // Failing loudly HERE, with names, is the point: the
            // alternative is passing creation and failing at the first
            // import -- or running spec-invalid on a driver that happens
            // to tolerate it.
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    // The session-level half of the preprocess-conversion capability check
    // (the build-level half is in the binder, which has no device). The
    // filter runs on a COMPUTE queue and the staging copy on a TRANSFER
    // queue, and they are distinct. Under a caller-supplied VkDevice the
    // compute queue is the embedder's, so the queue contract is
    // unenforceable and has to be VERIFIED rather than assumed.
    //
    // Failing here rather than dropping the conversion: with no compute queue
    // the filter object cannot be created, the frames that needed converting
    // would fall to the copy, and for a 3-plane source that copy is a GPU
    // hang, not a slower path.
    if ((VkEncClassifyInput(config.inputFormat, config.inputColorModel) ==
         VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER) &&
        (m_vkDevCtx.GetComputeQueueFamilyIdx() < 0)) {
        VkEncErr() << "[EncoderExt] inputFormat "
                   << (uint32_t)config.inputFormat
                   << " is encodable only through the preprocess compute "
                      "filter, but this device exposes no compute queue "
                      "family for the session to run it on" << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // ACCEPTANCE IS THE ADVERTISED SET, AND THIS IS WHERE THEY ARE MADE ONE.
    //
    // The library half of acceptance -- is this a pair this library routes at
    // all, and can the named profile carry it -- was settled by
    // BuildEncoderConfig above, device-free, and a format that failed there
    // never reached this line. What is settled HERE is the half that needs a
    // device: whether this device encodes the profile the binder derived, and
    // whether it takes the format the encoder would actually be handed.
    //
    // IT IS THE SAME CALL VkEncQueryInputFormatSupport AND
    // VkEncEnumerateInputFormats MAKE, on purpose. Those two advertise what a
    // caller may declare; this refuses what they do not advertise. Three
    // surfaces, one function, so "consult the list, then find out at init"
    // stops being two answers.
    //
    // WHY BEFORE CreateVideoEncoder AND NOT INSIDE IT. The driver refuses the
    // same configuration one call later, and its refusal is the reason this
    // gate exists rather than an argument against it: it arrives from
    // vkGetPhysicalDeviceVideoCapabilitiesKHR as
    // VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR with the library
    // reporting only "CreateVideoEncoder failed: <number>" -- naming neither
    // the format nor its subsampling, which are the two things a caller would
    // change.
    //
    // THE CODE IS THE POINT QUERY'S. VK_ERROR_FORMAT_NOT_SUPPORTED is what
    // VkEncQueryInputFormatSupport returns for exactly this verdict, and one
    // verdict with two spellings would put the caller back to asking which
    // surface it was talking to.
    {
        // THE ENCODE GEOMETRY, NOT THE INPUT'S. The video profile the session
        // is about to create is built from encodeChromaSubsampling and
        // encodeBitDepthLuma (EncoderConfig::InitVideoProfile), so a gate that
        // asked the device about the INPUT's geometry would be predicting a
        // different session than the one it is guarding -- the moment a chroma
        // resampler or a depth downgrade makes the two differ.
        VkFormat encodeFormat = VK_FORMAT_UNDEFINED;
        const VkEncDeviceFormatVerdict verdict = VkEncResolveDeviceEncodeFormat(
            m_vkDevCtx, m_vkDevCtx.getPhysicalDevice(), codecOp,
            m_encoderConfig->GetCodecProfile(),
            (uint32_t)m_encoderConfig->encodeChromaSubsampling,
            (uint32_t)m_encoderConfig->encodeBitDepthLuma,
            config.inputFormat,
            m_encoderConfig->IsPreprocessComputeFilterEnabled(),
            encodeFormat);
        if (verdict != VK_ENC_DEVICE_FORMAT_ACCEPTED) {
            VkEncErr() << "[EncoderExt] inputFormat "
                       << (uint32_t)config.inputFormat << " ("
                       << m_encoderConfig->input.numPlanes
                       << "-plane) cannot be encoded on this device: the "
                          "stream it derives is "
                       << VkEncChromaSubsamplingName(
                              m_encoderConfig->encodeChromaSubsampling)
                       << " at " << (uint32_t)m_encoderConfig->encodeBitDepthLuma
                       << " bits, profile "
                       << m_encoderConfig->GetCodecProfile() << ", and "
                       << VkEncDeviceFormatReason(verdict)
                       << ". VkEncEnumerateInputFormats lists what this device "
                          "does take for this codec and profile, and "
                          "VkEncQueryInputFormatSupport answers for one pair; "
                          "this refusal and those two answers are one function."
                       << std::endl;
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
    }

    // Create the internal encoder
    result = VkVideoEncoder::CreateVideoEncoder(&m_vkDevCtx, m_encoderConfig, m_encoder);
    if (result != VK_SUCCESS) {
        VkEncErr() << "[EncoderExt] CreateVideoEncoder failed: " << result << std::endl;
        return result;
    }

    SnapshotCaps();
    m_initConfig = config;
    // The session's input declaration, snapshotted for the lock-free readers
    // for the same reason as the compute-filter state: SupportsFormat takes no
    // lock, and m_initConfig is written here under one.
    m_sessionInputFormat.store(config.inputFormat, std::memory_order_relaxed);
    m_sessionInputColorModel.store(config.inputColorModel,
                                   std::memory_order_relaxed);
    // The chain was consumed above and points at caller stack storage;
    // a retained copy must not carry a pointer about to dangle.
    m_initConfig.pNext = nullptr;
    m_initialized = true;
    // Currency 3 is created up front, on the session-serial thread, so the
    // submit path can read the handle without a lock. Creation failure
    // degrades the currency (GetCompletionSemaphore stays VK_NULL_HANDLE);
    // it does not fail the session: no consumer requires the semaphore.
    if (m_encoder->CreateCompletionTimelineSemaphore() != VK_SUCCESS) {
        VkEncErr() << "[EncoderExt] completion timeline semaphore creation "
                      "failed; the GPU-ordering currency is unavailable "
                      "this session" << std::endl;
    }
    // Remember the rate-control mode for GetRuntimeInfo().
    m_rateControlMode = config.rateControlMode;
    // Completion deadline: 0 selects the 8 s default; clamp to stay
    // strictly above the library's internal 5 s fence wait so a
    // slow-but-successful frame is not converted into a lost one.
    m_frameTimeoutNs = (requestedFrameTimeoutNs != 0)
                           ? requestedFrameTimeoutNs
                           : 8000000000ull;
    if (m_frameTimeoutNs < 6000000000ull) {
        VkEncErr() << "[EncoderExt] frameCompletionTimeoutNs clamped to 6s "
                      "(must exceed the internal 5s fence wait)" << std::endl;
        m_frameTimeoutNs = 6000000000ull;
    }
    // M6: the capture push is the single completion edge; route it to the
    // callback machinery (counter + serialized user callback).
    m_encoder->SetOnBitstreamCaptured(
        [this](uint64_t frameId) { OnBitstreamCaptured(frameId); });

    if (config.verbose) {
        VkEncOut() << "[EncoderExt] Initialized: "
                  << config.encodeWidth << "x" << config.encodeHeight
                  << " codec=0x" << std::hex << (uint32_t)codecOp << std::dec
                  << " bitrate=" << config.averageBitrate
                  << " gop=" << config.gopLength
                  << std::endl;
    }

    return VK_SUCCESS;
}

// Create the PendingFrame entry for a successfully submitted frame. The
// registration reference travels IN the entry from the instant it exists:
// stamping it in afterwards (the previous shape) left a window in which a
// consumer thread could acquire and release the frame before the stamp
// landed, and the stamp's miss was a silent no-op -- an in-flight reference
// nothing would ever drop, which is R-2's pinned-pool-slot stall relocated
// to the success path. |resource| is NULL for the legacy (unregistered) arm.
uint64_t VulkanVideoEncoderExtImpl::ReservePendingFrame(
    const VkVideoEncodeInputFrame& frame,
    VkVideoEncoderResource resource,
    VkSemaphore releaseFenceSemaphore,
    const std::vector<VkSemaphore>* acquireFenceSemaphores)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    PendingFrame pending;
    pending.admissionToken = m_nextAdmissionToken++;
    pending.frameId = frame.frameId;
    pending.pts = frame.pts;
    pending.resource = resource;
    pending.releaseFenceSemaphore = releaseFenceSemaphore;
    if (acquireFenceSemaphores != nullptr) {
        pending.acquireFenceSemaphores = *acquireFenceSemaphores;
    }
    // The deadline runs from the reservation, which is the moment the caller
    // handed the frame over -- not from the return of a submit that may have
    // spent that time inside the driver.
    pending.submitTime = std::chrono::steady_clock::now();
    m_pendingFrames.push_back(std::move(pending));
    return m_pendingFrames.back().admissionToken;
}

void VulkanVideoEncoderExtImpl::CommitPendingFrame(
    uint64_t admissionToken,
    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto& p : m_pendingFrames) {
        if (p.admissionToken != admissionToken) {
            continue;
        }
        p.encodeFrameInfo = encodeFrameInfo;
        p.admitted = true;
        // Counted here and only here, so a rolled-back reservation never
        // appears in the submitted total.
        m_framesSubmitted++;
        return;
    }
    // Gone already: the frame was released or abandoned while this submit was
    // in the driver. Nothing to attach; the submitted count is not raised for
    // a frame no longer accounted anywhere.
}

void VulkanVideoEncoderExtImpl::RollbackPendingFrame(
    uint64_t admissionToken,
    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo>& encodeFrameInfo)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto it = m_pendingFrames.begin(); it != m_pendingFrames.end(); ++it) {
        if (it->admissionToken != admissionToken) {
            continue;
        }
        if (it->hasCapture) {
            // Work landed despite the refusal. Destroying this entry would
            // drop the registration reference, the release-fence semaphore
            // and the imported acquire semaphores while a submission that
            // named them completed -- so keep it and let the ordinary
            // release path retire them once it is drained.
            it->encodeFrameInfo = encodeFrameInfo;
            it->admitted = true;
            m_framesSubmitted++;
            return;
        }
        // Nothing was accepted, so no completion record can ever pop for
        // this entry. It is deliberately NOT disclaimed through
        // m_releasedWhilePending: that would leave a record able to swallow
        // a genuine capture if the caller reuses the id.
        m_pendingFrames.erase(it);
        return;
    }
}

void VulkanVideoEncoderExtImpl::EnqueuePendingFrame(
    const VkVideoEncodeInputFrame& frame,
    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo>& encodeFrameInfo,
    VkVideoEncoderResource resource,
    VkSemaphore releaseFenceSemaphore,
    const std::vector<VkSemaphore>* acquireFenceSemaphores)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    PendingFrame pending;
    pending.frameId = frame.frameId;
    pending.pts = frame.pts;
    pending.encodeFrameInfo = encodeFrameInfo;
    pending.resource = resource;
    // The entry owns the release-fence semaphore for the same reason it owns
    // the registration reference: the submission that signals it outlives
    // this call, so its destruction has to travel with the frame rather than
    // be attempted here.
    pending.releaseFenceSemaphore = releaseFenceSemaphore;
    // Same argument, same owner: the wait that consumes an acquire fence's
    // payload belongs to a submission that outlives this call.
    if (acquireFenceSemaphores != nullptr) {
        pending.acquireFenceSemaphores = *acquireFenceSemaphores;
    }
    pending.submitTime = std::chrono::steady_clock::now();
    m_pendingFrames.push_back(std::move(pending));
    m_framesSubmitted++;
}

VkResult VulkanVideoEncoderExtImpl::SubmitExternalFrameCommon(
    const VkVideoEncodeInputFrame& frame,
    VkSharedBaseObj<VulkanVideoImagePoolNode>* preparedNode,
    bool encodeCapable,
    bool routeViaFilter,
    VkVideoEncoderResource resource,
    VkSemaphore* pStagingCompleteSemaphore,
    VkSemaphore releaseFenceSemaphore,
    int* pReleaseFenceFd,
    const std::vector<VkSemaphore>* acquireFenceSemaphores,
    bool srcLayoutIsExplicit)
{
    // Class (b) versus Flush()/teardown, which null m_encoder under
    // m_pendingMutex and then release it: the ExportCompletionSemaphoreHandle
    // pattern. Take a reference under the same lock they clear it under and
    // submit through the local reference -- that both closes the TOCTOU on
    // the shared pointer and keeps the encoder alive across the inline
    // CPU-side encode below if a Flush lands mid-submit (the destructor then
    // runs on this thread when the local drops, outside m_pendingMutex,
    // exactly as the export path accepts for its driver call).
    // Acceptance stops at the start of shutdown, not when the encoder pointer
    // is finally cleared. An Unproven shutdown keeps the encoder alive
    // deliberately, and without this a caller could keep feeding a pipeline
    // whose workers have already been joined.
    if (m_shutdownStarted.load(std::memory_order_acquire)) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }

    VkSharedBaseObj<VkVideoEncoder> encoder;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (!m_initialized || (!m_encoder && (m_nullBackend == nullptr))) {
            return VK_ERROR_NOT_PERMITTED_KHR;
        }
        encoder = m_encoder;
    }

    // Structure-type gate (see the header's versioning rules).
    if ((frame.sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_FRAME) ||
        (frame.pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Typed rejection replacing a latent null-deref (see SupportsFormat):
    // a non-YCbCr frame accepted here would crash in the staging copy.
    if (SupportsFormat(frame.format) != VK_TRUE) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    // The compute tier is REGISTERED-ONLY, and this is where that is enforced.
    //
    // SupportsFormat is the SESSION's answer, so it says VK_TRUE for the
    // 3-plane family and for the 8-bit RGBA family on any session whose
    // filter takes that format. On the REGISTERED arm that VK_TRUE is backed
    // by four facts checked at registration: the descriptor's format matched
    // the session's filter-input format, the descriptor permitted the views
    // ITS arm of the filter reads (per-plane STORAGE views for a multi-planar
    // input, a storage-capable combined view for RGBA), the session's filter
    // is active, and those views were actually BUILT
    // (slot.planeStorageViews / slot.storageReadView).
    // The LEGACY arm has none of them -- it has a
    // raw VkImage and a format, and WrapExternalImage's LINEAR arm
    // deliberately builds a view-less wrapper while its non-linear arm
    // FABRICATES MUTABLE_FORMAT | EXTENDED_USAGE and STORAGE on an image whose
    // real create flags it cannot see. Routing a filter frame through it binds
    // either nothing or views the image never validated.
    //
    // |routeViaFilter| is the registration's resolved path, false on the
    // legacy arm by construction, so this keeps that arm at its documented
    // byte-for-byte pre-registration behaviour: the 3-plane family is refused
    // outright.
    if ((VkEncClassifyInput(frame.format, SessionColorModel(frame.format)) ==
         VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER) && !routeViaFilter) {
        VkEncErr() << "[EncoderExt] submit: inputFormat "
                   << (uint32_t)frame.format
                   << " encodes only through the preprocess compute filter, "
                      "which requires a REGISTERED resource whose descriptor "
                      "declared MUTABLE_FORMAT | EXTENDED_USAGE and STORAGE; "
                      "the unregistered submit path cannot supply the "
                      "per-plane views the filter reads" << std::endl;
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    // The handle gate. It sits BELOW the format and routing refusals on
    // purpose: those are statements about what this session can encode and
    // must keep answering first (the legacy arm's documented refusal of the
    // filter-only formats is VK_ERROR_FORMAT_NOT_SUPPORTED, and a null image
    // must not be able to change that answer). It sits ABOVE everything that
    // touches the image.
    //
    // frame.image is required. The gate is unconditional and independent
    // of the declared format, so no format classification can decide
    // whether it runs; without it a null image reaches the driver on the
    // staging path, where the failure is a dereference inside the driver
    // rather than a status this call can return.
    //
    // Unconditional, not legacy-arm-only: SubmitRegisteredFrame builds
    // |frame| from the registration slot and always fills image from it, so
    // a registered frame cannot reach here null and loses nothing.
    if (frame.image == VK_NULL_HANDLE) {
        VkEncErr() << "[EncoderExt] submit: frame.image is VK_NULL_HANDLE; "
                      "the input image is required" << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Test seam: from here down the submit needs a VkVideoEncoder, so a
    // null-backend session (fault injection; see the internal header)
    // terminates here -- AFTER every gate a real submit passes. Its success
    // arm runs EnqueuePendingFrame, the same bookkeeping the real success
    // arm runs below, so what the fault tests exercise is the production
    // handoff and not a copy of it.
    if (m_nullBackend != nullptr) {
        if (m_nullBackend->submitResult != VK_SUCCESS) {
            return m_nullBackend->submitResult;
        }
        if (pStagingCompleteSemaphore != nullptr) {
            *pStagingCompleteSemaphore = VK_NULL_HANDLE;
        }
        // Observation seam (VkEncProbeLastSubmitSync, internal header). This
        // point sits below every gate and below the chained-descriptor walk,
        // and above the two SetExternalInputFrame* call sites -- which
        // forward these same |frame| fields verbatim -- so the record is what
        // both of those sites would submit.
        RecordSubmitSyncForTest(frame);
        VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> noBackendNode;
        // No queue, so nothing ever signals a release fence here. The
        // caller's -1 default stands; the fault-injection sessions do not
        // create one to begin with.
        EnqueuePendingFrame(frame, noBackendNode, resource,
                            releaseFenceSemaphore, acquireFenceSemaphores);
        return VK_SUCCESS;
    }

    // Admission control, honoring the header's non-blocking contract:
    // refuse with VK_NOT_READY BEFORE any state changes (pool node, GOP
    // position, DPB) when the submit path would otherwise block on the
    // assembly queue's producer condvar, or when the captured-bitstream
    // backlog is unclaimed. A refused frame leaves no residue; the caller
    // retries after draining.
    if (!encoder->CanAcceptNewInputFrame()) {
        return VK_NOT_READY;
    }

    // Get an available frame info node from the pool
    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> encodeFrameInfo;
    bool gotNode = encoder->GetAvailablePoolNode(encodeFrameInfo);
    if (!gotNode || !encodeFrameInfo) {
        return VK_NOT_READY; // Pool is full, try again later
    }

    // No wait-stage masks are fabricated here. VkVideoEncodeInputFrame
    // carries none, and the right mask depends on which submission consumes
    // the wait, which only the encoder core knows: TRANSFER where the waits
    // gate the staging-copy submit (Paths B/C), VIDEO_ENCODE where they gate
    // the direct encode submit (Path A). Passing null lets each consumption
    // point apply its own default. The TRANSFER-for-everything vector this
    // replaces left Path A's vkCmdEncodeVideoKHR outside the waits' scope,
    // so the encode could read the input before the producer signaled.

    // Map the public residency declaration onto the internal enum.
    VkVideoEncoder::ExternalInputResidency residency =
        VkVideoEncoder::EXTERNAL_INPUT_RESIDENCY_AUTO;
    switch (frame.inputResidency) {
        case VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL:
            residency = VkVideoEncoder::EXTERNAL_INPUT_RESIDENCY_LOCAL;   break;
        case VK_VIDEO_ENCODER_INPUT_RESIDENCY_FOREIGN:
            residency = VkVideoEncoder::EXTERNAL_INPUT_RESIDENCY_FOREIGN; break;
        case VK_VIDEO_ENCODER_INPUT_RESIDENCY_AUTO:
        default:
            break;
    }

    // Forward the mid-stream keyframe request from the caller.
    //
    // Registered arm (|preparedNode| non-null): the image, its memory, its
    // view and its pool wrapper were created once at registration, so this
    // submit performs no wrap and creates no Vulkan object -- the header's
    // zero-allocations contract. Routing is the registration-time
    // |encodeCapable| predicate, not the per-frame tiling field.
    // Legacy arm: byte-for-byte the pre-registration behavior; the
    // reference renderer consumes exactly this shape until its M5
    // migration.
    // ADMIT BEFORE THE ENCODER CAN PUBLISH. The assembly worker can capture
    // this frame's bitstream and call OnBitstreamCaptured before the call
    // below returns; without an entry already in the queue that capture is
    // unmatched, gets counted as a late capture and is thrown away, and the
    // frame that arrives afterwards never completes. The lock is taken and
    // released inside ReservePendingFrame -- the workers take it too, so no
    // core or queue operation may run while it is held.
    const uint64_t admissionToken = ReservePendingFrame(
        frame, resource, releaseFenceSemaphore, acquireFenceSemaphores);

    VkResult result;
    if (preparedNode != nullptr) {
        result = encoder->SetExternalInputFrameWithNode(
            encodeFrameInfo,
            *preparedNode,
            // The registration id, so the staging arm can tell the content
            // probe WHICH BUFFER these pixels came out of. Everything else
            // the probe needs it already has; this is the only fact that
            // lives up here and nowhere down there.
            resource,
            encodeCapable,
            routeViaFilter,
            frame.currentLayout,
            srcLayoutIsExplicit,
            frame.frameId,
            frame.pts,
            (frame.isLastFrame == VK_TRUE),
            (frame.forceIDR == VK_TRUE),
            frame.qpOverride,
            residency,
            frame.waitSemaphoreCount,
            frame.pWaitSemaphores,
            frame.pWaitSemaphoreValues,
            nullptr,  // wait-stage masks: each consumption point defaults
            frame.signalSemaphoreCount,
            frame.pSignalSemaphores,
            frame.pSignalSemaphoreValues);
    } else {
        result = encoder->SetExternalInputFrame(
            encodeFrameInfo,
            frame.image,
            VK_NULL_HANDLE,  // memory (non-owning, encoder doesn't need it)
            frame.format,
            frame.width, frame.height,
            frame.imageTiling,
            frame.currentLayout,  // Producer's layout (e.g. GENERAL for compute output)
            frame.frameId,
            frame.pts,
            (frame.isLastFrame == VK_TRUE),
            (frame.forceIDR == VK_TRUE),
            frame.qpOverride,
            residency,
            frame.waitSemaphoreCount,
            frame.pWaitSemaphores,
            frame.pWaitSemaphoreValues,
            nullptr,  // wait-stage masks: each consumption point defaults
            frame.signalSemaphoreCount,
            frame.pSignalSemaphores,
            frame.pSignalSemaphoreValues);
    }


    if (result == VK_SUCCESS) {
        // Return the semaphore that signals when the encoder is done
        // reading the external input image. The caller (encoder service)
        // uses this to chain the display blit after the encode, then
        // signals the release semaphore from the display submit.
        //
        // Path A (direct encode): encodeCmdBuffer's semaphore, signaled
        //   after vkCmdEncodeVideoKHR reads the input.
        // Path B/C (staging): inputCmdBuffer's semaphore, signaled
        //   after the staging copy reads the input.
        if (pStagingCompleteSemaphore) {
            if (encodeFrameInfo->inputCmdBuffer) {
                // Paths B/C: staging copy was done, signal from inputCmdBuffer
                *pStagingCompleteSemaphore = encodeFrameInfo->inputCmdBuffer->GetSemaphore();
            } else {
                // Path A: no staging, sync is via inputSignalSemaphores (timeline)
                *pStagingCompleteSemaphore = VK_NULL_HANDLE;
            }
        }

        // Per-frame release fence (design 3.5): export the SYNC_FD now, and
        // only now.
        //
        // WHY HERE AND NOT EARLIER OR LATER. A SYNC_FD export requires the
        // semaphore to be signalled or to have a signal operation PENDING
        // EXECUTION, so it cannot precede the submit that carries the
        // signal. It also cannot be deferred past this function, because
        // pReleaseFenceFd is an out-parameter of the submit call. This point
        // -- immediately after the Set*Frame call, still holding the
        // frame-info node -- is the one place where the submit has happened
        // and the evidence that it happened is still readable.
        //
        // WHY THE SUBMIT IS NOT ALWAYS ISSUED. The staged paths submit the
        // staging copy inline from StageInputFrame, so their release signal
        // is always pending on return. The DIRECT path submits from the
        // deferred-GOP flush, which under B-frame reordering happens on a
        // LATER call -- the frame is queued, not submitted. Asking the
        // command-buffer node whether it reached vkQueueSubmit is the only
        // answer that cannot drift from that scheduling decision, so that is
        // what is asked, rather than predicting it from the GOP config.
        //
        // WHAT THE fd SIGNALS. The submission it rides is the one that READS
        // the input image -- the staging copy on Paths B/C, the encode on
        // Path A. It is deliberately NOT the encode's completion: the
        // bitstream becoming retrievable is a different event on a different
        // resource, and only the read-completion meaning is usable by the
        // end-of-read-access seam this fd exists to feed.
        if ((releaseFenceSemaphore != VK_NULL_HANDLE) &&
            (pReleaseFenceFd != nullptr)) {
            // ASK BOTH NODES WHETHER THEY WERE SUBMITTED. The staging arm
            // used to take the mere existence of inputCmdBuffer as proof,
            // which is true of a recorded batch the driver went on to reject:
            // the fd was then exported from a semaphore with no signal
            // operation queued, and the embedder waited on it forever.
            const bool inputConsumingSubmitIssued =
                (encodeFrameInfo->inputCmdBuffer &&
                 encodeFrameInfo->inputCmdBuffer->IsCommandBufferSubmitted()) ||
                (encodeFrameInfo->encodeCmdBuffer &&
                 encodeFrameInfo->encodeCmdBuffer->IsCommandBufferSubmitted());
            if (inputConsumingSubmitIssued) {
                *pReleaseFenceFd = ExportReleaseFenceFd(releaseFenceSemaphore);
            }
            // else: the -1 the walk already wrote stands. The signal will
            // still fire on the deferred submit and simply go unconsumed --
            // which is why this semaphore may not be recycled, only
            // destroyed once its batch has completed.
        }

        // Finish the admission the reservation opened. The entry already
        // owns the registration reference and the fence semaphores; what the
        // submit adds is the frame info that returns pool resources when the
        // frame is released.
        CommitPendingFrame(admissionToken, encodeFrameInfo);
    } else {
        RollbackPendingFrame(admissionToken, encodeFrameInfo);
    }

    return result;
}

VkResult VulkanVideoEncoderExtImpl::SubmitExternalFrame(
    const VkVideoEncodeInputFrame& frame,
    VkSemaphore* pStagingCompleteSemaphore)
{
    // Legacy arm: no prepared node. Behavior-preserving by construction --
    // the null-node branch of the common path IS the pre-registration code.
    // Legacy arm: no registration, so no resolved path. The encoder core
    // derives the rung from the frame's own format there.
    return NoteDeviceResult(
        SubmitExternalFrameCommon(frame, nullptr, false, false,
                                  VK_VIDEO_ENCODER_RESOURCE_NULL,
                                  pStagingCompleteSemaphore));
}

// Mark |frame| deliverable and record its place in completion order. This
// is the single point where hasCapture is raised, so no outcome -- capture,
// deadline drop, or cancellation -- can become deliverable without also
// becoming reachable by AcquireNextEncodedFrame. Caller holds m_pendingMutex.
void VulkanVideoEncoderExtImpl::MarkFrameReadyLocked(PendingFrame& frame)
{
    if (frame.hasCapture) {
        return;  // already deliverable; must not be enqueued twice
    }
    frame.hasCapture = true;
    m_readyOrder.push_back(frame.frameId);
}

// Pop the oldest deliverable frame off the ready queue and fill |result|.
// Caller holds m_pendingMutex. Stale ids -- a frame since released, or one
// taken first by the keyed AcquireEncodedFrame -- are skipped, which is why
// the queue never has to be kept in exact sync with m_pendingFrames.
bool VulkanVideoEncoderExtImpl::TryPopReadyLocked(VkVideoEncodeResult& result)
{
    while (!m_readyOrder.empty()) {
        const uint64_t readyId = m_readyOrder.front();
        m_readyOrder.pop_front();
        for (auto& p : m_pendingFrames) {
            if (p.frameId != readyId) {
                continue;
            }
            if (!p.acquired && p.hasCapture) {
                FillResultLocked(p, result);
                return true;
            }
            break;
        }
    }
    return false;
}

// Close a POSIX fd the library was handed and STILL HOLDS. The rule the
// caller sees is unconditional -- an fd is consumed on every exit path --
// so every pre-Vulkan early return in the registration paths funnels
// through here. What must never funnel through here is an exit past the
// vkAllocateMemory handoff in the image import: there the DRIVER owns the
// close (see VkEncImportExternalImage), and a second one would be design
// section 2.3's double close. Win32 handles are never touched.
static void VkEncConsumeOsHandle(VkVideoEncoderExternalHandleType handleType,
                                 uint64_t osHandle)
{
#if defined(__linux__)
    if (((handleType == VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD) ||
         (handleType == VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF)) &&
        ((int64_t)osHandle >= 0)) {
        close((int)osHandle);
    }
#else
    (void)handleType;
    (void)osHandle;
#endif
}

// Writes the ownership echo on every exit path -- a dozen early returns
// and one missed write is a caller asserting against garbage, the same
// failure mode the R-2 release guard exists for, applied to reporting.
// Writes only into a correctly stamped struct: a mis-stamped pStatus was
// answered STRUCTURE_TYPE_UNKNOWN and must not be written through as if
// it had the expected layout.
class VkEncStatusEcho {
public:
    VkEncStatusEcho(VkVideoEncoderStatus* pStatus, bool handlesConsumed)
        : m_pStatus(pStatus), m_handlesConsumed(handlesConsumed) {}
    // The import-ordinal guard's verdict rides the SAME every-exit
    // guarantee as the ownership echo, and for the same reason: the guard
    // runs before most of the early returns, so reporting it only on the
    // success path would leave the failures -- the ones worth reporting --
    // silent all over again.
    //
    // Installed only AFTER the chain walk has accepted the link. A refused
    // chain must not be written through, exactly as a mis-stamped pStatus
    // is not.
    void SetGuardInfo(VkVideoEncoderImportGuardInfo* pGuardInfo) {
        m_pGuardInfo = pGuardInfo;
    }
    // The content probe's ARMING result rides the same every-exit guarantee,
    // and needs it for the same reason: a registration that is refused before
    // the probe can be armed must still say so, rather than leaving the
    // caller's struct holding whatever it held before the call.
    //
    // Two setters and not one, because the two facts arrive at different
    // times: the POINTER is installed by the chain walk, which runs early,
    // and the VERDICT by the armer, which runs after the registration has
    // either succeeded or not. Between them the state stays NOT_EVALUATED,
    // which is the truthful answer for every exit in between.
    void SetContentInfo(VkVideoEncoderImportContentInfo* pContentInfo) {
        m_pContentInfo = pContentInfo;
    }
    void SetContentVerdict(VkVideoEncoderImportContentState state,
                           VkVideoEncoderResource resource) {
        m_contentState = state;
        m_contentResource = resource;
    }
    ~VkEncStatusEcho() {
        if ((m_pStatus != nullptr) &&
            (m_pStatus->sType == VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS)) {
            m_pStatus->handlesConsumed =
                m_handlesConsumed ? VK_TRUE : VK_FALSE;
        }
        if ((m_pGuardInfo != nullptr) &&
            (m_pGuardInfo->sType ==
             VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_GUARD_INFO)) {
            VkEncImportOrdinalGuardReport report;
            VkEncGetImportOrdinalGuardReport(&report);
            m_pGuardInfo->state          = report.state;
            m_pGuardInfo->requestedCount = report.requestedCount;
            m_pGuardInfo->retainedCount  = report.retainedCount;
            m_pGuardInfo->failureStatus  = report.failureStatus;
            m_pGuardInfo->failureErrno   = report.failureErrno;
        }
        if ((m_pContentInfo != nullptr) &&
            (m_pContentInfo->sType ==
             VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_CONTENT_INFO)) {
            // probeGeneration FIRST and unconditionally: it is the writer
            // proof, and a writer proof that is only stamped on the paths
            // that had something to say proves nothing about the paths that
            // did not. This is the field the guard's requestedCount stopped
            // being able to be at the guard count of 0.
            m_pContentInfo->probeGeneration =
                VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION;
            m_pContentInfo->state    = m_contentState;
            m_pContentInfo->resource = m_contentResource;
            // The registration echo carries no measurement and says so with
            // zeros: at RegisterImageResource time the producer has written
            // nothing, so there is nothing to have measured. The verdict and
            // its arithmetic arrive on GetCompletionInfo.
            m_pContentInfo->meanY = 0;
            m_pContentInfo->meanU = 0;
            m_pContentInfo->meanV = 0;
            m_pContentInfo->probedRegistrationCount = 0;
            m_pContentInfo->damagedRegistrationCount = 0;
        }
    }
    VkEncStatusEcho(const VkEncStatusEcho&) = delete;
    VkEncStatusEcho& operator=(const VkEncStatusEcho&) = delete;
private:
    VkVideoEncoderStatus* m_pStatus;
    bool                  m_handlesConsumed;
    VkVideoEncoderImportGuardInfo* m_pGuardInfo = nullptr;
    VkVideoEncoderImportContentInfo* m_pContentInfo = nullptr;
    VkVideoEncoderImportContentState m_contentState =
        VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED;
    VkVideoEncoderResource m_contentResource = VK_VIDEO_ENCODER_RESOURCE_NULL;
};

VulkanVideoEncoderExtImpl::RegisteredImage*
VulkanVideoEncoderExtImpl::LookupResourceLocked(VkVideoEncoderResource resource)
{
    if (resource == VK_VIDEO_ENCODER_RESOURCE_NULL) {
        return nullptr;
    }
    const size_t index = ResourceIndex(resource);
    if (index >= m_resources.size()) {
        return nullptr;
    }
    RegisteredImage& slot = m_resources[index];
    // The generation check is the point: a stale id from before an
    // unregister names a generation this slot no longer has.
    if (!slot.live || (slot.generation != ResourceGeneration(resource))) {
        return nullptr;
    }
    return &slot;
}

void VulkanVideoEncoderExtImpl::DestroyResourceLocked(RegisteredImage& slot)
{
    if (slot.node || slot.imageView) {
        // The refcounted view/node own the image and memory now (the import
        // arm wraps them owning via CreateFromImport). Dropping the refs IS
        // the release; per-frame holders (VkVideoEncodeFrameInfo) keep the
        // node alive until the last in-flight frame retires, which is what
        // makes this safe to call from deferred retirement -- the wrapper's
        // refcount, not this function, is the final arbiter.
        slot.node = nullptr;
        slot.imageView = nullptr;
    } else if (slot.ownsImage) {
        // Only reachable when registration failed before the wrapper was
        // built; the raw handles are still ours to free.
        VkDevice device = m_vkDevCtx;
        if (slot.image != VK_NULL_HANDLE) {
            m_vkDevCtx.DestroyImage(device, slot.image, nullptr);
        }
        if (slot.memory != VK_NULL_HANDLE) {
            // Also releases the imported fd: Vulkan took ownership of it.
            m_vkDevCtx.FreeMemory(device, slot.memory, nullptr);
        }
    }
    // A2 scratch (reserved): nothing creates these
    // today; destroying them here means the future population cannot leak
    // on retirement. Always library-owned, so never under the ownsImage
    // guard above -- view, then image, then memory.
    if ((slot.filterScratchView != VK_NULL_HANDLE) ||
        (slot.filterScratchImage != VK_NULL_HANDLE) ||
        (slot.filterScratchMemory != VK_NULL_HANDLE)) {
        VkDevice scratchDevice = m_vkDevCtx;
        if (slot.filterScratchView != VK_NULL_HANDLE) {
            m_vkDevCtx.DestroyImageView(scratchDevice,
                                        slot.filterScratchView, nullptr);
        }
        if (slot.filterScratchImage != VK_NULL_HANDLE) {
            m_vkDevCtx.DestroyImage(scratchDevice,
                                    slot.filterScratchImage, nullptr);
        }
        if (slot.filterScratchMemory != VK_NULL_HANDLE) {
            m_vkDevCtx.FreeMemory(scratchDevice,
                                  slot.filterScratchMemory, nullptr);
        }
    }
    slot.filterScratchView   = VK_NULL_HANDLE;
    slot.filterScratchImage  = VK_NULL_HANDLE;
    slot.filterScratchMemory = VK_NULL_HANDLE;
    slot.inputPath = VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT;
    slot.image  = VK_NULL_HANDLE;
    slot.memory = VK_NULL_HANDLE;
    slot.live   = false;
    slot.retired = false;
    slot.ownsImage = false;
    slot.inFlight = 0;
    slot.imageUsage = 0;
    slot.encodeCapable = false;
    slot.planeStorageViews = false;
    slot.storageReadView = false;
    slot.importedAllocSize = 0;
    slot.generation++;  // invalidate every id that named this slot
}

// Cache the capability scalars the lock-free query methods report. Called
// once per successful init, while m_encoderConfig is known live and no
// retrieval thread exists yet.
void VulkanVideoEncoderExtImpl::SnapshotCaps()
{
    if (!m_encoderConfig) {
        return;
    }
    const auto& caps = m_encoderConfig->videoCapabilities;
    m_capsMaxWidth.store(caps.maxCodedExtent.width, std::memory_order_relaxed);
    m_capsMaxHeight.store(caps.maxCodedExtent.height, std::memory_order_relaxed);
    m_capsGranularityW.store(caps.pictureAccessGranularity.width,
                             std::memory_order_relaxed);
    m_capsGranularityH.store(caps.pictureAccessGranularity.height,
                             std::memory_order_relaxed);
    // Compute-filter state, for the same lock-free readers. input.vkFormat is
    // meaningful here because the binder wrote input.numPlanes from the
    // caller's inputFormat (VkEncBuildEncoderConfig), so FinalizeConfig
    // reconstructed the format the caller actually declared rather than
    // EncoderConfig's 3-plane default.
    const bool filterEnabled =
        m_encoderConfig->IsPreprocessComputeFilterEnabled();
    m_computeFilterActive.store(filterEnabled, std::memory_order_relaxed);
    m_computeFilterInputFormat.store(
        filterEnabled ? m_encoderConfig->input.vkFormat : VK_FORMAT_UNDEFINED,
        std::memory_order_relaxed);
}

// Shared drain: move completed captures from the encoder's FIFO onto their
// PendingFrame entries. Caller holds m_pendingMutex. A capture for a frame
// already delivered as a deadline drop or a cancellation -- whether the
// entry is still resident or was released outright -- is LATE: discarded
// and counted, so a deadline/fence-cap collision is observable instead of
// silent. A capture for a frame released while it was still PENDING is not
// late -- nothing was delivered that it could arrive after -- and is
// discarded without comment (m_releasedWhilePending carries the ids).
void VulkanVideoEncoderExtImpl::DrainCapturesLocked()
{
    if (!m_encoder) {
        return;
    }
    uint64_t popped_id = 0;
    std::vector<uint8_t> popped_bytes;
    bool popped_idr = false;
    uint32_t popped_pic_type = 0;
    VkResult popped_status = VK_SUCCESS;
    while (m_encoder->TryPopCapturedBitstream(
               &popped_id, &popped_bytes, &popped_idr,
               &popped_pic_type, &popped_status)) {
        bool matched = false;
        for (auto& p : m_pendingFrames) {
            if (p.frameId == popped_id) {
                matched = true;
                if (p.timedOut) {
                    m_lateCaptures++;
                    VkEncErr() << "[EncoderExt] late capture for timed-out "
                                  "frame " << popped_id << " discarded "
                                  "(lateCaptures=" << m_lateCaptures << ")"
                               << std::endl;
                    break;
                }
                p.bytes = std::move(popped_bytes);
                p.isIdr = popped_idr;
                p.pictureType = popped_pic_type;
                p.status = popped_status;
                MarkFrameReadyLocked(p);
                break;
            }
        }
        if (!matched) {
            auto released = m_releasedWhilePending.find(popped_id);
            if (released != m_releasedWhilePending.end()) {
                // Disclaimed in advance: ReleaseEncodedFrame (or
                // AbandonAllFrames) erased this frame while it was still
                // PENDING, which the header documents as legal -- the
                // file-output fire-and-forget consumers it names (the
                // reference renderer) release on submit success as their
                // steady state. Not late: nothing was delivered for this
                // capture to arrive after. Consume the record and drop the
                // payload silently.
                m_releasedWhilePending.erase(released);
            } else {
                // No resident entry and no released-while-pending record:
                // the frame was DELIVERED -- as a deadline drop or a
                // cancellation -- then released before this capture, the
                // real one, arrived. Same late class as the timed-out arm
                // above, counted the same way. (An unmatched pop this arm
                // holds no record of is counted late too; it cannot
                // classify what it never saw disclaimed.)
                m_lateCaptures++;
                VkEncErr() << "[EncoderExt] late capture for released frame "
                           << popped_id << " discarded (lateCaptures="
                           << m_lateCaptures << ")" << std::endl;
            }
        }
    }
}

// Deadline check: true when |frame| (no capture yet) is past its completion
// deadline; synthesizes the 0-byte VK_TIMEOUT drop delivery in place.
bool VulkanVideoEncoderExtImpl::SynthesizeTimeoutLocked(PendingFrame& frame)
{
    if (!frame.admitted) {
        // Still a reservation: its submit has not returned, so there is no
        // outstanding encode to declare late. Dropping it here would also
        // make the capture that follows arrive for a timed-out frame and be
        // discarded, which is the loss this reservation exists to prevent.
        return false;
    }
    const auto waited = std::chrono::steady_clock::now() - frame.submitTime;
    const uint64_t waitedNs = (uint64_t)std::chrono::duration_cast<
        std::chrono::nanoseconds>(waited).count();
    if (waitedNs < m_frameTimeoutNs) {
        return false;
    }
    frame.bytes.clear();
    frame.isIdr = false;
    frame.pictureType =
        (uint32_t)VkVideoGopStructure::FRAME_TYPE_I;  // translated to I
    frame.status = VK_TIMEOUT;
    MarkFrameReadyLocked(frame);
    frame.timedOut = true;
    m_framesTimedOut++;
    VkEncErr() << "[EncoderExt] frame " << frame.frameId
               << " passed the completion deadline; delivered as a 0-byte "
                  "VK_TIMEOUT drop (framesTimedOut=" << m_framesTimedOut
               << ")" << std::endl;
    return true;
}

// Fill |result| from |frame| and mark it delivered. Caller holds the lock.
void VulkanVideoEncoderExtImpl::FillResultLocked(PendingFrame& frame,
                                                 VkVideoEncodeResult& result)
{
    result.frameId = frame.frameId;
    result.pts = frame.pts;
    // Always 0. With no B-frames there is no reorder, so decode order equals
    // presentation order and a consumer that needs a DTS can use the PTS.
    // Deriving a real DTS only becomes meaningful alongside B-frame support,
    // and is specified there rather than left as a bare TODO on a public field.
    result.dts = 0;
    result.pBitstreamData = frame.bytes.empty() ? nullptr : frame.bytes.data();
    result.bitstreamSize = static_cast<uint32_t>(frame.bytes.size());
    // Internal captures carry VkVideoGopStructure::FrameType numbering
    // (P=0, B=1, I=2, IDR=3, intra-refresh=6); the public field uses
    // VkVideoEncoderPictureType (I=0, P=1, B=2). Translate at the boundary.
    switch (static_cast<VkVideoGopStructure::FrameType>(frame.pictureType)) {
        case VkVideoGopStructure::FRAME_TYPE_P:
            result.pictureType = VK_VIDEO_ENCODER_PICTURE_TYPE_P;
            break;
        case VkVideoGopStructure::FRAME_TYPE_B:
            result.pictureType = VK_VIDEO_ENCODER_PICTURE_TYPE_B;
            break;
        default:  // I / IDR / intra-refresh
            result.pictureType = VK_VIDEO_ENCODER_PICTURE_TYPE_I;
            break;
    }
    result.isIDR = frame.isIdr ? VK_TRUE : VK_FALSE;
    result.temporalLayerId = 0;
    // Per-frame result from the capture (VK_SUCCESS, the assembly/readback
    // failure code, or VK_TIMEOUT for a deadline drop).
    result.status = frame.status;
    // The frame stays in m_pendingFrames until ReleaseEncodedFrame().
    frame.acquired = true;
}

void VulkanVideoEncoderExtImpl::OnBitstreamCaptured(uint64_t frameId)
{
    m_completionCounter.fetch_add(1, std::memory_order_release);

    // Invoke lock FIRST, snapshot second. A snapshot taken outside
    // m_callbackMutex could still be invoked after SetCompletionCallback
    // returned (the store only races the read, not the invocation), so a
    // detaching caller could never know when its pUserData became safe to
    // destroy. Reading the pair inside the invoke lock closes that hole;
    // the lock order (callback -> pending) matches SetCompletionCallback
    // and any class-(c) method a user callback re-enters.
    std::lock_guard<std::mutex> invokeLock(m_callbackMutex);
    PFN_vkVideoEncoderCompletionCallback callback;
    void* userData;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        // Route records onto their PendingFrames ON the edge, not lazily at
        // the next retrieval call. Both reasons are load-bearing:
        //   1. a consumer that never retrieves (file-output mode with
        //      fire-and-forget Release -- the reference renderer) would
        //      otherwise accumulate records nothing drains until the
        //      unclaimed-captures bound stalls its submits at ~64 frames;
        //   2. when the user callback below runs, the frame it names is
        //      already acquirable, so callback-then-acquire cannot observe
        //      a pushed-but-unrouted record.
        // Lock order callback -> pending -> captured matches the retrieval
        // methods; no path anywhere takes captured before pending.
        DrainCapturesLocked();
        callback = m_completionCallback;
        userData = m_completionUserData;
    }
    // AFTER the routing above and BEFORE the no-callback early return below.
    // Both halves of that placement are load-bearing:
    //   * after: the load is ordered after this thread's m_pendingMutex
    //     critical section, which orders it against the creator's critical
    //     section in GetCompletionEventHandle() (release store, then prime
    //     scan, under the same mutex). Whichever runs first, the wakeup is
    //     delivered: if the creator ran first, the acquire load observes the
    //     published handle; if this routing ran first, DrainCapturesLocked()
    //     above has already raised hasCapture on the frame, so the creator's
    //     prime scan signals for it. Loading before the critical section
    //     (the previous placement) satisfied neither arm: the load could
    //     miss a handle whose prime scan then ran before this frame's drain,
    //     and the wakeup was lost. The placement does not change what a
    //     woken waiter finds -- the record enters the capture FIFO before
    //     the notify edge, and every retrieval entry point drains under
    //     m_pendingMutex -- what it orders is this load against the
    //     handle's creation.
    //   * before: this function returns early when no callback is
    //     registered, and a handle-only consumer -- the out-of-process case
    //     this handle exists for -- would then never be woken at all.
    // The counter increment at the top of this function stays ahead of the
    // signal, so a waiter that reads the counter immediately on wake sees
    // this frame.
    vkenc::OsCompletionEventSignal(
        m_completionEventHandle.load(std::memory_order_acquire));
    if (callback == nullptr) {
        return;
    }
    m_callbackThreadId.store(std::this_thread::get_id(),
                             std::memory_order_relaxed);
#if defined(__cpp_exceptions) && (__cpp_exceptions != 0)
    // Live only for a consumer building this library WITH exceptions and
    // WITHOUT C++17 (so the typedef's noexcept expanded to nothing). Aborting
    // is the only safe response -- we hold m_callbackMutex and our caller
    // holds the assembly ordering lock -- but an abort inside an anonymous
    // library worker is near-impossible to attribute from a core dump, so
    // name the frame first. This is the diagnosed abort the design asks for.
    try {
        callback(frameId, userData);
    } catch (...) {
        VkEncErr() << "[EncoderExt] completion callback threw for frame "
                   << frameId << "; the callback contract is noexcept -- "
                      "aborting" << std::endl;
        std::abort();
    }
#else
    // Chromium compiles this TU with -fno-exceptions, where `try` is a
    // compile error rather than dead code. There is nothing to catch: a
    // throwing callback terminates at its own throw site, before any handler
    // here could run. The contract is carried by the typedef's noexcept and
    // by the header, which is all that is available in this build.
    callback(frameId, userData);
#endif
    m_callbackThreadId.store(std::thread::id(), std::memory_order_relaxed);
}

VkResult VulkanVideoEncoderExtImpl::SetCompletionCallback(
    PFN_vkVideoEncoderCompletionCallback callback, void* pUserData,
    PFN_vkVideoEncoderUserDataRelease releaseUserData)
{
    if (IsInCompletionCallback()) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    PFN_vkVideoEncoderUserDataRelease priorRelease = nullptr;
    void*                             priorUserData = nullptr;
    // Both locks, in the OnBitstreamCaptured order (callback -> pending):
    // holding m_callbackMutex across the store makes this call a quiesce
    // point -- on return, no invocation of the PREVIOUS callback is in
    // flight or can start, so a nullptr detach lets the caller safely
    // destroy whatever the previous pUserData referenced.
    {
        std::scoped_lock locks(m_callbackMutex, m_pendingMutex);
        priorRelease  = m_completionUserDataRelease;
        priorUserData = m_completionUserData;
        m_completionCallback        = callback;
        m_completionUserData        = pUserData;
        m_completionUserDataRelease = releaseUserData;
    }
    // Released AFTER the locks drop, and only once the quiesce above
    // guarantees no invocation can still be holding it. Calling it under the
    // locks would deadlock any release that touches this encoder, and a
    // release handler is exactly the place a caller tears things down.
    if (priorRelease != nullptr) {
        priorRelease(priorUserData);
    }
    return VK_SUCCESS;
}

uint64_t VulkanVideoEncoderExtImpl::GetCompletionCounter()
{
    return m_completionCounter.load(std::memory_order_acquire);
}

VkResult VulkanVideoEncoderExtImpl::SetFrameDeadline(uint32_t deadlineMs)
{
    // 6 s is the same floor InitializeExt applies: the encoder's internal
    // fence wait caps at 5 s, and a deadline at or under that would expire
    // on the encoder's own worst-case normal latency -- turning healthy
    // frames into timed-out drops, which is worse than no deadline at all.
    static const uint64_t kMinDeadlineNs = 6000000000ull;
    static const uint64_t kDefaultNs     = 8000000000ull;

    uint64_t requestedNs = (deadlineMs == 0)
        ? kDefaultNs
        : ((uint64_t)deadlineMs * 1000000ull);
    if (requestedNs < kMinDeadlineNs) {
        VkEncErr() << "[EncoderExt] SetFrameDeadline(" << deadlineMs
                   << " ms) is at or below the internal 5 s fence wait; "
                      "clamped to 6000 ms so normal encode latency is not "
                      "reported as a timeout." << std::endl;
        requestedNs = kMinDeadlineNs;
    }
    // Class (c): the deadline is read on the locked acquire path
    // (SynthesizeTimeoutLocked); write it under the same lock instead of
    // racing those readers. The InitializeExt write needs no lock -- it
    // happens before any other thread can hold a session.
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_frameTimeoutNs = requestedNs;
    }
    return VK_SUCCESS;
}

VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::GetCompletionEventHandle(
    uint64_t* outHandle)
{
    if (outHandle == nullptr) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    uint64_t handle = m_completionEventHandle.load(std::memory_order_acquire);
    if (handle == vkenc::kOsCompletionEventNone) {
        handle = vkenc::OsCompletionEventCreate();
        if (handle == vkenc::kOsCompletionEventNone) {
            // Either this build has no adapter for the platform, or the
            // syscall failed. Both mean the same thing to a caller: branch,
            // do not wait on a handle nothing will signal. (The 0 written
            // here is a benign fill, not a sentinel: the status code is the
            // failure signal, and 0 is a legal handle value.)
            *outHandle = 0;
            return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
        }
        // Release pairs with the capture path's acquire load: the eventfd's
        // creation happens-before any signal through the published value.
        m_completionEventHandle.store(handle, std::memory_order_release);

        // PRIME. A consumer is allowed to ask for the handle mid-stream, and
        // frames that became ready BEFORE it existed were signalled into
        // kOsCompletionEventNone -- a no-op. Without this, such a consumer
        // waits on an eventfd whose backlog it can never be woken for, and
        // the symptom is indistinguishable from a library stall. One signal
        // is enough regardless of how many are already ready: the eventfd
        // accumulates, and the header's contract is a drain loop reconciled
        // against GetCompletionCounter(), never one-wake-one-frame. Any
        // frame routed after this point takes m_pendingMutex, which this
        // thread holds, so it observes the store above and signals normally.
        for (const auto& p : m_pendingFrames) {
            if (!p.acquired && p.hasCapture) {
                vkenc::OsCompletionEventSignal(handle);
                break;
            }
        }
    }
    // The same handle every time: handing out a second one would give the
    // caller something the encoder does not signal.
    *outHandle = handle;
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

VkSemaphore VulkanVideoEncoderExtImpl::GetCompletionSemaphore() const
{
    // Class (c) versus Flush()/teardown, which clear m_encoder under
    // m_pendingMutex and then release the encoder: an unlocked read here
    // raced that null-and-destroy (TOCTOU on the shared pointer, then a
    // call through an object being torn down). The getter behind the
    // lock is a plain member read, so holding m_pendingMutex across it
    // cannot deadlock and costs nothing measurable.
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    if (!m_initialized || !m_encoder) {
        return VK_NULL_HANDLE;
    }
    return m_encoder->GetCompletionTimelineSemaphore();
}

VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::ExportCompletionSemaphoreHandle(
    VkVideoEncoderExternalHandleType handleType,
    uint64_t* outHandle)
{
    if (outHandle == nullptr) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    *outHandle = 0;
    // Type gate first: a property of the request alone, answered the same
    // with or without a session -- the RegisterSemaphore ordering. The
    // reserved Win32 arm and a device that cannot export share one answer
    // and one clean caller branch.
    if (handleType != VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
    }
    // Class (c) versus Flush()/teardown: take a reference to the encoder
    // under the same lock they clear it under, then export through the
    // local reference. The reference keeps the encoder -- and the
    // semaphore it owns -- alive across the driver call below without
    // holding m_pendingMutex over a driver entry point (the capture
    // path takes this lock).
    VkSharedBaseObj<VkVideoEncoder> encoder;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (!m_initialized || !m_encoder) {
            return VK_VIDEO_ENCODER_STATUS_ERROR_NOT_INITIALIZED;
        }
        encoder = m_encoder;
    }
    VkSemaphore sem = encoder->GetCompletionTimelineSemaphore();
    if ((sem == VK_NULL_HANDLE) ||
        !encoder->IsCompletionSemaphoreExportable() ||
        (m_vkDevCtx.GetSemaphoreFdKHR == nullptr)) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
    }
    // vkGetSemaphoreFdKHR is portable Vulkan: no OS header, so this stays
    // in the main TU (the section 3.2 portability constraint). The minted
    // fd is the CALLER's -- the reverse direction from the registration
    // rule -- to close or to hand to exactly one import, which consumes it.
    VkSemaphoreGetFdInfoKHR getInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    getInfo.semaphore  = sem;
    getInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (m_vkDevCtx.GetSemaphoreFdKHR(m_vkDevCtx.getDevice(), &getInfo,
                                     &fd) != VK_SUCCESS) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    }
    *outHandle = (uint64_t)fd;
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

VkResult VulkanVideoEncoderExtImpl::GetCompletionInfo(
    VkVideoEncoderCompletionInfo* pInfo)
{
    if ((pInfo == nullptr) ||
        (pInfo->sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_COMPLETION_INFO)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // Chain walk, the QueryImageSupport shape: the known diagnostic and
    // filter structs are consumed; anything else is refused rather than
    // ignored -- an extension the library does not understand means the
    // caller asked for something it is not getting. A REPEATED known sType
    // is refused too: two links of one type mean the caller believes it is
    // getting two different things.
    VkVideoEncoderDiagnosticInfo*      diagnostic = nullptr;
    VkVideoEncoderFilterInfo*          filter     = nullptr;
    VkVideoEncoderInputResidencyInfo*  residency  = nullptr;
    VkVideoEncoderStagedSubmitInfo*    stagedSubmit = nullptr;
    VkVideoEncoderImportGuardInfo*     importGuard  = nullptr;
    VkVideoEncoderImportContentInfo*   importContent = nullptr;
    for (void* link = const_cast<void*>(pInfo->pNext); link != nullptr;) {
        // The {sType, pNext} prefix is read through a SIBLING struct type;
        // VK_ENC_PIN_CHAIN_PREFIX is what makes that sound, and `next` is
        // taken before the link is re-cast to its own type.
        auto* prefix =
            reinterpret_cast<VkVideoEncoderDiagnosticInfo*>(link);
        const VkVideoEncoderStructureType linkType = prefix->sType;
        const void* next = prefix->pNext;
        if ((linkType == VK_VIDEO_ENCODER_STRUCTURE_TYPE_DIAGNOSTIC_INFO) &&
            (diagnostic == nullptr)) {
            diagnostic = prefix;
        } else if ((linkType == VK_VIDEO_ENCODER_STRUCTURE_TYPE_FILTER_INFO) &&
                   (filter == nullptr)) {
            filter = reinterpret_cast<VkVideoEncoderFilterInfo*>(link);
        } else if ((linkType ==
                    VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_RESIDENCY_INFO) &&
                   (residency == nullptr)) {
            residency =
                reinterpret_cast<VkVideoEncoderInputResidencyInfo*>(link);
        } else if ((linkType ==
                    VK_VIDEO_ENCODER_STRUCTURE_TYPE_STAGED_SUBMIT_INFO) &&
                   (stagedSubmit == nullptr)) {
            stagedSubmit =
                reinterpret_cast<VkVideoEncoderStagedSubmitInfo*>(link);
        } else if ((linkType ==
                    VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_GUARD_INFO) &&
                   (importGuard == nullptr)) {
            importGuard =
                reinterpret_cast<VkVideoEncoderImportGuardInfo*>(link);
        } else if ((linkType ==
                    VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_CONTENT_INFO) &&
                   (importContent == nullptr)) {
            importContent =
                reinterpret_cast<VkVideoEncoderImportContentInfo*>(link);
        } else {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        link = const_cast<void*>(next);
    }
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    if (diagnostic != nullptr) {
        diagnostic->diagnosticCount = m_diagnosticCount;
        std::snprintf(diagnostic->lastDiagnostic,
                      sizeof(diagnostic->lastDiagnostic), "%s",
                      m_lastDiagnostic);
    }
    if (importGuard != nullptr) {
        // The SESSION snapshot, not this thread's record: this call is
        // class (c) -- any thread -- and the registration that produced the
        // verdict ran on the submit thread. Answered from the snapshot a
        // registration wrote and never recomputed, because "what the guard
        // did" and "what the guard would do now" are exactly the two things
        // a workaround report must not conflate.
        importGuard->state          = m_importGuardReport.state;
        importGuard->requestedCount = m_importGuardReport.requestedCount;
        importGuard->retainedCount  = m_importGuardReport.retainedCount;
        importGuard->failureStatus  = m_importGuardReport.failureStatus;
        importGuard->failureErrno   = m_importGuardReport.failureErrno;
    }
    if (importContent != nullptr) {
        // THE VERDICT CHANNEL. Answered from the probe object, which is
        // reachable under m_pendingMutex -- already held here -- for the
        // same reason m_encoder is read under it below: it is the lock the
        // teardown path clears these under.
        //
        // probeGeneration on EVERY path including the no-probe one, which is
        // the whole point of it: a caller must be able to tell "this library
        // has the observable and has nothing to report" from "this library
        // predates the observable", and every other field's empty value is
        // 0, which is also what an untouched struct holds.
        importContent->probeGeneration =
            VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION;
        if (m_contentProbe) {
            VkVideoEncoderContentProbe::Verdict verdict;
            uint32_t probed = 0;
            uint32_t damaged = 0;
            uint32_t armed = 0;
            m_contentProbe->GetSnapshot(verdict, probed, damaged, armed);
            importContent->state    = VkEncMapContentState(verdict.state);
            importContent->resource = verdict.registrationId;
            importContent->meanY    = verdict.meanY;
            importContent->meanU    = verdict.meanU;
            importContent->meanV    = verdict.meanV;
            importContent->probedRegistrationCount  = probed;
            importContent->damagedRegistrationCount = damaged;
            importContent->armedRegistrationCount   = armed;
        } else {
            // Nobody ever chained the struct onto a registration, so nothing
            // was ever armed. NOT_EVALUATED, and the zeros mean it.
            importContent->state =
                VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED;
            importContent->resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
            importContent->meanY = 0;
            importContent->meanU = 0;
            importContent->meanV = 0;
            importContent->probedRegistrationCount = 0;
            importContent->damagedRegistrationCount = 0;
            importContent->armedRegistrationCount = 0;
        }
    }
    if (filter != nullptr) {
        // Answered from the ENCODER OBJECT, never from m_encoderConfig:
        // "requested" and "happened" are precisely the two things this
        // channel exists to tell apart, so reading the config here would
        // defeat its only purpose.
        //
        // m_encoder is read under m_pendingMutex because that is the lock
        // Flush()/teardown clears it under. A torn-down or null-backend
        // session reports an honest zeroed snapshot rather than failing:
        // the call is a status query and has no other failure mode.
        filter->filterCreated = VK_FALSE;
        filter->filterType = VK_VIDEO_ENCODER_FILTER_TYPE_NONE;
        filter->filterDispatchCount = 0;
        filter->stagedCopyCount = 0;
        if (m_encoder) {
            filter->filterCreated =
                m_encoder->HasInputComputeFilter() ? VK_TRUE : VK_FALSE;
            filter->filterType = static_cast<VkVideoEncoderFilterType>(
                m_encoder->GetInputFilterKind());
            filter->filterDispatchCount =
                m_encoder->GetInputFilterDispatchCount();
            filter->stagedCopyCount = m_encoder->GetStagedCopyCount();
        }
    }
    if (residency != nullptr) {
        // Same rule as the filter block above: answered from the ENCODER
        // OBJECT, never from the registration slots. What a caller DECLARED
        // is already knowable to it -- it wrote the descriptor -- and the
        // one thing it cannot see is what the library then DID with the
        // declaration, which is the entire purpose of this channel. Reading
        // it back off the slot would answer the question the caller already
        // knows the answer to and would stay green through a regression that
        // discarded the declaration.
        //
        // Zeroed first, then filled under the same `if (m_encoder)` and the
        // same m_pendingMutex the filter block uses, so a torn-down session
        // reports an honest snapshot instead of failing a status query.
        residency->foreignAcquireCount = 0;
        residency->localAcquireCount   = 0;
        if (m_encoder) {
            residency->foreignAcquireCount =
                m_encoder->GetForeignAcquireCount();
            residency->localAcquireCount =
                m_encoder->GetLocalAcquireCount();
        }
    }
    if (stagedSubmit != nullptr) {
        // 0 / VK_QUEUE_FAMILY_IGNORED is the honest "no session" answer: a
        // torn-down encoder has no staged-input queue, and 0 is not a legal
        // VK_QUEUE_* bit so it cannot be mistaken for one.
        //
        // Read through the SAME two accessors StageInputFrame and
        // SubmitStagedInputFrame read, never re-derived here from
        // ComputeFilterActive(). Re-deriving it is exactly how the barrier
        // site and the submit site once came to be able to disagree, and a
        // reporting channel that re-derives its answer cannot witness the one
        // class of bug those two accessors exist to prevent.
        stagedSubmit->submitTypeQueueFlags = 0;
        stagedSubmit->queueFamilyIndex     = VK_QUEUE_FAMILY_IGNORED;
        if (m_encoder) {
            stagedSubmit->submitTypeQueueFlags =
                m_encoder->GetStagedInputSubmitTypeFlag();
            stagedSubmit->queueFamilyIndex =
                m_encoder->GetStagedInputSubmitQueueFamilyIdx();
        }
    }
    pInfo->completionCounter = m_completionCounter.load(std::memory_order_acquire);
    pInfo->framesTimedOut = m_framesTimedOut;
    pInfo->lateCaptures = m_lateCaptures;
    pInfo->framesCancelled = m_framesCancelled;
    uint32_t pending = 0, ready = 0, acquired = 0;
    for (const auto& p : m_pendingFrames) {
        if (p.acquired) {
            acquired++;
        } else if (p.hasCapture) {
            ready++;
        } else {
            pending++;
        }
    }
    pInfo->framesPending = pending;
    pInfo->framesReady = ready;
    pInfo->framesAcquired = acquired;
    return VK_SUCCESS;
}

// Convert an unacquired frame to a 0-byte cancelled drop. Caller holds
// m_pendingMutex. Reuses the timed-out late-capture discard machinery.
void VulkanVideoEncoderExtImpl::CancelFrameLocked(PendingFrame& frame)
{
    frame.bytes.clear();
    frame.isIdr = false;
    frame.status = VK_INCOMPLETE;  // documented as cancelled
    MarkFrameReadyLocked(frame);
    frame.timedOut = true;  // a late capture is discarded, not delivered
    m_framesCancelled++;
}

VkResult VulkanVideoEncoderExtImpl::CancelFrame(uint64_t frameId)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto& p : m_pendingFrames) {
        if (p.frameId != frameId) {
            continue;
        }
        if (p.acquired) {
            return VK_ERROR_NOT_PERMITTED_KHR;
        }
        CancelFrameLocked(p);
        return VK_SUCCESS;
    }
    return VK_ERROR_UNKNOWN;
}

VkResult VulkanVideoEncoderExtImpl::AbandonAllFrames(
    uint32_t* pAbandonedCount)
{
    std::vector<VkVideoEncoderResource> orphaned;
    uint32_t abandoned = 0;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        for (auto it = m_pendingFrames.begin(); it != m_pendingFrames.end();) {
            if (it->acquired) {
                // Out in the caller's hands; see the header.
                ++it;
                continue;
            }
            // Collect rather than release here: ReleaseResourceReference
            // takes m_resourceMutex, and releasing after m_pendingMutex
            // drops keeps these two locks strictly un-nested. The existing
            // release path notes this is the cleaner shape; a new one has no
            // reason to inherit the old one's coupling.
            if (it->resource != VK_VIDEO_ENCODER_RESOURCE_NULL) {
                orphaned.push_back(it->resource);
            }
            const uint64_t frameId = it->frameId;
            if (!it->hasCapture &&
                (m_encoder || (m_nullBackend != nullptr))) {
                // Same rule as ReleaseEncodedFrame: abandoned while still
                // PENDING means the completion record is still coming and
                // will pop unmatched. Abandonment disclaimed it; it is not
                // the late class.
                m_releasedWhilePending.insert(frameId);
            }
            for (auto r = m_readyOrder.begin(); r != m_readyOrder.end();) {
                r = (*r == frameId) ? m_readyOrder.erase(r) : r + 1;
            }
            // An abandoned frame's submission may still be executing, and
            // vkDestroySemaphore requires every batch referring to the
            // semaphore to have completed. Nothing here proves that, so the
            // release-fence semaphore waits for the teardown wait-idle
            // rather than being destroyed on a guess.
            if (it->releaseFenceSemaphore != VK_NULL_HANDLE) {
                m_unprovenSemaphores.push_back(it->releaseFenceSemaphore);
            }
            // The acquire semaphores this frame's submission waits on have
            // the same unproven status for the same reason.
            m_unprovenSemaphores.insert(m_unprovenSemaphores.end(),
                                        it->acquireFenceSemaphores.begin(),
                                        it->acquireFenceSemaphores.end());
            it = m_pendingFrames.erase(it);
            abandoned++;
        }
    }
    for (VkVideoEncoderResource resource : orphaned) {
        ReleaseResourceReference(resource);
    }
    if (abandoned > 0) {
        VkEncErr() << "[EncoderExt] abandoned " << abandoned
                   << " undelivered frame(s) and released their registration "
                      "claims" << std::endl;
    }
    if (pAbandonedCount != nullptr) {
        *pAbandonedCount = abandoned;
    }
    return VK_SUCCESS;
}

VkResult VulkanVideoEncoderExtImpl::CancelAllPendingFrames(
    uint32_t* pCancelledCount)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    uint32_t cancelled = 0;
    for (auto& p : m_pendingFrames) {
        if (!p.acquired) {
            CancelFrameLocked(p);
            cancelled++;
        }
    }
    if (pCancelledCount != nullptr) {
        *pCancelledCount = cancelled;
    }
    return VK_SUCCESS;
}

VkResult VulkanVideoEncoderExtImpl::AcquireNextEncodedFrame(
    VkVideoEncodeResult& result)
{
    // Structure-type gate: the caller passes a value-initialized (and
    // therefore self-stamped) result struct. A chained struct is version
    // skew -- nothing chains onto the result today -- and is refused
    // rather than skipped, the rule every public pNext position applies.
    if ((result.sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_ENCODE_RESULT) ||
        (result.pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    DrainCapturesLocked();
    // Completion order (design 3.4, first half): deliver from the ready
    // queue, ordered by when each frame's outcome became deliverable rather
    // than by when it was submitted. A frame that never completes therefore
    // cannot park at the head and strand the completed frames behind it --
    // head-of-line blocking is structurally impossible here, not merely
    // bounded by the deadline.
    //
    // On a healthy stream this is indistinguishable from the submit order it
    // replaces: with no GOP reorder the captures arrive in submit order, so
    // delivery stays monotonic frame for frame. The two orders diverge only
    // when a frame stalls, which is the case the design is about.
    if (TryPopReadyLocked(result)) {
        return VK_SUCCESS;
    }
    // Nothing ready: apply the deadline, 3.4's second half. Undelivered
    // frames are checked oldest-first and any past its deadline becomes a
    // 0-byte VK_TIMEOUT drop, so a consumer that would otherwise wait
    // forever makes progress. Synthesis enqueues onto the ready queue, so
    // the retry below delivers it within this same call.
    for (auto& p : m_pendingFrames) {
        if (p.acquired || p.hasCapture) {
            continue;
        }
        SynthesizeTimeoutLocked(p);
    }
    if (TryPopReadyLocked(result)) {
        return VK_SUCCESS;
    }
    return VK_NOT_READY;
}

VkResult VulkanVideoEncoderExtImpl::AcquireEncodedFrame(
    uint64_t frameId, VkVideoEncodeResult& result)
{
    if ((result.sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_ENCODE_RESULT) ||
        (result.pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    DrainCapturesLocked();
    for (auto& p : m_pendingFrames) {
        if (p.frameId != frameId) {
            continue;
        }
        if (p.acquired) {
            return VK_ERROR_NOT_PERMITTED_KHR;  // re-acquire before Release
        }
        if (!p.hasCapture && !SynthesizeTimeoutLocked(p)) {
            return VK_NOT_READY;
        }
        FillResultLocked(p, result);
        return VK_SUCCESS;
    }
    return VK_ERROR_UNKNOWN;  // never submitted, or already released
}

VkVideoEncoderFrameState VulkanVideoEncoderExtImpl::GetFrameStatus(
    uint64_t frameId)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    DrainCapturesLocked();
    for (auto& p : m_pendingFrames) {
        if (p.frameId != frameId) {
            continue;
        }
        if (p.acquired) {
            return VK_VIDEO_ENCODER_FRAME_STATE_ACQUIRED;
        }
        if (p.hasCapture) {
            return VK_VIDEO_ENCODER_FRAME_STATE_READY;
        }
        return VK_VIDEO_ENCODER_FRAME_STATE_PENDING;
    }
    return VK_VIDEO_ENCODER_FRAME_STATE_UNKNOWN;
}

VkResult VulkanVideoEncoderExtImpl::GetEncodedFrame(VkVideoEncodeResult& result)
{
    // Alias of AcquireNextEncodedFrame(), kept so existing FIFO drain loops
    // migrate unchanged.
    return AcquireNextEncodedFrame(result);
}

void VulkanVideoEncoderExtImpl::ReleaseEncodedFrame(uint64_t frameId)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto it = m_pendingFrames.begin(); it != m_pendingFrames.end(); ++it) {
        if (it->frameId == frameId) {
            // Release the encodeFrameInfo ref - returns resources to pools
            it->encodeFrameInfo = nullptr;
            const VkVideoEncoderResource resource = it->resource;
            if (!it->hasCapture &&
                (m_encoder || (m_nullBackend != nullptr))) {
                // Released while still PENDING -- no capture, no deadline
                // drop, no cancellation has landed on this entry -- which
                // the header documents as legal. The frame's completion
                // record is still on its way and will pop with no entry to
                // match; record the id so DrainCapturesLocked discards
                // that pop silently instead of counting it late. Skipped
                // when no pop can ever arrive (after Flush() the capture
                // FIFO died with m_encoder); a null-backend session gets
                // its capture source lazily from VkEncPushCapture, so its
                // records are kept even before the source exists.
                m_releasedWhilePending.insert(frameId);
            }
            // Per-frame fence disposal, and the one path that can PROVE the
            // destroy precondition. The proof has THREE terms, not two.
            //
            //   hasCapture   a capture is published only after
            //                ReadbackBitstreamData's fence wait, and the
            //                encode submit waits on the staging submit, so
            //                both submissions that could touch these
            //                semaphores have completed;
            //   !timedOut    a deadline-synthesized drop carries hasCapture
            //                with no such wait behind it;
            //   VK_SUCCESS   and neither does a FAILED capture. VkVideoEncoder
            //                publishes a record whose status IS the failure
            //                -- AssemblyWorkerThread pushes a CapturedBitstream
            //                carrying the VkResult that
            //                SyncHostOnCmdBuffComplete returned -- so a fence
            //                wait that returned VK_TIMEOUT or
            //                VK_ERROR_DEVICE_LOST arrives here as
            //                hasCapture=true, timedOut=false, and the batch it
            //                failed to wait for may still be executing. Reading
            //                only the first two terms destroyed a semaphore a
            //                live batch still referenced
            //                (VUID-vkDestroySemaphore-semaphore-01137) on
            //                precisely the frames where the GPU was in trouble.
            //
            // Everything else goes to the unproven graveyard, which
            // manufactures the precondition with a DeviceWaitIdle instead of
            // asserting it. Residual, named rather than papered over:
            // ReadbackBitstreamData's `readbackDone = false; return VK_SUCCESS`
            // early exit (VkVideoEncoder.cpp) publishes a VK_SUCCESS record
            // with no fence wait at all, on a frame whose node carried no
            // output buffer or no encode command buffer. Status cannot see
            // that leg; closing it needs a completion signal the encoder does
            // not currently expose.
            const bool completionProved =
                it->hasCapture && !it->timedOut && (it->status == VK_SUCCESS);
            if (it->releaseFenceSemaphore != VK_NULL_HANDLE) {
                if (completionProved) {
                    m_releaseFenceRetired.push_back(it->releaseFenceSemaphore);
                } else {
                    m_unprovenSemaphores.push_back(
                        it->releaseFenceSemaphore);
                }
            }
            // The acquire half rides the same proof: the submission that
            // WAITS on an acquire semaphore is the submission that signals
            // the release fence.
            if (!it->acquireFenceSemaphores.empty()) {
                std::vector<VkSemaphore>& sink =
                    completionProved ? m_releaseFenceRetired
                                     : m_unprovenSemaphores;
                sink.insert(sink.end(), it->acquireFenceSemaphores.begin(),
                            it->acquireFenceSemaphores.end());
            }
            m_pendingFrames.erase(it);
            // Prune the ready queue too. Lazy skipping alone would be
            // correct but not bounded: a consumer that only ever uses the
            // keyed AcquireEncodedFrame never pops, so stale ids would
            // accumulate for the life of the session.
            for (auto r = m_readyOrder.begin(); r != m_readyOrder.end(); ) {
                if (*r == frameId) {
                    r = m_readyOrder.erase(r);
                } else {
                    ++r;
                }
            }
            if (resource != VK_VIDEO_ENCODER_RESOURCE_NULL) {
                // Drop this frame's claim on its registration. Taking
                // m_resourceMutex after releasing m_pendingMutex would be
                // cleaner still, but the two are never held in the opposite
                // order anywhere, so this cannot deadlock.
                ReleaseResourceReference(resource);
            }
            return;
        }
    }
    // Documented no-op: already released or never submitted. Logged AND
    // recorded: the stderr line dies under silenceStdio (the shipping
    // Chromium configuration), so the diagnostic channel carries the
    // same text to GetCompletionInfo, where the consumer can put it in
    // its own logging. m_pendingMutex is held for the whole function.
    m_diagnosticCount++;
    std::snprintf(m_lastDiagnostic, sizeof(m_lastDiagnostic),
                  "ReleaseEncodedFrame(%llu): unknown frame id -- no-op "
                  "(double release, or an id never submitted)",
                  static_cast<unsigned long long>(frameId));
    VkEncErr() << "[EncoderExt] " << m_lastDiagnostic << std::endl;
}

void VulkanVideoEncoderExtImpl::GetShutdownInfo(
    VkVideoEncoderShutdownInfo* pInfo) const
{
    if (pInfo == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_shutdownMutex);
    *pInfo = m_shutdown;
}

// Latch a device loss wherever it is first seen. A wait that later returns
// VK_SUCCESS does not mean the device recovered -- it means there is nothing
// left to wait for -- and it must not be allowed to erase this.
VkResult VulkanVideoEncoderExtImpl::NoteDeviceResult(VkResult result)
{
    if (result == VK_ERROR_DEVICE_LOST) {
        std::lock_guard<std::mutex> lock(m_shutdownMutex);
        m_shutdown.deviceLostObserved = VK_TRUE;
        if (m_shutdown.firstError == VK_SUCCESS) {
            m_shutdown.firstError = result;
        }
    }
    return result;
}

// Caller holds m_shutdownMutex.
VkVideoEncoderShutdownDisposition
VulkanVideoEncoderExtImpl::ClassifyShutdownLocked() const
{
    if (m_shutdown.workersJoined == VK_FALSE) {
        // A worker that did not join may still be submitting.
        return VK_VIDEO_ENCODER_SHUTDOWN_UNPROVEN;
    }
    if (m_shutdown.deviceLostObserved != VK_FALSE) {
        // On a lost device VK_SUCCESS and VK_ERROR_DEVICE_LOST say the same
        // thing about pending use -- there is none -- and neither says
        // anything about what the shared contents now hold.
        if ((m_shutdown.lastDeviceWait == VK_SUCCESS) ||
            (m_shutdown.lastDeviceWait == VK_ERROR_DEVICE_LOST)) {
            return VK_VIDEO_ENCODER_SHUTDOWN_LOST_DEVICE_RETIRED;
        }
        return VK_VIDEO_ENCODER_SHUTDOWN_UNPROVEN;
    }
    if (m_shutdown.lastDeviceWait == VK_SUCCESS) {
        return VK_VIDEO_ENCODER_SHUTDOWN_IDLE;
    }
    return VK_VIDEO_ENCODER_SHUTDOWN_UNPROVEN;
}

// Caller holds m_shutdownMutex.
VkResult VulkanVideoEncoderExtImpl::WaitWholeDeviceLocked()
{
    // This runs even when the join or an earlier encode failed. It is the only
    // step that covers staging work whose dependent encode never submitted --
    // queued work still bound to the producer's image, which no host thread is
    // waiting on and which WaitForThreadsToComplete() therefore cannot see.
    const VkResult waited = m_vkDevCtx.DeviceWaitIdle();
    m_shutdown.lastDeviceWait = waited;
    if (waited == VK_ERROR_DEVICE_LOST) {
        m_shutdown.deviceLostObserved = VK_TRUE;
    }
    if ((waited != VK_SUCCESS) && (m_shutdown.firstError == VK_SUCCESS)) {
        m_shutdown.firstError = waited;
    }
    m_shutdown.disposition = ClassifyShutdownLocked();
    return waited;
}

VkResult VulkanVideoEncoderExtImpl::Flush()
{
    if (IsInCompletionCallback()) {
        return VK_ERROR_NOT_PERMITTED_KHR;  // class (a) from the callback
    }

    // A repeated Finish is serialized here rather than racing itself. It never
    // drains twice, never restarts workers, and returns the sticky result --
    // except that it DOES retry a terminal wait left Unproven, because that is
    // the one step whose answer can still change.
    std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);

    if (m_shutdown.shutdownComplete != VK_FALSE) {
        return m_shutdown.firstError;
    }
    if (m_shutdown.workersJoined == VK_FALSE && (!m_initialized || !m_encoder)) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }

    if (m_shutdown.workersJoined == VK_FALSE) {
        // Stop acceptance once. Submissions arriving from here on are refused
        // rather than joining a pipeline that is being torn down.
        m_shutdownStarted.store(true, std::memory_order_release);

        // Detach the completion callback before the workers go. The detach is
        // itself a quiesce point and it hands the caller's cookie back to its
        // release, which must not be left owned by an encoder about to be
        // retired. No lock is held across it.
        const VkResult detached =
            SetCompletionCallback(nullptr, nullptr, nullptr);
        m_shutdown.callbackDetached =
            (detached == VK_SUCCESS) ? VK_TRUE : VK_FALSE;

        // Wait for all pending encodes to complete. The bool is the only
        // signal the core offers; a false is recorded as an unclassified
        // shutdown error rather than reconstructed into a VkResult that
        // claims to know more than it does.
        const bool joined = m_encoder->WaitForThreadsToComplete();
        m_shutdown.workersJoined = joined ? VK_TRUE : VK_FALSE;
        if (!joined && (m_shutdown.firstError == VK_SUCCESS)) {
            m_shutdown.firstError = VK_ERROR_UNKNOWN;
        }

        // Drain the pending queue
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            for (auto& pending : m_pendingFrames) {
                if (pending.encodeFrameInfo && pending.encodeFrameInfo->encodeCmdBuffer) {
                    pending.encodeFrameInfo->encodeCmdBuffer->ResetCommandBuffer(
                        true, "EncoderExtFlush");
                }
            }
        }
    }

    const VkResult waited = WaitWholeDeviceLocked();

    if (m_shutdown.disposition == VK_VIDEO_ENCODER_SHUTDOWN_UNPROVEN) {
        // Do NOT release the core here. Nothing has established that the
        // device stopped reading the images, imported semaphores and
        // registrations it owns, and destroying them on an unproven wait is
        // precisely the use-after-free the wait exists to rule out. The
        // encoder, its resources and every producer dependency stay alive; a
        // later Flush retries the wait.
        return (m_shutdown.firstError != VK_SUCCESS) ? m_shutdown.firstError
                                                     : waited;
    }

    // Release the encoder — the destructor calls DeinitEncoder() which
    // writes any buffered bitstream (deferred frames from GOP reordering)
    // and closes the output file.
    //
    // The pointer is cleared under m_pendingMutex because DrainCapturesLocked
    // tests it and then calls through it while holding that mutex; clearing
    // it outside let the last reference drop after a reader had already
    // passed its null check. The reference is handed to |doomed| so the
    // destructor still runs OUTSIDE the lock -- it joins library threads, and
    // those threads take m_pendingMutex on the capture path.
    VkSharedBaseObj<VkVideoEncoder> doomed;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        doomed = m_encoder;
        m_encoder = nullptr;
        // The released-while-pending records are consumed by capture pops,
        // and the capture FIFO dies with the encoder released below;
        // nothing can consume them past this point. Dropped so ids from a
        // finished session cannot swallow a genuine late capture after a
        // re-init (frame ids are consumer-chosen and may recur).
        m_releasedWhilePending.clear();
    }
    doomed = nullptr;

    m_shutdown.shutdownComplete = VK_TRUE;
    return m_shutdown.firstError;
}

VkResult VulkanVideoEncoderExtImpl::DrainPendingFrames()
{
    if (IsInCompletionCallback()) {
        return VK_ERROR_NOT_PERMITTED_KHR;  // class (a) from the callback
    }
    // Non-terminal drain. Unlike Flush() above, this does NOT null m_encoder:
    // the drain pushes the deferred GOP-reorder tail (PushOrderedFrames())
    // and joins the encoder-queue + assembly threads, so every frame
    // submitted so far is encoded and its capture is retrievable via
    // GetEncodedFrame(). The encoder stays usable for retrieval / Release.
    //
    // AND FOR SUBMITTING AGAIN, which is the half a plain wait does not cover.
    // WaitForThreadsToComplete() alone leaves m_asyncAssemblyEnabled false
    // and m_assemblyThreads empty, and nothing outside InitEncoder ever set
    // them again; ProcessOrderedFrames then took its synchronous fallback,
    // which publishes no CapturedBitstream. Every frame submitted after the
    // first drain was still encoded correctly -- the bytes reached the file
    // under file output -- and NONE of them ever raised a completion edge or
    // became acquirable, so each held its PendingFrame and its resource
    // registration for the rest of the session. DrainAndRestartThreads()
    // brings the assembly workers back up, which is what makes the word
    // "non-terminal" true of the completion surface and not just of the
    // encoder object.
    if (!m_initialized || !m_encoder) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    if (!m_encoder->DrainAndRestartThreads()) {
        // The drain itself happened; only the restart failed, so everything
        // submitted before this call is still complete and retrievable. What
        // is gone is the completion surface for anything submitted after --
        // so say so here rather than let the caller discover it as frames
        // that never come back. Submits from here on are refused by
        // ProcessOrderedFrames' synchronous-fallback guard.
        VkEncErr() << "[EncoderExt] DrainPendingFrames: the assembly workers "
                      "could not be restarted; frames already submitted are "
                      "complete, but this session can no longer report "
                      "completions" << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

VkResult VulkanVideoEncoderExtImpl::Reconfigure(const VkVideoEncoderConfig& config)
{
    if (IsInCompletionCallback()) {
        return VK_ERROR_NOT_PERMITTED_KHR;  // class (a) from the callback
    }
    // Structure-type gate (see the header's versioning rules).
    if ((config.sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG) ||
        (config.pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // MVP scope: MID-STREAM RATE-CONTROL update only --
    // average/max bitrate and frame rate, folded in on the encoder thread
    // and carried by the next ENCODE_RATE_CONTROL control command.
    // Resolution, codec, profile and rate-control-MODE changes still
    // require a session re-init.
    if (!m_initialized || !m_encoder) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    // Everything this call cannot carry is REFUSED rather than discarded.
    // Returning VK_SUCCESS for a field that was ignored is how a stream ends
    // up encoded one way and described another -- for colour, that is an HDR
    // frame signalled with the previous VUI, which nothing downstream can
    // detect. The sequence header is written once at init; changing any of
    // these needs a re-init until Reconfigure can rewrite it. The input
    // declaration is refused for a different reason: it is not in the
    // sequence header at all, it is what the session's conversion was built
    // around, and it is equally unable to change under a live encoder.
    //
    // inputColorModel is the OTHER half of that declaration, and it is
    // compared AS IT RESOLVES against the format rather than as it is
    // spelled. FROM_FORMAT and the model the format already carries name the
    // same input and route identically, and the session keeps no record of
    // which spelling arrived, so refusing a re-spelling would refuse a call
    // that changes nothing. What the resolved comparison catches is the
    // re-declaration an enumerant can absorb: R8G8B8A8_UNORM resolves to
    // R'G'B' undeclared and to Y'CbCr -- AYUV -- under
    // VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR, and those take opposite arms of the
    // preprocess filter. Accepting that would answer VK_SUCCESS while the
    // session went on converting as it was built to, which is the wrong
    // picture in every pixel ComputeFilterTakesFormat exists to prevent.
    const VkVideoEncoderColorModel initColorModel = VkEncResolveColorModel(
        m_initConfig.inputFormat, m_initConfig.inputColorModel);
    const VkVideoEncoderColorModel newColorModel =
        VkEncResolveColorModel(config.inputFormat, config.inputColorModel);
    struct Immutable {
        const char* name;
        bool        changed;
    };
    const Immutable immutables[] = {
        {"codec",           config.codec != m_initConfig.codec},
        {"profile",         config.profile != m_initConfig.profile},
        {"encodeWidth",     config.encodeWidth != m_initConfig.encodeWidth},
        {"encodeHeight",    config.encodeHeight != m_initConfig.encodeHeight},
        {"inputFormat",     config.inputFormat != m_initConfig.inputFormat},
        {"inputColorModel", newColorModel != initColorModel},
        {"inputWidth",      config.inputWidth != m_initConfig.inputWidth},
        {"inputHeight",     config.inputHeight != m_initConfig.inputHeight},
        {"rateControlMode", config.rateControlMode != m_initConfig.rateControlMode},
        {"colourPrimaries",
         config.colourPrimaries != m_initConfig.colourPrimaries},
        {"transferCharacteristics",
         config.transferCharacteristics != m_initConfig.transferCharacteristics},
        {"inputTransferCharacteristics",
         config.inputTransferCharacteristics !=
             m_initConfig.inputTransferCharacteristics},
        {"matrixCoefficients",
         config.matrixCoefficients != m_initConfig.matrixCoefficients},
        {"videoFullRange",  config.videoFullRange != m_initConfig.videoFullRange},
    };
    for (const auto& field : immutables) {
        if (field.changed) {
            VkEncErr() << "[EncoderExt] Reconfigure cannot change '"
                       << field.name << "' mid-stream: it is settled at "
                          "InitializeExt -- in the sequence header written "
                          "once there, or in the input routing the session "
                          "was built around. Re-initialize the session "
                          "instead." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    // THE ENCODING LEVERS -- the same contract, applied to the fields that
    // reach the BITSTREAM rather than the sequence header. Each one below is
    // settled at InitializeExt and can change how the stream is encoded: the
    // rate-control levers (vbvBufferSize, minQp, maxQp),
    // the GOP structure the session sequences to (gopLength,
    // consecutiveBFrames, idrPeriod, closedGop) and the encode-quality
    // controls (qualityLevel, tuningMode). NONE of them is carried by the
    // ENCODE_RATE_CONTROL command this call emits -- that command carries
    // averageBitrate, maxBitrate and the frame rate, and nothing else -- so
    // answering VK_SUCCESS to a change would leave the encoder using the
    // value it was built with while the caller believed otherwise. That is
    // the stream-encoded-one-way-and-described-another shape the comment
    // above forbids, so they are REFUSED.
    //
    // Compared against the init config and refused only on a CHANGE, exactly
    // as the group above is. That is what keeps a caller working when it hands
    // back a WHOLE config rather than a minimal one -- whether it retains a
    // baseline and edits the rate fields in place, or copies the init config
    // and edits a couple. Neither shape touches a field below, so neither
    // sees a new refusal.
    //
    // constQpI/P/B ARE NOT HERE, because they are APPLIED rather than
    // refused -- see the RequestRateControlUpdate call below. They are the
    // one lever a DISABLED (constant-QP) session actually has: the four
    // fields this call already carried land in m_rateControlLayersInfo, which
    // VkVideoEncoder drops entirely (pLayers null, layerCount 0) whenever the
    // mode is DISABLED, and the mode is itself immutable -- so before that
    // change Reconfigure answered VK_SUCCESS while changing nothing whatever
    // on exactly the sessions with the least other recourse. Applying is
    // cheap because the per-frame path already exists: EncodeFrameCommon
    // copies m_encoderConfig->constQp into every frame unconditionally, so
    // updating the config on the encoder thread is the whole of it.
    //
    // minQp AND maxQp ARE NOT HERE EITHER, for a reason that corrects the
    // note this replaces. They do reach the driver only through the
    // CODEC-SPECIFIC rate-control structs; what is not true is that those
    // structs are welded to session-parameter creation. The fill that
    // produces them, EncoderConfig::GetRateControlParameters, is a pure
    // function of config state -- VkEncBuildAndProbeConfig in this same file
    // already calls it a second time, on a fresh config, with no device
    // anywhere -- and CodecHandleRateControlCmd copies its output into the
    // frame and chains it onto EVERY ENCODE_RATE_CONTROL command, not just
    // the first. So the fill can be re-invoked on the encoder thread and the
    // result rides the very command this call already causes. That
    // re-invocation is VkVideoEncoder::RefreshCodecRateControlParameters,
    // and the three cases where it would carry nothing are refused below
    // rather than answered VK_SUCCESS.
    //
    // WHY vbvBufferSize IS STILL REFUSED, which is not the same reason. It
    // is not an independent input to that fill. What the command carries is
    // virtualBufferSizeInMs, computed as vbvBufferSize * 1000 / hrdBitrate
    // and paired with initialVirtualBufferSizeInMs, computed the same way
    // from vbvInitialDelay -- and vbvInitialDelay is DERIVED FROM THE OLD
    // vbvBufferSize, once, inside the codec InitRateControl finalize step
    // that this call does not re-run. Both divide by the config hrdBitrate,
    // which a bitrate change deliberately does not update: the new bitrate
    // lands on the rate-control LAYER, leaving the config holding the
    // bitrate the session was built with. Applying vbvBufferSize alone
    // would therefore emit a CPB whose initial fullness was computed for a
    // different buffer size, against a bitrate the session is no longer
    // using -- and on a shrink it can put the initial fullness ABOVE the
    // buffer size, which is not a state to hand a driver. Making it correct
    // means folding the bitrate into the config and re-running the codec
    // finalize, which changes the semantics of the already-shipped bitrate
    // path. That is the separate change; refusing is the honest answer
    // until it is made.
    //
    // gopLength, idrPeriod and consecutiveBFrames additionally drive
    // gopStructure, which sequences frame types and DPB references --
    // updating the rate-control copy alone would tell the driver one GOP
    // while the encoder sequenced another, which is worse than refusing.
    // The refresh above would in fact carry them, which is exactly why they
    // must not become mutable without the sequencing half of the change.
    // qualityLevel is baked into the video session parameters at creation:
    // Vulkan does have a coding-control bit for it
    // (VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR, which
    // HandleCtrlCmd already emits at session start), but the session
    // parameters are CREATED against a quality level, so moving it
    // mid-stream means recreating them as well -- noted, not attempted.
    //
    // WHAT IS DELIBERATELY NOT IN EITHER GROUP. The session-creation inputs
    // (deviceId, gpuUUID, externalInstance, externalPhysicalDevice,
    // externalDevice and the two external queue-family indices) and the
    // diagnostic ones (outputPath, verbose, validate, disableFileOutput,
    // silenceStdio) remain accepted and unread. Not one of them can change
    // an encoded bit, so not one can misdescribe the stream -- the rationale
    // above does not reach them. Refusing them would gain no correctness and
    // would break a caller that builds a fresh minimal config for the
    // reconfigure instead of copying its stored one.
    const Immutable levers[] = {
        {"vbvBufferSize",   config.vbvBufferSize != m_initConfig.vbvBufferSize},
        {"gopLength",       config.gopLength != m_initConfig.gopLength},
        {"consecutiveBFrames",
         config.consecutiveBFrames != m_initConfig.consecutiveBFrames},
        {"idrPeriod",       config.idrPeriod != m_initConfig.idrPeriod},
        {"closedGop",       config.closedGop != m_initConfig.closedGop},
        {"qualityLevel",    config.qualityLevel != m_initConfig.qualityLevel},
        {"tuningMode",      config.tuningMode != m_initConfig.tuningMode},
    };
    for (const auto& field : levers) {
        if (field.changed) {
            VkEncErr() << "[EncoderExt] Reconfigure cannot change '"
                       << field.name << "' mid-stream: this call carries "
                          "averageBitrate, maxBitrate, frameRateNum, "
                          "frameRateDen and the constQp defaults, and "
                          "nothing else, so the encoder would go on using "
                          "the value it was initialized with. Re-initialize "
                          "the session instead." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    // THE CONSTANT QUANTIZERS, on the same range rule and for the same reason
    // InitializeExt applies it -- so a value refused at init cannot arrive
    // here instead. A refusal that guarded only one of the two entry points
    // would be worse than none, because it would teach the caller that the
    // value is validated.
    //
    // Judged against the codec the SESSION WAS INITIALIZED AS. config.codec
    // is refused above on a change, so on every call that reaches this line
    // the two agree; the record is the one that stays right if that ever
    // stops being true.
    //
    // Checked on the value as PASSED and not on a change, unlike the clamps
    // below: a negative member is "not named" and is skipped by the range
    // check exactly as it is skipped by the application below, and every
    // non-negative one is applied whether or not it equals the record.
    const VkResult constQpRangeResult = VkEncValidateConstQpRange(
        config.constQpI, config.constQpP, config.constQpB, m_initConfig.codec,
        "Reconfigure ");
    if (constQpRangeResult != VK_SUCCESS) {
        return constQpRangeResult;
    }

    // THE THREE PLACES A QP CLAMP CHANGE STILL HAS TO BE REFUSED, because
    // on each of them the codec fill would run and carry nothing -- which
    // is the accepted-and-ignored shape the contract at the top of this
    // function forbids, not a lesser version of it.
    //
    // Only checked ON A CHANGE, exactly as every group above is, so a
    // caller handing back its stored config is unaffected. The clamps are
    // carried LITERALLY: a zero means "no clamp", which is the reading
    // InitializeExt already gives an explicit zero, so a clamp set here can
    // also be cleared here. Anything else would be a second contract for
    // the same two fields.
    const bool qpClampChanged = (config.minQp != m_initConfig.minQp) ||
                                (config.maxQp != m_initConfig.maxQp);
    if (qpClampChanged) {
        if (m_initConfig.codec ==
            VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) {
            // EncoderConfigAV1::GetRateControlParameters reads minQIndex
            // and maxQIndex -- derived from the DEVICE capability limits --
            // and never reads these QP-unit fields at all. InitializeExt
            // rejects a non-zero clamp on an AV1 session rather than
            // ignoring it; this is that rule, at the same strength, on the
            // mid-stream path.
            VkEncErr() << "[EncoderExt] Reconfigure cannot change "
                          "minQp/maxQp on an AV1 session: these are H.26x "
                          "QP-unit clamps and AV1 rate control is "
                          "quantizer-index based, so the value would reach "
                          "nothing. Leave both as they were." << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (m_initConfig.rateControlMode ==
            VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
            // The DISABLED arm of the H.26x fill sets the codec clamp from
            // the quality-level constant QP and ignores the caller request
            // entirely, so a clamp change on a constant-QP session is
            // ignored BY CONSTRUCTION however the update is delivered.
            // constQpI/P/B are that session's lever, and they are applied.
            VkEncErr() << "[EncoderExt] Reconfigure cannot change "
                          "minQp/maxQp on a constant-QP (DISABLED) session: "
                          "that mode takes its quantizer from constQpI/P/B, "
                          "which this call does carry, and ignores the "
                          "clamps. Change the constQp values instead."
                       << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // The same syntactic range and the same inverted-window rule
        // InitializeExt applies, so a value refused at init cannot arrive
        // here instead. The DEVICE QP window is checked one layer down, in
        // RequestRateControlUpdate, which is where the window this session
        // recorded at codec-init lives.
        if ((config.minQp < 0) || (config.minQp > 51) ||
            (config.maxQp < 0) || (config.maxQp > 51)) {
            VkEncErr() << "[EncoderExt] Reconfigure minQp/maxQp outside the "
                          "H.26x QP range 0..51 (minQp=" << config.minQp
                       << ", maxQp=" << config.maxQp << ")" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if ((config.minQp > 0) && (config.maxQp > 0) &&
            (config.minQp > config.maxQp)) {
            VkEncErr() << "[EncoderExt] Reconfigure minQp " << config.minQp
                       << " > maxQp " << config.maxQp
                       << " -- inverted clamp window" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    const uint64_t averageBitrate = config.averageBitrate;
    const uint64_t maxBitrate =
        (config.maxBitrate != 0) ? config.maxBitrate : config.averageBitrate;
    // The constant-QP defaults ride the same armed update, so a session
    // that changes both changes them together. A NEGATIVE member means
    // "not specified" -- the same reading InitializeExt gives it -- and
    // leaves that quantizer alone, so a caller copying a config it built
    // for a non-CQP session, where all three sit at -1, rewrites nothing.
    //
    // The clamps ride it too, but they are carried ONLY ON A CHANGE. A
    // negative there means the update carries no clamp, which is what
    // keeps a bitrate-only reconfigure from re-invoking the codec fill it
    // has no reason to run.
    const VkResult result = m_encoder->RequestRateControlUpdate(
        averageBitrate, maxBitrate, config.frameRateNum, config.frameRateDen,
        config.constQpI, config.constQpP, config.constQpB,
        qpClampChanged ? config.minQp : -1,
        qpClampChanged ? config.maxQp : -1);
    if (result == VK_SUCCESS) {
        // Keep the record current so a later Reconfigure compares against
        // what is actually IN FORCE -- which is not always what was passed.
        // Two of these members are coerced on the way in and a third can be
        // dropped outright, and recording the raw config made the record
        // disagree with the session on exactly those:
        //
        //   * a zero maxBitrate means "track averageBitrate", and the
        //     session then runs at averageBitrate. The record said 0.
        //   * a zero frameRateNum leaves the frame rate ALONE -- both
        //     halves of it keep the values already in force. The record
        //     said 0, and stored whatever denominator came beside it.
        //   * a zero frameRateDen beside a non-zero numerator becomes 1.
        //     The record said 0.
        //
        // NOTHING COMPARED ABOVE READS THESE MEMBERS. The set written here
        // -- the two bitrates, the two frame-rate halves, the constant-QP
        // triple and the two clamps -- is disjoint from the set the
        // immutables and the levers compare, so recording the applied value
        // instead of the raw one cannot alter a single refusal decision.
        // What it changes is the record telling the truth about the
        // session, which is the only thing the record is for.
        m_initConfig.averageBitrate = (uint32_t)averageBitrate;
        m_initConfig.maxBitrate = (uint32_t)maxBitrate;
        if (config.frameRateNum != 0) {
            m_initConfig.frameRateNum = config.frameRateNum;
            m_initConfig.frameRateDen =
                (config.frameRateDen != 0) ? config.frameRateDen : 1;
        }
        // Only the quantizers actually named; an unnamed one keeps
        // whatever the record already held.
        if (config.constQpI >= 0) { m_initConfig.constQpI = config.constQpI; }
        if (config.constQpP >= 0) { m_initConfig.constQpP = config.constQpP; }
        if (config.constQpB >= 0) { m_initConfig.constQpB = config.constQpB; }
        // Carried literally, so applied and passed are the same value; the
        // assignment is a no-op when the clamp did not change.
        m_initConfig.minQp = config.minQp;
        m_initConfig.maxQp = config.maxQp;
    }
    return result;
}

bool VulkanVideoEncoderExtImpl::ComputeFilterActive() const
{
    // The session's own answer to "can this encoder convert?", as opposed to
    // the format taxonomy's "would conversion help?". Both halves matter: the
    // filter has to be compiled in (IsPreprocessComputeFilterEnabled is false
    // outright when it is not), and the session's declared input format has
    // to be one the binder built a filter for.
    //
    // Answered from the INIT-TIME SNAPSHOT, not from m_encoderConfig. This is
    // reached from SupportsFormat, which the header lists as class (c) --
    // any thread, no lock -- and Deinitialize nulls and then destroys
    // m_encoderConfig under m_pendingMutex, which this path does not take.
    // Dereferencing it here is the exact hazard the two comments at the
    // m_capsMaxWidth declaration and in GetMaxWidth exist to forbid, and it
    // is a data race on the shared_ptr itself independently of the free.
    //
    // Still VK_FALSE before InitializeExt has bound a session, which is the
    // honest answer: with no session there is no filter.
    return m_computeFilterActive.load(std::memory_order_relaxed);
}

// Whether this session's filter takes |inputFormat| under |colorModel| as its
// input. A stricter question than ComputeFilterActive(), and the one the
// registration gate actually asks: ValidateImageDescriptor admits a
// non-encode-format descriptor only when it equals
// m_encoderConfig->input.vkFormat. Answering the loose question on the query
// surface and the strict one at registration is how a producer ends up
// allocating a whole pool on a VK_TRUE it is then refused.
//
// THE COLOUR MODEL IS HALF OF THE MATCH. A filter is built for ONE declared
// pair -- VkEncDeriveFilterType reads the model the config declares and the
// format the device takes as an encode source -- and the packed 4:4:4 layouts
// put two pairs on one enumerant with OPPOSITE arms: R8G8B8A8_UNORM declared
// R'G'B' builds the forward matrix, and the same enumerant declared Y'CbCr is
// AYUV and builds the copy. Matching on the format alone would hand an AYUV
// frame to a filter that applies the R'G'B'-to-Y'CbCr matrix to samples that
// are already luma and chroma -- no error anywhere, and a wrong picture in
// every pixel.
bool VulkanVideoEncoderExtImpl::ComputeFilterTakesFormat(
    VkFormat inputFormat, VkVideoEncoderColorModel colorModel) const
{
    return m_computeFilterActive.load(std::memory_order_relaxed) &&
           (m_computeFilterInputFormat.load(std::memory_order_relaxed) ==
            inputFormat) &&
           (VkEncResolveColorModel(inputFormat, colorModel) ==
            VkEncResolveColorModel(inputFormat,
                                   SessionColorModel(inputFormat)));
}

VkVideoEncoderColorModel VulkanVideoEncoderExtImpl::SessionColorModel(
    VkFormat inputFormat) const
{
    return (inputFormat == m_sessionInputFormat.load(std::memory_order_relaxed))
               ? m_sessionInputColorModel.load(std::memory_order_relaxed)
               : VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT;
}

VkBool32 VulkanVideoEncoderExtImpl::SupportsFormat(VkFormat inputFormat) const
{
    // No declaration: the model is the session's. This is the public query,
    // which is handed a format and nothing else.
    return SupportsFormat(inputFormat,
                          VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT);
}

VkBool32 VulkanVideoEncoderExtImpl::SupportsFormat(
    VkFormat inputFormat, VkVideoEncoderColorModel declaredColorModel) const
{
    // THE MODEL THE CLASS QUESTION IS ASKED UNDER. A declaration is a
    // statement about the samples that only their producer can make, so it
    // outranks the session's model wherever one is given; and it has to,
    // because the packed 4:4:4 layouts (AYUV, Y410, Y416) ride RGBA format
    // enumerants and are otherwise indistinguishable from an ordinary R'G'B'
    // image. Classifying a declared surface under someone else's model
    // answers a question the caller did not ask, and answers it about a
    // different picture.
    //
    // FROM_FORMAT declares nothing -- it is what a zero-initialised
    // descriptor and the public query both say -- and falls back to the
    // session's model, which is the model this session negotiated for the
    // format it was initialized with and FROM_FORMAT for every other.
    const VkVideoEncoderColorModel colorModel =
        (declaredColorModel == VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT)
            ? SessionColorModel(inputFormat)
            : declaredColorModel;

    // The SESSION's answer, which is why this is a member and the taxonomy is
    // not. A format that is only encodable after a conversion is supported
    // exactly when this session can perform that conversion; answering VK_TRUE
    // without one would be the "accepted and ignored" shape, which is the
    // worst of the options -- and for the 3-plane family it is worse than
    // that, because the copy the frame would fall back to hangs the GPU.
    switch (VkEncClassifyInput(inputFormat, colorModel)) {
        case VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT:
            return VK_TRUE;
        case VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER:
            // Not merely "a filter exists" -- "the filter takes THIS declared
            // pair". A session declared in a different format, or in the same
            // format under the other colour model, has a filter built for
            // that pair, so it converts nothing for this descriptor;
            // ValidateImageDescriptor refuses the descriptor with
            // FORMAT_UNSUPPORTED, and a VK_TRUE here would be the over-promise
            // that costs the producer its pool allocation.
            return ComputeFilterTakesFormat(inputFormat, colorModel)
                       ? VK_TRUE : VK_FALSE;
        case VK_ENC_INPUT_FORMAT_UNSUPPORTED:
        default:
            return VK_FALSE;
    }
}

VkVideoEncoderColorModel VkEncResolveColorModel(
    VkFormat inputFormat, VkVideoEncoderColorModel declared)
{
    // What the FORMAT says. A Y'CbCr VkFormat is one the multi-planar table
    // places; everything else names an RGB layout. This answer is what
    // EncoderInputImageParameters::colorSpace is written from, and that field
    // is what VkEncDeriveFilterType reads to pick a filter arm, so the
    // taxonomy and the filter cannot answer differently: one feeds the other.
    const VkVideoEncoderColorModel fromFormat =
        (YcbcrVkFormatInfo(inputFormat) != nullptr)
            ? VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR
            : VK_VIDEO_ENCODER_COLOR_MODEL_RGB;

    if (declared == VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) {
        return fromFormat;
    }
    if (declared == fromFormat) {
        return declared;
    }
    // The declaration disagrees with the format. Exactly one disagreement is
    // real rather than a caller error: the packed 4:4:4 Y'CbCr layouts have no
    // Vulkan enumerant of their own and ride the RGBA ones, so Y'CbCr declared
    // over one of those is the ONLY thing that tells AYUV, Y410 and Y416 from
    // an ordinary RGBA image. PackedYcbcrFormatDesc is the single table that
    // names them, and asking it here is what keeps this from becoming a second
    // copy of that one.
    if ((declared == VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR) &&
        (PackedYcbcrFormatDesc(inputFormat) != nullptr)) {
        return VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR;
    }
    // Every other disagreement is unresolvable, and the caller hears so. RGB
    // declared over a Y'CbCr format, or Y'CbCr declared over an RGBA layout
    // that carries no packed reading, cannot be reconciled: nothing here can
    // know which of the two the caller meant, and choosing one produces a
    // picture that is plausible and wrong everywhere.
    return VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT;
}

// ---------------------------------------------------------------------------
// WHAT THIS LIBRARY ROUTES, READ OFF THE FORMAT TABLES RATHER THAN LISTED
// ---------------------------------------------------------------------------
//
// The compute filter's Y'CbCr arm is GENERATED, not written per format:
// InitYCBCRCOPY takes its bit depth from GetBitsPerChannel(planesLayout), its
// chroma block from planesLayout.secondaryPlaneSubsampledX/Y, its plane count
// from the image's aspects -- which are numberOfExtraPlanes + 1 -- and, for
// the packed 4:4:4 aliases, the channel each of Y', Cb and Cr sits in from
// PackedYcbcrFormatDesc. Every one of those is a FIELD of a table this file
// can read.
//
// So the routable set is DERIVED from those tables, and what is written here
// is the POLICY: three predicates, each carrying the reason it is one. Which
// formats satisfy them, what each converts into and how many there are is read
// out of the tables. A format the tables gain is routed with no edit here,
// which is the point of deriving it: a list kept beside the tables is a second
// statement of the same set, and what a list holds is what somebody remembered
// to type.
//
// WHAT THE LIST THIS REPLACES HELD. Eleven entries, 4:2:0 throughout on its
// converted arm, while the direct arm beside it already carried 4:4:4. Nothing
// in the filter, in these tables or in the codec layer makes 4:2:0 the
// boundary -- CodecGetVkFormat spells 4:2:2 and 4:4:4 at all three depths, and
// the generator is parameterised over subsampling on both sides -- so the
// restriction was the list's and not the library's. The derivation does not
// reproduce it.
//
// WHAT IT WIDENS IS BOTH WHAT IS ACCEPTED AND WHAT IS ADVERTISED. A converted
// entry still has to name a target the DEVICE reports before
// VkEncAdvertiseInputFormats will write it -- but that question is now put at
// the profile the entry's own binding derives, so a 4:4:4 target is asked
// about at a 4:4:4 profile and reaches the advertised list wherever the device
// takes it. For a caller that DECLARES one of these formats the class question
// is still the library question, "does this library route this input", with
// the device question asked separately where the session is created; the
// difference is that the same pair of questions is now answerable before a
// session exists.

// The plane layouts the filter's generator MODELS.
//
// YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED and YCBCR_PLANAR_STRIDE_PADDED are the
// two its read and write paths are parameterised over: both take their
// per-plane bindings from numberOfExtraPlanes and their chroma addressing from
// the subsampling bits, so neither has a per-format arm that could be missing.
//
// YCBCR_SINGLE_PLANE_INTERLEAVED -- the packed 4:2:2 family, YUY2 and UYVY and
// their 10-, 12- and 16-bit spellings -- is REFUSED, and this is the one
// exclusion that is about the generator being WRONG rather than about a limit
// elsewhere. The table gives those rows numberOfExtraPlanes = 1 although they
// are one plane, so the generator declares a two-plane read over a
// single-plane image; and the block coordinates emit one luma sample per texel
// for a format that carries two, which no field parameterises. The shader
// COMPILES either way, so admitting them would buy a plausible wrong picture
// instead of an error -- which is why the refusal is here, early, rather than
// left to be discovered.
//
// YCBCR_SINGLE_PLANE_UNNORMALIZED -- the R10X6 / R12X4 rows -- describes a
// PLANE's component format rather than an image a caller hands in, and carries
// neither subsampling nor a plane count to convert.
static bool VkEncFilterModelsLayout(const VkMpFormatInfo& mpInfo)
{
    switch (mpInfo.planesLayout.layout) {
        case YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED:
        case YCBCR_PLANAR_STRIDE_PADDED:
            return true;
        default:
            return false;
    }
}

// The component depths an encode input can be SPELLED at.
// VkVideoComponentBitDepthFlagBitsKHR names 8, 10 and 12 and nothing else, so
// a 14- or 16-bit surface has no encode input geometry to be described by and
// CodecGetVkFormat spells no target for one. This is the refusal Y416 gets and
// the reason it gets it; stating it once covers the packed half and the
// multi-planar half together.
static bool VkEncEncodableComponentDepth(uint32_t bitsPerChannel)
{
    return (bitsPerChannel == 8u) || (bitsPerChannel == 10u) ||
           (bitsPerChannel == 12u);
}

// Rung 1's depth. What vkGetPhysicalDeviceVideoFormatPropertiesKHR reports as
// an encode source is the 8- and 10-bit set: no driver table carries a 12-bit
// encode-input row, so a 12-bit semi-planar input has the encode format's
// plane layout AND its subsampling and is still rung 2, on its depth alone.
static bool VkEncDeviceReadsDepthUnconverted(uint32_t bitsPerChannel)
{
    return (bitsPerChannel == 8u) || (bitsPerChannel == 10u);
}

// The RGB spellings this library routes. The one part of the set still written
// out, and it has to be: an RGB layout is exactly what the Y'CbCr table does
// not describe, so there is no table to read here. It is READ twice -- by the
// classifier's RGB arm and by the routable enumeration -- rather than stated
// twice, which is what keeps those two from naming different sets.
//
// THE SET IS DELIBERATELY THE 8-BIT UNORM FAMILY AND NOTHING ELSE. A false
// ENCODABLE is worse than a clean refusal, so each exclusion is a positive
// decision, not an oversight:
//
//   *_SRGB (R8G8B8A8_SRGB, B8G8R8A8_SRGB, A8B8G8R8_SRGB_PACK32) -- EXCLUDED,
//     and the storage read is what decides it. VulkanFilterYuvCompute binds
//     the RGBA source as a VK_DESCRIPTOR_TYPE_STORAGE_IMAGE over one combined
//     view and reads it with imageLoad, so what an RGBA input must grant is
//     VK_IMAGE_USAGE_STORAGE_BIT -- not the sampled usage a texture read would
//     need, and no create flags at all. An *_SRGB format does not carry
//     VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT -- sRGB is a sampled-image feature
//     -- and a view whose usage includes VK_IMAGE_USAGE_STORAGE_BIT must carry
//     that feature itself (VUID-VkImageViewCreateInfo-usage-02275). An sRGB
//     view can therefore never be the STORAGE_IMAGE this filter binds, so
//     admitting the format would only move the refusal to view creation, past
//     the point where the producer can still allocate differently.
//     Independently of the access form: the filter applies the colour MATRIX
//     ONLY, with no transfer function, and that is correct precisely because
//     Rec.601/709/2020 Y'CbCr is defined on GAMMA-ENCODED R'G'B'. That is what
//     makes the _UNORM spelling of each format the one to take -- it hands the
//     stored code values to the matrix untouched.
//   A2B10G10R10_UNORM_PACK32, R16G16B16A16_UNORM -- EXCLUDED as RGBA inputs,
//     and not merely for bit depth: those two enumerants are also how Y410 and
//     Y416 are spelled, and a Y'CbCr declaration over them means exactly that.
//     Claiming them as RGBA as well would make one enumerant carry two colour
//     models on one path.
//   R16G16B16A16_SFLOAT -- EXCLUDED. scRGB is linear and unbounded; both the
//     transfer function and the out-of-[0,1] range are unhandled here.
static const VkFormat kVkEncRoutableRgbFormats[] = {
    VK_FORMAT_R8G8B8A8_UNORM,                               // RGBA8
    VK_FORMAT_B8G8R8A8_UNORM,                               // BGRA8
    VK_FORMAT_A8B8G8R8_UNORM_PACK32,                        // packed RGBA8
};

// The two-plane semi-planar Y'CbCr format at |bitsPerChannel| and this
// subsampling, or VK_FORMAT_UNDEFINED where the table names none.
//
// This IS the conversion-target derivation. The filter's Y'CbCr arm changes
// plane layout and packing and resamples neither chroma nor bit depth, so the
// target is the semi-planar row that agrees with the input on both -- a
// question for the table, rather than a list of input/target pairs kept beside
// it that could name a row the filter would not produce.
static VkFormat VkEncSemiPlanarFormatAt(uint32_t bitsPerChannel,
                                        uint32_t subsampledX,
                                        uint32_t subsampledY)
{
    for (uint32_t i = 0;; i++) {
        const VkMpFormatInfo* const mpInfo = YcbcrVkFormatInfoByIndex(i);
        if (mpInfo == nullptr) {
            break;
        }
        if ((mpInfo->planesLayout.layout !=
             YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED) ||
            (GetBitsPerChannel(mpInfo->planesLayout) != bitsPerChannel) ||
            (mpInfo->planesLayout.secondaryPlaneSubsampledX != subsampledX) ||
            (mpInfo->planesLayout.secondaryPlaneSubsampledY != subsampledY)) {
            continue;
        }
        return mpInfo->vkFormat;
    }
    return VK_FORMAT_UNDEFINED;
}

// How a Y'CbCr input reaches the encoder: which rung of the adaptation ladder
// it is on, and what it is converted into if it is converted at all. ONE
// function, because those are one decision -- a class that says a filter runs
// and a target that says no conversion exists describe different libraries.
struct VkEncYcbcrRoute {
    VkEncInputFormatClass inputClass;
    VkFormat              target;  // UNDEFINED unless the class is VIA_FILTER
};

static VkEncYcbcrRoute VkEncDeriveYcbcrRoute(VkFormat inputFormat)
{
    VkEncYcbcrRoute route = { VK_ENC_INPUT_FORMAT_UNSUPPORTED,
                              VK_FORMAT_UNDEFINED };

    // Rung 2, packed half, and the arm the colour-model declaration exists
    // for. AYUV and Y410 are Y'CbCr 4:4:4 carried one interleaved texel per
    // pixel; they have no Vulkan enumerant of their own and ride the RGBA
    // ones, so reaching this arm at all took the Y'CbCr declaration resolved
    // by the caller -- undeclared, the same enumerants are an ordinary R'G'B'
    // image and are answered by the RGB arm.
    //
    // What the filter converts for them is the PLANE COUNT, one against the
    // encode source's two, which is the mismatch the 3-plane family has and is
    // equally beyond a transfer copy. It reads them natively: the packed table
    // gives the component depth and the channel each of Y', Cb and Cr sits in,
    // and the single texel plane binds as ONE storage image rather than as
    // per-plane views.
    //
    // Y416 (R16G16B16A16_UNORM) is refused by the depth predicate although the
    // same packed table names it, and that is a decision rather than an
    // oversight: 16 bits per component is not a
    // VkVideoComponentBitDepthFlagBitsKHR, so no encode input geometry can
    // carry it and CodecGetVkFormat spells no packed 16-bit target. Admitting
    // it would replace this early, clear refusal with an opaque one raised
    // after the caller had already built a frame pool.
    //
    // NO TARGET IS NAMED on this arm, and that is not an omission either:
    // VkEncConversionTargetFormat takes no colour model and these enumerants
    // resolve to R'G'B' without one, so it never reaches here. A target
    // written here would be a claim nothing reads.
    const VkPackedYcbcrFormatDesc* const packed =
        PackedYcbcrFormatDesc(inputFormat);
    if (packed != nullptr) {
        if (VkEncEncodableComponentDepth(packed->bitDepth)) {
            route.inputClass = VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER;
        }
        return route;
    }

    const VkMpFormatInfo* const mpInfo = YcbcrVkFormatInfo(inputFormat);
    if ((mpInfo == nullptr) || !VkEncFilterModelsLayout(*mpInfo)) {
        return route;
    }
    const uint32_t bitsPerChannel = GetBitsPerChannel(mpInfo->planesLayout);
    if (!VkEncEncodableComponentDepth(bitsPerChannel)) {
        return route;
    }

    // Rung 1. The input already has the encode source's layout and the device
    // is handed it as it lies.
    //
    // THE SUBSAMPLING IS NOT FIXED AT 4:2:0. A 4:4:4 or 4:2:2 semi-planar
    // input is the encoder input of a profile at that subsampling -- H.264
    // High 4:4:4 Predictive, H.265 Range Extensions -- and the codec config
    // derives that profile from the input's own chroma subsampling, so nothing
    // else has to be asked for. What this answers is whether the LIBRARY
    // routes the input unconverted; whether THIS device exposes an encode
    // profile at that subsampling is a device question, and it is answered
    // where the session is created rather than guessed here.
    if ((mpInfo->planesLayout.layout ==
         YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED) &&
        VkEncDeviceReadsDepthUnconverted(bitsPerChannel)) {
        route.inputClass = VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT;
        return route;
    }

    // Rung 2. Either the PLANE COUNT is the mismatch -- three against the
    // encode source's two -- or, for a semi-planar input that reached here,
    // the bit depth is. The ladder's own table assigns subsampling and
    // plane-layout conversion to the compute tier and restricts the transfer
    // tier to "pure tiling mismatches", and a plane-count mismatch that takes
    // the copy anyway hangs the GPU.
    //
    // A row with no semi-planar sibling at its own depth and subsampling has
    // no route and stays UNSUPPORTED: the filter would have nothing to write
    // into, and saying otherwise is the accepted-then-refused shape this
    // taxonomy exists to prevent.
    route.target = VkEncSemiPlanarFormatAt(
        bitsPerChannel, mpInfo->planesLayout.secondaryPlaneSubsampledX,
        mpInfo->planesLayout.secondaryPlaneSubsampledY);
    if (route.target != VK_FORMAT_UNDEFINED) {
        route.inputClass = VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER;
    }
    return route;
}


VkEncInputFormatClass VkEncClassifyInput(VkFormat inputFormat,
                                         VkVideoEncoderColorModel colorModel)
{
    const VkVideoEncoderColorModel model =
        VkEncResolveColorModel(inputFormat, colorModel);

    if (model == VK_VIDEO_ENCODER_COLOR_MODEL_RGB) {
        // Rung 2, RGBA half. The set is kVkEncRoutableRgbFormats, which is
        // READ here rather than restated: the routable enumeration reads the
        // same array, so the classifier and the advertisement cannot name
        // different RGB sets. Every exclusion, and the storage-image reason
        // that decides them, is stated where that array is declared.
        //
        // These are single-plane, so VkEncInputFormatPlaneCount cannot answer
        // from the class alone -- see the note there.
        for (const VkFormat routed : kVkEncRoutableRgbFormats) {
            if (routed == inputFormat) {
                return VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER;
            }
        }
        return VK_ENC_INPUT_FORMAT_UNSUPPORTED;
    }

    if (model != VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR) {
        // The declaration and the format cannot both be true -- see
        // VkEncResolveColorModel. Refused rather than reconciled.
        return VK_ENC_INPUT_FORMAT_UNSUPPORTED;
    }

    // Y'CbCr, and the whole of the answer is derived -- see
    // VkEncDeriveYcbcrRoute, which is also what names the conversion target,
    // so a format's rung and its target are one decision rather than two.
    return VkEncDeriveYcbcrRoute(inputFormat).inputClass;
}

// The routable list, in the order the advertisement emits converted entries:
// the direct rung first, then the converted rung, then RGB.
//
// DERIVED FROM THE SAME TABLES THE CLASSIFIER READS, so the list and the
// classifier are one predicate rather than two statements of one set. The
// eleven-entry literal this replaces had to be held against the classifier by
// a test, because nothing else held them together.
//
// Within each rung the order is the FORMAT TABLE's own, which groups by depth
// and then by subsampling. That keeps the 4:2:0 entries -- the only ones a
// 4:2:0 device list can reach -- in the relative order they have always been
// advertised in.
//
// THE PACKED 4:4:4 ALIASES ARE NOT HERE, and their absence is the decision the
// public header states rather than an omission: this list carries no colour
// model, so an enumerant whose only accepted reading is the Y'CbCr one cannot
// appear as itself. AYUV's enumerant is on the list under its RGB reading;
// Y410's is not on it at all, and absence from the list is not a refusal.
namespace {

struct VkEncRoutableFormatSet {
    VkFormat formats[VK_ENC_MAX_ROUTABLE_INPUT_FORMATS];
    uint32_t count;

    VkEncRoutableFormatSet() : formats{}, count(0)
    {
        AppendYcbcrRung(VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT);
        AppendYcbcrRung(VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER);
        for (const VkFormat rgb : kVkEncRoutableRgbFormats) {
            Append(rgb);
        }
    }

    void AppendYcbcrRung(VkEncInputFormatClass rung)
    {
        for (uint32_t i = 0;; i++) {
            const VkMpFormatInfo* const mpInfo = YcbcrVkFormatInfoByIndex(i);
            if (mpInfo == nullptr) {
                break;
            }
            if (VkEncDeriveYcbcrRoute(mpInfo->vkFormat).inputClass == rung) {
                Append(mpInfo->vkFormat);
            }
        }
    }

    void Append(VkFormat format)
    {
        for (uint32_t i = 0; i < count; i++) {
            if (formats[i] == format) {
                return;
            }
        }
        // Dropping an entry would make the advertisement quietly narrower than
        // the taxonomy, which is the one failure a derived list could still
        // introduce. The bound is static_asserted against the table's own
        // length below, so this cannot fire.
        assert(count < VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
        if (count < VK_ENC_MAX_ROUTABLE_INPUT_FORMATS) {
            formats[count++] = format;
        }
    }
};

}  // anonymous namespace

static_assert(VK_ENC_MAX_ROUTABLE_INPUT_FORMATS >=
                  (YCBCR_VK_FORMAT_INFO_TABLE_SIZE +
                   (sizeof(kVkEncRoutableRgbFormats) / sizeof(VkFormat))),
              "the routable set is at most every row of the multi-planar "
              "table plus the RGB spellings, so a bound below that could drop "
              "a format the classifier routes");
const VkFormat* VkEncRoutableInputFormats(uint32_t& outCount)
{
    // Built on the first call and immutable afterwards; the initialisation is
    // thread-safe by the language, which is what lets the derivation run once
    // rather than on every classification.
    static const VkEncRoutableFormatSet kRoutable;
    outCount = kRoutable.count;
    return kRoutable.formats;
}

VkFormat VkEncConversionTargetFormat(VkFormat inputFormat,
                                     const VkFormat* deviceFormats,
                                     uint32_t deviceFormatCount)
{
    if (VkEncResolveColorModel(inputFormat,
                               VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
        VK_VIDEO_ENCODER_COLOR_MODEL_RGB) {
        // The encode source an RGB session ends up with. InitEncoder passes
        // VK_FORMAT_UNDEFINED as the encode-source request for an RGB
        // session and takes the driver's first choice, so naming the same
        // entry here makes the advertisement and the session agree by
        // construction rather than by luck.
        return ((deviceFormats != nullptr) && (deviceFormatCount > 0))
                   ? deviceFormats[0]
                   : VK_FORMAT_UNDEFINED;
    }

    // Y'CbCr. Same subsampling, same bit depth, two planes -- which is what
    // the filter's Y'CbCr arm produces and the whole of what it changes.
    //
    // DERIVED, and by the same call that classified the input, so a format's
    // rung and the target it converts into are ONE decision, not two switches
    // side by side: a second statement of the same set drifts, and the target's
    // switch naming four inputs where the classifier's names six is exactly that
    // drift.
    //
    // VK_FORMAT_UNDEFINED for a directly encodable input, which converts into
    // nothing, and for one this library does not route at all.
    return VkEncDeriveYcbcrRoute(inputFormat).target;
}

namespace {

bool VkEncFormatListContains(const VkFormat* formats, uint32_t count,
                             VkFormat format)
{
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i] == format) {
            return true;
        }
    }
    return false;
}

bool VkEncEntryListContains(const VkVideoEncoderInputFormatProperties* entries,
                            uint32_t count, VkFormat format)
{
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].format == format) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

uint32_t VkEncAdvertiseInputFormats(
    VkEncInputFormatAdmitFn admit, void* userData,
    VkVideoEncoderInputFormatProperties* outEntries, uint32_t outCapacity)
{
    if ((admit == nullptr) || (outEntries == nullptr)) {
        return 0;
    }

    // ONE CANDIDATE SET, OFFERED ONCE EACH. The routable list is what this
    // library can route at all; whether a given device and profile will take
    // any particular one of them is the admission's question, asked here and
    // answered nowhere else in this function. Walking the routable list rather
    // than a device list is what lets the admission be a live per-candidate
    // resolve: a candidate is bound at ITS OWN derived profile, and different
    // candidates therefore see different device answers.
    uint32_t routableCount = 0;
    const VkFormat* const routable = VkEncRoutableInputFormats(routableCount);

    VkVideoEncoderInputFormatProperties admitted[
        VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    uint32_t admittedCount = 0;
    for (uint32_t i = 0; (i < routableCount) &&
                         (admittedCount < VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
         i++) {
        const VkFormat format = routable[i];
        // Each format is written once however many times a device reports it,
        // because tiling is a property of an image and not of a format. The
        // routable list is already unique, so this catches only an admission
        // that answered about a format it was not asked about.
        if (VkEncEntryListContains(admitted, admittedCount, format)) {
            continue;
        }
        VkVideoEncoderInputFormatProperties entry = {};
        entry.format = format;
        if (!admit(userData, format, &entry)) {
            continue;
        }
#ifndef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
        // NOT ADVERTISED AT ALL WHEN THE FILTER IS NOT COMPILED IN. Every
        // SUBOPTIMAL entry is ENCODABLE_VIA_FILTER, and InitializeExt refuses
        // exactly that class with VK_ERROR_INITIALIZATION_FAILED when
        // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED is undefined -- the two
        // test the same predicate, so the correspondence is exact rather than
        // approximate. Advertising them in such a build would name formats the
        // library then refuses, which is the one failure this list exists to
        // prevent. The condition is the BUILD's, not the device's and not the
        // admission's, so it is applied here where no admission can bypass it.
        //
        // The OPTIMAL entries are deliberately outside this: they are
        // ENCODABLE_DIRECT, the encoder reads them as they lie, and no filter
        // is involved. A build without the filter advertises exactly the same
        // OPTIMAL set as one with it.
        if (entry.optimality != VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL) {
            continue;
        }
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
        admitted[admittedCount++] = entry;
    }

    // OPTIMAL first, then SUBOPTIMAL, each group in the routable list's order.
    // A caller reading the list top-down meets the entries the encoder takes as
    // they lie before any that cost a conversion.
    uint32_t written = 0;
    for (uint32_t pass = 0; (pass < 2u) && (written < outCapacity); pass++) {
        const VkVideoEncoderInputFormatOptimality wanted =
            (pass == 0u) ? VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL
                         : VK_VIDEO_ENCODER_INPUT_FORMAT_SUBOPTIMAL;
        for (uint32_t i = 0; (i < admittedCount) && (written < outCapacity);
             i++) {
            if (admitted[i].optimality != wanted) {
                continue;
            }
            outEntries[written++] = admitted[i];
        }
    }
    return written;
}

VkBool32 VkEncSupportsInput(VkFormat inputFormat,
                            VkVideoEncoderColorModel colorModel)
{
    return (VkEncClassifyInput(inputFormat, colorModel) !=
            VK_ENC_INPUT_FORMAT_UNSUPPORTED) ? VK_TRUE : VK_FALSE;
}

uint32_t VkEncInputFormatPlaneCount(VkFormat inputFormat)
{
    // The plane count is a property of the LAYOUT, and no class implies it.
    // RGBA is a single plane and sits in the same class as 3-plane I420;
    // 2-plane P012 sits there too, because what puts it on the filter is its
    // bit depth and not its layout. This function is what
    // EncoderConfig::input.numPlanes is written from, which drives the input
    // geometry and the staging pool shape, so answering from the class would
    // describe planes that do not exist and size allocations from them.
    //
    // A layout this library does not route has no plane count to give, and
    // that test comes FIRST: an sRGB or scRGB surface is an RGB layout the
    // library refuses, so asking "is it RGB" before "is it routed" would
    // answer 1 for an input no allocation is ever sized for.
    //
    // "Routed" is asked under BOTH readings an enumerant can carry, because a
    // packed 4:4:4 alias is routed only under a Y'CbCr declaration --
    // undeclared it reads as R'G'B', and as R'G'B' this library routes only
    // the 8-bit set. Asking under FROM_FORMAT alone answers 0 for Y410, a
    // layout this library does route and does size allocations for.
    if ((VkEncClassifyInput(inputFormat,
                            VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
         VK_ENC_INPUT_FORMAT_UNSUPPORTED) &&
        (VkEncClassifyInput(inputFormat,
                            VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR) ==
         VK_ENC_INPUT_FORMAT_UNSUPPORTED)) {
        return 0;
    }
    // The packed 4:4:4 layouts are ONE interleaved plane. Asking the packed
    // table before the colour model is what makes this answer the same under
    // either declaration, which is what the declaration-free signature
    // promises: AYUV and the R'G'B' image it shares an enumerant with are
    // both one plane, and Y410 is one plane whether or not it was declared.
    if (PackedYcbcrFormatDesc(inputFormat) != nullptr) {
        return 1;
    }
    if (VkEncResolveColorModel(inputFormat,
                               VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
        VK_VIDEO_ENCODER_COLOR_MODEL_RGB) {
        return 1;
    }
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(inputFormat);
    if (mpInfo == nullptr) {
        return 0;
    }
    return mpInfo->planesLayout.numberOfExtraPlanes + 1u;
}

void VkEncSelectImportMemoryType(const VkEncImportMemoryTypeRequest& request,
                                 const VkPhysicalDeviceMemoryProperties& memProps,
                                 VkEncImportMemoryTypeChoice* outChoice)
{
    outChoice->index = UINT32_MAX;
    outChoice->outcome = VK_ENC_IMPORT_MEMTYPE_NONE;
    outChoice->exporterIndexOverridden = VK_FALSE;

    uint32_t mask = request.requirementsMask;
    if (request.authoritativeMask != 0) {
        mask &= request.authoritativeMask;
    }

    // An index >= VK_MAX_MEMORY_TYPES cannot be shifted into a mask (and
    // the old code would have shifted it anyway -- undefined behaviour).
    const bool exporterIndexGiven = (request.exporterIndex != UINT32_MAX);
    const bool exporterIndexUsable =
        exporterIndexGiven && (request.exporterIndex < VK_MAX_MEMORY_TYPES);

    if (request.exporterIndexExact == VK_TRUE) {
        if (exporterIndexGiven) {
            // The exporter's parameters or nothing: substituting a
            // different type violates the exact-match rule, so there is
            // no valid fallback from here.
            if (exporterIndexUsable &&
                ((mask & (1u << request.exporterIndex)) != 0)) {
                outChoice->index = request.exporterIndex;
                outChoice->outcome = VK_ENC_IMPORT_MEMTYPE_EXPORTER_INDEX;
            }
            return;
        }
        // Index unknown: constrain the heuristic by the exporter's own
        // mask when it sent one. The type numbering is same-device -- a
        // declared cross-device handle already failed DEVICE_MISMATCH at
        // validation.
        if (request.exporterMask != 0) {
            mask &= request.exporterMask;
        }
    } else if (exporterIndexGiven) {
        if (exporterIndexUsable &&
            ((mask & (1u << request.exporterIndex)) != 0)) {
            outChoice->index = request.exporterIndex;
            outChoice->outcome = VK_ENC_IMPORT_MEMTYPE_EXPORTER_INDEX;
            return;
        }
        outChoice->exporterIndexOverridden = VK_TRUE;
    }

    const uint32_t typeCount = (memProps.memoryTypeCount < VK_MAX_MEMORY_TYPES)
                                   ? memProps.memoryTypeCount
                                   : VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < typeCount; i++) {
        if (((mask & (1u << i)) != 0) &&
            ((memProps.memoryTypes[i].propertyFlags &
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)) {
            outChoice->index = i;
            outChoice->outcome = VK_ENC_IMPORT_MEMTYPE_MASK_DEVICE_LOCAL;
            return;
        }
    }
    // A type the driver declares importable for this handle is spec-valid
    // and functional; refusing it over a DEVICE_LOCAL preference would turn
    // a workable registration into a failure.
    for (uint32_t i = 0; i < typeCount; i++) {
        if ((mask & (1u << i)) != 0) {
            outChoice->index = i;
            outChoice->outcome = VK_ENC_IMPORT_MEMTYPE_MASK_FIRST;
            return;
        }
    }
}

VkBool32 VkEncDescriptorWithinCreationLimits(
    const VkVideoEncoderExternalImageDescriptor& desc,
    const VkImageFormatProperties& limits)
{
    // The same 0-means-default rules ImportImageLocked applies when it
    // builds VkImageCreateInfo -- anything else and the pre-check and the
    // import judge two different images. extent.depth is the create call's
    // constant 1, inside any maxExtent a successful query can return.
    const uint32_t mipLevels =
        (desc.mipLevels == 0) ? 1u : desc.mipLevels;
    const uint32_t arrayLayers =
        (desc.arrayLayers == 0) ? 1u : desc.arrayLayers;
    const VkSampleCountFlags samples =
        (desc.samples == 0) ? VK_SAMPLE_COUNT_1_BIT : desc.samples;
    if ((desc.width > limits.maxExtent.width) ||
        (desc.height > limits.maxExtent.height)) {
        return VK_FALSE;  // VUID-VkImageCreateInfo-extent-02252/-02253
    }
    if (mipLevels > limits.maxMipLevels) {
        return VK_FALSE;  // VUID-VkImageCreateInfo-mipLevels-02255
    }
    if (arrayLayers > limits.maxArrayLayers) {
        return VK_FALSE;  // VUID-VkImageCreateInfo-arrayLayers-02256
    }
    if ((samples & limits.sampleCounts) == 0) {
        return VK_FALSE;  // VUID-VkImageCreateInfo-samples-02258
    }
    return VK_TRUE;
}

uint32_t VulkanVideoEncoderExtImpl::GetMaxWidth() const
{
    // Pre-init this is not knowable: no device has been probed, so any
    // number here is a guess the caller cannot tell apart from a measurement.
    // 8192 was that guess, and a producer sizing a pool from it before
    // InitializeExt was being told this encoder supports a resolution nothing
    // had verified. Answer 0 -- "unknown" -- and report the probed capability
    // once one exists. Read from the init-time snapshot, not m_encoderConfig:
    // this method takes no lock and Deinitialize can null that concurrently.
    return m_capsMaxWidth.load(std::memory_order_relaxed);
}

uint32_t VulkanVideoEncoderExtImpl::GetMaxHeight() const
{
    return m_capsMaxHeight.load(std::memory_order_relaxed);
}

VkResult VulkanVideoEncoderExtImpl::GetRuntimeInfo(
    VkVideoEncoderRuntimeInfo* outInfo) const
{
    // The struct gate is a property of the argument alone, answered the
    // same with or without a session -- malformed is named as malformed,
    // never as "not ready yet". A chained struct is refused rather than
    // skipped: the snapshot reset below would otherwise silently wipe the
    // caller's chain pointer, exactly the accepted-and-ignored failure the
    // sType/pNext rules exist to prevent.
    if ((outInfo == nullptr) ||
        (outInfo->sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_RUNTIME_INFO) ||
        (outInfo->pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!m_initialized) {
        return VK_NOT_READY;
    }

    *outInfo = {};

    // Static identity. Vulkan-Video encoders are HW-only by definition (no
    // SW-fallback path), and the Ext interface accepts external VkImages.
    std::snprintf(outInfo->implementationName,
                  sizeof(outInfo->implementationName),
                  "VulkanVideoEncoder");
    outInfo->isHardwareAccelerated = VK_TRUE;
    outInfo->supportsNativeHandle  = VK_TRUE;

    // trustedRateController: VK_TRUE iff the active rate-control mode is a
    // tracked/managed-bitrate mode (CBR or VBR). We trust any driver advertising
    // a tracked mode -- Vulkan-Video drivers are HW-only, so the historical
    // SW-fallback allow-list does not apply; no vendor table here. DEFAULT(0)
    // and DISABLED/const-QP(1) are not bitrate-tracked, so report VK_FALSE.
    outInfo->trustedRateController =
        ((m_rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR) ||
         (m_rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR))
            ? VK_TRUE
            : VK_FALSE;

    // MVP: simulcast is handled by N parallel sessions on the Chromium side;
    // frame-size-change requires a session re-init (the MVP Reconfigure
    // covers mid-stream rate-control only).
    outInfo->supportsSimulcast       = VK_FALSE;
    outInfo->supportsFrameSizeChange = VK_FALSE;

    // We do not yet surface per-frame average QP back through
    // VkVideoEncodeResult; report VK_FALSE until that metadata is wired.
    outInfo->reportsAverageQp = VK_FALSE;

    // Resolution alignment: report the driver's probed
    // pictureAccessGranularity once the session config carries it; 16 stays
    // the conservative pre-probe fallback (no HW encoder rejects 16-aligned
    // input, and the encoder crops internally).
    const uint32_t granularityW =
        m_capsGranularityW.load(std::memory_order_relaxed);
    const uint32_t granularityH =
        m_capsGranularityH.load(std::memory_order_relaxed);
    outInfo->requestedResolutionAlignmentWidth  =
        (granularityW > 0u) ? granularityW : 16u;
    outInfo->requestedResolutionAlignmentHeight =
        (granularityH > 0u) ? granularityH : 16u;
    outInfo->applyAlignmentToAllSimulcastLayers = VK_TRUE;

    return VK_SUCCESS;
}

void VulkanVideoEncoderExtImpl::Deinitialize()
{
    if (m_encoder) {
        m_encoder->WaitForThreadsToComplete();
    }

    // R-5 telemetry summary: the design's removal condition was "zero
    // fallbacks", a number nobody could produce. Now the session log can:
    // silence here means every import selection ran under its authoritative
    // mask.
    const uint64_t heuristicSelections =
        m_importMemTypeHeuristicSelections.load(std::memory_order_relaxed);
    const uint64_t exporterOverrides =
        m_importMemTypeExporterOverrides.load(std::memory_order_relaxed);
    if ((heuristicSelections != 0) || (exporterOverrides != 0)) {
        VkEncErr() << "[EncoderExt] import memory-type telemetry: "
                   << heuristicSelections << " heuristic selection(s), "
                   << exporterOverrides << " exporter-index override(s)"
                   << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (!m_pendingFrames.empty()) {
        VkEncErr() << "[EncoderExt] Deinitialize with "
                   << m_pendingFrames.size()
                   << " undelivered/unreleased frames; any retained "
                      "pBitstreamData pointers are invalid from here"
                   << std::endl;
    }
    for (auto& p : m_pendingFrames) {
        if (p.releaseFenceSemaphore != VK_NULL_HANDLE) {
            m_unprovenSemaphores.push_back(p.releaseFenceSemaphore);
        }
        m_unprovenSemaphores.insert(m_unprovenSemaphores.end(),
                                    p.acquireFenceSemaphores.begin(),
                                    p.acquireFenceSemaphores.end());
    }
    m_pendingFrames.clear();
    m_readyOrder.clear();
    m_releasedWhilePending.clear();
    }

    // Same one-sided-lock hazard as Flush: clear under the mutex the
    // retrieval path holds, destruct outside it.
    VkSharedBaseObj<VkVideoEncoder> doomedEncoder;
    VkSharedBaseObj<EncoderConfig>  doomedConfig;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        doomedEncoder = m_encoder;
        doomedConfig  = m_encoderConfig;
        m_encoder = nullptr;
        m_encoderConfig = nullptr;
    }
    doomedEncoder = nullptr;
    doomedConfig  = nullptr;

    // Handle-exchange teardown: registrations the consumer never retired
    // must not outlive the session -- the images and memory they own were
    // created on this session's device. The encoder threads are joined and
    // m_encoder is released above, so no frame can still hold a node ref;
    // dropping the slot refs here is the final release.
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        size_t leaked = 0;
        for (auto& slot : m_resources) {
            if (slot.live || slot.retired) {
                if (slot.inFlight != 0) {
                    VkEncErr() << "[EncoderExt] Deinitialize: registration "
                                  "still in flight (inFlight="
                               << slot.inFlight
                               << "); destroying anyway -- its frames were "
                                  "torn down with the encoder." << std::endl;
                }
                DestroyResourceLocked(slot);
                leaked++;
            }
        }
        if (leaked != 0) {
            VkEncErr() << "[EncoderExt] Deinitialize retired " << leaked
                       << " registration(s) the consumer never unregistered"
                       << std::endl;
        }
    }

    // The semaphore registry's half of the same rule: an imported
    // VkSemaphore the consumer never unregistered must not outlive the
    // session. The destroy is legal here for the same reason the encoder's
    // own completion timeline may be destroyed in DeinitEncoder: by this
    // point the encoder has been released (above, or by an earlier Flush),
    // which joins the library threads and waits the ENCODE queue idle, so
    // no submitted batch can still reference these handles
    // (VUID-vkDestroySemaphore-semaphore-01137). Slot hygiene mirrors
    // UnregisterSemaphore -- handle nulled, generation bumped -- so an id
    // that survives into a re-initialized session resolves to
    // RESOURCE_UNKNOWN rather than to a recycled slot.
    {
        std::lock_guard<std::mutex> lock(m_semaphoreMutex);
        size_t leakedSemaphores = 0;
        for (auto& slot : m_semaphores) {
            if (slot.live) {
                m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(),
                                            slot.semaphore, nullptr);
                slot.semaphore = VK_NULL_HANDLE;
                slot.live      = false;
                slot.generation++;
                leakedSemaphores++;
            }
        }
        if (leakedSemaphores != 0) {
            VkEncErr() << "[EncoderExt] Deinitialize retired "
                       << leakedSemaphores
                       << " semaphore registration(s) the consumer never "
                          "unregistered" << std::endl;
        }
    }

    // Per-frame release fences. The same legality argument as the semaphore
    // registry above -- the encoder has been released, which joins the
    // library threads and waits the ENCODE queue idle -- with one gap closed
    // explicitly: a staging submit can ride the TRANSFER queue, and an
    // ENCODE-queue wait alone does not cover a staging batch whose encode was
    // never submitted. A device-wide wait does, and teardown is the one place
    // where its cost does not matter.
    {
        std::vector<VkSemaphore> doomed;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            doomed.swap(m_releaseFenceRetired);
            doomed.insert(doomed.end(), m_unprovenSemaphores.begin(),
                          m_unprovenSemaphores.end());
            m_unprovenSemaphores.clear();
        }
        if (!doomed.empty() && (m_vkDevCtx.getDevice() != VK_NULL_HANDLE)) {
            m_vkDevCtx.DeviceWaitIdle();
            for (VkSemaphore sem : doomed) {
                m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(), sem,
                                            nullptr);
            }
        }
        m_releaseFenceExportProbed = false;
        m_releaseFenceExportable   = false;
    }

    // The dma-buf import-ordinal guard, and the last Vulkan object this
    // session destroys. At the disabled default (count 0) the guard holds
    // nothing, this finds nothing and returns 0 -- but the ORDER stays
    // load-bearing for any build that re-arms it, so the call stays where the
    // armed guard needs it. LAST is then the requirement and not a
    // preference: the guard's whole function is to hold the leading dma-buf
    // import live-positions, so it has to outlive every import this device
    // will ever make. Everything above
    // -- the encoder release that joins the library threads, the registration
    // retirement, the semaphore registry, the release fences -- is above a
    // line past which VkEncImportExternalImage is no longer reachable on
    // m_vkDevCtx; below it, ~VulkanDeviceContext destroys the device itself as
    // soon as this object's destructor body returns.
    //
    // Deinitialize() is private and the destructor is its only caller, so this
    // runs exactly once per session, on every exit path including a failed
    // init. If Deinitialize ever becomes re-enterable, this call has to move
    // with the device's lifetime and not with the session's -- releasing
    // while the device lives on un-does the phase an armed guard was built
    // for, and reports nothing.
    const uint32_t importGuardsReleased =
        VkEncReleaseImportOrdinalGuard(m_vkDevCtx);
    if (importGuardsReleased != 0) {
        VkEncErr() << "[EncoderExt] Deinitialize released "
                   << importGuardsReleased
                   << " import-ordinal guard import(s)" << std::endl;
    }

    m_initialized = false;
    m_rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
    m_capsMaxWidth.store(0, std::memory_order_relaxed);
    m_capsMaxHeight.store(0, std::memory_order_relaxed);
    m_capsGranularityW.store(0, std::memory_order_relaxed);
    m_capsGranularityH.store(0, std::memory_order_relaxed);
    m_computeFilterActive.store(false, std::memory_order_relaxed);
    m_computeFilterInputFormat.store(VK_FORMAT_UNDEFINED,
                                     std::memory_order_relaxed);
    m_sessionInputFormat.store(VK_FORMAT_UNDEFINED, std::memory_order_relaxed);
    m_sessionInputColorModel.store(VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT,
                                   std::memory_order_relaxed);
    m_importMemTypeHeuristicSelections.store(0, std::memory_order_relaxed);
    m_importMemTypeExporterOverrides.store(0, std::memory_order_relaxed);
}

//=============================================================================
// Handle exchange
//=============================================================================

// The view formats a MUTABLE_FORMAT registration must declare, written into
// |outFormats| (capacity kVkEncMaxViewFormats) and returned as a count.
//
// MUTABLE_FORMAT obliges a VkImageFormatListCreateInfo
// (VUID-VkImageCreateInfo-tiling-02353), and the list is not decoration: a
// view whose format is absent from it is invalid
// (VUID-VkImageViewCreateInfo-pNext-01585). So the list has to name every
// format the library will actually build a view over -- the descriptor's own
// format for the combined view, and each PLANE format (R8_UNORM,
// R8G8_UNORM, ...) for the per-plane views the compute filter binds.
//
// Measured on this stack (A4000, GBM dma-buf, modifier 0x0): per-plane
// STORAGE views on an imported multi-planar image are creatable, and
// validation-clean, only with MUTABLE_FORMAT | EXTENDED_USAGE *and* this
// list present. Modifier 0 for the multi-planar format carries no STORAGE in
// its tilingFeatures; modifier 0 for R8_UNORM / R8G8_UNORM does, and
// EXTENDED_USAGE is what makes the usage validate against these VIEW formats
// instead of the image format. The library never adds EXTENDED_USAGE itself
// -- it is the exporter's declaration to make, and the library never
// invents a usage or a flag the exporter did not grant -- but when the
// exporter declared MUTABLE_FORMAT the list is OURS to emit.
enum { kVkEncMaxViewFormats = 4 };

// Whether this registration's image can carry the per-plane STORAGE views the
// preprocess compute filter binds. Every clause is the DESCRIPTOR's, never
// ours: the exporter has to have created the image MUTABLE_FORMAT (a plane
// view reinterprets the format, VUID-VkImageViewCreateInfo-image-01762) and
// to have granted STORAGE (a plane view cannot carry a usage the image lacks,
// VUID-VkImageViewCreateInfo-pNext-02662).
//
// EXTENDED_USAGE is part of the requirement, not decoration. Measured on this
// stack: MUTABLE_FORMAT | EXTENDED_USAGE plus the format list is what makes
// per-plane STORAGE views creatable AND validation-clean; MUTABLE_FORMAT
// ALONE makes vkGetPhysicalDeviceImageFormatProperties2 answer
// VK_ERROR_FORMAT_NOT_SUPPORTED, because modifier 0 for the MULTI-PLANAR
// format carries no STORAGE in its tilingFeatures and EXTENDED_USAGE is what
// re-validates the usage against the VIEW formats instead. Without this
// clause a descriptor that has MUTABLE_FORMAT and STORAGE but not
// EXTENDED_USAGE passes the gate whose refusal message already names
// EXTENDED_USAGE in prose, and fails later at ModifierWouldRegister -- which
// then reports "no modifier on this device would work", when the actual fix
// is one create flag the caller was never asked for.
static bool VkEncDescriptorPermitsPlaneStorageViews(
    const VkVideoEncoderExternalImageDescriptor& desc,
    VkImageUsageFlags resolvedUsage)
{
    return ((desc.imageFlags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) &&
           ((desc.imageFlags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) != 0) &&
           ((resolvedUsage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) &&
           (YcbcrVkFormatInfo(desc.format) != nullptr);
}

// The SINGLE-PLANE counterpart -- a separate predicate rather than a relaxed
// conjunct in the one above, because the filter's two arms bind different
// VIEWS of a descriptor and collapsing them refuses one arm's shipped
// producer shape.
//
// Both arms bind VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, so both need STORAGE.
// What only the multi-planar arm needs is MUTABLE_FORMAT | EXTENDED_USAGE: a
// per-plane view reinterprets the image as a different format
// (VUID-VkImageViewCreateInfo-image-01762), and that cost belongs to the
// per-plane views, not to the format's colour model.
//
// The SINGLE-PLANE arm binds ONE descriptor over the COMBINED view:
// VulkanFilterYuvCompute sets m_inputImageAspects =
// VK_IMAGE_ASPECT_COLOR_BIT for every input the multi-planar table does not
// place -- an R'G'B' image and, equally, a packed 4:4:4 Y'CbCr surface on an
// RGBA-typed enumerant -- and UpdateImageDescriptorSets binds aspect 0
// through GetImageView(). So the arm is chosen by PLANE COUNT rather than by
// colour model: AYUV needs this arm and no create flags, exactly as R'G'B'
// does. What this gate requires of a descriptor is exactly two things:
//
//   * the image must GRANT VK_IMAGE_USAGE_STORAGE_BIT. A STORAGE_IMAGE
//     descriptor may only name a view whose image carries it
//     (VUID-VkWriteDescriptorSet-descriptorType-00339), and this library
//     never invents a usage the exporter did not grant; and
//   * the DEVICE must support a storage read of that format on that tiling
//     -- |deviceCanStorageRead|.
//
// It needs NO create flags. Nothing on this arm reinterprets the format, so
// MUTABLE_FORMAT buys nothing here, and requiring it would refuse the shipped
// producer shape (imageFlags = 0) over a constraint that belongs to the other
// arm.
static bool VkEncDescriptorPermitsStorageRead(
    const VkVideoEncoderExternalImageDescriptor& desc,
    VkImageUsageFlags resolvedUsage,
    bool deviceCanStorageRead)
{
    return (VkEncInputFormatPlaneCount(desc.format) == 1u) &&
           ((resolvedUsage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) &&
           deviceCanStorageRead;
}

static uint32_t VkEncCollectViewFormats(
    const VkVideoEncoderExternalImageDescriptor& desc,
    VkFormat outFormats[kVkEncMaxViewFormats])
{
    uint32_t count = 0;
    outFormats[count++] = desc.format;
    // Only when plane views will actually be built. A format list is a
    // CONSTRAINT the create call has to satisfy, not a hint: naming R8_UNORM
    // on a modifier whose R8 views the device does not support would turn a
    // registration that works today into a vkCreateImage failure. So the list
    // names exactly the views this registration will ask for.
    //
    // "Will be built" is VkImageResourceView::Create's condition, NOT the
    // narrower storage-view gate. Both Create() overloads build per-plane
    // views whenever the image is MUTABLE_FORMAT and the derived plane usage
    // is non-zero -- STORAGE is not required by either -- so keying the list
    // on the storage gate left a MUTABLE_FORMAT-without-STORAGE registration
    // with R8/R8G8 views whose formats are absent from the image's format
    // list (VUID-VkImageViewCreateInfo-pNext-01585). Inert on the shipped
    // path, where the producer declares imageFlags = 0 and no list is chained
    // at all, and wrong for the first producer that declares MUTABLE_FORMAT
    // for any other reason. One condition, so the query, the create and the
    // views describe one image.
    if (((desc.imageFlags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0) ||
        (YcbcrVkFormatInfo(desc.format) == nullptr)) {
        return count;
    }
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(desc.format);
    for (uint32_t plane = 0;
         (plane < 3u) && (count < kVkEncMaxViewFormats); plane++) {
        const VkFormat planeFormat = mpInfo->vkPlaneFormat[plane];
        if (planeFormat == VK_FORMAT_UNDEFINED) {
            break;
        }
        bool duplicate = false;
        for (uint32_t i = 0; i < count; i++) {
            duplicate = duplicate || (outFormats[i] == planeFormat);
        }
        if (!duplicate) {
            outFormats[count++] = planeFormat;
        }
    }
    return count;
}

// Import the descriptor into an owned VkImage + VkDeviceMemory. The full
// contract -- including the fd-ownership split on the vkAllocateMemory
// handoff -- is at the declaration in vulkan_video_encoder_ext_internal.h;
// in short, this function is the ONE place that closes the fd, and only
// on the failure exits that never handed it to the driver.

// ================= THE dma-buf IMPORT-ORDINAL GUARD ========================
//
// DISABLED BY DEFAULT. kVkEncImportOrdinalGuardCount below is 0, so none of
// this runs in a stock build and every dma-buf import on an NVIDIA device
// reports the verdict DISABLED. The mechanism is kept compiled, reachable
// and tested because it is the only lever this library has ever had on the
// defect described here, and a future driver may make some phase of it worth
// pulling again. It is NOT a fix, and the block below says why so that
// nobody re-arms it expecting one.
//
// WHAT IS WRONG, AND IT IS NOT OURS TO FIX. A dma-buf image import can come
// back bound to memory the buffer's contents never reach. THE DEFECT IS IN
// THE DRIVER'S dma-buf IMPORT PATH.
// Nothing readable at the import boundary distinguishes a damaged import
// from a good one -- descriptor, explicit DRM plane layouts, memReqs,
// vkGetMemoryFdPropertiesKHR mask, chosen memory type and the driver's own
// vkGetImageSubresourceLayout answer for
// VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT are identical on both. Only the
// CONTENT the GPU reads back differs, and there are two damage modes:
//
//   CHROMA_ZERO  plane 1 of the imported VkImage does not alias the buffer's
//                chroma pages; plane 0 does. Correct luma over zero chroma.
//                A write through this import is SWALLOWED -- the mapping is
//                dead, so a consumer cannot repair the buffer through it.
//   ALL_ZERO     Y = U = V = 0 in the buffer itself. The mapping is fine and
//                a write through it lands; what was lost is the EXPORTER's
//                write, before this library imported anything at all.
//
// Both render as one saturated green frame in a consumer, on every frame the
// affected buffer serves and on no frame of any other buffer. Legal black
// would be U = V = 128, so a zeroed plane is missing data and not a black
// frame -- which is what makes this visible at all.
//
// WHAT THE GUARD DOES: IT MOVES THE DAMAGE. IT DOES NOT REMOVE IT.
// The guard takes K sacrificial dma-buf imports and RETAINS them, so every
// caller-visible import lands K live-positions later.
//
// The damage is PERIODIC in the import ordinal. A run that registers only
// two or three buffers samples one period at one phase, so a K that moves
// the damaged ordinals off exactly those buffers reads as a fix. Over enough
// periods the PROPORTION of damaged imports does not move with K at all.
// The guard shifts the PHASE of the pattern: it changes WHICH imports are
// damaged and never HOW MANY. Anyone re-arming this is buying a phase, and
// owes the next reader which phase and why.
//
// AND THERE IS NO CONSUMER-SIDE RECOVERY:
//   * writing through a damaged import is swallowed (the CHROMA_ZERO mode
//     above), so a consumer cannot heal a damaged buffer by rewriting it.
//     Where such a write DOES land, the damage was the producer's lost write
//     and there was never anything on this side to repair;
//   * re-importing the same dma-buf at the next ordinal does not rescue it.
//     Once inflicted, the damage is a property of the BUFFER and not of the
//     import ordinal that produced it, so retrying the import is not a
//     workaround either.
// Those two properties, and not a preference for less code, are why the
// count is 0.
//
// LIFETIME -- BOTH ENDS OF IT, binding again the moment the count is not 0:
//
//   * NOT ONE IMPORT EARLY. Freeing a guard hands its live-position straight
//     back to the next caller import and un-does the phase the count was
//     chosen for -- silently, because the only symptom is green chroma in
//     the consumer's output. The guard lives until the device can no longer
//     import at all.
//   * NOT ONE CALL LATE. Guard VkImage and VkDeviceMemory still alive when
//     vkDestroyDevice is called is VUID-vkDestroyDevice-device-05137.
//     Vulkan has no rule by which a device destroys its children --
//     vkDestroyDevice destroys nothing but the device, and it is the
//     application that must have destroyed the children first. Leaving them
//     leaks two images and two allocations per device and violates the spec
//     on every teardown, invisibly unless a validation layer is running.
//
// The single point that satisfies both is
// VulkanVideoEncoderExtImpl::Deinitialize(), which the session destructor
// calls after the encoder is released and every registration retired -- so
// no further import is reachable -- and before ~VulkanDeviceContext destroys
// the device. VkEncReleaseImportOrdinalGuard() below is what it calls.
//
// COST when re-armed: K VkImage + K VkDeviceMemory and K extra references on
// the first registered dma-buf, for the lifetime of the VkDevice; no
// per-frame cost. At the default of 0 none of it is taken, and no per-device
// registry entry is created either.
//
// WHAT IS NOT ESTABLISHED, so the next reader does not over-trust any of it:
//   * WHY imports are damaged, and why periodically, is not known. The
//     pattern is an observation, not a model of the driver.
//   * It applies to the GBM dma-buf import path. A composited lane on the
//     same driver and the same descriptor shows no damage at all, so some
//     lane-level precondition is required and is unidentified.
//   * Whether any K helps on a different driver is unknown: K is a phase,
//     and the phase-invariance is a property of one import path.
// Kill switch: VK_VIDEO_ENCODER_NO_IMPORT_ORDINAL_GUARD=1, which reports the
// same DISABLED verdict the disabled default already reports.
namespace {

// THE KNOB, and the only line that has to change to re-arm the guard.
// 0 = disabled: no sacrificial import is taken and every dma-buf import on an
// NVIDIA device reports DISABLED. Non-zero re-arms it with that many retained
// imports, which buys a PHASE SHIFT of the driver's damage pattern and not a
// repair of it -- read the block above before changing this. Two suites will
// have opinions about a change here, deliberately:
//   * test/encoder-ext-import-guard pins this value device-free, in CI.
//   * test/encoder-ext-import-ordinal-guard takes --guard-count=N, which
//     re-arms its whole positional machinery on hardware for a build that
//     sets this to N.
constexpr uint32_t kVkEncImportOrdinalGuardCount = 0;

// Capacity of the per-device retained-image array, deliberately independent
// of the count above. Two reasons, both concrete: a count of 0 must not
// produce a zero-length array (not standard C++; -Wpedantic rejects it
// outright, and neither this library's nor Chromium's flags happen to pass
// it today, which is not a property to depend on), and the measured phase
// sweep ran K = 0..4, so the whole sweep stays reachable by editing the one
// line above.
constexpr uint32_t kVkEncImportOrdinalGuardCapacity = 4;
static_assert(kVkEncImportOrdinalGuardCount <= kVkEncImportOrdinalGuardCapacity,
              "kVkEncImportOrdinalGuardCount must fit the retained-image "
              "array; raise kVkEncImportOrdinalGuardCapacity with it");

// What one device's guards are. |live| is how many of |images| have actually
// been imported; it can sit below the requested count after a failed dup(2)
// or a failed sacrificial import, and the next import on the device tops it
// up. At the disabled default the guard returns before this is ever reached,
// and no entry is created at all.
struct VkEncImportOrdinalGuardEntry {
    VkDevice           device = VK_NULL_HANDLE;
    uint32_t           live   = 0;
    VkEncImportedImage images[kVkEncImportOrdinalGuardCapacity]{};
};

std::mutex g_importGuardMutex;
// One entry per VkDevice that has taken guards; erased by
// VkEncReleaseImportOrdinalGuard when that device is torn down. Guarded by
// g_importGuardMutex on every access, read and write.
//
// KEYED BY DEVICE, not a single slot, and that is a correctness point rather
// than a generality one. The single slot this replaces carried one VkDevice
// and one counter, and its device-change branch reset the counter and moved
// on. With two sessions alive at once that branch fired on every alternation
// and it (a) ORPHANED the other device's images -- the only handles to them
// were in the array it had just abandoned, so nothing could ever free them --
// and (b) lost the record that the other device was already guarded, so it
// re-guarded it on the next import and grew the leak by two more objects each
// time. A per-device entry has no such branch: a device's guards are found,
// or created, and are released exactly once by handle.
std::vector<VkEncImportOrdinalGuardEntry> g_importGuards;

// Re-entrancy: the guard builds its imports through the very function it is
// called from, so the nested calls must not try to build guards of their own.
// It is also what keeps the non-recursive mutex above safe: the nested call
// returns at the TOP of VkEncEnsureImportOrdinalGuard, above the lock.
// Dead at the disabled default, which takes no nested import at all.
thread_local bool g_inImportOrdinalGuard = false;

// What the guard did on THIS thread's most recent import attempt. See
// VkEncImportOrdinalGuardReport (internal header) for why it is
// thread_local: the guard produces it deep inside the import and
// RegisterImageResource -- the only frame that can hand it to a caller --
// consumes it on the way back out, on the same thread.
thread_local VkEncImportOrdinalGuardReport g_importGuardReport{};

void VkEncSetImportGuardVerdict(VkVideoEncoderImportGuardState state,
                                uint32_t retainedCount,
                                VkVideoEncoderStatusCode failureStatus,
                                int32_t failureErrno)
{
    g_importGuardReport.state         = state;
    g_importGuardReport.retainedCount = retainedCount;
    g_importGuardReport.failureStatus = failureStatus;
    g_importGuardReport.failureErrno  = failureErrno;
}

bool VkEncImportOrdinalGuardDisabled()
{
    // ONE INITIALIZATION, NOT A SENTINEL. The sentinel form was a
    // check-then-store on the registration path, which is reached before the
    // guard-count-zero return and well before any mutex is taken, so two
    // sessions registering at once both saw -1 and both wrote. A function-local
    // static with a dynamic initializer is initialized exactly once, and the
    // supported CMake and GN builds both keep the thread-safe guard that makes
    // that true -- none of the reviewed Chromium settings disable it. (If a
    // supported build ever did, this would need std::call_once; const alone
    // would not make dynamic initialization safe.)
    //
    // The predicate is unchanged: absent, empty and leading-zero all mean
    // enabled. The environment must be settled before concurrent use, which
    // was already true of the sentinel form.
    static const bool disabled = []() {
        const char* env = getenv("VK_VIDEO_ENCODER_NO_IMPORT_ORDINAL_GUARD");
        return (env != nullptr) && (*env != '\0') && (*env != '0');
    }();
    return disabled;
}

bool VkEncIsNvidiaDevice(const VulkanDeviceContext& vkDevCtx)
{
    // BOTH checks are load-bearing, and neither is defensive habit.
    //
    // Until the import-ordinal guard existed, VkEncImportExternalImage touched
    // no INSTANCE-level dispatch at all -- it worked entirely off the device.
    // The guard calls this on the way in to every dma-buf import, so a context
    // whose instance table was never populated now reaches a null function
    // pointer on a path a caller can reach. That is not
    // hypothetical: it segfaults all ten of Chromium's
    // VulkanVideoEncoderImportOwnershipTest cases, which construct a bare
    // VulkanDeviceContext and stub only the device-level entries the import
    // itself uses. ip=0, SEGV_MAPERR at address 0.
    //
    // The direction of the failure is chosen, not incidental. Returning false
    // means "not NVIDIA", so the guard reports NOT_NVIDIA and does not run, and
    // the import proceeds exactly as it did before the guard was added. A
    // workaround that cannot confirm the vendor it works around MUST NOT run --
    // the alternative is applying an NVIDIA-specific ordering hack to an
    // unknown driver.
    //
    // The physical-device check follows the convention already in
    // VulkanDeviceContext.h, which gates GetPhysicalDeviceMemoryProperties on
    // `if (m_physDevice)` for the same reason.
    if ((vkDevCtx.GetPhysicalDeviceProperties == nullptr) ||
        (vkDevCtx.getPhysicalDevice() == VK_NULL_HANDLE)) {
        return false;
    }
    VkPhysicalDeviceProperties props{};
    vkDevCtx.GetPhysicalDeviceProperties(vkDevCtx.getPhysicalDevice(), &props);
    return props.vendorID == 0x10DE;
}

// Index of |device|'s entry, appending an empty one on its first guarded
// import. Caller holds g_importGuardMutex. An INDEX and not a reference or a
// pointer: the caller keeps using it across a nested VkEncImportExternalImage,
// and a reference into a std::vector is exactly the thing a push_back would
// invalidate.
size_t VkEncImportOrdinalGuardEntryIndexLocked(VkDevice device)
{
    for (size_t i = 0; i < g_importGuards.size(); i++) {
        if (g_importGuards[i].device == device) {
            return i;
        }
    }
    VkEncImportOrdinalGuardEntry entry{};
    entry.device = device;
    g_importGuards.push_back(entry);
    return g_importGuards.size() - 1;
}

}  // namespace

void VkEncResetImportOrdinalGuardReport()
{
    g_importGuardReport = VkEncImportOrdinalGuardReport{};
#if defined(__linux__)
    // requestedCount is a BUILD constant, not an outcome, so it is stamped
    // here rather than on the paths that reach the guard: it must be
    // readable even when the guard never runs. Zero off Linux, where the
    // guard is not compiled -- and zero on Linux too now that the guard is
    // retired, which costs a second property this field can carry.
    // While the count was non-zero it also PROVED the library had written
    // the caller's struct, every other field's "nothing happened" value
    // being 0, which is what an untouched struct already holds. What is left
    // of that proof is |state|: it is non-zero on every path the import
    // itself reaches (NOT_APPLICABLE, NOT_NVIDIA, DISABLED, COMPLETE,
    // INCOMPLETE). A registration that never reaches the import reports
    // NOT_EVALUATED and is, at the disabled default, indistinguishable from a
    // struct nothing wrote -- so a caller or a test that needs the write
    // proved has to poison the struct first and assert the poison is gone.
    // test/encoder-ext-import-guard C1 does exactly that.
    g_importGuardReport.requestedCount = kVkEncImportOrdinalGuardCount;
#endif
}

void VkEncGetImportOrdinalGuardReport(VkEncImportOrdinalGuardReport* outReport)
{
    if (outReport != nullptr) {
        *outReport = g_importGuardReport;
    }
}

// Runs at the top of every import: a no-op on EVERY import at the retired
// default (count 0), and a no-op after the first one when armed. |osHandle|
// is only ever dup()'d here -- the caller's fd is untouched and its ownership
// rule is unaffected on every path.
static void VkEncEnsureImportOrdinalGuard(
    const VulkanDeviceContext& vkDevCtx,
    const VkVideoEncoderExternalImageDescriptor& desc,
    uint64_t osHandle)
{
#if defined(__linux__)
    if (g_inImportOrdinalGuard) {
        // A nested (sacrificial) import. It writes NO verdict: the record
        // belongs to the OUTER, caller-visible import, and overwriting it
        // here would report the guard's own recursion instead of what the
        // guard achieved.
        return;
    }
    // EVERY remaining exit names a verdict, and that is the point of this
    // block rather than a tidiness preference. The two failure paths below
    // return SILENTLY when the report they write to VkEncErr() is
    // discarded -- and the shipping Chromium configuration discards it, by
    // setting silenceStdio -- so a workaround that had degraded back to the
    // defect was indistinguishable from one that was working. The verdict
    // is the channel that survives that. See VkVideoEncoderImportGuardInfo.
    if (VkEncImportOrdinalGuardDisabled()) {
        VkEncSetImportGuardVerdict(
            VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_DISABLED, 0,
            VK_VIDEO_ENCODER_STATUS_SUCCESS, 0);
        return;
    }
    if (desc.handleType != VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF) {
        VkEncSetImportGuardVerdict(
            VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_APPLICABLE, 0,
            VK_VIDEO_ENCODER_STATUS_SUCCESS, 0);
        return;
    }
    if (!VkEncIsNvidiaDevice(vkDevCtx)) {
        // Measured on NVIDIA only. A workaround must not run where the defect
        // it works around has never been observed.
        VkEncSetImportGuardVerdict(
            VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_NVIDIA, 0,
            VK_VIDEO_ENCODER_STATUS_SUCCESS, 0);
        return;
    }
    if (kVkEncImportOrdinalGuardCount == 0) {
        // THE RETIRED DEFAULT, and DISABLED is the verdict on purpose.
        //
        // Falling through would reach the already-satisfied early return
        // below, whose 0 >= 0 is true, and report COMPLETE -- which the
        // public header defines as "retainedCount == requestedCount, caller
        // imports land at live-position requestedCount+1 or later". The
        // arithmetic holds at 0 == 0 and the claim does not: nothing was
        // moved anywhere. A verdict that says a workaround ran on a build
        // that removed it is the exact failure this reporting channel was
        // added to prevent, and it would travel straight into the embedder's
        // per-registration log.
        //
        // DISABLED already means "the workaround is off deliberately", which
        // is what a build-time count of 0 is. Reported BEFORE the registry
        // lock, so a disabled build also creates no per-device entry for the
        // release path to find.
        VkEncSetImportGuardVerdict(
            VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_DISABLED, 0,
            VK_VIDEO_ENCODER_STATUS_SUCCESS, 0);
        return;
    }
    std::lock_guard<std::mutex> lock(g_importGuardMutex);
    VkDevice device = vkDevCtx;
    if (device == VK_NULL_HANDLE) {
        // No device to hold a position on. Outside the guard's scope in the
        // same sense a non-dma-buf handle type is, and reported the same
        // way -- the import itself is about to fail on its own terms.
        VkEncSetImportGuardVerdict(
            VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_APPLICABLE, 0,
            VK_VIDEO_ENCODER_STATUS_SUCCESS, 0);
        return;
    }
    const size_t entryIdx = VkEncImportOrdinalGuardEntryIndexLocked(device);
    if (g_importGuards[entryIdx].live >= kVkEncImportOrdinalGuardCount) {
        VkEncSetImportGuardVerdict(
            VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_COMPLETE,
            g_importGuards[entryIdx].live,
            VK_VIDEO_ENCODER_STATUS_SUCCESS, 0);
        return;
    }
    while (g_importGuards[entryIdx].live < kVkEncImportOrdinalGuardCount) {
        const int guardFd = dup((int)osHandle);
        if (guardFd < 0) {
            // Captured before anything else can clobber it; the verdict
            // carries it, and errno is the whole diagnosis on this path
            // (EMFILE under fd pressure is the realistic one).
            const int dupErrno = errno;
            VkEncErr() << "[EncoderExt] import-ordinal guard: dup failed ("
                       << dupErrno << "); the guard is INCOMPLETE at "
                       << g_importGuards[entryIdx].live << " of "
                       << kVkEncImportOrdinalGuardCount << std::endl;
            VkEncSetImportGuardVerdict(
                VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_INCOMPLETE,
                g_importGuards[entryIdx].live,
                VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED, dupErrno);
            return;
        }
        VkEncImportedImage guard{};
        g_inImportOrdinalGuard = true;
        const VkVideoEncoderStatusCode status =
            VkEncImportExternalImage(vkDevCtx, desc, (uint64_t)guardFd, &guard);
        g_inImportOrdinalGuard = false;
        if (status != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            // The import consumed or closed guardFd itself on every exit.
            VkEncErr() << "[EncoderExt] import-ordinal guard: sacrificial "
                          "import "
                       << (g_importGuards[entryIdx].live + 1) << " failed ("
                       << (int)status << "); the guard is INCOMPLETE -- this "
                          "build asked to shift the import-damage phase and "
                          "did not get the shift it asked for" << std::endl;
            VkEncSetImportGuardVerdict(
                VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_INCOMPLETE,
                g_importGuards[entryIdx].live, status, 0);
            return;
        }
        // Recorded BEFORE live is bumped, so live is never a count of
        // handles the registry does not actually hold -- the release below
        // walks exactly [0, live).
        g_importGuards[entryIdx].images[g_importGuards[entryIdx].live] = guard;
        g_importGuards[entryIdx].live++;
    }
    VkEncSetImportGuardVerdict(
        VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_COMPLETE,
        g_importGuards[entryIdx].live, VK_VIDEO_ENCODER_STATUS_SUCCESS, 0);
    VkEncErr() << "[EncoderExt] import-ordinal guard: "
               << kVkEncImportOrdinalGuardCount
               << " sacrificial dma-buf imports retained; caller imports now "
                  "start at live-position "
               << (kVkEncImportOrdinalGuardCount + 1)
               << ". This is a PHASE SHIFT of the driver's import-damage "
                  "pattern and not a repair of it; the measured damage rate "
                  "is unchanged. See the block above this function."
               << std::endl;
#else
    (void)vkDevCtx;
    (void)desc;
    (void)osHandle;
#endif
}
// The other end of the guard's lifetime. Contract and the reason the count is
// returned rather than only printed: vulkan_video_encoder_ext_internal.h.
uint32_t VkEncReleaseImportOrdinalGuard(const VulkanDeviceContext& vkDevCtx)
{
#if defined(__linux__)
    const VkDevice device = vkDevCtx;
    if (device == VK_NULL_HANDLE) {
        return 0;
    }
    // Taken off the registry under the lock, destroyed outside it. Removing
    // it first is what makes a second call a no-op instead of a double free,
    // and it means no other thread can find these handles while they are
    // being destroyed.
    VkEncImportOrdinalGuardEntry doomed{};
    {
        std::lock_guard<std::mutex> lock(g_importGuardMutex);
        for (size_t i = 0; i < g_importGuards.size(); i++) {
            if (g_importGuards[i].device != device) {
                continue;
            }
            doomed = g_importGuards[i];
            g_importGuards.erase(g_importGuards.begin() + (ptrdiff_t)i);
            break;
        }
    }
    // NO QUEUE WAIT IS OWED HERE, and that is a property of what a guard is
    // rather than an assumption about the caller. A guard image is imported
    // and then never touched again -- it is never written into a command
    // buffer, never bound, never transitioned, never submitted -- so
    // VUID-vkDestroyImage-image-01000 ("must not be in use by the device") is
    // satisfied by construction. The registrations destroyed above it in
    // Deinitialize need the encoder release and the DeviceWaitIdle; these do
    // not.
    //
    // Image before memory: freeing a VkDeviceMemory that a live VkImage is
    // still bound to is VUID-vkFreeMemory-memory-00677. Same order
    // DestroyResourceLocked uses.
    for (uint32_t i = 0; i < doomed.live; i++) {
        if (doomed.images[i].image != VK_NULL_HANDLE) {
            vkDevCtx.DestroyImage(device, doomed.images[i].image, nullptr);
        }
        if (doomed.images[i].memory != VK_NULL_HANDLE) {
            // This is also what closes the dup()'d dma-buf fd the guard took:
            // from the vkAllocateMemory import chain onward the fd belongs to
            // the VkDeviceMemory, and vkFreeMemory is its release (design
            // section 2.3). Nothing close()s it here or anywhere else.
            vkDevCtx.FreeMemory(device, doomed.images[i].memory, nullptr);
        }
    }
    return doomed.live;
#else
    (void)vkDevCtx;
    return 0;
#endif
}
// ============== END dma-buf IMPORT-ORDINAL GUARD ===========================

VkVideoEncoderStatusCode VkEncImportExternalImage(
    const VulkanDeviceContext& vkDevCtx,
    const VkVideoEncoderExternalImageDescriptor& desc,
    uint64_t osHandle,
    VkEncImportedImage* outImport)
{
    VkDevice device = vkDevCtx;

    // The import-ordinal guard, RETIRED BY DEFAULT: at the shipped count of 0
    // this returns immediately with the verdict DISABLED and takes no
    // imports. It stays on the path so the knob remains reachable, reported
    // and tested. When armed it shifts which import ordinals the driver
    // damages -- not how many -- and is a no-op after the first call, and on
    // every non-NVIDIA / non-dma-buf import.
    VkEncEnsureImportOrdinalGuard(vkDevCtx, desc, osHandle);

    const bool isDrmModifier =
        (desc.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);

    VkExternalMemoryImageCreateInfo extMemCI{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    VkExternalMemoryHandleTypeFlagBits vkHandleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    switch (desc.handleType) {
        case VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF:
            vkHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            break;
        case VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_WIN32:
            vkHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            break;
        case VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_D3D11_TEXTURE:
            vkHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
            break;
        default:
            break;
    }
    extMemCI.handleTypes = vkHandleType;

    VkImageCreateInfo imageCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCI.pNext = &extMemCI;
    imageCI.imageType = (desc.imageType == 0) ? VK_IMAGE_TYPE_2D
                                              : desc.imageType;
    imageCI.format = desc.format;
    imageCI.extent = {desc.width, desc.height, 1};
    imageCI.mipLevels = (desc.mipLevels == 0) ? 1u : desc.mipLevels;
    imageCI.arrayLayers = (desc.arrayLayers == 0) ? 1u : desc.arrayLayers;
    imageCI.samples =
        (desc.samples == 0) ? VK_SAMPLE_COUNT_1_BIT : desc.samples;
    imageCI.tiling = desc.tiling;
    // The exporter's usage, never a fabricated one.
    imageCI.usage = desc.imageUsage;
    imageCI.flags = desc.imageFlags;
    imageCI.sharingMode = desc.sharingMode;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // MUTABLE_FORMAT obliges us to declare the view formats
    // (VUID-VkImageCreateInfo-tiling-02353) -- and the list must name the
    // PLANE formats too, or the per-plane views the filter binds are invalid
    // (VUID-VkImageViewCreateInfo-pNext-01585). See VkEncCollectViewFormats.
    VkFormat viewFormats[kVkEncMaxViewFormats] = {};
    const uint32_t viewFormatCount =
        VkEncCollectViewFormats(desc, viewFormats);
    VkImageFormatListCreateInfo formatListCI{
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    formatListCI.viewFormatCount = viewFormatCount;
    formatListCI.pViewFormats = viewFormats;

    VkSubresourceLayout planeLayouts[VK_VIDEO_ENCODER_MAX_PLANES]{};
    VkImageDrmFormatModifierExplicitCreateInfoEXT drmExplicit{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    if (isDrmModifier) {
        for (uint32_t i = 0;
             (i < desc.planeCount) && (i < VK_VIDEO_ENCODER_MAX_PLANES); i++) {
            planeLayouts[i].offset     = desc.planeLayouts[i].offset;
            // Must be 0 on the explicit path (VUID-...-size-02267).
            planeLayouts[i].size       = 0;
            planeLayouts[i].rowPitch   = desc.planeLayouts[i].rowPitch;
            planeLayouts[i].arrayPitch = desc.planeLayouts[i].arrayPitch;
            planeLayouts[i].depthPitch = desc.planeLayouts[i].depthPitch;
        }
        drmExplicit.drmFormatModifier = desc.drmFormatModifier;
        drmExplicit.drmFormatModifierPlaneCount = desc.planeCount;
        drmExplicit.pPlaneLayouts = planeLayouts;
        drmExplicit.pNext = &extMemCI;
        imageCI.pNext = &drmExplicit;
    }
    if ((desc.imageFlags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) {
        formatListCI.pNext = imageCI.pNext;
        imageCI.pNext = &formatListCI;
    }

    VkResult result =
        vkDevCtx.CreateImage(device, &imageCI, nullptr, &outImport->image);
    if (result != VK_SUCCESS) {
        VkEncErr() << "[EncoderExt] register: vkCreateImage failed ("
                   << result << ")" << std::endl;
        // Nothing was handed to the driver yet: the fd is the library's
        // to close, on this exit like every other pre-handoff one.
        VkEncConsumeOsHandle(desc.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    }

    VkMemoryRequirements memReqs{};
    vkDevCtx.GetImageMemoryRequirements(device, outImport->image, &memReqs);

    // Memory-type selection. The candidate SET is spec-constrained, not
    // guessable; the preference ORDER within it is the design's policy
    // (exporter's index if representable, else DEVICE_LOCAL).
    //
    //  - DMA_BUF: the importable types for THIS fd are what
    //    vkGetMemoryFdPropertiesKHR reports, intersected with the image's
    //    requirements (VUID-VkMemoryAllocateInfo-memoryTypeIndex-00648).
    //    A type outside that mask imports "successfully" on NVIDIA and
    //    reads garbage where import-capable heaps are segregated. If the
    //    query is unavailable, the legacy heuristic runs as a LOGGED
    //    fallback, never silently (risk R-5).
    //  - OPAQUE_FD: querying is forbidden
    //    (VUID-vkGetMemoryFdPropertiesKHR-handleType-00674) and the import
    //    must reuse the exporter's own allocation parameters
    //    (VUID-VkMemoryAllocateInfo-allocationSize-01742), which only the
    //    descriptor can carry.
    VkEncImportMemoryTypeRequest typeRequest{};
    typeRequest.requirementsMask = memReqs.memoryTypeBits;
    typeRequest.exporterMask     = desc.memoryTypeBits;
    typeRequest.exporterIndex    = desc.memoryTypeIndex;

#if defined(__linux__)
    if (vkHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
        // Query only -- it does not consume the fd. The handoff is the
        // vkAllocateMemory call below: until then a failure leaves the fd
        // ours to close, and from that call on it never is again.
        VkMemoryFdPropertiesKHR fdProps{
            VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        VkResult propsResult = VK_ERROR_EXTENSION_NOT_PRESENT;
        if (vkDevCtx.GetMemoryFdPropertiesKHR != nullptr) {
            propsResult = vkDevCtx.GetMemoryFdPropertiesKHR(
                device, vkHandleType, (int)osHandle, &fdProps);
        }
        if (propsResult == VK_SUCCESS) {
            if (fdProps.memoryTypeBits == 0) {
                // A successful query that names NO importable type is a
                // constraint, not an absence of one: the selector reads a
                // zero mask as unconstrained and would pick a type this fd
                // cannot legally import into (garbage reads where
                // import-capable heaps are segregated).
                VkEncErr() << "[EncoderExt] register: "
                              "vkGetMemoryFdPropertiesKHR reports no "
                              "importable memory type for this fd"
                           << std::endl;
                vkDevCtx.DestroyImage(device, outImport->image, nullptr);
                outImport->image = VK_NULL_HANDLE;
                // Still pre-handoff: nothing reached the driver, so the
                // close is the library's obligation here.
                VkEncConsumeOsHandle(desc.handleType, osHandle);
                return VK_VIDEO_ENCODER_STATUS_ERROR_MEMORY_TYPE_UNSUPPORTED;
            }
            typeRequest.authoritativeMask = fdProps.memoryTypeBits;
        } else {
            outImport->heuristicSelections += 1;
            VkEncErr() << "[EncoderExt] register: "
                       << "vkGetMemoryFdPropertiesKHR unavailable ("
                       << propsResult << "); selecting the memory type "
                       << "heuristically (R-5 fallback)" << std::endl;
        }
    } else {
        // OPAQUE_FD: the exporter's exact allocation parameters or nothing.
        // The type numbering is same-device -- a declared cross-device
        // handle already failed DEVICE_MISMATCH at validation.
        typeRequest.exporterIndexExact = VK_TRUE;
    }
#elif defined(_WIN32)
    if (vkHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT) {
        // VUID-VkMemoryAllocateInfo-allocationSize-01743: exact match; the
        // analogous query is forbidden for opaque handles
        // (VUID-vkGetMemoryWin32HandlePropertiesKHR-handleType-00666). On
        // WDDM a guessed type is a hard error, never a fallback: the OS
        // validates allocation properties downstream, where nothing names
        // the cause.
        if (desc.memoryTypeIndex == UINT32_MAX) {
            VkEncErr() << "[EncoderExt] register: an OPAQUE_WIN32 import "
                       << "requires the exporter's memoryTypeIndex"
                       << std::endl;
            vkDevCtx.DestroyImage(device, outImport->image, nullptr);
            outImport->image = VK_NULL_HANDLE;
            return VK_VIDEO_ENCODER_STATUS_ERROR_MEMORY_TYPE_UNSUPPORTED;
        }
        typeRequest.exporterIndexExact = VK_TRUE;
    } else {
        // D3D11_TEXTURE: an NT handle created outside the Vulkan API, so
        // vkGetMemoryWin32HandlePropertiesKHR is the authoritative
        // constraint (VUID-VkMemoryAllocateInfo-memoryTypeIndex-00645),
        // and running without it is not an option on WDDM.
        VkMemoryWin32HandlePropertiesKHR win32Props{
            VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        VkResult propsResult = VK_ERROR_EXTENSION_NOT_PRESENT;
        if (vkDevCtx.GetMemoryWin32HandlePropertiesKHR != nullptr) {
            propsResult = vkDevCtx.GetMemoryWin32HandlePropertiesKHR(
                device, vkHandleType, (HANDLE)osHandle, &win32Props);
        }
        if (propsResult != VK_SUCCESS) {
            VkEncErr() << "[EncoderExt] register: "
                       << "vkGetMemoryWin32HandlePropertiesKHR failed ("
                       << propsResult << "); refusing to guess a memory "
                       << "type on WDDM" << std::endl;
            vkDevCtx.DestroyImage(device, outImport->image, nullptr);
            outImport->image = VK_NULL_HANDLE;
            return VK_VIDEO_ENCODER_STATUS_ERROR_MEMORY_TYPE_UNSUPPORTED;
        }
        if (win32Props.memoryTypeBits == 0) {
            // A successful query that names NO importable type is a
            // constraint, not an absence of one: the selector reads a zero
            // mask as unconstrained and would pick a type this handle
            // cannot legally import into.
            VkEncErr() << "[EncoderExt] register: "
                          "vkGetMemoryWin32HandlePropertiesKHR reports no "
                          "importable memory type for this handle"
                       << std::endl;
            vkDevCtx.DestroyImage(device, outImport->image, nullptr);
            outImport->image = VK_NULL_HANDLE;
            return VK_VIDEO_ENCODER_STATUS_ERROR_MEMORY_TYPE_UNSUPPORTED;
        }
        typeRequest.authoritativeMask = win32Props.memoryTypeBits;
    }
#endif

    VkPhysicalDeviceMemoryProperties memProps{};
    vkDevCtx.GetPhysicalDeviceMemoryProperties(
        vkDevCtx.getPhysicalDevice(), &memProps);

    VkEncImportMemoryTypeChoice typeChoice{};
    VkEncSelectImportMemoryType(typeRequest, memProps, &typeChoice);

    if (typeChoice.outcome == VK_ENC_IMPORT_MEMTYPE_NONE) {
        VkEncErr() << "[EncoderExt] register: no importable memory type: "
                   << "requirements mask 0x" << std::hex
                   << typeRequest.requirementsMask
                   << ", handle-properties mask 0x"
                   << typeRequest.authoritativeMask << std::dec
                   << ", exporter index "
                   << (int64_t)desc.memoryTypeIndex << std::endl;
        vkDevCtx.DestroyImage(device, outImport->image, nullptr);
        outImport->image = VK_NULL_HANDLE;
        // Still pre-handoff: nothing reached the driver, so the close is
        // the library's obligation here.
        VkEncConsumeOsHandle(desc.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_MEMORY_TYPE_UNSUPPORTED;
    }
    if (typeChoice.exporterIndexOverridden == VK_TRUE) {
        outImport->exporterOverrides += 1;
        VkEncErr() << "[EncoderExt] register: exporter memoryTypeIndex "
                   << desc.memoryTypeIndex << " is outside the importable "
                   << "mask; using type " << typeChoice.index << " instead"
                   << std::endl;
    }
    if ((typeRequest.exporterIndexExact == VK_TRUE) &&
        (typeChoice.outcome != VK_ENC_IMPORT_MEMTYPE_EXPORTER_INDEX)) {
        // Opaque import without the exporter's index: the selection is a
        // heuristic, and a heuristic here must never be silent.
        outImport->heuristicSelections += 1;
        VkEncErr() << "[EncoderExt] register: opaque import without the "
                   << "exporter's memoryTypeIndex; type " << typeChoice.index
                   << " selected heuristically (R-5 fallback)" << std::endl;
    }
    const uint32_t memoryTypeIndex = typeChoice.index;

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    // The exporter's size when it gave us one: deriving it from
    // vkGetImageMemoryRequirements is wrong for dma-buf on NVIDIA. The
    // header accepts 0 ("unknown") WITH A LOG, deliberately -- a silent
    // fallback leaves no audit trail when that size mismatch bites.
    if (desc.allocationSize == 0) {
        VkEncErr() << "[EncoderExt] register: descriptor allocationSize is "
                      "0 (unknown); falling back to "
                      "vkGetImageMemoryRequirements size " << memReqs.size
                   << " -- prefer the exporter-reported size for dma-buf"
                   << std::endl;
    }
    allocInfo.allocationSize =
        (desc.allocationSize != 0) ? desc.allocationSize : memReqs.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    // A non-zero exporter size SMALLER than the image's requirement can
    // never bind: the dedicated allocation below is bound at offset 0 and
    // must cover the image (VUID-vkBindImageMemory-size-01049). Refuse it
    // by name rather than flooring to memReqs.size -- the opaque arms
    // demand the exporter's exact size
    // (VUID-VkMemoryAllocateInfo-allocationSize-01742), and a dma-buf
    // allocation larger than the buffer is equally unbindable.
    if ((desc.allocationSize != 0) && (desc.allocationSize < memReqs.size)) {
        VkEncErr() << "[EncoderExt] register: exporter allocationSize "
                   << desc.allocationSize << " is below the image's memory "
                      "requirement " << memReqs.size << "; a dedicated "
                      "allocation bound at offset 0 cannot cover the image"
                   << std::endl;
        vkDevCtx.DestroyImage(device, outImport->image, nullptr);
        outImport->image = VK_NULL_HANDLE;
        // Still pre-handoff: nothing reached the driver, so the close is
        // the library's obligation here.
        VkEncConsumeOsHandle(desc.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_ALLOCATION_SIZE_INVALID;
    }

    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = outImport->image;

#if defined(__linux__)
    VkImportMemoryFdInfoKHR importFd{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    importFd.handleType = vkHandleType;
    importFd.fd = (int)osHandle;
    importFd.pNext = &dedicated;
    allocInfo.pNext = &importFd;
#elif defined(_WIN32)
    VkImportMemoryWin32HandleInfoKHR importWin32{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    importWin32.handleType = vkHandleType;
    importWin32.handle = (HANDLE)osHandle;
    importWin32.pNext = &dedicated;
    allocInfo.pNext = &importWin32;
#else
    // Neither an fd import nor a Win32 handle import exists on this target,
    // and referencing the Win32 structure here is what made this block fail to
    // compile on macOS/Fuchsia -- NOT on Windows, which is the platform the
    // bare #else looked like it was for. Refuse, releasing what we already
    // own: the image created above and the caller's OS handle.
    vkDevCtx.DestroyImage(device, outImport->image, nullptr);
    outImport->image = VK_NULL_HANDLE;
    VkEncConsumeOsHandle(desc.handleType, osHandle);
    return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
#endif

    // THE HANDOFF. From this call on -- success or failure -- the fd is
    // never the library's to close: NVIDIA consumes it even when the
    // import allocation FAILS (Chromium documents the behavior at
    // gpu/vulkan/vulkan_memory.cc and splits its own import path on
    // exactly this line, gpu/vulkan/vulkan_image.h InitializeResult).
    // Closing it again here or in any caller is a double close of an fd
    // number the process may already have recycled.
    result = vkDevCtx.AllocateMemory(device, &allocInfo, nullptr,
                                     &outImport->memory);
    if (result != VK_SUCCESS) {
        VkEncErr() << "[EncoderExt] register: import allocation failed ("
                   << result << ")" << std::endl;
        vkDevCtx.DestroyImage(device, outImport->image, nullptr);
        outImport->image = VK_NULL_HANDLE;
        outImport->result = VK_ENC_IMPORT_FAILED_AFTER_ALLOCATE;
        return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    }

    result = vkDevCtx.BindImageMemory(device, outImport->image,
                                      outImport->memory, 0);
    if (result != VK_SUCCESS) {
        // The allocation succeeded, so the fd belongs to |memory| and
        // this vkFreeMemory is what releases it. Still AFTER_ALLOCATE:
        // no close() anywhere.
        vkDevCtx.FreeMemory(device, outImport->memory, nullptr);
        vkDevCtx.DestroyImage(device, outImport->image, nullptr);
        outImport->memory = VK_NULL_HANDLE;
        outImport->image  = VK_NULL_HANDLE;
        outImport->result = VK_ENC_IMPORT_FAILED_AFTER_ALLOCATE;
        return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    }

    outImport->allocationSize = allocInfo.allocationSize;
    outImport->result = VK_ENC_IMPORT_SUCCESS;
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

// Thin member wrapper over VkEncImportExternalImage: fold the import's
// memory-type telemetry into the session counters and adopt the imported
// objects into |slot|. Caller holds m_resourceMutex. No fd handling at this
// layer ON ANY PATH -- the import already applied the ownership split (it
// closes the fd itself exactly when the failure preceded the
// vkAllocateMemory handoff), so a close here or in any caller above would be
// a double close.
VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::ImportImageLocked(
    const VkVideoEncoderExternalImageDescriptor& desc,
    uint64_t osHandle, RegisteredImage& slot)
{
    VkEncImportedImage imported;
    const VkVideoEncoderStatusCode status =
        VkEncImportExternalImage(m_vkDevCtx, desc, osHandle, &imported);
    if (imported.heuristicSelections != 0) {
        m_importMemTypeHeuristicSelections.fetch_add(
            imported.heuristicSelections, std::memory_order_relaxed);
    }
    if (imported.exporterOverrides != 0) {
        m_importMemTypeExporterOverrides.fetch_add(
            imported.exporterOverrides, std::memory_order_relaxed);
    }
    if (status != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
        return status;
    }
    slot.image             = imported.image;
    slot.memory            = imported.memory;
    slot.ownsImage         = true;
    slot.importedAllocSize = imported.allocationSize;
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

// The image usage a registration ACTUALLY carries.
//
// Factored out of BuildRegisteredViewLocked because it now has TWO readers and
// they must not drift: the view builder, and the content probe's arm decision.
// The arm decision cannot read slot.imageUsage instead -- BuildRegisteredViewLocked
// is the only writer of that field and RegisterImageResource SKIPS it entirely on a
// null-backend session (`if (m_nullBackend == nullptr)`), so every device-free
// registration would see 0 and answer NOT_APPLICABLE. Reading the DESCRIPTOR is
// available on every path, which is what a registration-time predicate needs.
static VkImageUsageFlags VkEncResolveRegistrationUsage(
    const VkVideoEncoderExternalImageDescriptor& desc)
{
    // Undeclared usage presumes ONLY the access the staging copy needs. See
    // BuildRegisteredViewLocked for why it is not VIDEO_ENCODE_SRC.
    // Both arms typed as the flags type the function returns. A conditional
    // whose arms are an enumerator and its own flags type has no common type
    // the language will pick for us, so it promotes to whatever it can and
    // the result stops being obviously VkImageUsageFlags.
    return (desc.imageUsage == 0)
               ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
               : desc.imageUsage;
}

// DIRECT and only DIRECT -- the registration-time routing predicate, as a pure
// function of the descriptor.
//
// It lived inline in BuildRegisteredViewLocked, which is skipped wholesale on a
// null-backend session, so slot.encodeCapable was left at its default there and
// the device-free suite could not observe the routing decision at ALL. That is
// not a cosmetic gap: encodeCapable is what used to decide whether the content
// probe armed, so the probe's blindness to directly-encodable (block-linear)
// imports was unobservable in the only suite that runs on every host.
static bool VkEncRegistrationIsDirectlyEncodable(
    const VkVideoEncoderExternalImageDescriptor& desc)
{
    // The taxonomy's first rung, read from the one classifier so "encodable as
    // it stands" cannot drift from "encodable after a conversion".
    return (VkEncClassifyInput(desc.format, desc.colorModel) ==
            VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT) &&
           (desc.tiling != VK_IMAGE_TILING_LINEAR) &&
           ((VkEncResolveRegistrationUsage(desc) &
             VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR) != 0);
}

// Build the once-per-registration wrapper + view + pool node for |slot|
// from the descriptor's TRUE properties. The import cost -- and the view
// creation -- belong to registration, never to the submit path. This
// replaces the per-frame WrapExternalImage on the registered
// path, and with it the fabricated STORAGE|SAMPLED plane views
// (VUID-VkImageViewCreateInfo-pNext-02662) v1 invented on images it could
// not verify: every view built here carries a usage that is a strict subset
// of what the image actually carries, and per-plane views are built ONLY
// when the descriptor declared MUTABLE_FORMAT and granted STORAGE. Nothing
// on the encode path reads a plane view (the codecs consume
// GetPictureResourceInfo()/GetImageView()); the preprocess compute filter
// does, which is why the plane views exist at all now that the filter is
// reachable for external input.
//
// Caller holds m_resourceMutex. On failure every Vulkan object created for
// the slot -- including, on the import arm, the image and memory
// ImportImageLocked created -- is freed and the slot's handles are zeroed,
// so the caller returns the status without touching them. The fd is NOT
// closed on that path: after a successful import Vulkan owns it, and the
// vkFreeMemory performed by dropping the owning wrapper is what releases it.
VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::BuildRegisteredViewLocked(
    const VkVideoEncoderExternalImageDescriptor& desc, RegisteredImage& slot)
{
    // Metadata mirrors the true create info -- the same fields
    // ImportImageLocked created the image with. For a VK_IMAGE registration
    // with no declared usage, only TRANSFER_SRC is presumed -- see the
    // usage selection below.
    VkImageCreateInfo imageCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCI.imageType   = (desc.imageType == 0) ? VK_IMAGE_TYPE_2D
                                                : desc.imageType;
    imageCI.format      = desc.format;
    imageCI.extent      = {desc.width, desc.height, 1};
    imageCI.mipLevels   = (desc.mipLevels == 0) ? 1u : desc.mipLevels;
    imageCI.arrayLayers = (desc.arrayLayers == 0) ? 1u : desc.arrayLayers;
    imageCI.samples     =
        (desc.samples == 0) ? VK_SAMPLE_COUNT_1_BIT : desc.samples;
    imageCI.tiling      = desc.tiling;
    imageCI.flags       = desc.imageFlags;
    imageCI.sharingMode = desc.sharingMode;

    const VkImageUsageFlags usage = VkEncResolveRegistrationUsage(desc);
    if (desc.imageUsage == 0) {
        // VK_IMAGE arm, usage undeclared (legal; see ValidateImageDescriptor).
        // Presume ONLY the access the staging copy needs. The previous shape
        // fabricated VIDEO_ENCODE_SRC for OPTIMAL tiling, and encodeCapable
        // below derived from that fabricated bit -- an undeclared-usage
        // registration could route DIRECT and vkCmdEncodeVideoKHR would read
        // an image that may carry no encode usage: v1's fabricated-flags
        // defect, narrowed to one arm. An undeclared usage now always routes
        // STAGED; DIRECT requires the caller to declare a usage that
        // includes VIDEO_ENCODE_SRC.
        //
        // The substitution itself now lives in VkEncResolveRegistrationUsage
        // above, so the content probe's arm decision can apply the SAME rule
        // without depending on this function having run -- which on a
        // null-backend session it has not.
    }
    imageCI.usage   = usage;

    // Already resolved by RegisterImageResource, from the same descriptor and
    // through the same two helpers, BEFORE this function is reached -- so a
    // null-backend session (which never reaches it) gets the same answer.
    // Asserted rather than recomputed: two copies of a routing predicate is
    // how "the path the caller was told about" and "the path its frames take"
    // drift apart.
    assert(slot.imageUsage == usage);
    assert(slot.encodeCapable == VkEncRegistrationIsDirectlyEncodable(desc));

    VkSharedBaseObj<VkImageResource> imageResource;
    VkResult result;
    if (slot.ownsImage) {
        // Import arm: hand image+memory lifetime to the refcounted wrapper,
        // so slot retirement and per-frame refs share one release path.
        result = VkImageResource::CreateFromImport(
            &m_vkDevCtx, slot.image, slot.memory, slot.importedAllocSize,
            &imageCI, imageResource);
        if (result != VK_SUCCESS) {
            // The wrapper was never built: the raw handles are still ours.
            VkDevice device = m_vkDevCtx;
            m_vkDevCtx.DestroyImage(device, slot.image, nullptr);
            m_vkDevCtx.FreeMemory(device, slot.memory, nullptr);
            slot.image  = VK_NULL_HANDLE;
            slot.memory = VK_NULL_HANDLE;
            slot.ownsImage = false;
            return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
        }
    } else {
        // VK_IMAGE arm: non-owning wrapper; the caller frees its image
        // after Unregister, per the header's precondition.
        result = VkImageResource::CreateFromExternal(
            &m_vkDevCtx, slot.image, slot.memory, &imageCI, imageResource);
        if (result != VK_SUCCESS) {
            return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
        }
    }

    VkImageSubresourceRange subresRange{};
    subresRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresRange.levelCount = 1;
    subresRange.layerCount = 1;

    // One entry point for both arms. Create() derives what this registration
    // needs from the image itself, so nothing here has to be pre-computed:
    //   * staging arm -- a transfer-only image carries no view-compatible
    //     usage, so no view is created and the slot holds the resource alone.
    //     The copy path (vkCmdCopyImage + barriers) reads the raw VkImage.
    //   * encodable arm -- the combined view's usage is the image's own usage
    //     minus STORAGE|SAMPLED (a sampled YCbCr combined view would demand a
    //     conversion object, VUID-VkImageViewCreateInfo-usage-06415).
    //
    // FILTER arm: when the EXPORTER declared MUTABLE_FORMAT and granted
    // STORAGE, ask for the per-plane STORAGE views as well. Without them
    // GetPlaneImageView() answers VK_NULL_HANDLE for every registered
    // external image, which is precisely why the compute filter could not
    // bind one and the external-input bypass had nothing to lift. The
    // condition is the descriptor's, not a guess: MUTABLE_FORMAT is what
    // makes a plane view legal at all
    // (VUID-VkImageViewCreateInfo-image-01762) and STORAGE is what the view
    // may carry (VUID-VkImageViewCreateInfo-pNext-02662), and EXTENDED_USAGE
    // is what makes that usage validate against the PLANE formats rather than
    // the image format (measured: MUTABLE_FORMAT alone answers
    // VK_ERROR_FORMAT_NOT_SUPPORTED from the capability query).
    //
    // CAVEAT, stated because it is easy to read the opposite: Create()
    // re-tests MUTABLE_FORMAT, but on the create-info this function
    // SYNTHESIZED from |desc| and handed to CreateFromExternal -- so it
    // re-tests the DESCRIPTOR's claim, not the image's truth. On the IMPORT
    // arm the two coincide, because vkCreateImage used the same synthesized
    // flags and would have failed otherwise. On the VK_IMAGE arm there is no
    // vkCreateImage to catch a false claim: a caller that declares
    // MUTABLE_FORMAT for a VkImage created without it gets plane views the
    // image never validated, which the NVIDIA driver accepts silently.
    // Closing that needs a capability query against the caller's real image;
    // it is not closed here.
    //
    // Combined view for that arm: the image's usage minus STORAGE|SAMPLED,
    // the same subset the no-override arm of Create() computes -- a sampled
    // or storage COMBINED view of a YCbCr format needs a conversion object
    // (VUID-VkImageViewCreateInfo-usage-06415). A registration whose whole
    // usage is STORAGE leaves that subset empty and so has no legal combined
    // view; it takes the plain arm rather than being handed a zero usage the
    // 6-argument overload would silently widen back to the image's own.
    const VkImageUsageFlags combinedUsage =
        usage & ~(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (VkEncDescriptorPermitsPlaneStorageViews(desc, usage) &&
        (combinedUsage != 0)) {
        result = VkImageResourceView::Create(
            &m_vkDevCtx, imageResource, subresRange,
            VK_IMAGE_USAGE_STORAGE_BIT, VK_NULL_HANDLE, combinedUsage,
            slot.imageView);
    } else {
        result = VkImageResourceView::Create(
            &m_vkDevCtx, imageResource, subresRange, slot.imageView);
    }
    if (result != VK_SUCCESS) {
        VkEncErr() << "[EncoderExt] register: view creation failed ("
                   << result << ")" << std::endl;
        // Dropping the (owning, on the import arm) wrapper frees the image
        // and memory -- and with them the imported fd.
        imageResource = nullptr;
        slot.image  = VK_NULL_HANDLE;
        slot.memory = VK_NULL_HANDLE;
        slot.ownsImage = false;
        return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    }

    // What was actually BUILT, not what was asked for. Create() answers a
    // plane count of 0 when it declined the plane views (no MUTABLE_FORMAT,
    // or a usage that could not carry them), and routing must key off the
    // views that exist -- a slot claiming plane views it does not have would
    // route a frame to a filter that then binds VK_NULL_HANDLE.
    slot.planeStorageViews =
        slot.imageView && (slot.imageView->GetNumberOfPlanes() >= 2);
    // The single-plane parallel. GetImageView() is checked EXPLICITLY rather
    // than inferred from the wrapper existing, because Create() answers
    // VK_NULL_HANDLE for the combined view whenever the image carries no
    // view-compatible usage (VUID-VkImageViewCreateInfo-image-04441) and
    // still returns a wrapper. A slot claiming a storage read it cannot
    // perform would route a frame to a filter that then binds
    // VK_NULL_HANDLE -- the same failure the plane-count line above prevents,
    // one view over.
    //
    // The STORAGE bit is re-tested here on the RESOLVED usage rather than
    // trusted from the registration gate: on the VK_IMAGE arm the descriptor
    // may declare 0 and |usage| is then the legacy set, so the gate's answer
    // and the built view's usage are two different facts.
    slot.storageReadView =
        slot.imageView &&
        (slot.imageView->GetImageView() != VK_NULL_HANDLE) &&
        (slot.imageView->GetNumberOfPlanes() == 1) &&
        (VkEncInputFormatPlaneCount(desc.format) == 1u) &&
        ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0);

    const VkImageLayout initialLayout =
        (desc.defaultLayout != VK_IMAGE_LAYOUT_UNDEFINED)
            ? desc.defaultLayout
            : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;  // the legacy wrap's default
    result = VulkanVideoImagePoolNode::CreateExternal(
        &m_vkDevCtx, slot.imageView, initialLayout, slot.node);
    if (result != VK_SUCCESS) {
        slot.imageView = nullptr;
        imageResource  = nullptr;
        slot.image  = VK_NULL_HANDLE;
        slot.memory = VK_NULL_HANDLE;
        slot.ownsImage = false;
        return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    }
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

std::vector<VkDrmFormatModifierPropertiesEXT>
VulkanVideoEncoderExtImpl::EnumerateDrmModifiers(VkFormat format) const
{
    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers;
    if (!m_initialized ||
        (m_vkDevCtx.FindRequiredDeviceExtension(
             VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == nullptr)) {
        return modifiers;
    }
    VkDrmFormatModifierPropertiesListEXT modifierList{
        VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 formatProps{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    formatProps.pNext = &modifierList;
    m_vkDevCtx.GetPhysicalDeviceFormatProperties2(
        m_vkDevCtx.getPhysicalDevice(), format, &formatProps);
    if (modifierList.drmFormatModifierCount == 0) {
        return modifiers;
    }
    modifiers.resize(modifierList.drmFormatModifierCount);
    modifierList.pDrmFormatModifierProperties = modifiers.data();
    m_vkDevCtx.GetPhysicalDeviceFormatProperties2(
        m_vkDevCtx.getPhysicalDevice(), format, &formatProps);
    modifiers.resize(modifierList.drmFormatModifierCount);
    return modifiers;
}

bool VulkanVideoEncoderExtImpl::ModifierWouldRegister(
    const VkVideoEncoderExternalImageDescriptor& desc,
    uint64_t modifier) const
{
    // No usage, no registerable image: the OS-import validation refuses
    // imageUsage == 0 (USAGE_INSUFFICIENT -- "0 is INVALID", design
    // section 2.2a) before any modifier is judged, so no modifier can make
    // this descriptor register -- and asking the device would itself be
    // spec-invalid (VUID-VkPhysicalDeviceImageFormatInfo2-usage-
    // requiredbitmask demands a non-zero usage). Reachable with 0 only
    // through the query surface: QueryImageSupport fills the details
    // struct INDEPENDENTLY of the validation verdict, deliberately, so
    // this predicate cannot rely on the register path's earlier gate.
    if (desc.imageUsage == 0) {
        return false;
    }

    // CONCURRENT sharing demands a queue-family list the descriptor cannot
    // carry, so the validation refuses the descriptor by name
    // (SHARING_MODE_UNSUPPORTED) before any image is created. No modifier
    // can change that answer, and advertising direct modifiers for a
    // descriptor that can never register would be a false promise.
    if (desc.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        return false;
    }

    VkExternalMemoryHandleTypeFlagBits extHandleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (desc.handleType == VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF) {
        extHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    }
    VkPhysicalDeviceExternalImageFormatInfo extInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    extInfo.handleType = extHandleType;

    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
    modifierInfo.drmFormatModifier = modifier;
    modifierInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    modifierInfo.pNext = &extInfo;

    VkFormat viewFormats[kVkEncMaxViewFormats] = {};
    VkImageFormatListCreateInfo formatListCI{
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    formatListCI.viewFormatCount = VkEncCollectViewFormats(desc, viewFormats);
    formatListCI.pViewFormats = viewFormats;

    VkPhysicalDeviceImageFormatInfo2 imageInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    imageInfo.pNext = &modifierInfo;
    if ((desc.imageFlags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) {
        // Mirrors ImportImageLocked: MUTABLE_FORMAT obliges the view-format
        // declaration (VUID-VkImageCreateInfo-tiling-02353).
        formatListCI.pNext = imageInfo.pNext;
        imageInfo.pNext = &formatListCI;
    }
    imageInfo.format = desc.format;
    imageInfo.type   = (desc.imageType == 0) ? VK_IMAGE_TYPE_2D
                                             : desc.imageType;
    imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    imageInfo.usage  = desc.imageUsage;
    imageInfo.flags  = desc.imageFlags;

    VkExternalImageFormatProperties extProps{
        VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 outProps{
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    outProps.pNext = &extProps;

    if (m_vkDevCtx.GetPhysicalDeviceImageFormatProperties2(
            m_vkDevCtx.getPhysicalDevice(), &imageInfo, &outProps) !=
        VK_SUCCESS) {
        return false;
    }
    if ((extProps.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
        return false;
    }
    return (VkEncDescriptorWithinCreationLimits(
                desc, outProps.imageFormatProperties) == VK_TRUE);
}

// Per-modifier tiling features for DRM tilings -- never
// optimalTilingFeatures, which is simply wrong for DRM-tiled images -- and
// the matching tiling's features otherwise. STORAGE_IMAGE is the same bit in
// the 32- and 64-bit format-feature flag spaces, so the 32-bit list carried
// by VK_EXT_image_drm_format_modifier itself answers without a
// VK_KHR_format_feature_flags2 dependency.
//
// This is the registration predicate for the single-plane arm. One body, so
// the query -- which runs the same gate -- and the gate cannot answer
// differently.
bool VulkanVideoEncoderExtImpl::DeviceCanStorageRead(
    const VkVideoEncoderExternalImageDescriptor& desc) const
{
    // Same guard FillImageSupportDetails applied: a null-backend session
    // reports initialized with no device, and this dispatches through PFNs
    // only a real device load populates.
    if (!m_initialized ||
        (m_vkDevCtx.getPhysicalDevice() == VK_NULL_HANDLE) ||
        (desc.format == VK_FORMAT_UNDEFINED)) {
        return false;
    }
    if (desc.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        // Unknowable without a modifier, and "unknowable" must answer FALSE
        // on a gate: a true here would admit a registration on a modifier
        // that may carry no STORAGE at all.
        if (desc.hasDrmFormatModifier != VK_TRUE) {
            return false;
        }
        for (const auto& props : EnumerateDrmModifiers(desc.format)) {
            if (props.drmFormatModifier == desc.drmFormatModifier) {
                return (props.drmFormatModifierTilingFeatures &
                        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
            }
        }
        // A modifier this device does not report at all.
        return false;
    }
    VkFormatProperties2 formatProps{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    m_vkDevCtx.GetPhysicalDeviceFormatProperties2(
        m_vkDevCtx.getPhysicalDevice(), desc.format, &formatProps);
    const VkFormatFeatureFlags features =
        (desc.tiling == VK_IMAGE_TILING_LINEAR)
            ? formatProps.formatProperties.linearTilingFeatures
            : formatProps.formatProperties.optimalTilingFeatures;
    return (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

void VulkanVideoEncoderExtImpl::FillImageSupportDetails(
    const VkVideoEncoderExternalImageDescriptor& desc,
    VkVideoEncoderImageSupportDetails* details) const
{
    details->directModifierCount = 0;
    // getPhysicalDevice() as well as m_initialized: a null-backend session
    // (internal header) reports initialized with no device, and both detail
    // queries below dispatch through PFNs only a real device load populates.
    // The struct was zeroed above, so a device-free session honestly reports
    // "no details" instead of crashing.
    if (!m_initialized ||
        (m_vkDevCtx.getPhysicalDevice() == VK_NULL_HANDLE) ||
        (desc.format == VK_FORMAT_UNDEFINED) ||
        (desc.width == 0) || (desc.height == 0)) {
        return;
    }

    // directModifiers: the renegotiation list, meaningful only for an
    // OS-handle import (VK_IMAGE never re-imports; the Win32 arms are
    // reserved).
    if ((desc.handleType ==
         VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD) ||
        (desc.handleType == VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF)) {
        for (const auto& props : EnumerateDrmModifiers(desc.format)) {
            if (details->directModifierCount >=
                VK_VIDEO_ENCODER_MAX_DIRECT_MODIFIERS) {
                break;
            }
            if (ModifierWouldRegister(desc, props.drmFormatModifier)) {
                details->directModifiers[details->directModifierCount++] =
                    props.drmFormatModifier;
            }
        }
    }
}

VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::ValidateImageDescriptor(
    const VkVideoEncoderExternalImageDescriptor& descriptor) const
{
    if (descriptor.sType !=
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    // Nothing chains onto this descriptor today, so anything chained is an
    // extension this build does not understand: refuse it rather than
    // import an image the caller believes it described more precisely.
    // Living in the shared predicate, the refusal is the same through
    // QueryImageSupport and RegisterImageResource by construction.
    if (descriptor.pNext != nullptr) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    switch (descriptor.handleType) {
        case VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD:
        case VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF:
#if !defined(__linux__)
            return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
#endif
            break;
        case VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_WIN32:
        case VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_D3D11_TEXTURE:
#if !defined(_WIN32)
            return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
#else
            break;
#endif
        case VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE:
            break;
        default:
            return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
    }

    // 0 is not a default on an import: the library creates the VkImage there
    // and must never grant itself access the exporter did not give -- v1
    // fabricated STORAGE|SAMPLED|... on an image it could not verify.
    //
    // A VK_IMAGE registration is different in kind: the caller created the
    // image, we do not re-create it, and the field is neither used nor
    // verifiable. Requiring it would only teach callers to write down a
    // plausible constant, which is the habit this field exists to break.
    if ((descriptor.handleType !=
         VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE) &&
        (descriptor.imageUsage == 0)) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_USAGE_INSUFFICIENT;
    }

    // The import re-creates the image from this descriptor, and CONCURRENT
    // sharing demands a queue-family list the descriptor cannot carry
    // (VUID-VkImageCreateInfo-sharingMode-00942): the create would run
    // spec-invalid. A VK_IMAGE registration is different in kind: the
    // caller's own image was created with whatever list it needed, and
    // nothing is re-created there.
    if ((descriptor.handleType !=
         VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE) &&
        (descriptor.sharingMode == VK_SHARING_MODE_CONCURRENT)) {
        VkEncErr() << "[EncoderExt] register: sharingMode CONCURRENT is not "
                      "importable -- the descriptor carries no queue-family "
                      "list, so the re-created image could not be "
                      "spec-valid; export with EXCLUSIVE sharing"
                   << std::endl;
        return VK_VIDEO_ENCODER_STATUS_ERROR_SHARING_MODE_UNSUPPORTED;
    }

    // A colour-model DECLARATION must be readable against the format before
    // anything is read from it. VkEncResolveColorModel answers
    // VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT for a (format, colorModel)
    // pair that cannot be reconciled -- RGB declared over a Y'CbCr format, or
    // Y'CbCr declared over an RGBA layout that carries no packed 4:4:4
    // reading -- and there is no route to choose from an answer that means
    // "the caller stated two things that cannot both be true".
    //
    // Judged HERE, from the descriptor alone, because that is what it is: the
    // question needs no session, no device and no negotiated encode format,
    // so QueryImageSupport and RegisterImageResource answer it identically,
    // and on every session the same descriptor is offered to. Deferring it to
    // the point where the route is picked is what lets a contradictory
    // declaration REGISTER and then quietly take the staging copy -- an
    // accepted registration that costs a copy the caller never asked for and
    // has no way to see. A negotiation interface exists to make exactly that
    // answerable before the allocation is committed.
    //
    // A declaration that AGREES with its format resolves to itself and
    // passes. FROM_FORMAT -- what a zero-initialised descriptor says, and the
    // ordinary case -- reads the model off the format and passes for every
    // format the taxonomy places.
    if (VkEncResolveColorModel(descriptor.format, descriptor.colorModel) ==
        VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) {
        VkEncErr() << "[EncoderExt] register: declared colorModel "
                   << (uint32_t)descriptor.colorModel
                   << " contradicts format " << (uint32_t)descriptor.format
                   << "; the two cannot both be true and nothing here can "
                      "know which was meant. Declare the colour model the "
                      "format carries, or leave the field "
                      "VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT to read it "
                      "off the format." << std::endl;
        // The DECLARATION is what is refused. The format itself may be
        // perfectly encodable -- NV12 declared RGB reaches here -- so the
        // code names the field the caller has to change.
        return VK_VIDEO_ENCODER_STATUS_ERROR_COLOR_MODEL_UNSUPPORTED;
    }

    // An RGBA input is not "unsupported" -- it is convertible, and the
    // distinction is what lets a client fix it rather than give up.
    //
    // The SESSION's predicate, not the taxonomy's. A 3-plane 4:2:0 input is
    // convertible in principle and encodable in practice only on a session
    // whose compute filter is active, so on any other session the honest
    // answer here is still CONVERSION_REQUIRED -- and it must be, because the
    // gates below this one are what keep a 3-plane frame away from the
    // transfer copy that hangs the GPU on it.
    //
    // Asked under the model the DESCRIPTOR declares. The gate above
    // established that the declaration can be read against the format; this
    // one is what reads it. A descriptor is judged as what it says it is, so
    // that the answer a producer negotiates is an answer about the frames it
    // intends to hand in -- and the packed 4:4:4 layouts are why that has to
    // be said, because they ride RGBA format enumerants and the format alone
    // cannot separate them from an R'G'B' image.
    if (SupportsFormat(descriptor.format, descriptor.colorModel) != VK_TRUE) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_CONVERSION_REQUIRED;
    }

    if ((descriptor.width == 0) || (descriptor.height == 0)) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_PLANE_LAYOUT_INVALID;
    }
    if (descriptor.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        if ((descriptor.planeCount == 0) ||
            (descriptor.planeCount > VK_VIDEO_ENCODER_MAX_PLANES)) {
            return VK_VIDEO_ENCODER_STATUS_ERROR_PLANE_LAYOUT_INVALID;
        }
        // Registration binds ONE memory object, so every plane must live in
        // the single allocation named by the handle. What is PROVABLE here
        // is only the necessary half of that: two planes cannot both begin
        // at the same byte of one allocation, so equal offsets describe an
        // image that cannot exist in a single memory object. The common
        // shape that trips it is exactly the one worth refusing -- a
        // disjoint (multi-buffer-object) producer writes offset 0 for every
        // plane, because each starts at the base of its own buffer.
        //
        // This is NOT a disjointness test, and the previous rule here --
        // "offset == 0 for any plane > 0" -- was wrong to be read as one, in
        // both directions. It missed a disjoint producer whose chroma plane
        // sits at a non-zero offset inside its OWN buffer (accepted, then
        // silently wrong chroma: the very failure it claimed to prevent),
        // and it refused a legal single-BO image whose planes are packed in
        // a different order, where plane 0 follows plane 1.
        //
        // No arithmetic on ONE fd can do better. Disjointness is a question
        // about the planes' PROVENANCE, and this entry point is handed a
        // single handle; the descriptor simply does not carry the answer.
        // The sufficient test therefore belongs to the caller, which holds
        // one fd per plane and can compare their identity directly -- see
        // the planeLayouts[] contract in vulkan_video_encoder_ext.h.
        // Chromium's VulkanVideoEncodeAccelerator runs it (fstat st_dev/
        // st_ino over gfx::NativePixmapHandle::planes[]) before it builds
        // this descriptor at all, and routes a disjoint pixmap to the
        // staging copy instead of registering it.
        for (uint32_t i = 0; i < descriptor.planeCount; i++) {
            for (uint32_t j = i + 1; j < descriptor.planeCount; j++) {
                if (descriptor.planeLayouts[i].offset ==
                    descriptor.planeLayouts[j].offset) {
                    VkEncErr() << "[EncoderExt] register: planes " << i
                               << " and " << j << " both begin at offset "
                               << descriptor.planeLayouts[i].offset
                               << "; one memory object cannot hold two planes "
                                  "at one offset (a disjoint producer "
                                  "describes every plane at offset 0), and "
                                  "this interface binds a single handle."
                               << std::endl;
                    return VK_VIDEO_ENCODER_STATUS_ERROR_PLANE_LAYOUT_INVALID;
                }
            }
        }
        if (descriptor.hasDrmFormatModifier != VK_TRUE) {
            // Tiling says modifier, the descriptor supplies none: the caller
            // would otherwise get modifier 0 (LINEAR) by accident, which
            // NVIDIA refuses for VIDEO_ENCODE_SRC on multiplanar YCbCr.
            return VK_VIDEO_ENCODER_STATUS_ERROR_MODIFIER_UNSUPPORTED;
        }
    }

    // Multi-GPU: a mismatched handle fails HERE with a name, instead of
    // failing the import later with a driver error nobody can act on.
    // Everything above is a property of the descriptor alone and is
    // answered the same with or without a session. Everything below needs a
    // physical device, so the initialization gate belongs here rather than at
    // the top: a caller with a malformed descriptor learns what is wrong with
    // it instead of only that it called too early.
    if (!m_initialized) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_NOT_INITIALIZED;
    }

    // A device that cannot service the import must say so BY NAME here, at
    // the negotiation point -- not later at vkAllocateMemory, and never by
    // running spec-invalid. What is checkable is stated honestly: on the
    // library's own device this set is what vkCreateDevice enabled; on a
    // caller's device Vulkan cannot report enablement, so this is
    // physical-device support, and enablement is the device creator's to
    // audit (Chromium's accelerator does exactly that).
    const bool isOsImport =
        (descriptor.handleType !=
         VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE);
    if (isOsImport) {
        const char* missing = nullptr;
#if defined(__linux__)
        if (m_vkDevCtx.FindRequiredDeviceExtension(
                VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == nullptr) {
            missing = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        } else if ((descriptor.handleType ==
                    VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF) &&
                   (m_vkDevCtx.FindRequiredDeviceExtension(
                        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) ==
                    nullptr)) {
            missing = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
        } else if (m_vkDevCtx.FindRequiredDeviceExtension(
                       VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) ==
                   nullptr) {
            // An OS-handle import that declares nothing still DERIVES
            // FOREIGN, so the staging/encode acquire barrier can need the
            // extension. Required unconditionally rather than only for a
            // declared/derived FOREIGN: residency is a REGISTRATION-time
            // field but the acquire is chosen per frame, and this gate runs
            // before the slot exists. Refusing here costs a caller nothing
            // real -- every device that can import an OS handle for video
            // encode exposes VK_EXT_queue_family_foreign -- whereas
            // admitting the registration and discovering the gap at the
            // first barrier is a spec-invalid submit.
            missing = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
        } else if ((descriptor.tiling ==
                    VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) &&
                   (m_vkDevCtx.FindRequiredDeviceExtension(
                        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) ==
                    nullptr)) {
            missing = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
        }
#elif defined(_WIN32)
        // The Win32 arms are reserved through M8; when they open, the
        // import needs the external-memory extension on the session device.
        if (m_vkDevCtx.FindRequiredDeviceExtension(
                VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == nullptr) {
            missing = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
        }
#endif
        if (missing != nullptr) {
            VkEncErr() << "[EncoderExt] register: the session device lacks "
                       << missing << ", required for this import"
                       << std::endl;
            return VK_VIDEO_ENCODER_STATUS_ERROR_EXTENSION_MISSING;
        }
    } else if ((descriptor.residency ==
                VK_VIDEO_ENCODER_INPUT_RESIDENCY_FOREIGN) &&
               (m_vkDevCtx.FindRequiredDeviceExtension(
                    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) ==
                nullptr)) {
        // A VK_IMAGE registration declaring FOREIGN residency takes a
        // FOREIGN queue-family acquire at submit; without the extension
        // that barrier is spec-invalid.
        VkEncErr() << "[EncoderExt] register: the session device lacks "
                   << VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME
                   << ", required for FOREIGN-residency input" << std::endl;
        return VK_VIDEO_ENCODER_STATUS_ERROR_EXTENSION_MISSING;
    }

    static const uint8_t kZeroUuid[VK_UUID_SIZE] = {};
    static const uint8_t kZeroLuid[VK_LUID_SIZE] = {};
    const bool uuidMatters =
        (descriptor.handleType !=
         VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE);
    const bool haveDeviceUuid =
        (memcmp(descriptor.deviceUUID, kZeroUuid, VK_UUID_SIZE) != 0);
    const bool haveDriverUuid =
        (memcmp(descriptor.driverUUID, kZeroUuid, VK_UUID_SIZE) != 0);
    const bool haveDeviceLuid =
        (descriptor.deviceLUIDValid == VK_TRUE) &&
        (memcmp(descriptor.deviceLUID, kZeroLuid, VK_LUID_SIZE) != 0);
    if (!haveDeviceUuid) {
        // Only meaningful for an IMPORT. A VK_IMAGE registration names an
        // image the caller created on this very device, so there is nothing
        // to propagate and nothing to check -- warning there would fire on
        // every in-process run and teach people to ignore the message.
        //
        // On an import, a zeroed UUID is exactly what a consumer that
        // propagates nothing looks like, and on a multi-GPU host that is the
        // difference between a named mismatch and a driver error nobody can
        // act on. Warn once rather than per registration.
        if (uuidMatters) {
            // ONCE PER PROCESS, and "once" has to be true rather
            // than likely: independent sessions reach this branch
            // concurrently, and a plain check-then-store lets two
            // of them both read false and both print. The flag
            // arbitrates emission and publishes nothing else, so
            // relaxed ordering is the entire requirement. It does
            // not rely on stdio locking, on per-session
            // serialization, or on the output being suppressed.
            static std::atomic<bool> warnedZeroUuid{false};
            if (!warnedZeroUuid.exchange(true, std::memory_order_relaxed)) {
                VkEncErr() << "[EncoderExt] register: imported handle carries "
                              "a zeroed deviceUUID, so a cross-device handle "
                              "cannot be detected. Propagate the exporting "
                              "device's UUID." << std::endl;
            }
        }
    }
    if (haveDeviceUuid || haveDriverUuid || haveDeviceLuid) {
        VkPhysicalDeviceIDProperties idProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &idProps;
        m_vkDevCtx.GetPhysicalDeviceProperties2(m_vkDevCtx.getPhysicalDevice(),
                                                &props2);
        if (haveDeviceUuid &&
            (memcmp(descriptor.deviceUUID, idProps.deviceUUID,
                    VK_UUID_SIZE) != 0)) {
            VkEncErr() << "[EncoderExt] register: handle was exported by a "
                          "different physical device" << std::endl;
            return VK_VIDEO_ENCODER_STATUS_ERROR_DEVICE_MISMATCH;
        }
        if (haveDriverUuid &&
            (memcmp(descriptor.driverUUID, idProps.driverUUID,
                    VK_UUID_SIZE) != 0)) {
            // The Vulkan external-memory compatibility table binds
            // driverUUID identity for the OPAQUE handle types: a
            // same-device, different-driver OPAQUE_FD is exactly the
            // mismatch these fields exist to catch before the driver's
            // unhelpful import error. A dma-buf is a kernel object and
            // cross-driver import of one is spec-legal -- suspicious here,
            // but not ours to refuse.
            const bool opaqueHandle =
                (descriptor.handleType ==
                 VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD) ||
                (descriptor.handleType ==
                 VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_WIN32) ||
                (descriptor.handleType ==
                 VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_D3D11_TEXTURE);
            if (opaqueHandle) {
                VkEncErr() << "[EncoderExt] register: opaque handle was "
                              "exported by a different driver (driverUUID "
                              "mismatch)" << std::endl;
                return VK_VIDEO_ENCODER_STATUS_ERROR_DEVICE_MISMATCH;
            }
            // Same reasoning as the zeroed-UUID claim above: independent
            // sessions reach this through the shared descriptor validator
            // and need no common lock to do so.
            static std::atomic<bool> warnedDriverUuid{false};
            if (!warnedDriverUuid.exchange(true, std::memory_order_relaxed)) {
                VkEncErr() << "[EncoderExt] register: the dma-buf exporter's "
                              "driverUUID differs from the encode device's; "
                              "legal for a kernel object, but worth knowing"
                           << std::endl;
            }
        }
        // Windows device identity. The fields exist on every platform, so
        // this compiles everywhere and stays inert on Linux (no caller sets
        // deviceLUIDValid); on Windows the LUID is what the spec says
        // identifies the device for D3D/OPAQUE_WIN32 interop.
        if (haveDeviceLuid && (idProps.deviceLUIDValid == VK_TRUE) &&
            (memcmp(descriptor.deviceLUID, idProps.deviceLUID,
                    VK_LUID_SIZE) != 0)) {
            VkEncErr() << "[EncoderExt] register: handle was exported by a "
                          "device with a different LUID" << std::endl;
            return VK_VIDEO_ENCODER_STATUS_ERROR_DEVICE_MISMATCH;
        }
    }

    // Session compatibility -- the negotiation point the initialization
    // gate above guarantees. A registration that cannot feed THIS session
    // must say so by name here, not as a wrong-size staging copy or a
    // driver error later. m_encoderConfig is absent only behind the
    // null-backend test seam, which negotiates nothing to compare against.
    // Checked after the device-identity block for the same precedence
    // reason the modifier check runs last: a wrong-GPU handle answers
    // DEVICE_MISMATCH before its format or extent is judged against OUR
    // session.
    if (m_encoderConfig) {
        // TWO formats can register against one session, and conflating them
        // is what kept the compute tier unreachable:
        //
        //  * the ENCODE-SOURCE format -- semi-planar, derived from the
        //    negotiated subsampling and bit depth rather than read out of
        //    input.vkFormat -- which registers on the DIRECT path; and
        //  * the FILTER-INPUT format, which is what input.vkFormat holds
        //    once the binder has written numPlanes down, and which registers
        //    only when this session can actually convert it. A frame that
        //    arrives in the wrong format is an in-contract case, not an
        //    error case -- but only where the
        //    adaptation exists, which is what ComputeFilterActive() answers.
        const VkFormat sessionEncodeFormat =
            VkVideoCoreProfile::CodecGetVkFormat(
                m_encoderConfig->input.chromaSubsampling,
                GetComponentBitDepthFlagBits(m_encoderConfig->input.bpp),
                VkVideoCoreProfile::PLANE_LAYOUT_SEMIPLANAR_2);
        const bool filterActive = ComputeFilterActive();
        const VkFormat sessionFilterFormat =
            filterActive ? m_encoderConfig->input.vkFormat
                         : VK_FORMAT_UNDEFINED;
        if ((descriptor.format != sessionEncodeFormat) &&
            (descriptor.format != sessionFilterFormat)) {
            VkEncErr() << "[EncoderExt] register: descriptor format "
                       << (uint32_t)descriptor.format
                       << " does not match the session's negotiated encode "
                          "input format " << (uint32_t)sessionEncodeFormat
                       << (filterActive
                               ? " nor its filter input format "
                               : " (no compute filter on this session, so no "
                                 "filter input format) ")
                       << (filterActive ? (uint32_t)sessionFilterFormat : 0u)
                       << "; renegotiate the allocation or reinitialize "
                          "the session" << std::endl;
            return VK_VIDEO_ENCODER_STATUS_ERROR_FORMAT_UNSUPPORTED;
        }
        // A filter-input registration needs the views the filter reads --
        // and the filter's TWO arms bind different VIEWS of the image, which
        // is what this split says: per-plane views for a multi-planar input,
        // one combined view for a single-plane one. Both are bound as
        // VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, so both require STORAGE; only the
        // per-plane arm additionally requires the create flags a plane view
        // costs. The split is by PLANE COUNT and not by colour model,
        // because a packed 4:4:4 Y'CbCr surface is one interleaved plane and
        // the filter binds it exactly as it binds R'G'B'. Refusing here, at
        // the negotiation point, is what lets a producer re-export with the
        // right declaration instead of discovering at submit that its frames
        // convert to nothing.
        if (descriptor.format != sessionEncodeFormat) {
            if (VkEncInputFormatPlaneCount(descriptor.format) == 1u) {
                // Single plane, storage read: usage + device capability, no
                // create flags. Computed once -- the DRM arm of this walks
                // the modifier list.
                const bool deviceCanStorageRead =
                    DeviceCanStorageRead(descriptor);
                if (!VkEncDescriptorPermitsStorageRead(
                        descriptor, descriptor.imageUsage,
                        deviceCanStorageRead)) {
                    VkEncErr()
                        << "[EncoderExt] register: format "
                        << (uint32_t)descriptor.format
                        << " encodes through the compute filter's "
                           "single-plane arm, which binds ONE combined view "
                           "as a VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: the "
                           "descriptor must grant VK_IMAGE_USAGE_STORAGE_BIT "
                           "(imageUsage declared: "
                        << (uint32_t)descriptor.imageUsage
                        << ") and the device must report "
                           "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT for this "
                           "format on this tiling (device storage read: "
                        << (deviceCanStorageRead ? 1u : 0u)
                        << "). No create flags are required on this arm"
                        << std::endl;
                    return VK_VIDEO_ENCODER_STATUS_ERROR_CONVERSION_REQUIRED;
                }
            } else if (!VkEncDescriptorPermitsPlaneStorageViews(
                           descriptor, descriptor.imageUsage)) {
                // Multi-planar, per-plane STORAGE views. Unchanged, and
                // still the only arm for which the create flags are real.
                VkEncErr() << "[EncoderExt] register: format "
                           << (uint32_t)descriptor.format
                           << " encodes only through the compute filter, which "
                              "reads per-plane views; the descriptor must declare "
                              "VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT (with "
                              "EXTENDED_USAGE) and VK_IMAGE_USAGE_STORAGE_BIT"
                           << std::endl;
                return VK_VIDEO_ENCODER_STATUS_ERROR_CONVERSION_REQUIRED;
            }
        }
        // Strictly-smaller extents read out of bounds at encode; LARGER
        // stays legal, because producer pools pad and align (1920x1088
        // feeding a 1080p session is the normal case, not an error).
        if ((descriptor.width < m_encoderConfig->encodeWidth) ||
            (descriptor.height < m_encoderConfig->encodeHeight)) {
            VkEncErr() << "[EncoderExt] register: descriptor extent "
                       << descriptor.width << "x" << descriptor.height
                       << " does not cover the session's coded extent "
                       << m_encoderConfig->encodeWidth << "x"
                       << m_encoderConfig->encodeHeight << std::endl;
            return VK_VIDEO_ENCODER_STATUS_ERROR_EXTENT_INVALID;
        }
    }

    // A modifier the device cannot service is RENEGOTIABLE and must be
    // named as such here, not discovered as IMPORT_FAILED at vkCreateImage
    // -- "renegotiate the allocation" and "fail the stream" are different
    // caller responses, and the code must tell them apart. Checked last:
    // a wrong-GPU handle answers DEVICE_MISMATCH before its modifier is
    // judged against OUR GPU.
    if (isOsImport &&
        (descriptor.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) &&
        (descriptor.hasDrmFormatModifier == VK_TRUE) &&
        !ModifierWouldRegister(descriptor, descriptor.drmFormatModifier)) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_MODIFIER_UNSUPPORTED;
    }

    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::QueryImageSupport(
    const VkVideoEncoderExternalImageDescriptor& descriptor,
    VkVideoEncoderImageSupport* outSupport)
{
    if (outSupport == nullptr) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    if (outSupport->sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMAGE_SUPPORT) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    // The chain is no longer rejected wholesale: the known details struct
    // is consumed; anything else is still refused, because an extension the
    // library does not understand means the caller asked for something it
    // is not getting.
    VkVideoEncoderImageSupportDetails* details = nullptr;
    for (void* link = const_cast<void*>(outSupport->pNext);
         link != nullptr;) {
        auto* candidate =
            reinterpret_cast<VkVideoEncoderImageSupportDetails*>(link);
        if ((candidate->sType !=
             VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMAGE_SUPPORT_DETAILS) ||
            (details != nullptr)) {
            return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
        }
        details = candidate;
        link = const_cast<void*>(candidate->pNext);
    }

    // Same predicate registration runs -- deliberately, so the query cannot
    // become a second opinion that disagrees with the answer.
    const VkVideoEncoderStatusCode status = ValidateImageDescriptor(descriptor);
    outSupport->status    = status;
    outSupport->supported =
        (status == VK_VIDEO_ENCODER_STATUS_SUCCESS) ? VK_TRUE : VK_FALSE;

    // Filled INDEPENDENTLY of the verdict: a MODIFIER_UNSUPPORTED answer
    // carries the modifiers that WOULD work, which is renegotiation in one
    // round trip, and a caller that never reads the verdict still gets it.
    if (details != nullptr) {
        FillImageSupportDetails(descriptor, details);
    }

    // The call itself succeeded; whether the image is usable is the answer,
    // not the return code. A caller must not have to distinguish "the query
    // failed" from "the query says no".
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

// Fold the calling thread's import-guard verdict into the session-level
// snapshot GetCompletionInfo reports, and record the one verdict that means
// the guard did not do what its build asked of it -- INCOMPLETE -- in the
// diagnostic channel, which a consumer can read with NO new chained struct
// at all. At the disabled default the guard asks for nothing and INCOMPLETE
// is unreachable; the channel stays for builds that re-arm it.
//
// LOCK ORDER, which is why this is a separate function and not three lines
// inside the registration. It takes m_pendingMutex, and the only order this
// file establishes between the two is pending -> resource
// (ReleaseEncodedFrame holds m_pendingMutex and calls
// ReleaseResourceReference, which takes m_resourceMutex). Taking pending
// while holding resource would invert it. The caller therefore runs this
// from a scope guard declared AHEAD of its m_resourceMutex lock guard, so
// reverse-declaration-order destruction puts it strictly after the release.
void VulkanVideoEncoderExtImpl::PublishImportGuardVerdict()
{
    VkEncImportOrdinalGuardReport report;
    VkEncGetImportOrdinalGuardReport(&report);
    if (report.state == VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_EVALUATED) {
        // The guard did not run for this registration -- a VK_IMAGE slot,
        // or a refusal ahead of the import. Leaving the snapshot alone is
        // the point: such a registration must not erase the verdict a
        // dma-buf registration established -- DISABLED at the shipped
        // default of 0, COMPLETE or INCOMPLETE only on a re-armed build.
        return;
    }
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_importGuardReport = report;
    if (report.state != VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_INCOMPLETE) {
        return;
    }
    // No VkEncErr() here: the guard already printed its own line on this
    // path. What was missing was a channel that survives silenceStdio.
    m_diagnosticCount++;
    std::snprintf(m_lastDiagnostic, sizeof(m_lastDiagnostic),
                  // Kept inside VK_VIDEO_ENCODER_MAX_DIAGNOSTIC_CHARS with
                  // the widest possible expansion of the four conversions;
                  // a longer sentence here is silently truncated.
                  "import-ordinal guard INCOMPLETE: %u of %u sacrificial "
                  "dma-buf imports retained (status %d, errno %d) -- the "
                  "requested import-phase shift was NOT applied",
                  (unsigned)report.retainedCount,
                  (unsigned)report.requestedCount,
                  (int)report.failureStatus, (int)report.failureErrno);
}

// Arm the content probe for one registration, and hand the encoder the probe
// object if it does not have it yet.
//
// LOCK ORDER, the same constraint PublishImportGuardVerdict documents at
// length: this takes m_pendingMutex (to reach m_encoder), so it must not run
// under m_resourceMutex. The caller runs it from a scope guard declared ahead
// of its lock guard.
//
// A NULL-BACKEND SESSION HAS NO ENCODER, and that is not a failure here: the
// probe's registration table is pure host state, so arming, the ARMED /
// NOT_APPLICABLE answer and the GetCompletionInfo snapshot all work with no
// device at all. Only CAPTURE and SCORE need the encoder, and those simply
// never happen -- which is exactly what a device-free session should report.
VkVideoEncoderImportContentState
VulkanVideoEncoderExtImpl::ArmImportContentProbe(
    VkVideoEncoderResource resource,
    VkVideoEncoderContentProbe::CaptureSite captureSite)
{
    VkSharedBaseObj<VkVideoEncoder> encoder;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (!m_contentProbe) {
            if (VkVideoEncoderContentProbe::Create(m_contentProbe) != VK_SUCCESS) {
                return VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED;
            }
        }
        encoder = m_encoder;
    }
    m_contentProbe->ArmRegistration(resource, captureSite);
    if (encoder) {
        // Idempotent, and deliberately done here rather than at session
        // init: the probe object does not exist until the first caller asks
        // for it, and by then the encoder is long since built.
        encoder->SetContentProbe(m_contentProbe);
    }
    return VkEncMapContentState(m_contentProbe->GetRegistrationState(resource));
}

void VulkanVideoEncoderExtImpl::ForgetImportContentProbe(
    VkVideoEncoderResource resource)
{
    VkSharedBaseObj<VkVideoEncoderContentProbe> probe;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        probe = m_contentProbe;
    }
    if (probe) {
        // This is also what makes GetCompletionInfo's damaged report DRAIN.
        // A consumer reacting to a DAMAGED_* verdict retires the
        // registration; that retirement is what stops the same verdict being
        // reported forever and lets the next damaged buffer surface.
        probe->ForgetRegistration(resource);
    }
}

VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::RegisterImageResource(
    const VkVideoEncoderExternalImageDescriptor& descriptor,
    uint64_t osHandle,
    VkVideoEncoderResource* outResource,
    VkVideoEncoderStatus* pStatus)
{
    // The import-ordinal guard's verdict is per CALL. Clear it before
    // anything can reach the import, so a registration that never gets
    // there reports NOT_EVALUATED rather than the previous one's answer.
    VkEncResetImportOrdinalGuardReport();

    // Declared HERE, ahead of the m_resourceMutex lock guard further down,
    // because destruction runs in reverse declaration order: this therefore
    // fires AFTER that lock is released, which is what keeps
    // PublishImportGuardVerdict's m_pendingMutex acquisition on the right
    // side of the pending -> resource order. See that function.
    struct GuardVerdictPublisher {
        VulkanVideoEncoderExtImpl* self;
        ~GuardVerdictPublisher() { self->PublishImportGuardVerdict(); }
    } guardVerdictPublisher{this};

    // Ownership is read before any validation, for the same reason
    // handleType already is on the failure exits below: the mode must be
    // applicable on EVERY exit, including a malformed descriptor.
    const bool isPosixFdType =
        (descriptor.handleType ==
         VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD) ||
        (descriptor.handleType ==
         VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF);
    const bool borrow =
        (descriptor.ownership == VK_VIDEO_ENCODER_HANDLE_OWNERSHIP_BORROW);
    // The echo is a constant of (platform, type, mode), decided here and
    // written on every return -- the caller asserts, never guesses.
#if defined(__linux__)
    VkEncStatusEcho statusEcho(pStatus, isPosixFdType && !borrow);
#else
    VkEncStatusEcho statusEcho(pStatus, false);
#endif

    // Declared AFTER |statusEcho| and BEFORE the m_resourceMutex lock guard,
    // and both halves of that are load-bearing. Reverse-declaration-order
    // destruction puts this AFTER the resource lock is released (so
    // ArmImportContentProbe's m_pendingMutex acquisition stays on the right
    // side of the pending -> resource order) and BEFORE the echo is written
    // (so the echo reports what the arming actually achieved, rather than
    // what it was about to attempt).
    struct ContentProbeArmer {
        VulkanVideoEncoderExtImpl* self;
        VkEncStatusEcho*           echo;
        // Set by the chain walk: did the caller ask at all.
        bool requested = false;
        // Set by the success tail: is there a registration to arm.
        bool registered = false;
        VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        VkVideoEncoderContentProbe::CaptureSite captureSite =
            VkVideoEncoderContentProbe::CaptureSite::kUnreachable;
        ~ContentProbeArmer() {
            if (!requested || !registered) {
                // Refused before a registration existed. The echo's
                // NOT_EVALUATED default is the answer, and probeGeneration
                // is still stamped, so the caller can tell that apart from
                // a library that never wrote the struct.
                return;
            }
            echo->SetContentVerdict(
                self->ArmImportContentProbe(resource, captureSite),
                resource);
        }
    } contentProbeArmer{this, &statusEcho};

#if defined(__linux__)
    if (borrow && isPosixFdType && ((int64_t)osHandle >= 0)) {
        // BORROW: an immediate private duplicate, taken before anything
        // that can fail, so no exit below can ever touch the caller's fd.
        // Everything from here down runs the unconditional TRANSFER rule
        // on our copy. CLOEXEC because this process may spawn.
        const int dupFd = fcntl((int)osHandle, F_DUPFD_CLOEXEC, 0);
        if (dupFd < 0) {
            VkEncErr() << "[EncoderExt] register: BORROW dup failed (errno "
                       << errno << "); the caller retains its handle"
                       << std::endl;
            return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
        }
        osHandle = (uint64_t)dupFd;
    }
#endif

    // A mis-stamped status struct is version skew, refused like any other
    // unstamped struct -- with the ownership rule applied on this exit
    // like every other (under BORROW, |osHandle| is by now the library's
    // private duplicate, so the caller's fd survives regardless).
    if ((pStatus != nullptr) &&
        (pStatus->sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS)) {
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    // The chain is no longer refused wholesale -- the QueryImageSupport
    // shape. Exactly one VkVideoEncoderImportGuardInfo is consumed;
    // anything else, and a REPEATED known sType, is still refused rather
    // than ignored, because an extension the library does not understand
    // means the caller asked for something it is not getting. Callers built
    // against a header that had nothing to chain here always passed NULL,
    // so relaxing this cannot change what any of them see.
    if (pStatus != nullptr) {
        VkVideoEncoderImportGuardInfo*   guardInfo   = nullptr;
        VkVideoEncoderImportContentInfo* contentInfo = nullptr;
        for (void* link = const_cast<void*>(pStatus->pNext); link != nullptr;) {
            // The {sType, pNext} prefix is read through a SIBLING struct
            // type before the link is re-cast; VK_ENC_PIN_CHAIN_PREFIX is
            // what makes that sound.
            auto* prefix =
                reinterpret_cast<VkVideoEncoderImportGuardInfo*>(link);
            const VkVideoEncoderStructureType linkType = prefix->sType;
            const void* next = prefix->pNext;
            if ((linkType ==
                 VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_GUARD_INFO) &&
                (guardInfo == nullptr)) {
                guardInfo = prefix;
            } else if ((linkType ==
                        VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_CONTENT_INFO) &&
                       (contentInfo == nullptr)) {
                contentInfo =
                    reinterpret_cast<VkVideoEncoderImportContentInfo*>(link);
            } else {
                VkEncConsumeOsHandle(descriptor.handleType, osHandle);
                return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
            }
            link = const_cast<void*>(next);
        }
        // Installed only now: the echo must never write through a chain
        // this gate refused.
        statusEcho.SetGuardInfo(guardInfo);
        statusEcho.SetContentInfo(contentInfo);
        // CHAINING THE STRUCT IS THE OPT-IN. There is no second switch: a
        // registration whose caller did not ask for a content verdict is
        // never armed, records no readback and allocates nothing.
        contentProbeArmer.requested = (contentInfo != nullptr);
    }

    // Validation runs cheapest-first and BEFORE any Vulkan call, so a
    // cross-process caller gets a specific, actionable answer without the
    // library having touched the driver. Every early return consumes the fd.
    if (outResource == nullptr) {
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    *outResource = VK_VIDEO_ENCODER_RESOURCE_NULL;

    const VkVideoEncoderStatusCode validation =
        ValidateImageDescriptor(descriptor);
    if (validation != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
        if (validation ==
            VK_VIDEO_ENCODER_STATUS_ERROR_MODIFIER_UNSUPPORTED) {
            // The renegotiation list rides QueryImageSupport; at the
            // register boundary it is at least LOGGED, so a refusal is
            // never bare.
            VkVideoEncoderImageSupportDetails details;
            FillImageSupportDetails(descriptor, &details);
            std::ostringstream workable;
            for (uint32_t i = 0; i < details.directModifierCount; i++) {
                workable << (i ? ", " : "") << "0x" << std::hex
                         << details.directModifiers[i];
            }
            VkEncErr() << "[EncoderExt] register: DRM modifier 0x"
                       << std::hex << descriptor.drmFormatModifier
                       << std::dec << " is not usable for this descriptor; "
                       << details.directModifierCount
                       << " modifier(s) would be: [" << workable.str()
                       << "]" << std::endl;
        }
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        return validation;
    }

    std::lock_guard<std::mutex> lock(m_resourceMutex);

    size_t index = m_resources.size();
    for (size_t i = 0; i < m_resources.size(); i++) {
        if (!m_resources[i].live && (m_resources[i].inFlight == 0)) {
            index = i;
            break;
        }
    }
    if (index == m_resources.size()) {
        if (m_resources.size() >= 4096) {
            VkEncConsumeOsHandle(descriptor.handleType, osHandle);
            return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_LIMIT;
        }
        m_resources.emplace_back();
    }
    RegisteredImage& slot = m_resources[index];

    if (descriptor.handleType ==
        VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE) {
        if (descriptor.existingImage == VK_NULL_HANDLE) {
            return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
        }
        slot.image     = descriptor.existingImage;
        slot.memory    = VK_NULL_HANDLE;
        slot.ownsImage = false;  // the caller's image; we never free it
    } else {
        const VkVideoEncoderStatusCode status =
            ImportImageLocked(descriptor, osHandle, slot);
        // Ownership rule: the fd is consumed either way, by exactly ONE
        // owner. On success the imported VkDeviceMemory holds it and
        // vkFreeMemory releases it; on failure the import already applied
        // the ownership split -- it closed the fd itself iff the
        // failure preceded the vkAllocateMemory handoff, and past that
        // call the DRIVER consumed it (NVIDIA does so even when the
        // allocation fails; gpu/vulkan/vulkan_memory.cc documents it).
        // A close here would be the second close of an fd number this
        // multithreaded process may already have recycled.
        if (status != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            return status;
        }
    }

    // RESOLVED ON EVERY PATH, from the descriptor alone, and BEFORE the
    // null-backend fork below. These two are written here rather than only inside
    // BuildRegisteredViewLocked, which that fork skips -- so on a device-free
    // session the routing decision and everything derived from it (inputPath,
    // and the content probe's arm decision) would silently read a default.
    // Neither needs a device to compute.
    slot.imageUsage    = VkEncResolveRegistrationUsage(descriptor);
    slot.encodeCapable = VkEncRegistrationIsDirectlyEncodable(descriptor);

    if (m_nullBackend == nullptr) {
        const VkVideoEncoderStatusCode viewStatus =
            BuildRegisteredViewLocked(descriptor, slot);
        if (viewStatus != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            // BuildRegisteredViewLocked freed everything it and the import
            // created (the fd included, via the owning wrapper); the slot
            // is clean for reuse and was never live.
            return viewStatus;
        }
    }
    // else: null-backend session (test seam). Only the VK_IMAGE arm can get
    // here without a device (the import arms already failed validation on
    // the missing extensions), its bookkeeping -- slot, generation, id,
    // inFlight -- is exactly what the fault tests exercise, and the view is
    // the one step that needs a device. Nothing downstream reads it there:
    // the null backend terminates the submit before the encoder would.

    slot.live          = true;
    slot.retired       = false;
    slot.inFlight      = 0;
    slot.handleType    = descriptor.handleType;
    slot.residency     = descriptor.residency;
    slot.format        = descriptor.format;
    slot.width         = descriptor.width;
    slot.height        = descriptor.height;
    slot.defaultLayout = descriptor.defaultLayout;
    slot.tiling        = descriptor.tiling;
    // Section 6 routing, resolved ONCE here and read back by the submit --
    // which is what makes the path the caller is told about and the path its
    // frames take the same decision rather than two that happen to agree.
    //
    // FILTER is now produced, and every clause of it is checkable:
    //   * the format needs converting rather than re-tiling (rung 2, not
    //     rung 3: a format that MATCHES the session's encode format and is
    //     merely wrongly tiled takes the copy, per the ladder's
    //     "transfer only for pure tiling mismatches");
    //   * this session can convert (the filter compiled in and requested);
    //   * and the views the filter reads were actually BUILT on this image,
    //     not merely asked for -- per-plane STORAGE views for a multi-planar
    //     input, ONE storage-capable combined view for RGBA. EITHER satisfies
    //     this clause and neither substitutes for the other: the filter binds
    //     whichever its arm was built for, and asking only the plane-count
    //     question is what routed every RGBA registration to STAGED.
    // Any clause failing leaves STAGED, which for a format that genuinely
    // needs converting is unreachable -- ValidateImageDescriptor refused the
    // registration before this line. Unreachable, not harmless: were the two
    // ever to disagree, the submit's own filter-only-format fence answers
    // VK_ERROR_FORMAT_NOT_SUPPORTED rather than letting the frame fall into
    // the staging copy, which for a format that copy cannot handle is a
    // measured device loss.
    const bool routesViaFilter =
        !slot.encodeCapable &&
        (VkEncClassifyInput(descriptor.format, descriptor.colorModel) ==
         VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER) &&
        ComputeFilterActive() &&
        (slot.planeStorageViews || slot.storageReadView);
    slot.inputPath = slot.encodeCapable
                         ? VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT
                         : (routesViaFilter
                                ? VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER
                                : VK_VIDEO_EXTERNAL_INPUT_PATH_STAGED);

    *outResource = MakeResourceId(index, slot.generation);
    // ===== WHAT THE PROBE CAN ACTUALLY RIDE -- AND IT IS NO LONGER
    // ===== |encodeCapable|
    //
    // This must NOT read `contentProbeArmer.directlyEncodable =
    // slot.encodeCapable`, because encodeCapable is
    //
    //     encodableFormat && (tiling != VK_IMAGE_TILING_LINEAR) &&
    //     (usage & VIDEO_ENCODE_SRC)
    //
    // Two of those three clauses have nothing to say about whether a readback
    // is possible, and the TILING clause inverted the answer for the one
    // damage class the defect appears on. A BLOCK-LINEAR import
    // (tiling != LINEAR) that also declares VIDEO_ENCODE_SRC came out
    // encodeCapable, and the probe latched NOT_APPLICABLE for it -- while
    // block-linear buffers are precisely the class that gets poisoned.
    // Tiling is gone from this decision.
    //
    // What is left is the only thing that decides it: does a
    // transfer-readable copy of the producer's pixels pass through this
    // library for this registration.
    //
    //   * TRANSFER_SRC on the imported image. The probe's capture is a
    //     vkCmdCopyImage OUT of that image; without the usage bit the copy is
    //     a VUID violation, and arming would be a promise the capture site
    //     cannot keep.
    //   * NOT the FILTER path. A filter-routed frame is read in place by
    //     the filter, never copied, so it never reaches the capture site at
    //     all -- arming it
    //     would leave the registration ARMED forever, reporting
    //     NOT_EVALUATED, which is the "cannot fail" shape rather than an
    //     answer, and this is the line that decides it.)
    //
    // DIRECT registrations are now INCLUDED. They reach the capture site
    // through a one-frame staged detour taken only while a capture is still
    // owed -- see VkVideoEncoder::SetExternalInputFrameWithNode.
    //
    // READ FROM THE DESCRIPTOR, NOT FROM |slot|. slot.imageUsage and
    // slot.encodeCapable are written only by BuildRegisteredViewLocked, which
    // RegisterImageResource skips on a null-backend session -- so a predicate
    // reading them answers NOT_APPLICABLE for every device-free registration
    // and the whole device-free carrier suite goes dark. slot.inputPath is
    // safe: it is assigned above on every path.
    //
    // THE THIRD CLAUSE IS THE FORMAT, and it was missing. The first two ask
    // whether a copy out of this image can be RECORDED; this one asks whether
    // the bytes that copy produces can be READ. They are different questions
    // and only RecordCapture used to ask the second, on the first frame,
    // which made a 10-bit registration echo ARMED at import and get silently
    // downgraded to NOT_APPLICABLE afterwards -- leaving the session
    // reporting probed=0 damaged=0 armed=0, which is exactly what a session
    // that probed everything and found it clean reports. Measured, before
    // this clause existed, on RTX A4000 by test/encoder-ext-format-encode
    // --content-probe row [2/13] P010.
    const bool probeCanRideThisRegistration =
        (slot.inputPath != VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER) &&
        ((VkEncResolveRegistrationUsage(descriptor) &
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) &&
        VkVideoEncoderContentProbe::IsProbeableFormat(descriptor.format);
    contentProbeArmer.registered = true;
    contentProbeArmer.resource = *outResource;
    contentProbeArmer.captureSite =
        probeCanRideThisRegistration
            ? VkVideoEncoderContentProbe::CaptureSite::kReachable
            : VkVideoEncoderContentProbe::CaptureSite::kUnreachable;
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::RegisterSemaphore(
    const VkVideoEncoderSemaphoreDescriptor& descriptor,
    uint64_t osHandle,
    VkVideoEncoderResource* outResource,
    VkVideoEncoderStatus* pStatus)
{
    // Same ownership contract as image registration, applied in the same
    // order: mode read first so it holds on every exit, dup before
    // anything that can fail, echo written on every return.
    const bool isPosixFdType =
        (descriptor.handleType ==
         VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD) ||
        (descriptor.handleType ==
         VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF);
    const bool borrow =
        (descriptor.ownership == VK_VIDEO_ENCODER_HANDLE_OWNERSHIP_BORROW);
#if defined(__linux__)
    VkEncStatusEcho statusEcho(pStatus, isPosixFdType && !borrow);
#else
    VkEncStatusEcho statusEcho(pStatus, false);
#endif

#if defined(__linux__)
    if (borrow && isPosixFdType && ((int64_t)osHandle >= 0)) {
        // BORROW: dup-first, so no exit below -- including the sType gates
        // -- can ever touch the caller's fd.
        const int dupFd = fcntl((int)osHandle, F_DUPFD_CLOEXEC, 0);
        if (dupFd < 0) {
            VkEncErr() << "[EncoderExt] register: BORROW dup failed (errno "
                       << errno << "); the caller retains its handle"
                       << std::endl;
            return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
        }
        osHandle = (uint64_t)dupFd;
    }
#endif

    if ((pStatus != nullptr) &&
        ((pStatus->sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS) ||
         (pStatus->pNext != nullptr))) {
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }

    // A chained descriptor is refused like a mis-stamped one -- nothing
    // chains onto it today -- with the ownership rule applied on this
    // exit like every other.
    if ((descriptor.sType !=
         VK_VIDEO_ENCODER_STRUCTURE_TYPE_SEMAPHORE_DESCRIPTOR) ||
        (descriptor.pNext != nullptr)) {
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    if (outResource == nullptr) {
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    *outResource = VK_VIDEO_ENCODER_RESOURCE_NULL;

    // A binary semaphore cannot express "wait for frame N" and is single-use,
    // so it cannot be registered once and named repeatedly -- which is the
    // only reason to register it at all.
    if (descriptor.semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE) {
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
    }
    VkExternalSemaphoreHandleTypeFlagBits vkHandleType;
    switch (descriptor.handleType) {
        case VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD:
#if defined(__linux__)
            vkHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            break;
#else
            return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
#endif
        case VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_WIN32:
#if defined(_WIN32)
            vkHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            break;
#else
            return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
#endif
        default:
            // DMA_BUF and VK_IMAGE are image handle types; naming one here is
            // a caller error worth saying out loud rather than coercing.
            VkEncConsumeOsHandle(descriptor.handleType, osHandle);
            return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
    }

    // Everything above is a property of the descriptor alone, answered the
    // same with or without a session -- same ordering as image registration,
    // so a malformed descriptor is named as malformed rather than merely
    // early. Everything below needs a device, and m_initialized alone does
    // not prove one: a null-backend session (internal header) reports
    // initialized with no device behind it, and the create/import calls
    // below would then go through never-loaded PFNs.
    if (!m_initialized || (m_vkDevCtx.getDevice() == VK_NULL_HANDLE)) {
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_NOT_INITIALIZED;
    }

    // A device that cannot service the import must say so BY NAME here, at
    // the negotiation point -- the image arm's always-on rule, applied
    // before the first device call rather than inside it: without the
    // extension the import PFN below was never populated, so the miss was a
    // call through null, a crash rather than a status. Same honesty caveat
    // as the image arm: on the library's own device this set is what
    // vkCreateDevice enabled; on a caller's device Vulkan cannot report
    // enablement, so this is physical-device support, and enablement is the
    // device creator's to audit.
    const char* missing = nullptr;
#if defined(__linux__)
    if (m_vkDevCtx.FindRequiredDeviceExtension(
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == nullptr) {
        missing = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    }
#elif defined(_WIN32)
    if (m_vkDevCtx.FindRequiredDeviceExtension(
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) == nullptr) {
        missing = VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;
    }
#endif
    if (missing != nullptr) {
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        VkEncErr() << "[EncoderExt] register: the session device lacks "
                   << missing << ", required for this import" << std::endl;
        return VK_VIDEO_ENCODER_STATUS_ERROR_EXTENSION_MISSING;
    }

    VkSemaphoreTypeCreateInfo typeInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue  = 0;
    VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &typeInfo;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (m_vkDevCtx.CreateSemaphore(m_vkDevCtx.getDevice(), &createInfo,
                                   nullptr, &semaphore) != VK_SUCCESS) {
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    }

#if defined(__linux__)
    VkImportSemaphoreFdInfoKHR importInfo{
        VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    importInfo.semaphore  = semaphore;
    importInfo.handleType = vkHandleType;
    importInfo.fd         = (int)osHandle;
    // No TEMPORARY bit: a temporary import is consumed by the first wait,
    // which would silently turn a registration into a one-shot.
    importInfo.flags = 0;
    if (m_vkDevCtx.ImportSemaphoreFdKHR(m_vkDevCtx.getDevice(),
                                        &importInfo) != VK_SUCCESS) {
        m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(), semaphore,
                                    nullptr);
        // The import consumed the fd on success only; on failure it is still
        // ours to close, and the ownership rule says every exit path.
        VkEncConsumeOsHandle(descriptor.handleType, osHandle);
        return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    }
#elif defined(_WIN32)
    // |osHandle| must already be valid in THIS process -- see the header's
    // note on RegisterSemaphore. A Win32 handle is process-local, and this
    // interface carries no source PID with which to OpenProcess and
    // duplicate, so the caller does that.
    //
    // Not TEMPORARY, for the same reason as the fd path: a temporary import
    // is consumed by the first wait, turning a registration into a one-shot.
    //
    // The handle is NOT closed here on any path, success or failure. That is
    // this header's published rule for Win32 and it is the right one: the
    // value belongs to the caller's handle table, and closing a number we do
    // not own closes whatever else happens to hold it.
    VkImportSemaphoreWin32HandleInfoKHR importInfo{
        VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    importInfo.semaphore  = semaphore;
    importInfo.handleType = vkHandleType;
    importInfo.handle     = (HANDLE)osHandle;
    importInfo.name       = nullptr;
    importInfo.flags      = 0;
    if (m_vkDevCtx.ImportSemaphoreWin32HandleKHR(m_vkDevCtx.getDevice(),
                                                 &importInfo) != VK_SUCCESS) {
        m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(), semaphore,
                                    nullptr);
        return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    }
#else
    (void)vkHandleType;
    m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(), semaphore, nullptr);
    return VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED;
#endif

    std::lock_guard<std::mutex> lock(m_semaphoreMutex);
    size_t index = m_semaphores.size();
    for (size_t i = 0; i < m_semaphores.size(); i++) {
        if (!m_semaphores[i].live) {
            index = i;
            break;
        }
    }
    if (index == m_semaphores.size()) {
        if (m_semaphores.size() >= 4096) {
            m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(), semaphore,
                                        nullptr);
            return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_LIMIT;
        }
        m_semaphores.emplace_back();
    }
    RegisteredSemaphore& slot = m_semaphores[index];
    slot.semaphore = semaphore;
    slot.live      = true;
    *outResource = MakeResourceId(index, slot.generation) | kSemaphoreTag;
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::UnregisterSemaphore(
    VkVideoEncoderResource resource)
{
    if ((resource & kSemaphoreTag) == 0) {
        // An image id, or nothing. Rejected rather than resolved against the
        // wrong table.
        return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
    }
    const uint64_t untagged = resource & ~kSemaphoreTag;
    const size_t   index    = (size_t)((untagged & 0xFFFFFFFFull) - 1);
    const uint32_t generation = (uint32_t)(untagged >> 32);

    VkSemaphore doomed = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(m_semaphoreMutex);
        if (index >= m_semaphores.size()) {
            return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
        }
        RegisteredSemaphore& slot = m_semaphores[index];
        if (!slot.live || (slot.generation != generation)) {
            return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
        }
        doomed = slot.semaphore;
        slot.semaphore = VK_NULL_HANDLE;
        slot.live      = false;
        slot.generation++;  // every id naming this slot is now stale
    }
    if (doomed != VK_NULL_HANDLE) {
        m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(), doomed, nullptr);
    }
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

VkSemaphore VulkanVideoEncoderExtImpl::ResolveSemaphore(
    VkVideoEncoderResource resource)
{
    if ((resource & kSemaphoreTag) == 0) {
        return VK_NULL_HANDLE;
    }
    const uint64_t untagged = resource & ~kSemaphoreTag;
    const size_t   index    = (size_t)((untagged & 0xFFFFFFFFull) - 1);
    const uint32_t generation = (uint32_t)(untagged >> 32);
    std::lock_guard<std::mutex> lock(m_semaphoreMutex);
    if (index >= m_semaphores.size()) {
        return VK_NULL_HANDLE;
    }
    const RegisteredSemaphore& slot = m_semaphores[index];
    if (!slot.live || (slot.generation != generation)) {
        return VK_NULL_HANDLE;
    }
    return slot.semaphore;
}

// Import a per-frame acquire fence (sync_fd) as a BINARY semaphore the encode
// waits on. TEMPORARY, unlike RegisterSemaphore's permanent import: SYNC_FD is
// copy-transference, so the payload is consumed by the first wait -- the
// defining property of a per-frame fence. Takes ownership of |fd| on every
// path on POSIX; on Win32 the field is unused and nothing is closed.
VkSemaphore VulkanVideoEncoderExtImpl::ImportAcquireFenceLocked(int fd)
{
    if (fd < 0) {
        return VK_NULL_HANDLE;
    }
#if defined(__linux__)
    VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (m_vkDevCtx.CreateSemaphore(m_vkDevCtx.getDevice(), &createInfo,
                                   nullptr, &semaphore) != VK_SUCCESS) {
        close(fd);
        return VK_NULL_HANDLE;
    }
    VkImportSemaphoreFdInfoKHR importInfo{
        VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    importInfo.semaphore  = semaphore;
    importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR;
    importInfo.fd         = fd;
    importInfo.flags      = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    if (m_vkDevCtx.ImportSemaphoreFdKHR(m_vkDevCtx.getDevice(),
                                        &importInfo) != VK_SUCCESS) {
        m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(), semaphore, nullptr);
        close(fd);
        return VK_NULL_HANDLE;
    }
    return semaphore;
#elif defined(_WIN32)
    // NOT closed, and this is not a build-order point: on Win32 the value in
    // acquireFenceFd is not a file descriptor at all, and this file's own
    // Win32 rule is that a handle we do not own is never closed. There is no
    // SYNC_FD import on this arm, so refuse.
    (void)fd;
    return VK_NULL_HANDLE;
#else
    // Any other POSIX target -- macOS, the BSDs. The value IS an fd here
    // (gpu_fence_handle.h: ScopedPlatformFence = base::ScopedFD under
    // IS_POSIX), and Chromium hands this entry point a dup() it then forgets,
    // its own comment saying "the library's close hits only its own copy".
    // There is no VK_KHR_external_semaphore_fd import wired up on this arm, so
    // consume the fd the only way left. Dropping this close is one leaked fd
    // per submitted frame, not a no-op -- which is why the include above is
    // guarded on !_WIN32 rather than on __linux__.
    close(fd);
    return VK_NULL_HANDLE;
#endif
}

// The export half of the same per-frame entry point as the acquire import
// above (design 3.5). A BINARY semaphore, exportable as SYNC_FD, created per
// fenced frame: SYNC_FD is copy-transference, so a release fence is consumed
// by whoever waits on it and cannot be registered once and named repeatedly
// -- the same property that keeps this off RegisterSemaphore's timeline-only
// path in the acquire direction.
VkSemaphore VulkanVideoEncoderExtImpl::CreateReleaseFenceSemaphore()
{
#if defined(__linux__)
    if (m_vkDevCtx.GetSemaphoreFdKHR == nullptr) {
        return VK_NULL_HANDLE;
    }
    // Ask once, never assume: naming a handle type in VkExportSemaphoreCreateInfo
    // that the physical device does not report EXPORTABLE is invalid usage, so
    // the query has to run before the first create. Same shape, and same
    // reason, as CreateCompletionTimelineSemaphore's OPAQUE_FD probe.
    if (!m_releaseFenceExportProbed) {
        m_releaseFenceExportProbed = true;
        if (m_vkDevCtx.GetPhysicalDeviceExternalSemaphoreProperties != nullptr) {
            VkSemaphoreTypeCreateInfo queryTypeInfo{
                VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            queryTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_BINARY;
            VkPhysicalDeviceExternalSemaphoreInfo extInfo{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
            extInfo.pNext      = &queryTypeInfo;
            extInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            VkExternalSemaphoreProperties extProps{
                VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
            m_vkDevCtx.GetPhysicalDeviceExternalSemaphoreProperties(
                m_vkDevCtx.getPhysicalDevice(), &extInfo, &extProps);
            m_releaseFenceExportable =
                ((extProps.externalSemaphoreFeatures &
                  VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0);
        }
        if (!m_releaseFenceExportable) {
            VkEncErr() << "[EncoderExt] this device cannot export SYNC_FD "
                          "binary semaphores; per-frame release fences will "
                          "answer -1" << std::endl;
        }
    }
    if (!m_releaseFenceExportable) {
        return VK_NULL_HANDLE;
    }
    VkExportSemaphoreCreateInfo exportInfo{
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &exportInfo;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (m_vkDevCtx.CreateSemaphore(m_vkDevCtx.getDevice(), &createInfo,
                                   nullptr, &semaphore) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return semaphore;
#else
    // SYNC_FD is a POSIX-only currency; the Win32 arm of the release fence
    // is reserved, exactly as the Win32 semaphore-export arm is.
    return VK_NULL_HANDLE;
#endif
}

// Export the pending signal on |semaphore| as a SYNC_FD.
//
// MUST be called only after the batch carrying that signal has been handed
// to vkQueueSubmit: a SYNC_FD export requires the semaphore to be signalled
// or to have a signal operation pending execution. Exporting earlier is
// invalid usage, and the ordering is the caller's obligation, not something
// this function can check.
//
// The export is a copy-transference operation and therefore has the side
// effects of a WAIT on the source semaphore: the payload is moved into the
// returned fd and the semaphore is left unsignalled. That is what makes the
// returned fd independent of the VkSemaphore's remaining lifetime.
//
// -1 is a legal answer, not an error: vkGetSemaphoreFdKHR is permitted to
// return -1 for an already-signalled SYNC_FD export, which means "nothing to
// wait for". It coincides with this API's "-1 = no fence, not failure".
int VulkanVideoEncoderExtImpl::ExportReleaseFenceFd(VkSemaphore semaphore)
{
#if defined(__linux__)
    if ((semaphore == VK_NULL_HANDLE) ||
        (m_vkDevCtx.GetSemaphoreFdKHR == nullptr)) {
        return -1;
    }
    VkSemaphoreGetFdInfoKHR getInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    getInfo.semaphore  = semaphore;
    getInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int fd = -1;
    if (m_vkDevCtx.GetSemaphoreFdKHR(m_vkDevCtx.getDevice(), &getInfo,
                                     &fd) != VK_SUCCESS) {
        return -1;
    }
    return fd;
#else
    (void)semaphore;
    return -1;
#endif
}

// Destroy the release-fence semaphores whose frames retired with a real
// capture. A capture is published only after the encode command buffer's
// fence wait, and the encode submit waits on the staging submit, so both
// submissions that can signal a release fence have completed by then --
// which is the vkDestroySemaphore precondition, stated rather than assumed.
void VulkanVideoEncoderExtImpl::DrainRetiredReleaseFences()
{
    std::vector<VkSemaphore> doomed;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        doomed.swap(m_releaseFenceRetired);
    }
    for (VkSemaphore s : doomed) {
        if (s != VK_NULL_HANDLE) {
            m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(), s, nullptr);
        }
    }
}

// The unproven graveyard's in-session drain. Every route into it -- a deadline
// drop, a CancelFrame, an AbandonAllFrames, a failed capture, a non-NOT_READY
// submit failure -- is a repeatable operation a long session performs many
// times, so leaving Deinitialize as its only exit made it grow without bound
// for the life of the session. Nothing in it carries a completion proof, which
// is why the sweep costs a DeviceWaitIdle; the bound is what keeps that cost
// off the steady-state path, and the log line is what keeps a session that
// pays it from doing so silently.
void VulkanVideoEncoderExtImpl::DrainUnprovenSemaphores()
{
    std::vector<VkSemaphore> doomed;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (m_unprovenSemaphores.size() < kUnprovenSemaphoreBound) {
            return;
        }
        doomed.swap(m_unprovenSemaphores);
    }
    if (m_vkDevCtx.getDevice() == VK_NULL_HANDLE) {
        // No device to destroy them on and none coming: dropping the handles
        // is all that is left, and it is what Deinitialize would have done.
        return;
    }
    m_vkDevCtx.DeviceWaitIdle();
    for (VkSemaphore s : doomed) {
        if (s != VK_NULL_HANDLE) {
            m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(), s, nullptr);
        }
    }
    VkEncErr() << "[EncoderExt] swept " << doomed.size()
               << " per-frame fence semaphore(s) whose completion no "
                  "retirement could prove, behind a device wait-idle"
               << std::endl;
}

VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::SubmitRegisteredFrame(
    const VkVideoEncoderFrameSubmitInfo& info,
    VkSemaphore* pStagingCompleteSemaphore)
{
    // PRE-PASS over the chained descriptors, ahead of EVERY refusal in this
    // function -- including the three at the top of it, which is why it now
    // stands in FRONT of them rather than after the registration lookup.
    //
    // It discharges the two handle promises this entry point makes. Both are
    // unconditional and neither can be kept by the resolving walk further
    // down:
    //
    //   pReleaseFenceFd  [out]  ext.h promises the library always writes it,
    //                           which is what makes a bare `int fd;` plus
    //                           close(fd) a legal caller shape.
    //   acquireFenceFd   [in]   ext.h promises "the library takes ownership
    //                           and closes it on every exit path", and design
    //                           section 2.3 states the rule the whole API is
    //                           built on -- the library consumes POSIX fds it
    //                           is given, always, on every exit path,
    //                           "including argument-validation failures that
    //                           never reach Vulkan". The SYNC_FD row of that
    //                           section's ownership table names the acquire
    //                           fence explicitly, so it is in scope.
    //
    // The resolving walk cannot keep either promise, for one shared reason: it
    // refuses on an unknown sType or an unresolvable id, and a chain may
    // legally put either of those AHEAD of the fence descriptor
    // (`info.pNext = &sync; sync.pNext = &fence;` is one of the three shapes
    // this header blesses), in which case the fence node is never reached at
    // all. The three top-of-function refusals are worse still -- a mis-stamped
    // info.sType, a session that is not initialized, and a resource id that
    // does not resolve all return before the walk begins. Chromium's VEA arms
    // acquireFenceFd on essentially every frame
    // (vulkan_video_encode_accelerator.cc: `fence_desc.acquireFenceFd =
    // fd.release();`, under a comment saying ownership transfers), so each of
    // those exits was one leaked sync_fd on the production path, not a
    // theoretical one.
    //
    // AN UNRESOLVED TENSION, LEFT UNRESOLVED ON PURPOSE. Read this before
    // moving either half of it. This pre-pass dereferences info.pNext BEFORE
    // info.sType has been validated, and the two rules behind that ordering
    // genuinely conflict:
    //
    //   FOR reading pNext first: section 2.3 says the library consumes a
    //   handle it was given on EVERY exit path, "including argument-validation
    //   failures that never reach Vulkan". A refusal ON info.sType is one of
    //   those exits. To consume the fd there it has to be FOUND there, which
    //   means walking pNext before the sType is known to be good. There is no
    //   ordering that validates sType first and still honours 2.3 for a handle
    //   supplied alongside a bad sType.
    //
    //   AGAINST: VkVideoEncoderFrameSubmitInfo has NO default member
    //   initialisers -- unlike VkVideoEncoderFrameFenceDescriptor and
    //   VkVideoEncoderFrameSyncDescriptor, which both default pNext to
    //   nullptr. A stack-local `VkVideoEncoderFrameSubmitInfo info;` is
    //   therefore indeterminate in every field, and the sType check is
    //   the gate that caught exactly that. Reading pNext off such a struct is
    //   undefined behaviour, and if the garbage reads as a fence node this
    //   pre-pass stores -1 through a garbage int* and records a garbage int
    //   for the guard to close.
    //
    // WHAT WAS CHANGED WHEN THIS WAS RAISED: only the boundedness of the walk
    // below -- a node cap and a cycle check, which removes the
    // spin-at-100%-CPU half of the hazard and protects the pReleaseFenceFd
    // half of this same pre-pass as a side effect. The ORDERING was not
    // touched.
    //
    // WHAT WAS NOT, AND WHY IT IS A HUMAN'S CALL: the obvious middle is to
    // validate info.sType first and treat a wrong-sType submit as "no handle
    // was legibly given" -- nothing is walked, so nothing is consumed. That is
    // defensible on its own terms (a struct whose type tag is wrong is not a
    // struct whose pNext can be trusted to name anything), and it costs
    // exactly one row of 2.3's promise: a caller who mis-stamps sType while
    // arming a real fd leaks it, and ext.h's "closes it on every exit path"
    // would need that exception written into it in so many words. The other
    // option, giving VkVideoEncoderFrameSubmitInfo default member
    // initialisers, removes the indeterminate-struct case for C++ callers
    // only, and changes the initialisation rules of a pinned public layout.
    // Both are API decisions rather than implementation ones, so neither was
    // taken here.
    //
    // It traverses UNKNOWN nodes rather than stopping at them, and that is not
    // a liberty: this header states that its chain is "Vulkan's own
    // convention", and the whole point of that convention is that every
    // chained struct opens with {sType, pNext} so a consumer can traverse a
    // chain containing structs it does not know -- which is what
    // VkBaseInStructure exists for. A caller whose struct does not open that
    // way has already broken the ABI in a way the resolving walk's very first
    // sType read would fault on too. Traversing is not HONOURING: the walk
    // below still refuses on an unknown sType rather than skipping it, because
    // refuse-don't-skip is about never silently ignoring a struct the caller
    // believed was acted on, which is a different question from layout.
    struct ChainNode {
        VkVideoEncoderStructureType sType;
        const void*                 pNext;
    };
    // The IN half. Ownership of every armed acquire fd is taken HERE and held
    // until either the import takes it or this function returns; exactly one
    // of those two closes it. Consume() hands ownership to
    // ImportAcquireFenceLocked, which owns the fd on every one of its own
    // paths (POSIX; the Win32 arm refuses without an fd to own)
    // -- both failure legs close it, and a successful
    // vkImportSemaphoreFdKHR of a SYNC_FD consumes it -- and anything this
    // guard still holds at scope exit provably never reached an import.
    //
    // The handoff is a REMOVAL from the list, not a flag, because a double
    // close is strictly worse than the leak this replaces: the fd number is
    // free the instant the first close returns, so a second one can close an
    // unrelated descriptor the process has since opened at that number.
    struct AcquireFdOwnership {
        std::vector<int> pending;
        void Record(int fd) {
            // Deduplicated by VALUE, so this list holds each number at most
            // once and the destructor below can never close one twice.
            //
            // That is the whole of what the dedup does, and the whole of what
            // it claims. NOTHING HERE DIAGNOSES THE DUPLICATE -- an earlier
            // revision of this comment said the walk would report it when "the
            // second import fails on an fd the first one consumed", and that
            // was wrong twice over: nothing compared the two nodes, and a
            // second vkImportSemaphoreFdKHR on a number the first import
            // consumed is not guaranteed to fail, so the caller could come
            // away with two semaphores whose payload came from one fence. The
            // duplicate is caught at the CONSUME site in the walk below
            // instead, which is the first point at which it is knowable.
            for (size_t i = 0; i < pending.size(); i++) {
                if (pending[i] == fd) {
                    return;
                }
            }
            pending.push_back(fd);
        }
        // Hands ownership of |fd| to the caller. Returns false when this guard
        // does not hold it -- which, since the pre-pass recorded every armed
        // fd in the chain before anything could consume one, can only mean an
        // EARLIER fence node named the same number and its import already took
        // it. The walk turns that into a typed refusal rather than importing
        // one descriptor twice.
        bool Consume(int fd) {
            for (std::vector<int>::iterator it = pending.begin();
                 it != pending.end(); ++it) {
                if (*it == fd) {
                    pending.erase(it);
                    return true;
                }
            }
            return false;
        }
        ~AcquireFdOwnership() {
#if defined(__linux__)
            for (size_t i = 0; i < pending.size(); i++) {
                close(pending[i]);
            }
#endif
        }
    } acquireFds;
    // BOUNDED, because the chain is caller memory and nothing in the ABI stops
    // it from pointing at itself. `fence.pNext = &fence` is one assignment
    // away, and an unbounded walk over it does not refuse and does not return
    // -- it spins inside the library at 100% CPU with the caller's fd still
    // open. A hang is a worse answer than any refusal, so the traversal is
    // capped and a cycle is named.
    //
    // ONE bound covers both walks in this function. The resolving walk further
    // down traverses the same chain and is equally unbounded on paper, but it
    // is unreachable with a cyclic or absurd chain because this pre-pass runs
    // first and refuses; bounding it twice would be two things to keep in step
    // for no extra coverage. The same bound is what now protects the
    // pReleaseFenceFd half of this pre-pass, which had the identical exposure.
    //
    // The cap is a sanity bound, not a design limit: the header blesses three
    // chain shapes and none is longer than two nodes.
    static const size_t kMaxChainNodes = 64;
    size_t              chainNodes     = 0;
    const ChainNode*    slow           = (const ChainNode*)info.pNext;
    for (const ChainNode* pre = (const ChainNode*)info.pNext; pre != nullptr;
         pre = (const ChainNode*)pre->pNext) {
        if (++chainNodes > kMaxChainNodes) {
            return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
        }
        if ((chainNodes % 2) == 0) {
            // Floyd at half speed: |slow| sits at node chainNodes/2 while
            // |pre| sits at node chainNodes-1, so the gap between them grows
            // through every non-negative integer and therefore reaches a
            // multiple of the cycle length once both are inside one -- at
            // which point they are the same node. |slow| always trails |pre|
            // through nodes |pre| has already dereferenced, so its pNext is
            // never a fresh read.
            //
            // The chainNodes > 2 guard is load-bearing, not decoration: at
            // chainNodes 1 and 2 those two indices coincide legitimately on a
            // straight chain, and comparing there would call every two-node
            // chain -- one of the three shapes the header blesses -- a cycle.
            slow = (const ChainNode*)slow->pNext;
            if ((chainNodes > 2) && (slow == pre)) {
                return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
            }
        }
        if (pre->sType !=
            VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_FENCE_DESCRIPTOR) {
            continue;
        }
        const VkVideoEncoderFrameFenceDescriptor* f =
            (const VkVideoEncoderFrameFenceDescriptor*)pre;
        // The OUT half, carried across unchanged from where this pre-pass used
        // to sit: answering -1 for every fence descriptor in the chain first
        // is the only order in which the promise holds for all of them. A
        // successful export overwrites it after the submit.
        if (f->pReleaseFenceFd != nullptr) {
            *f->pReleaseFenceFd = -1;
        }
        if (f->acquireFenceFd >= 0) {
            acquireFds.Record(f->acquireFenceFd);
        }
    }

    if (info.sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    // A session is a real encoder OR the test seam's null backend standing
    // in for one -- never both (see VkEncInstallNullBackend).
    if (!m_initialized || (!m_encoder && (m_nullBackend == nullptr))) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_NOT_INITIALIZED;
    }

    // Reap release-fence semaphores whose frames retired with a real capture.
    // Done on the submit path rather than at retirement because the
    // retirement sites hold m_pendingMutex, and a driver call under that lock
    // is the one-sided-lock hazard the rest of this file avoids by
    // construction. Bounded: a steady-state consumer releases every frame it
    // acquires, so this stays at the in-flight depth.
    DrainRetiredReleaseFences();
    // And the graveyard that cannot prove itself, once it has grown past its
    // bound. A no-op below the bound, which is every steady-state submit.
    DrainUnprovenSemaphores();

    // Resolve the registration and take a reference in one locked step, so
    // an Unregister cannot land between the lookup and the reference.
    VkImage       image = VK_NULL_HANDLE;
    VkFormat      format = VK_FORMAT_UNDEFINED;
    uint32_t      width = 0;
    uint32_t      height = 0;
    VkImageLayout registeredLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkVideoEncoderInputResidency residency =
        VK_VIDEO_ENCODER_INPUT_RESIDENCY_FOREIGN;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkSharedBaseObj<VulkanVideoImagePoolNode> node;
    bool encodeCapable = false;
    bool routeViaFilter = false;

    // Release-obligation guard. Once the reference below is taken, EVERY exit
    // from this function owes a release, and a single missed site does not
    // fail loudly -- it pins a producer's pool slot forever, which surfaces
    // much later as a stall with no cause attached. Arming a scope-exit here
    // and disarming it only on successful enqueue makes the obligation
    // structural instead of a rule each new early return has to remember.
    struct ReleaseObligation {
        VulkanVideoEncoderExtImpl* owner = nullptr;
        VkVideoEncoderResource     resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        bool                       armed = false;
        void Arm(VulkanVideoEncoderExtImpl* o, VkVideoEncoderResource r) {
            owner = o; resource = r; armed = true;
        }
        void Disarm() { armed = false; }
        ~ReleaseObligation() {
            if (armed) {
                owner->ReleaseResourceReference(resource);
            }
        }
    } obligation;

    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        RegisteredImage* slot = LookupResourceLocked(info.resource);
        if ((slot == nullptr) || slot->retired) {
            // Stale, retired, or never registered. The generation counter is
            // what makes this a rejection rather than a wrong-image encode.
            return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
        }
        slot->inFlight++;
        obligation.Arm(this, info.resource);
        image            = slot->image;
        format           = slot->format;
        width            = slot->width;
        height           = slot->height;
        registeredLayout = slot->defaultLayout;
        tiling = slot->tiling;
        // The once-per-registration node: a VkSharedBaseObj copy (refcount
        // bump, no allocation). Shared by every frame of this registration
        // and read-only on the encode path. encodeCapable is the
        // registration-time routing predicate; the per-frame tiling field
        // no longer routes registered frames.
        node          = slot->node;
        encodeCapable = slot->encodeCapable;
        // Read from the resolved path, not recomputed: the registration
        // already answered which rung this image takes. That answer is
        // INTERNAL: no public accessor returns it. The route is the
        // library's choice, not a contract, and the type that names it is
        // declared in the internal header for that reason.
        routeViaFilter =
            (slot->inputPath == VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER);
        // RESIDENCY: HONOURED WHEREVER IT WAS DECLARED, DERIVED ONLY WHERE
        // IT WAS NOT.
        //
        // This must NOT read `handleType == VK_IMAGE ? slot->residency :
        // FOREIGN`, which DISCARDS an explicit declaration on every
        // OS-handle type. The justification given for that -- "the library
        // performed the import, so it knows the memory is foreign to the
        // encode device and the caller cannot get it wrong" -- is false of
        // the one shipping consumer of this path. Chromium's CPU staging
        // tier is a SELF-IMPORT: the staging VkImage and its VkDeviceMemory
        // are created on the LIBRARY's own VkDevice (it asks for it with
        // GetVkDevice()), exported OPAQUE_FD from that device, and
        // re-imported into that same device. There is no second device and
        // no second queue family, so nothing about that memory is foreign
        // and the caller is the only party that knows it.
        //
        // WHAT ACTUALLY DISCRIMINATES is ownership by an external allocator
        // or queue family -- NOT who wrote the pixels, and NOT the handle
        // type. The distinction has a live counterexample in each direction:
        //   * host-written yet correctly FOREIGN -- the Wayland zero-copy
        //     lane writes its pixels from the CPU, but the EXPORTER is
        //     GBM/DRM and the buffer really is owned outside this device;
        //   * OS-handle yet correctly LOCAL -- the self-import above.
        // A rule keyed on handleType cannot express either.
        //
        // SO THE DERIVATION IS KEPT, AND ONLY WHERE IT IS THE ONLY ANSWER
        // AVAILABLE: an OS-handle registration that declared AUTO (which is
        // 0, so it is also what a zero-initialised descriptor says) told the
        // library nothing, and FOREIGN remains the safe reading -- a
        // cross-process producer that propagates no residency is far more
        // likely to be a genuine import than a self-import. Both live
        // AUTO consumers depend on that and are covered by name in
        // test/encoder-ext-input-residency: the Wayland/GBM lane and the
        // out-of-tree Vulkan renderer app, neither of which declares
        // residency at all.
        //
        // VK_IMAGE IS UNTOUCHED IN BOTH DIRECTIONS, including VK_IMAGE +
        // AUTO, which still reaches VkVideoEncoder's legacy layout
        // heuristic rather than being forced to FOREIGN. That matters
        // concretely: the sibling suites register VK_IMAGE with a
        // zero-initialised descriptor, and forcing those to FOREIGN would
        // hand their images to VK_QUEUE_FAMILY_FOREIGN_EXT.
        //
        // THE HAZARD IS THE RELEASE, NOT A MISSING ONE. This comment used to
        // say such a registration would take "an acquire with no matching
        // release". That has been false since VkVideoEncoder gained
        // ReleaseImageToForeignQueue, which every acquire arm now pairs with
        // under a byte-identical gate -- StageInputFrame's copy and filter
        // arms, and RecordVideoCodingCmd's direct arm. The real cost of the
        // hypothetical is the opposite, and worse: the forced registration
        // would get a CORRECTLY paired acquire AND release, and the release
        // is what does the damage. It gives a library-local image away to a
        // foreign owner that does not exist, so nothing transitions it back,
        // and the paired SetStagedInputResidualLayout(VK_IMAGE_LAYOUT_MAX_ENUM)
        // discards the library's record of the layout it left behind. The
        // next frame's acquire then names a layout the image is not in.
        const bool deriveForeign =
            (slot->handleType !=
             VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE) &&
            (slot->residency == VK_VIDEO_ENCODER_INPUT_RESIDENCY_AUTO);
        residency = deriveForeign
                        ? VK_VIDEO_ENCODER_INPUT_RESIDENCY_FOREIGN
                        : slot->residency;
    }

    VkVideoEncodeInputFrame frame = {};
    frame.image  = image;
    frame.format = format;
    frame.width  = width;
    frame.height = height;
    frame.imageTiling = tiling;
    // THE SENTINEL, AND THE FACT IT ERASES. VkVideoEncoderFrameSubmitInfo::
    // currentLayout documents UNDEFINED as "as declared at registration",
    // so this line is where a per-frame statement by the producer and a
    // standing registration default become one indistinguishable value.
    // The encoder core needs them distinguished -- an explicit per-frame
    // declaration means "I moved it" and must beat the library's own record
    // of where its last handback left the image -- so the bit is captured
    // here, at the only place that still has it, and passed down.
    const bool srcLayoutIsExplicit =
        (info.currentLayout != VK_IMAGE_LAYOUT_UNDEFINED);
    frame.currentLayout = srcLayoutIsExplicit ? info.currentLayout
                                             : registeredLayout;
    frame.frameId     = info.frameId;
    frame.pts         = info.pts;
    frame.forceIDR    = info.forceIDR;
    frame.isLastFrame = info.isLastFrame;
    frame.qpOverride  = info.qpOverride;
    frame.inputResidency = residency;
    // No caller tag on this path: VkVideoEncoderFrameSubmitInfo carries
    // none, because the registration already names the image.
    frame.uniqueImageIndex = -1;
    frame.waitSemaphoreCount   = info.waitSemaphoreCount;
    frame.pWaitSemaphores      = info.pWaitSemaphores;
    frame.pWaitSemaphoreValues = info.pWaitSemaphoreValues;
    frame.signalSemaphoreCount   = info.signalSemaphoreCount;
    frame.pSignalSemaphores      = info.pSignalSemaphores;
    frame.pSignalSemaphoreValues = info.pSignalSemaphoreValues;

    // A chained FrameSyncDescriptor names REGISTERED semaphores by id, which
    // is what a cross-process producer can actually send: its handles crossed
    // once, at registration, and per frame it has only integers. Resolved
    // here into the same arrays the in-process path fills, so the encode path
    // below is identical either way.
    //
    // ONE FLAT CHAIN, POSITION-INDEPENDENT. Both descriptor types hang off
    // VkVideoEncoderFrameSubmitInfo::pNext and both are honoured wherever in
    // the list they appear -- fence alone, fence ahead of sync, or fence
    // behind it. That is Vulkan's own pNext convention and it is now what the
    // header says. The fence descriptor does not have to chain off the SYNC
    // descriptor's pNext specifically: this loop does not require it and no
    // caller does.
    //
    // These live until the submit below returns, which is the only span the
    // encoder reads them for.
    // Where the walk found a caller-supplied release-fence out-parameter, if
    // any. Recorded rather than acted on inside the loop: the loop still has
    // failing exits after the fence descriptor, and creating a Vulkan object
    // in front of them would leave it stranded on every one of them.
    // Both halves of the chain's fd contract were already discharged by the
    // pre-pass at the top of this function: the -1 stores are done, and every
    // armed acquire fd is held by a guard that closes anything no import takes.
    int* pReleaseFenceFd = nullptr;
    // Imported acquire-fence semaphores, in a scope guard rather than a bare
    // vector: every refusal exit BELOW an import -- an unknown sType or an
    // unresolvable id in a node the fence descriptor precedes -- would
    // otherwise strand one with no submit to retire it. Nothing has been
    // queued at any of those exits, so destroying there is legal; on the
    // success path the guard is disarmed and the PendingFrame takes over.
    struct AcquireFenceScope {
        VulkanVideoEncoderExtImpl* owner = nullptr;
        std::vector<VkSemaphore>   semaphores;
        bool                       armed = true;
        ~AcquireFenceScope() {
            if (!armed) {
                return;
            }
            for (VkSemaphore s : semaphores) {
                if (s != VK_NULL_HANDLE) {
                    owner->m_vkDevCtx.DestroySemaphore(
                        owner->m_vkDevCtx.getDevice(), s, nullptr);
                }
            }
        }
    } acquireFences;
    acquireFences.owner = this;
    std::vector<VkSemaphore> resolvedWait;
    std::vector<uint64_t>    resolvedWaitValues;
    std::vector<VkSemaphore> resolvedSignal;
    std::vector<uint64_t>    resolvedSignalValues;
    for (const void* next = info.pNext; next != nullptr;) {
        const VkVideoEncoderStructureType* stype =
            (const VkVideoEncoderStructureType*)next;
        if ((*stype != VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_SYNC_DESCRIPTOR) &&
            (*stype != VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_FENCE_DESCRIPTOR)) {
            // Unknown chained struct: refuse rather than skip. Silently
            // ignoring a struct a caller believed was honoured is the
            // accepted-and-ignored failure this ABI exists to prevent.
            // The armed guard drops the reference on the way out -- the
            // a per-site release/disarm pair at this exit would be the memory-based
            // pattern the guard exists to abolish (V2 design, risk R-2).
            return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
        }
        if (*stype == VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_FENCE_DESCRIPTOR) {
            const VkVideoEncoderFrameFenceDescriptor* fence =
                (const VkVideoEncoderFrameFenceDescriptor*)next;
            // Recorded BEFORE the import below, which has its own refusal
            // exit. The -1 itself was already written by the pre-pass above;
            // this only remembers where to put the export.
            if (fence->pReleaseFenceFd != nullptr) {
                pReleaseFenceFd = fence->pReleaseFenceFd;
            }
            if (fence->acquireFenceFd >= 0) {
                // Ownership passes from the pre-pass guard to the importer
                // HERE, before the call rather than after it: the importer
                // owns the fd on every one of its own paths (POSIX; the
                // Win32 arm refuses without an fd to own), both failure legs
                // included, so leaving it in the guard's list across this call
                // would be the double close the guard's comment forbids.
                if (!acquireFds.Consume(fence->acquireFenceFd)) {
                    // The pre-pass recorded every armed fd in this chain, so
                    // the only way the guard no longer holds this one is that
                    // an EARLIER fence node named the same number and its
                    // import has already taken it. Importing it again is not a
                    // smaller error than refusing: SYNC_FD is consumed by the
                    // first import, so the second call runs against a number
                    // that is either closed or has since been reused, and if
                    // it happens to succeed the caller is handed two
                    // semaphores whose payload came from one fence.
                    //
                    // Refuse, and close NOTHING: the descriptor belongs to the
                    // first import now, and a close here is the double close
                    // the guard exists to prevent. IMPORT_FAILED rather than a
                    // new code -- this is an acquire fence that could not be
                    // turned into a semaphore, which is what that status
                    // already says at the site just below.
                    return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
                }
                VkSemaphore acquired =
                    ImportAcquireFenceLocked(fence->acquireFenceFd);
                if (acquired == VK_NULL_HANDLE) {
                    // fd already closed by the helper on POSIX; on Win32
                    // there was no fd. Refuse rather than
                    // encode without the wait -- that reads the surface before
                    // the producer finished writing it.
                    return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
                }
                // NOT resolvedWait. resolvedWait is the REGISTERED-id answer
                // to "whose wait list does this frame use", and an acquire
                // fence is not an answer to that question -- it is one
                // additional producer fence, and the caller's raw
                // pWaitSemaphores may name others that no sync_fd covers (a
                // sync_fd is a single fence object; N producers need N fds or
                // a merge). Putting it here made `!resolvedWait.empty()` fire
                // and threw those raw handles away, encoding from a surface
                // a producer was still writing -- the exact mirror, on the
                // wait side, of the signal-side defect this walk was fixed
                // for. It is APPENDED below instead, like the release fence
                // on the signal side.
                acquireFences.semaphores.push_back(acquired);
            }
            next = fence->pNext;
            continue;
        }

        const VkVideoEncoderFrameSyncDescriptor* sync =
            (const VkVideoEncoderFrameSyncDescriptor*)next;

        for (uint32_t i = 0; i < sync->waitCount; i++) {
            VkSemaphore s = ResolveSemaphore(sync->pWaitSemaphores[i]);
            if (s == VK_NULL_HANDLE) {
                // A wait on an unresolvable id would either be skipped --
                // encoding from a surface the producer has not finished
                // writing -- or hang. Both are worse than a named refusal.
                // The guard releases on the way out.
                return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
            }
            resolvedWait.push_back(s);
            resolvedWaitValues.push_back(sync->pWaitValues[i]);
        }
        for (uint32_t i = 0; i < sync->signalCount; i++) {
            VkSemaphore s = ResolveSemaphore(sync->pSignalSemaphores[i]);
            if (s == VK_NULL_HANDLE) {
                // Same refusal as the wait side; the guard releases.
                return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
            }
            resolvedSignal.push_back(s);
            resolvedSignalValues.push_back(sync->pSignalValues[i]);
        }
        next = sync->pNext;
    }
    // Registered ids win outright rather than being merged with the raw
    // arrays: naming a frame's sync twice is two answers to one question,
    // and merging would make which one took effect unknowable.
    //
    // The override is PER DIRECTION, and the direction the chain did NOT name
    // keeps the caller's own array. One combined test that then overwrote both
    // is what this replaces, and it was live rather than latent: a chain
    // carrying only an acquire fence -- exactly what Chromium's VEA sends,
    // chaining a FrameFenceDescriptor whose only armed field is acquireFenceFd
    // while it fills the submit info's pSignalSemaphores from its own
    // end_semaphores -- resolves a non-empty wait list and an EMPTY signal
    // list. That passed the combined test and then set signalSemaphoreCount to
    // zero, discarding the caller's end-of-access semaphores on every fenced
    // frame without a word. Whoever waits on those semaphores waits forever,
    // which is the accepted-and-ignored failure this ABI exists to prevent.
    if (!resolvedWait.empty()) {
        frame.waitSemaphoreCount     = (uint32_t)resolvedWait.size();
        frame.pWaitSemaphores        = resolvedWait.data();
        frame.pWaitSemaphoreValues   = resolvedWaitValues.data();
    }
    if (!resolvedSignal.empty()) {
        frame.signalSemaphoreCount   = (uint32_t)resolvedSignal.size();
        frame.pSignalSemaphores      = resolvedSignal.data();
        frame.pSignalSemaphoreValues = resolvedSignalValues.data();
    }

    // Per-frame ACQUIRE fences, appended to whichever wait array won above --
    // the exact mirror of the release fence's append on the signal side, and
    // for the same reason. An acquire fence is additive: it names one more
    // producer, not a different way of naming the ones the caller already
    // named. These vectors have to outlive the submit call below, which is
    // why they are declared here rather than inside the branch.
    std::vector<VkSemaphore> waitWithAcquire;
    std::vector<uint64_t>    waitWithAcquireValues;
    if (!acquireFences.semaphores.empty()) {
        for (uint32_t i = 0; i < frame.waitSemaphoreCount; i++) {
            waitWithAcquire.push_back(frame.pWaitSemaphores[i]);
            waitWithAcquireValues.push_back(
                frame.pWaitSemaphoreValues ? frame.pWaitSemaphoreValues[i] : 0);
        }
        for (VkSemaphore s : acquireFences.semaphores) {
            // Value 0: binary semantics, as for the release fence.
            waitWithAcquire.push_back(s);
            waitWithAcquireValues.push_back(0);
        }
        frame.waitSemaphoreCount   = (uint32_t)waitWithAcquire.size();
        frame.pWaitSemaphores      = waitWithAcquire.data();
        frame.pWaitSemaphoreValues = waitWithAcquireValues.data();
    }

    // THE WAIT SIDE OF kMaxCallerSignalsForReleaseFence -- and the reason it
    // is here as well as in the submit.
    //
    // The direct-encode submit assembles its waits into a fixed 8-slot array.
    // It used to stop quietly at the boundary, discarding the surplus; since
    // the acquire semaphore is appended LAST, just above, the surplus was
    // exactly the producer fence this entry point exists to honour.
    // SubmitVideoCodingCmds now refuses that shape outright instead of
    // encoding without a wait -- but it runs on the assembly path, LONG AFTER
    // this function has returned a status to the caller. Measured on an
    // A4000: the frame is correctly not submitted and no release fd is
    // produced, and the caller is still handed SUCCESS. Whoever believed that
    // status waits forever for a completion that is never coming, which is
    // the same accepted-and-ignored failure in a new place.
    //
    // So the count is checked HERE too, where a status can still be returned,
    // and RESOURCE_LIMIT is what it is: a fixed resource, named, exceeded.
    //
    // Gated on |encodeCapable| because the limit is a property of the DIRECT
    // submit alone. A staged registration assembles its waits into a
    // std::vector that never truncates -- nine waits work there today, and
    // refusing them because a different path has an array would be inventing
    // a limit the library does not have.
    //
    // The bound is the array, not a smaller number of "caller" waits: seven
    // caller waits plus an acquire fence is eight entries, it fits, and it
    // must keep working.
    //
    // Not exact in one direction, deliberately: an enableQpMap session also
    // spends a slot on its qpMap command buffer, which this layer cannot see,
    // so such a session can still be refused by the submit rather than here.
    // That path keeps its own check; this one removes the common case from
    // the silent-failure class without pretending to knowledge it lacks.
    //
    // Exiting here leaves |acquireFences| ARMED, so the semaphore imported
    // above is destroyed on the way out, and the caller's fd was already
    // consumed by the import -- the ownership contract this entry point
    // publishes is unchanged by the refusal.
    constexpr uint32_t kMaxDirectSubmitWaits = 8;
    if (encodeCapable && (frame.waitSemaphoreCount > kMaxDirectSubmitWaits)) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_LIMIT;
    }

    // Per-frame release fence (design 3.5), the export half of this entry
    // point: a library-owned BINARY semaphore riding the frame's signal list,
    // so that whichever submission consumes the input image signals it.
    // SubmitExternalFrameCommon exports the SYNC_FD from it, once that
    // submission has been issued -- see the long note there.
    //
    // APPENDED to the winner of the per-direction override above, never
    // merged into the override decision itself. Two reasons, both load-
    // bearing. First, the override answers "whose sync list does this frame
    // use", and a library semaphore joining the list must not change that
    // answer -- pushing into resolvedSignal would make an acquire-only chain
    // look like a chain that named signals, and throw the caller's signal
    // array away, which is the defect the per-direction rule exists to
    // prevent. Second, appending is what keeps this OUT of index 0: the
    // direct-encode submit gives signal index 0 the batched input-release
    // timeline treatment (max-value-so-far at a queue flush point), which is
    // correct for a timeline and wrong for a binary semaphore.
    //
    // The count gate is not decoration. The direct-encode submit assembles
    // its signal list into a fixed 8-slot array shared with the encoder's own
    // internal signals and silently stops appending when it fills. A release
    // fence that got dropped there would never be signalled while its fd had
    // already been handed out -- a caller waiting forever on a fence nobody
    // signals, i.e. the hang this fence exists to prevent, caused by the
    // fence. Refusing with -1 is the honest answer.
    constexpr uint32_t kMaxCallerSignalsForReleaseFence = 4;
    VkSemaphore releaseFenceSemaphore = VK_NULL_HANDLE;
    std::vector<VkSemaphore> signalWithRelease;
    std::vector<uint64_t>    signalWithReleaseValues;
    if ((pReleaseFenceFd != nullptr) && (m_nullBackend == nullptr) &&
        (frame.signalSemaphoreCount < kMaxCallerSignalsForReleaseFence)) {
        releaseFenceSemaphore = CreateReleaseFenceSemaphore();
    }
    if (releaseFenceSemaphore != VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < frame.signalSemaphoreCount; i++) {
            signalWithRelease.push_back(frame.pSignalSemaphores[i]);
            signalWithReleaseValues.push_back(
                frame.pSignalSemaphoreValues ? frame.pSignalSemaphoreValues[i]
                                             : 0);
        }
        // Value 0: binary semantics, which is also what tells the direct
        // encode submit's release-timeline block to pass this entry through
        // per frame instead of batching it.
        signalWithRelease.push_back(releaseFenceSemaphore);
        signalWithReleaseValues.push_back(0);
        frame.signalSemaphoreCount   = (uint32_t)signalWithRelease.size();
        frame.pSignalSemaphores      = signalWithRelease.data();
        frame.pSignalSemaphoreValues = signalWithReleaseValues.data();
    }

    const VkResult result = NoteDeviceResult(SubmitExternalFrameCommon(
        frame, node ? &node : nullptr, encodeCapable, routeViaFilter,
        info.resource,
        pStagingCompleteSemaphore, releaseFenceSemaphore, pReleaseFenceFd,
        acquireFences.semaphores.empty() ? nullptr
                                         : &acquireFences.semaphores,
        srcLayoutIsExplicit));
    if (result != VK_SUCCESS) {
        // The release-fence semaphore never reached a PendingFrame, so its
        // disposal is this scope's. VK_NOT_READY is admission control, which
        // refuses BEFORE the encoder is touched -- nothing was queued and the
        // destroy precondition is trivially met. Any other failure can in
        // principle have got as far as a staging submit before failing, and
        // vkDestroySemaphore requires every batch referring to the semaphore
        // to have completed, so those go to the teardown graveyard instead of
        // being destroyed on a guess.
        if (releaseFenceSemaphore != VK_NULL_HANDLE) {
            if (result == VK_NOT_READY) {
                m_vkDevCtx.DestroySemaphore(m_vkDevCtx.getDevice(),
                                            releaseFenceSemaphore, nullptr);
            } else {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_unprovenSemaphores.push_back(releaseFenceSemaphore);
            }
        }
        // The imported acquire semaphores follow the same rule, and for the
        // same reason. On VK_NOT_READY the guard's destructor destroys them
        // (nothing was queued); on anything else a staging submit may already
        // be waiting on them, so they go to the graveyard and the guard is
        // told to keep its hands off.
        if ((result != VK_NOT_READY) && !acquireFences.semaphores.empty()) {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_unprovenSemaphores.insert(m_unprovenSemaphores.end(),
                                        acquireFences.semaphores.begin(),
                                        acquireFences.semaphores.end());
            acquireFences.armed = false;
        }
        // Nothing was queued, so the reference must not survive the failure.
        // The guard does this on the way out; no explicit release here.
        if (result == VK_NOT_READY) {
            // Admission-control backpressure: transient by construction --
            // the caller drains completions and retries this same call.
            // Distinct from ERROR_RESOURCE_LIMIT, which the registries
            // return for a FULL 4096-slot table: a condition only an
            // Unregister can clear, where retry alone never succeeds.
            return VK_VIDEO_ENCODER_STATUS_NOT_READY;
        }
        return VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    }
    // Queued: EnqueuePendingFrame copied the acquire semaphores into the
    // PendingFrame, which now owns them, so the scope guard must not.
    acquireFences.armed = false;

    // Queued, and the entry took ownership of the reference at its own
    // creation, under the lock that created it (EnqueuePendingFrame) -- so
    // the frame drops it when it retires and this scope must not. Stamping
    // the resource in AFTER the fact was R-2's window inside this
    // function's own mitigation: a consumer thread could retire the entry
    // between the enqueue and the stamp, the stamp's miss was silent, and
    // the reference became un-droppable -- with the guard already
    // disarmed. Nothing can return between the enqueue and this disarm, so
    // the reference has exactly one owner on every path.
    obligation.Disarm();
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

void VulkanVideoEncoderExtImpl::ReleaseResourceReference(
    VkVideoEncoderResource resource)
{
    if (resource == VK_VIDEO_ENCODER_RESOURCE_NULL) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_resourceMutex);
    const size_t index = ResourceIndex(resource);
    if (index >= m_resources.size()) {
        return;
    }
    RegisteredImage& slot = m_resources[index];
    // Deliberately NOT generation-checked: this drops a reference taken
    // while the id was valid, and the slot may already be retired.
    if (slot.inFlight > 0) {
        slot.inFlight--;
    }
    // The deferred half of retirement: the last frame out frees the image.
    if (slot.retired && (slot.inFlight == 0)) {
        DestroyResourceLocked(slot);
    }
}

VkVideoEncoderStatusCode VulkanVideoEncoderExtImpl::UnregisterImageResource(
    VkVideoEncoderResource resource)
{
    // Declared ahead of the lock guard so it fires after the lock is
    // released -- ForgetImportContentProbe takes m_pendingMutex, and the
    // order is pending -> resource. Unconditional: forgetting an id the
    // probe never knew is a no-op, and running it on the RESOURCE_UNKNOWN
    // exit too is what keeps a double-unregister from leaving a verdict
    // behind that GetCompletionInfo would keep reporting forever.
    struct ContentProbeForgetter {
        VulkanVideoEncoderExtImpl* self;
        VkVideoEncoderResource     resource;
        ~ContentProbeForgetter() { self->ForgetImportContentProbe(resource); }
    } contentProbeForgetter{this, resource};

    std::lock_guard<std::mutex> lock(m_resourceMutex);
    RegisteredImage* slot = LookupResourceLocked(resource);
    if (slot == nullptr) {
        // Either never registered or already retired. Naming it rather than
        // returning a bare failure is the difference between a client that
        // can debug its own map and one that cannot.
        return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
    }
    if (slot->inFlight > 0) {
        // Deferred retirement: frames still reference this image. Marking it
        // and freeing on the last release is what keeps a teardown from
        // freeing memory the GPU is still reading.
        slot->retired = true;
        return VK_VIDEO_ENCODER_STATUS_SUCCESS;
    }
    DestroyResourceLocked(*slot);
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

VkResult VkEncInjectImportContentMeasurement(VulkanVideoEncoderExt* encoder,
                                             VkVideoEncoderResource resource,
                                             uint32_t meanYQ8,
                                             uint32_t meanUQ8,
                                             uint32_t meanVQ8)
{
    if (encoder == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto* impl = static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    VkSharedBaseObj<VkVideoEncoderContentProbe> probe;
    {
        std::lock_guard<std::mutex> lock(impl->m_pendingMutex);
        probe = impl->m_contentProbe;
    }
    if (!probe) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    probe->ApplyVerdict(resource, meanYQ8, meanUQ8, meanVQ8);
    return VK_SUCCESS;
}

//=============================================================================
// Factory function
//=============================================================================

VK_VIDEO_ENCODER_EXPORT
VkResult CreateVulkanVideoEncoderExt(
    VkSharedBaseObj<VulkanVideoEncoderExt>& vulkanVideoEncoder)
{
    VkSharedBaseObj<VulkanVideoEncoderExt> impl(new VulkanVideoEncoderExtImpl());
    if (!impl) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    vulkanVideoEncoder = impl;
    return VK_SUCCESS;
}

//=============================================================================
// Fault-injection seam (vulkan_video_encoder_ext_internal.h). Only objects
// from CreateVulkanVideoEncoderExt may be passed: the static_cast below is
// sound because that factory mints every real VulkanVideoEncoderExt in
// existence. (The Chromium test fake implements the interface but never
// travels through these functions -- they exist for exactly what the fake
// cannot test.)
//=============================================================================

VkResult VkEncInstallNullBackend(VulkanVideoEncoderExt* encoder,
                                 const VkEncNullBackendState* state)
{
    if ((encoder == nullptr) || (state == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    if (impl->m_initialized) {
        // Backends stand in for a real session; they never replace one.
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    impl->m_nullBackend = state;
    impl->m_initialized = true;
    return VK_SUCCESS;
}

// Copy out what the submit was handed. Counts are reported in full even when
// they exceed the probe's array capacity, so a test can never mistake a
// truncated record for a short list.
void VulkanVideoEncoderExtImpl::RecordSubmitSyncForTest(
    const VkVideoEncodeInputFrame& frame)
{
    const uint32_t cap = (uint32_t)kVkEncSubmitSyncProbeCapacity;
    std::lock_guard<std::mutex> lock(m_lastSubmitSyncMutex);
    m_lastSubmitSync = VkEncSubmitSyncProbe{};
    m_lastSubmitSync.recorded    = VK_TRUE;
    m_lastSubmitSync.waitCount   = frame.waitSemaphoreCount;
    m_lastSubmitSync.signalCount = frame.signalSemaphoreCount;
    for (uint32_t i = 0; (i < frame.waitSemaphoreCount) && (i < cap); i++) {
        if (frame.pWaitSemaphores != nullptr) {
            m_lastSubmitSync.waitSemaphores[i] = frame.pWaitSemaphores[i];
        }
        if (frame.pWaitSemaphoreValues != nullptr) {
            m_lastSubmitSync.waitValues[i] = frame.pWaitSemaphoreValues[i];
        }
    }
    for (uint32_t i = 0; (i < frame.signalSemaphoreCount) && (i < cap); i++) {
        if (frame.pSignalSemaphores != nullptr) {
            m_lastSubmitSync.signalSemaphores[i] = frame.pSignalSemaphores[i];
        }
        if (frame.pSignalSemaphoreValues != nullptr) {
            m_lastSubmitSync.signalValues[i] = frame.pSignalSemaphoreValues[i];
        }
    }
}

VkResult VkEncInstallTestSemaphore(VulkanVideoEncoderExt* encoder,
                                   VkSemaphore semaphore,
                                   VkVideoEncoderResource* outResource)
{
    if ((encoder == nullptr) || (outResource == nullptr) ||
        (semaphore == VK_NULL_HANDLE)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    if (impl->m_nullBackend == nullptr) {
        // A real session registers through RegisterSemaphore, which creates a
        // timeline semaphore and imports an OS handle into it. This
        // device-free shortcut stands in for that; it never stands beside it.
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    std::lock_guard<std::mutex> lock(impl->m_semaphoreMutex);
    size_t index = impl->m_semaphores.size();
    for (size_t i = 0; i < impl->m_semaphores.size(); i++) {
        if (!impl->m_semaphores[i].live) {
            index = i;
            break;
        }
    }
    if (index == impl->m_semaphores.size()) {
        impl->m_semaphores.emplace_back();
    }
    VulkanVideoEncoderExtImpl::RegisteredSemaphore& slot =
        impl->m_semaphores[index];
    slot.semaphore = semaphore;
    slot.live      = true;
    *outResource =
        VulkanVideoEncoderExtImpl::MakeResourceId(index, slot.generation) |
        VulkanVideoEncoderExtImpl::kSemaphoreTag;
    return VK_SUCCESS;
}

VkVideoEncoderStatusCode VkEncUninstallTestSemaphore(
    VulkanVideoEncoderExt* encoder, VkVideoEncoderResource resource)
{
    if (encoder == nullptr) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    if (impl->m_nullBackend == nullptr) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
    }
    if ((resource & VulkanVideoEncoderExtImpl::kSemaphoreTag) == 0) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
    }
    const uint64_t untagged =
        resource & ~VulkanVideoEncoderExtImpl::kSemaphoreTag;
    const size_t   index      = (size_t)((untagged & 0xFFFFFFFFull) - 1);
    const uint32_t generation = (uint32_t)(untagged >> 32);
    std::lock_guard<std::mutex> lock(impl->m_semaphoreMutex);
    if (index >= impl->m_semaphores.size()) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
    }
    VulkanVideoEncoderExtImpl::RegisteredSemaphore& slot =
        impl->m_semaphores[index];
    if (!slot.live || (slot.generation != generation)) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
    }
    // UnregisterSemaphore's slot hygiene without its driver destroy: the
    // handle was never a real VkSemaphore and the session has no device to
    // destroy one with.
    slot.semaphore = VK_NULL_HANDLE;
    slot.live      = false;
    slot.generation++;
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

VkVideoEncoderStatusCode VkEncProbeLastSubmitSync(
    VulkanVideoEncoderExt* encoder, VkEncSubmitSyncProbe* outProbe)
{
    if ((encoder == nullptr) || (outProbe == nullptr)) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    std::lock_guard<std::mutex> lock(impl->m_lastSubmitSyncMutex);
    *outProbe = impl->m_lastSubmitSync;
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

VkVideoEncoderStatusCode VkEncProbeResource(VulkanVideoEncoderExt* encoder,
                                            VkVideoEncoderResource resource,
                                            VkEncResourceProbe* outProbe)
{
    if ((encoder == nullptr) || (outProbe == nullptr)) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    std::lock_guard<std::mutex> lock(impl->m_resourceMutex);
    const size_t index = VulkanVideoEncoderExtImpl::ResourceIndex(resource);
    if (index >= impl->m_resources.size()) {
        return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
    }
    const VulkanVideoEncoderExtImpl::RegisteredImage& slot =
        impl->m_resources[index];
    if (slot.generation !=
        VulkanVideoEncoderExtImpl::ResourceGeneration(resource)) {
        // Retirement completed: the generation advanced past this id.
        return VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN;
    }
    outProbe->inFlight = slot.inFlight;
    outProbe->live     = slot.live ? VK_TRUE : VK_FALSE;
    outProbe->retired  = slot.retired ? VK_TRUE : VK_FALSE;
    outProbe->inputPath = slot.inputPath;
    outProbe->planeStorageViews =
        slot.planeStorageViews ? VK_TRUE : VK_FALSE;
    outProbe->storageReadView =
        slot.storageReadView ? VK_TRUE : VK_FALSE;
    return VK_VIDEO_ENCODER_STATUS_SUCCESS;
}

VkBool32 VkEncSessionInitialized(VulkanVideoEncoderExt* encoder)
{
    if (encoder == nullptr) {
        return VK_FALSE;
    }
    return static_cast<VulkanVideoEncoderExtImpl*>(encoder)
                   ->m_initialized.load()
               ? VK_TRUE
               : VK_FALSE;
}

void VkEncFireCompletionEdge(VulkanVideoEncoderExt* encoder, uint64_t frameId)
{
    if (encoder == nullptr) {
        return;
    }
    // The production edge, verbatim: OnBitstreamCaptured is what the
    // assembly path calls on every capture push, so the stress exercises
    // the real counter/signal/invoke sequence and the real lock order.
    static_cast<VulkanVideoEncoderExtImpl*>(encoder)
        ->OnBitstreamCaptured(frameId);
}

namespace {

// Device-free capture source for VkEncPushCapture: the codec pure virtuals
// are stubbed (never called -- the null backend terminates every submit
// before the encoder would run) and the protected funnel entry point is
// re-exposed. Constructed with a null device context, which the base
// class's constructor (pure member-init) and destructor path (DeinitEncoder
// guards the missing device; no threads were started) both tolerate -- the
// same shape the capture-funnel unit tests rely on.
// AN H.264 ENCODER, NOT AN IMITATION OF ONE. The device-free capture
// backend derives from VkVideoEncoderH264 and holds a real
// EncoderConfigH264 so that the codec arm a test drives is the SHIPPED
// one: VkVideoEncoderH264::RefreshCodecRateControlParameters runs the
// real GetRateControlParameters into the real
// m_h264.m_rateControlLayersInfoH264, which is the struct
// CodecHandleRateControlCmd chains onto a control command. A stand-in
// that reimplemented that one line would prove only that the stand-in
// worked.
//
// NONE OF IT NEEDS A DEVICE. VkVideoEncoderH264 constructs from a null
// device context by pure member-init; its destructor joins threads that
// were never started and releases handles that were never created; and
// EncoderConfigH264 default-constructs and its rate-control fill reads
// config state only. The one thing InitEncoderCodec would have done that
// matters here is record the device QP window, so this declares one --
// [0, 51], the window a real H.264 device reports.
class VkEncNullBackendCaptureSource : public VkVideoEncoderH264 {
public:
    VkEncNullBackendCaptureSource() : VkVideoEncoderH264(nullptr) {
        // Qualified: VkVideoEncoderH264 declares a private member of the
        // same name that aliases this one in a device-initialized session.
        // The base pointer is the one ApplyPendingRateControlUpdate writes
        // and the one the refresh reads, so it is the one to stand up.
        VkVideoEncoder::m_encoderConfig =
            VkSharedBaseObj<EncoderConfig>(new EncoderConfigH264());
        SetDeviceQpWindowForTest(0, 51);
    }

    void PushCaptureRecord(uint64_t frameId, VkResult status) {
        CapturedBitstream cap;
        cap.frameId = frameId;
        cap.isIdr = false;
        cap.pictureType = 0;
        cap.status = status;
        PushCapturedBitstream(std::move(cap));
    }

    VkResult CreateFrameInfoBuffersQueue(uint32_t) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    bool GetAvailablePoolNode(
        VkSharedBaseObj<VkVideoEncodeFrameInfo>&) override {
        return false;
    }
    VkResult InitEncoderCodec(VkSharedBaseObj<EncoderConfig>&) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkResult EncodeFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>&) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkResult CodecHandleRateControlCmd(
        VkSharedBaseObj<VkVideoEncodeFrameInfo>&) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkResult InitRateControl(VkCommandBuffer, uint32_t) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkResult ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>&,
                        uint32_t, uint32_t) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
};

}  // namespace

VkResult VkEncPushCapture(VulkanVideoEncoderExt* encoder,
                          uint64_t frameId,
                          VkResult status)
{
    if (encoder == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    if (impl->m_nullBackend == nullptr) {
        // Injection stands in for a real session's completion path; it
        // never runs beside one.
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    VkSharedBaseObj<VkVideoEncoder> source;
    {
        std::lock_guard<std::mutex> lock(impl->m_pendingMutex);
        if (!impl->m_encoder) {
            VkSharedBaseObj<VkVideoEncoder> created(
                new VkEncNullBackendCaptureSource());
            // The production wiring, verbatim (InitializeExt): the push
            // below then raises the real completion edge, which routes
            // the record through DrainCapturesLocked under the real lock
            // order.
            created->SetOnBitstreamCaptured(
                [impl](uint64_t id) { impl->OnBitstreamCaptured(id); });
            impl->m_encoder = created;
        }
        source = impl->m_encoder;
    }
    // Pushed OUTSIDE m_pendingMutex, like every production push: the
    // completion edge takes m_callbackMutex then m_pendingMutex. The cast
    // is sound because only this function installs an encoder on a
    // null-backend session, and only null-backend sessions get here.
    static_cast<VkEncNullBackendCaptureSource*>(source.get())
        ->PushCaptureRecord(frameId, status);
    return VK_SUCCESS;
}

VkResult VkEncApplyAndGetSessionConstQp(VulkanVideoEncoderExt* encoder,
                                        int32_t* pQpIntra,
                                        int32_t* pQpInterP,
                                        int32_t* pQpInterB)
{
    if ((encoder == nullptr) || (pQpIntra == nullptr) ||
        (pQpInterP == nullptr) || (pQpInterB == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    if (impl->m_nullBackend == nullptr) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    VkSharedBaseObj<VkVideoEncoder> source;
    {
        std::lock_guard<std::mutex> lock(impl->m_pendingMutex);
        source = impl->m_encoder;
    }
    if (!source) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    return source->ApplyAndGetConstQpForTest(pQpIntra, pQpInterP, pQpInterB);
}

VkResult VkEncApplyAndGetRateControl(VulkanVideoEncoderExt* encoder,
                                     VkEncRateControlObservation* pOut)
{
    if ((encoder == nullptr) || (pOut == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    if (impl->m_nullBackend == nullptr) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    VkSharedBaseObj<VkVideoEncoder> source;
    {
        std::lock_guard<std::mutex> lock(impl->m_pendingMutex);
        source = impl->m_encoder;
    }
    if (!source) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    VkVideoEncoder::RateControlObservation observed{};
    const VkResult result = source->ApplyAndGetRateControlForTest(&observed);
    if (result != VK_SUCCESS) {
        return result;
    }
    *pOut = {};
    pOut->layerAverageBitrate       = observed.layerAverageBitrate;
    pOut->layerMaxBitrate           = observed.layerMaxBitrate;
    pOut->layerFrameRateNumerator   = observed.layerFrameRateNumerator;
    pOut->layerFrameRateDenominator = observed.layerFrameRateDenominator;
    pOut->constQpIntra              = observed.constQpIntra;
    pOut->constQpInterP             = observed.constQpInterP;
    pOut->constQpInterB             = observed.constQpInterB;
    pOut->configMinQp               = observed.configMinQp;
    pOut->configMaxQp               = observed.configMaxQp;
    pOut->configMinQpSet            = observed.configMinQpSet;
    pOut->configMaxQpSet            = observed.configMaxQpSet;
    pOut->resolvedUseMinQp          = observed.resolvedUseMinQp;
    pOut->resolvedUseMaxQp          = observed.resolvedUseMaxQp;
    pOut->resolvedMinQpI            = observed.resolvedMinQpI;
    pOut->resolvedMaxQpI            = observed.resolvedMaxQpI;
    pOut->codecRefreshCount         = observed.codecRefreshCount;
    return VK_SUCCESS;
}

VkResult VkEncGetRecordedConfig(VulkanVideoEncoderExt* encoder,
                                VkVideoEncoderConfig* pOut)
{
    if ((encoder == nullptr) || (pOut == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    if (impl->m_nullBackend == nullptr) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    *pOut = impl->m_initConfig;
    return VK_SUCCESS;
}

VkResult VkEncSeedRecordedConfig(VulkanVideoEncoderExt* encoder,
                                 const VkVideoEncoderConfig* pConfig)
{
    if ((encoder == nullptr) || (pConfig == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    if (impl->m_nullBackend == nullptr) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    impl->m_initConfig = *pConfig;
    impl->m_initConfig.pNext = nullptr;
    return VK_SUCCESS;
}

VkResult VkEncSetDeviceQpWindow(VulkanVideoEncoderExt* encoder,
                                int32_t minQp, int32_t maxQp)
{
    if (encoder == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VulkanVideoEncoderExtImpl* impl =
        static_cast<VulkanVideoEncoderExtImpl*>(encoder);
    if (impl->m_nullBackend == nullptr) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    VkSharedBaseObj<VkVideoEncoder> source;
    {
        std::lock_guard<std::mutex> lock(impl->m_pendingMutex);
        source = impl->m_encoder;
    }
    if (!source) {
        return VK_ERROR_NOT_PERMITTED_KHR;
    }
    source->SetDeviceQpWindowForTest(minQp, maxQp);
    return VK_SUCCESS;
}

//=============================================================================
// Capability enumeration before InitializeExt()
//
// These free functions query the driver for a codec's encode capabilities
// WITHOUT creating a VkDevice or an encode session. They reuse the library's
// existing VkVideoCoreProfile + VulkanVideoCapabilities::GetVideoEncodeCap-
// abilities<>() machinery (the same code EncoderConfig*::InitDeviceCapabilities
// runs at session build time) and the VulkanDeviceContext instance/physical-
// device bring-up (which already tracks imported-handle ownership on this
// path via m_importedInstanceHandle, so caller handles are never destroyed).
//=============================================================================

namespace {

// (codec, profile, bit depth) -> probe parameters. The probe is
// per-VkVideoProfileInfoKHR, so each advertised (profile-idc, bit-depth)
// combination must be queried literally. VK_VIDEO_ENCODER_PROFILE_DEFAULT
// probes the representative profile per codec (H.264 High, H.265 Main, AV1
// Main -- all 8-bit); a named profile probes exactly itself.
//
// THE BIT DEPTH IS PART OF THE KEY AND IS NOT DERIVED FROM THE PROFILE
// NUMBER. For H.264 and H.265 the number does decide the depth -- Baseline,
// Main and High are 8-bit, Main 10 is the 10-bit one -- so those arms accept
// exactly the depth their profile carries and refuse every other, which is
// the answer they already gave. AV1 IS NOT LIKE THAT: seq_profile 0 (Main)
// carries 8 OR 10 bits at 4:2:0 (AV1 A.2), one profile at two depths. A key
// that was only a profile number could not name the 10-bit half at all, so it
// was the one combination this probe could not put to a driver -- while a
// 10-bit AV1 session is built and encoded on hardware today.
//
// H.264 High 10 (profile_idc 110) is deliberately NOT a row here. It has
// never been probed on any device in this tree, and a row is an
// advertisement: adding one would claim a capability with no evidence behind
// it. The same reasoning keeps H.265 Main 10 at 10 bits only, although the
// standard admits 8-bit input under it (H.265 A.3.3) -- that pairing has
// never been probed either. Widening either is a row with a visible diff,
// which is the point of putting the depth in the key.
struct ProbeProfile {
    uint32_t                          profileIdc;
    VkVideoComponentBitDepthFlagBitsKHR lumaBits;
    VkVideoComponentBitDepthFlagBitsKHR chromaBits;
    VkVideoChromaSubsamplingFlagBitsKHR chromaSubsampling;
};

// Returns false when `profile` is not a value this library probes for `codec`
// (the caller maps that onto VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR).
// Profile numbers are the codec standard's own, so they repeat across codecs:
// 1 is H.265 Main and is not an H.264 profile_idc at all. The codec arm is
// what disambiguates, and a number that belongs to another codec falls through
// to the refusal rather than answering with that codec's capabilities.
// The Vulkan component-bit-depth flag for |bitDepth|, or false for a depth
// this probe cannot spell. NOT a default: silently probing 8 bits for a
// caller that asked about 12 would answer a question nobody put.
static bool MapProbeBitDepth(uint32_t bitDepth,
                             VkVideoComponentBitDepthFlagBitsKHR& out)
{
    switch (bitDepth) {
        case 8:
            out = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;  return true;
        case 10:
            out = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR; return true;
        default:
            return false;
    }
}

static bool MapProbeProfile(VkVideoCodecOperationFlagBitsKHR codec,
                            uint32_t profile,
                            uint32_t bitDepth,
                            ProbeProfile& out)
{
    VkVideoComponentBitDepthFlagBitsKHR depthFlag =
        VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    if (!MapProbeBitDepth(bitDepth, depthFlag)) {
        return false;
    }
    out.lumaBits   = depthFlag;
    out.chromaBits = depthFlag;
    // THE PROBE ENVELOPE, and it is narrower than the taxonomy it serves.
    //
    // Every profile below is 4:2:0, so every capability query THIS PROBE
    // issues asks the device about a 4:2:0 profile.
    //
    // THAT IS THE RIGHT ENVELOPE FOR WHAT THE PROBE STILL ANSWERS -- the
    // capability SCALARS: coded extent, bitrate ceiling, DPB slots, rate
    // control modes, quality levels, std syntax flags. It was the wrong one
    // for a format list, and a format list is no longer built from it:
    // VkEncEnumerateInputFormats resolves each candidate live, at the profile
    // that candidate's own binding derives, so a 4:4:4 input asks the device
    // about a 4:4:4 profile whichever entry point put the question.
    //
    // Widening the envelope is still not a matter of changing the value below.
    // A 4:4:4 encode profile IS a different profile -- H.264 High 4:4:4
    // Predictive, H.265 Range Extensions -- so it needs a row of its own here
    // to be probed at all, and what a driver then answers is a device fact to
    // be measured rather than assumed. The field exists so that widening is a
    // row in this table with a visible diff, rather than a literal buried in
    // the call below.
    out.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    const bool eightBit = (bitDepth == 8);
    const bool tenBit   = (bitDepth == 10);
    switch ((uint32_t)codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            // Depth is gated per profile, not once for the codec: the
            // admissible depth is a property of the profile, not of H.264.
            // Baseline, Main and High admit 8-bit input only (H.264 A.2);
            // High 10 admits 10-bit. Gating the codec as a whole would
            // refuse a pairing before its profile is read.
            switch (profile) {
                case VK_VIDEO_ENCODER_PROFILE_DEFAULT:
                case VK_VIDEO_ENCODER_PROFILE_H264_HIGH:
                    if (!eightBit) {
                        return false;
                    }
                    out.profileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;     return true;
                case VK_VIDEO_ENCODER_PROFILE_H264_BASELINE:
                    if (!eightBit) {
                        return false;
                    }
                    out.profileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE; return true;
                case VK_VIDEO_ENCODER_PROFILE_H264_MAIN:
                    if (!eightBit) {
                        return false;
                    }
                    out.profileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;     return true;
                case VK_VIDEO_ENCODER_PROFILE_H264_HIGH_10:
                    if (!tenBit) {
                        return false;
                    }
                    out.profileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH_10;  return true;
                default:
                    return false;
            }
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            switch (profile) {
                case VK_VIDEO_ENCODER_PROFILE_DEFAULT:
                case VK_VIDEO_ENCODER_PROFILE_H265_MAIN:
                    // Main is 8-bit 4:2:0 (H.265 A.3.2).
                    if (!eightBit) {
                        return false;
                    }
                    out.profileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;     return true;
                case VK_VIDEO_ENCODER_PROFILE_H265_MAIN10:
                    // 10 bits only, which is NARROWER than H.265 A.3.3
                    // allows. See the note on the struct above for why the
                    // 8-bit Main 10 pairing is not a row.
                    if (!tenBit) {
                        return false;
                    }
                    out.profileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;  return true;
                default:
                    return false;
            }
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            switch (profile) {
                // seq_profile 0 IS Main and IS DEFAULT: one case, not two.
                // BOTH DEPTHS, because Main is one profile at two of them and
                // the depth is a separate term of this key. This is the row
                // the table could not previously express.
                case VK_VIDEO_ENCODER_PROFILE_AV1_MAIN:
                    if (!eightBit && !tenBit) {
                        return false;
                    }
                    out.profileIdc = STD_VIDEO_AV1_PROFILE_MAIN;          return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

static bool DeviceHasExtension(const VulkanDeviceContext& ctx,
                               const char* extName)
{
    // Read the context's populated list rather than re-asking the driver.
    //
    // This used to enumerate device extensions from scratch on every call --
    // two vkEnumerateDeviceExtensionProperties round-trips each, three calls
    // per capability probe, so six for a list the context already holds. That
    // was a workaround, not a preference: PopulateDeviceExtensions() lives
    // inside InitPhysicalDevice's selection block, and the caller-handle
    // capability path could never reach that block (it passed
    // requestQueueTypes = 0), so m_deviceExtensions was empty and a lookup
    // would have answered false for everything.
    //
    // Both bring-up paths now populate it -- AdoptPhysicalDevice() for the
    // adopted probe, InitPhysicalDevice()'s selection for the ephemeral one
    // -- so the list is authoritative and the re-query is dead weight.
    return ctx.FindDeviceExtension(extName) != nullptr;
}

// Query caps for (`codec`, `profile`, `bitDepth`) against the (already
// instance+physical-device initialized) device context. No VkDevice /
// session is created.
static VkResult QueryEncoderCapsInternal(const VulkanDeviceContext& ctx,
                                         VkVideoCodecOperationFlagBitsKHR codec,
                                         uint32_t profile,
                                         uint32_t bitDepth,
                                         VkEncProfileCapabilitySnapshot* outSnapshot)
{
    if (outSnapshot == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkVideoEncoderCapabilities* const outCaps = &outSnapshot->caps;
    switch ((uint32_t)codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            break;
        default:
            return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    ProbeProfile pp;
    if (!MapProbeProfile(codec, profile, bitDepth, pp)) {
        // Valid codec, but (`profile`, `bitDepth`) is not a pairing this
        // library probes for it -- either the profile belongs to another
        // codec, or the depth is not one this profile is probed at.
        return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
    }

    VkVideoCoreProfile coreProfile(codec,
                                   pp.chromaSubsampling,
                                   pp.lumaBits, pp.chromaBits,
                                   pp.profileIdc);

    VkVideoCapabilitiesKHR                       videoCaps{};
    VkVideoEncodeCapabilitiesKHR                 encodeCaps{};
    VkVideoEncodeQuantizationMapCapabilitiesKHR  qpMapCaps{};
    VkVideoEncodeIntraRefreshCapabilitiesKHR     intraRefreshCaps{};

    VkResult result = VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    if ((outCaps->sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_CAPABILITIES) ||
        (outCaps->pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *outSnapshot = {};
    outCaps->codec = codec;

    // Per-codec caps query (reuses the library's chaining template).
    switch ((uint32_t)codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR: {
            VkVideoEncodeH264CapabilitiesKHR              h264Caps{};
            VkVideoEncodeH264QuantizationMapCapabilitiesKHR h264QpMap{};
            result = VulkanVideoCapabilities::GetVideoEncodeCapabilities<
                        VkVideoEncodeH264CapabilitiesKHR,
                        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR,
                        VkVideoEncodeH264QuantizationMapCapabilitiesKHR,
                        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUANTIZATION_MAP_CAPABILITIES_KHR>(
                            &ctx, coreProfile, videoCaps, encodeCaps, h264Caps,
                            qpMapCaps, h264QpMap, intraRefreshCaps);
            if (result == VK_SUCCESS) {
                outCaps->maxLevelIdc = (uint32_t)h264Caps.maxLevelIdc;
                outSnapshot->stdFlags[outSnapshot->stdFlagCount++] =
                    h264Caps.stdSyntaxFlags;
            }
            break;
        }
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR: {
            VkVideoEncodeH265CapabilitiesKHR              h265Caps{};
            VkVideoEncodeH265QuantizationMapCapabilitiesKHR h265QpMap{};
            result = VulkanVideoCapabilities::GetVideoEncodeCapabilities<
                        VkVideoEncodeH265CapabilitiesKHR,
                        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR,
                        VkVideoEncodeH265QuantizationMapCapabilitiesKHR,
                        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUANTIZATION_MAP_CAPABILITIES_KHR>(
                            &ctx, coreProfile, videoCaps, encodeCaps, h265Caps,
                            qpMapCaps, h265QpMap, intraRefreshCaps);
            if (result == VK_SUCCESS) {
                outCaps->maxLevelIdc = (uint32_t)h265Caps.maxLevelIdc;
                outSnapshot->stdFlags[outSnapshot->stdFlagCount++] =
                    h265Caps.stdSyntaxFlags;
            }
            break;
        }
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR: {
            VkVideoEncodeAV1CapabilitiesKHR              av1Caps{};
            VkVideoEncodeAV1QuantizationMapCapabilitiesKHR av1QpMap{};
            result = VulkanVideoCapabilities::GetVideoEncodeCapabilities<
                        VkVideoEncodeAV1CapabilitiesKHR,
                        VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR,
                        VkVideoEncodeAV1QuantizationMapCapabilitiesKHR,
                        VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_QUANTIZATION_MAP_CAPABILITIES_KHR>(
                            &ctx, coreProfile, videoCaps, encodeCaps, av1Caps,
                            qpMapCaps, av1QpMap, intraRefreshCaps);
            if (result == VK_SUCCESS) {
                outCaps->maxLevelIdc = (uint32_t)av1Caps.maxLevel;
                outSnapshot->stdFlags[outSnapshot->stdFlagCount++] =
                    av1Caps.stdSyntaxFlags;
            }
            break;
        }
        default:
            return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    if (result != VK_SUCCESS) {
        // Driver does not support encode for this codec on this device. Leave
        // *outCaps zeroed (codec field already set) and propagate the result;
        // callers (e.g. the Chromium enumerator) treat this as "no profile".
        return result;
    }

    // --- Map the common (codec-agnostic) capability fields. ---
    outCaps->minLevelIdc               = 0; // no min-level cap in Vulkan today
    outCaps->maxDpbSlots               = videoCaps.maxDpbSlots;
    outCaps->maxActiveReferencePictures= videoCaps.maxActiveReferencePictures;
    outCaps->maxQualityLevels          = encodeCaps.maxQualityLevels;
    outCaps->maxBitrate                = encodeCaps.maxBitrate;
    outCaps->pictureAccessGranularityWidth  = videoCaps.pictureAccessGranularity.width;
    outCaps->pictureAccessGranularityHeight = videoCaps.pictureAccessGranularity.height;
    outCaps->supportedRateControlModes = encodeCaps.rateControlModes;
    outCaps->flags                     = encodeCaps.flags;
    outCaps->minCodedExtent            = videoCaps.minCodedExtent;
    outCaps->maxCodedExtent            = videoCaps.maxCodedExtent;

    // Optional-feature availability: extension presence + non-empty caps.
    outCaps->supportsMaintenance1 =
        DeviceHasExtension(ctx, VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME);
    outCaps->supportsQuantizationMap =
        DeviceHasExtension(ctx, VK_KHR_VIDEO_ENCODE_QUANTIZATION_MAP_EXTENSION_NAME);
    outCaps->supportsIntraRefresh =
        DeviceHasExtension(ctx, VK_KHR_VIDEO_ENCODE_INTRA_REFRESH_EXTENSION_NAME) &&
        (intraRefreshCaps.intraRefreshModes != 0);
    // Resize-without-IDR is not advertised by a dedicated Vulkan cap today; the
    // safe/default answer is false (resize forces an IDR unless a driver
    // later exposes an explicit capability we can map here).
    outCaps->supportsResizeWithoutIdr = false;

    // NO INPUT-FORMAT LIST IS BUILT HERE. Building one from a device format
    // query issued at this probe's own 4:2:0 envelope is the right
    // envelope for the scalars above and the wrong one for a format list. A
    // caller asking which formats it may feed the encoder is asking about the
    // profile ITS OWN input derives, and no fixed envelope answers that for
    // every input. VkEncEnumerateInputFormats now resolves each candidate live,
    // through the same function the point query answers from.
    return VK_SUCCESS;
}

// Common device-extension requests for a capability-only bring-up. We request
// the video-queue + encode-queue extensions (required to load the physical-
// device video PFNs) and the optional feature extensions so DeviceHasExtension
// reflects true device support.
static void AddCapsProbeExtensions(VulkanDeviceContext& ctx)
{
    static const char* const requiredDeviceExtension[] = {
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        nullptr
    };
    static const char* const optionalDeviceExtension[] = {
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUANTIZATION_MAP_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_INTRA_REFRESH_EXTENSION_NAME,
        nullptr
    };
    ctx.AddReqDeviceExtensions(requiredDeviceExtension);
    ctx.AddOptDeviceExtensions(optionalDeviceExtension);
}

} // anonymous namespace

//=============================================================================
// VulkanVideoEncoderContext -- the encoder context (context design 3.1, 3.2)
//
// Phases 1+2 of the library's Vulkan bring-up held once, so the capability
// path stops paying a loader load, an instance creation and a teardown per
// query (D1). Phases 3+4 -- the VkDevice and its queues -- stay with the
// encode session; a context never creates one.
//=============================================================================

namespace {

// Snapshot table geometry.
//
// Profile numbers are the codec standard's own: they are not contiguous, they
// do not start at the same place per codec, and they repeat across codecs. So
// the profile axis is a SLOT INDEX into a per-codec probe list rather than the
// number itself, and the list below is the single place that says which pairs
// the snapshot covers.
enum {
    VK_ENC_CTX_CODEC_H264  = 0,
    VK_ENC_CTX_CODEC_H265  = 1,
    VK_ENC_CTX_CODEC_AV1   = 2,
    VK_ENC_CTX_CODEC_COUNT = 3,
};

// One row of the snapshot: the profile number AND the bit depth it is probed
// at, because the probe is per-VkVideoProfileInfoKHR and the depth is part of
// that structure. Two rows may carry the SAME profile number at different
// depths -- see the AV1 list -- which is exactly what a profile number alone
// could not express.
struct VkEncCtxProbeEntry {
    uint32_t profile;
    uint32_t bitDepth;
};

const VkEncCtxProbeEntry kVkEncCtxProfilesH264[] = {
    { VK_VIDEO_ENCODER_PROFILE_DEFAULT,       8 },
    { VK_VIDEO_ENCODER_PROFILE_H264_BASELINE, 8 },
    { VK_VIDEO_ENCODER_PROFILE_H264_MAIN,     8 },
    { VK_VIDEO_ENCODER_PROFILE_H264_HIGH,     8 },
    { VK_VIDEO_ENCODER_PROFILE_H264_HIGH_10, 10 },
};
const VkEncCtxProbeEntry kVkEncCtxProfilesH265[] = {
    { VK_VIDEO_ENCODER_PROFILE_DEFAULT,      8 },
    { VK_VIDEO_ENCODER_PROFILE_H265_MAIN,    8 },
    { VK_VIDEO_ENCODER_PROFILE_H265_MAIN10, 10 },
};
// AV1 seq_profile 0 IS Main and IS DEFAULT, so the codec has one PROFILE
// NUMBER and not two: a second row holding a DIFFERENT number would report
// the same profile under two names. It has TWO ROWS all the same, because
// Main carries 8 or 10 bits (AV1 A.2) and the row key is (profile, depth).
// Those are two different VkVideoProfileInfoKHR values and a driver answers
// them separately, so one row could only ever describe half the profile.
//
// THE 8-BIT ROW IS FIRST, AND THAT ORDER IS LOAD-BEARING. The public lookup
// below resolves a profile number to the FIRST row carrying it, so every
// published answer for AV1 Main is the 8-bit one, byte for byte what it was.
//
// THE 10-BIT ROW IS PROBED AND STORED BUT NOT PUBLISHED, deliberately and not
// by oversight. There is no public key for it: every capability entry point
// names a profile by the codec standard number alone, and AV1 has one number
// for both depths. Publishing it needs either a depth argument on those entry
// points -- a public-header change this library does not make on its own -- or
// a rule for folding two rows into one answer. Folding is not free: the
// scalars (coded extent, bitrate ceiling, rate-control modes, quality levels)
// may differ between the depths, and an advertisement assembled from the wider
// of two rows would outrun what a session at the other depth accepts. Neither
// is decided here. What IS decided here is that the question now reaches the
// driver, so whichever route is chosen has an answer to publish.
const VkEncCtxProbeEntry kVkEncCtxProfilesAV1[] = {
    { VK_VIDEO_ENCODER_PROFILE_AV1_MAIN,  8 },
    { VK_VIDEO_ENCODER_PROFILE_AV1_MAIN, 10 },
};

// The widest per-codec list. Sized from the lists so a list that grows past
// the array fails the build here instead of truncating the snapshot.
enum { VK_ENC_CTX_PROFILE_SLOTS = 5 };
static_assert(sizeof(kVkEncCtxProfilesH264) / sizeof(VkEncCtxProbeEntry) <=
                      VK_ENC_CTX_PROFILE_SLOTS &&
                  sizeof(kVkEncCtxProfilesH265) / sizeof(VkEncCtxProbeEntry) <=
                      VK_ENC_CTX_PROFILE_SLOTS &&
                  sizeof(kVkEncCtxProfilesAV1) / sizeof(VkEncCtxProbeEntry) <=
                      VK_ENC_CTX_PROFILE_SLOTS,
              "a per-codec probe list outgrew the snapshot's profile axis");

// The probe list for |codec|, or nullptr with a zero count for a codec the
// snapshot does not cover.
const VkEncCtxProbeEntry* VkEncCtxProfileList(
    VkVideoCodecOperationFlagBitsKHR codec, uint32_t& outCount)
{
    switch ((uint32_t)codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            outCount = sizeof(kVkEncCtxProfilesH264) / sizeof(VkEncCtxProbeEntry);
            return kVkEncCtxProfilesH264;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            outCount = sizeof(kVkEncCtxProfilesH265) / sizeof(VkEncCtxProbeEntry);
            return kVkEncCtxProfilesH265;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            outCount = sizeof(kVkEncCtxProfilesAV1) / sizeof(VkEncCtxProbeEntry);
            return kVkEncCtxProfilesAV1;
        default:
            outCount = 0;
            return nullptr;
    }
}

// The probe row at |slot| for |codec|. False when the codec has no such slot.
bool VkEncCtxProbeAtSlot(VkVideoCodecOperationFlagBitsKHR codec,
                         uint32_t slot, VkEncCtxProbeEntry& outEntry)
{
    uint32_t count = 0;
    const VkEncCtxProbeEntry* list = VkEncCtxProfileList(codec, count);
    if ((list == nullptr) || (slot >= count)) {
        return false;
    }
    outEntry = list[slot];
    return true;
}

// Slot holding |profile| for |codec|, or -1 when this library does not probe
// that pair. A number belonging to a different codec lands here, which is what
// keeps a cross-codec request from reading another codec's row.
//
// FIRST MATCH ON THE PROFILE NUMBER, and the depth is not part of the lookup
// because it is not part of the public key -- an entry point names a profile
// by the standard number alone. Where a codec has two rows under one number
// (AV1 Main, at 8 and 10 bits) this therefore resolves to the first, which the
// list orders as the 8-bit one so that every published answer is unchanged.
int32_t VkEncCtxProfileSlotIndex(VkVideoCodecOperationFlagBitsKHR codec,
                                 uint32_t profile)
{
    uint32_t count = 0;
    const VkEncCtxProbeEntry* list = VkEncCtxProfileList(codec, count);
    for (uint32_t slot = 0; slot < count; slot++) {
        if (list[slot].profile == profile) {
            return (int32_t)slot;
        }
    }
    return -1;
}

VkVideoCodecOperationFlagBitsKHR VkEncCtxCodecAtIndex(uint32_t index)
{
    switch (index) {
        case VK_ENC_CTX_CODEC_H264:
            return VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
        case VK_ENC_CTX_CODEC_H265:
            return VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
        case VK_ENC_CTX_CODEC_AV1:
            return VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
        default:
            return VK_VIDEO_CODEC_OPERATION_NONE_KHR;
    }
}

int32_t VkEncCtxCodecIndex(VkVideoCodecOperationFlagBitsKHR codec)
{
    switch ((uint32_t)codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            return VK_ENC_CTX_CODEC_H264;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            return VK_ENC_CTX_CODEC_H265;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            return VK_ENC_CTX_CODEC_AV1;
        default:
            return -1;
    }
}

bool VkEncCtxUuidIsZero(const uint8_t* uuid)
{
    for (uint32_t i = 0; i < VK_UUID_SIZE; i++) {
        if (uuid[i] != 0) {
            return false;
        }
    }
    return true;
}

// The 64-bit format features a DRM modifier must carry for an image with
// |usage| to be creatable on it. Returns false -- refusing to answer -- for a
// usage bit this mapping does not cover, because a filter that silently drops
// a term it did not understand is worse than one that says it cannot answer.
bool VkEncCtxUsageToFormatFeatures(VkImageUsageFlags usage,
                                   VkFormatFeatureFlags2& outFeatures)
{
    struct UsageFeature {
        VkImageUsageFlags     usageBit;
        VkFormatFeatureFlags2 featureBit;
    };
    static const UsageFeature kMap[] = {
        { VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
          VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT_BIT_KHR },
        { VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
          VK_FORMAT_FEATURE_2_VIDEO_ENCODE_DPB_BIT_KHR },
        { VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT },
        { VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT },
        { VK_IMAGE_USAGE_SAMPLED_BIT,
          VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT },
        { VK_IMAGE_USAGE_STORAGE_BIT,
          VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT },
        { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
          VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT },
    };

    VkFormatFeatureFlags2 features  = 0;
    VkImageUsageFlags     unmapped  = usage;
    for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
        if ((usage & kMap[i].usageBit) != 0) {
            features |= kMap[i].featureBit;
            unmapped &= ~kMap[i].usageBit;
        }
    }
    if (unmapped != 0) {
        VkEncErr() << "[EncoderContext] no format-feature mapping for image "
                   << "usage bits 0x" << std::hex << unmapped << std::dec
                   << "; refusing to filter modifiers on a term this build "
                   << "does not understand" << std::endl;
        return false;
    }
    outFeatures = features;
    return true;
}

} // anonymous namespace

//=============================================================================
// CAPABILITY-PROBE KEY OBSERVATION SEAM (internal; device-free).
//
// What the probe CAN ask a driver is a build-time fact of the two tables
// above, and it is invisible from the public surface on the only host where
// it matters: a machine with no encode-capable device enumerates no device,
// so every capability entry point answers "not present" whatever the tables
// say. These three read the tables directly, so the shape is regression-tested
// on a GPU-less runner and a row that disappears fails a test rather than an
// advertisement.
//
// What they CANNOT say is whether a driver answers yes to any of it. That is a
// device fact and is measured on hardware or not at all.
//=============================================================================

// Is (codec, profile, bitDepth) a combination this library probes?
bool VkEncProbeNamesProfileBitDepth(VkVideoCodecOperationFlagBitsKHR codec,
                                    uint32_t profile,
                                    uint32_t bitDepth)
{
    ProbeProfile pp;
    return MapProbeProfile(codec, profile, bitDepth, pp);
}

// How many probe rows the context snapshot carries for |codec|.
uint32_t VkEncProbeSnapshotRowCount(VkVideoCodecOperationFlagBitsKHR codec)
{
    uint32_t count = 0;
    (void)VkEncCtxProfileList(codec, count);
    return count;
}

// The (profile, bit depth) of snapshot row |slot| for |codec|.
bool VkEncProbeSnapshotRowAt(VkVideoCodecOperationFlagBitsKHR codec,
                             uint32_t slot,
                             uint32_t* outProfile,
                             uint32_t* outBitDepth)
{
    VkEncCtxProbeEntry entry = {};
    if (!VkEncCtxProbeAtSlot(codec, slot, entry)) {
        return false;
    }
    if (outProfile != nullptr) {
        *outProfile = entry.profile;
    }
    if (outBitDepth != nullptr) {
        *outBitDepth = entry.bitDepth;
    }
    return true;
}

class VulkanVideoEncoderContext : public VkVideoRefCountBase {
public:
    // One enumerated physical device and everything the context knows about
    // it. Written ONLY by Build(), which runs inside Create() under the
    // construction lock; const for the object's whole visible life.
    struct DeviceEntry {
        VkPhysicalDevice             physDevice = VK_NULL_HANDLE;
        VkVideoEncoderDeviceIdentity identity   = {};

        // Union of videoCodecOperations over the queue families that carry
        // VK_QUEUE_VIDEO_ENCODE_BIT_KHR. This is the exact predicate
        // InitPhysicalDevice() applies when it picks a "first capable"
        // device for a codec, kept here so the ephemeral capability entry
        // points can apply the same one without a second bring-up.
        VkVideoCodecOperationFlagsKHR encodeCodecOps = 0;

        bool hasRequiredVideoExtensions    = false;
        bool hasDrmFormatModifierExtension = false;
        // 64-bit format features, needed to answer a modifier query about
        // VIDEO_ENCODE_INPUT at all.
        bool hasFormatFeatureFlags2        = false;

        // probed[c][p] is false when no driver query was issued for that
        // pair, in which case capsResult[c][p] says why and caps[c][p] must
        // not be handed out.
        bool                       probed[VK_ENC_CTX_CODEC_COUNT]
                                         [VK_ENC_CTX_PROFILE_SLOTS];
        VkResult                       capsResult[VK_ENC_CTX_CODEC_COUNT]
                                                 [VK_ENC_CTX_PROFILE_SLOTS];
        VkEncProfileCapabilitySnapshot snapshot[VK_ENC_CTX_CODEC_COUNT]
                                               [VK_ENC_CTX_PROFILE_SLOTS];
    };

    static VkResult Create(const VkVideoEncoderContextCreateInfo& createInfo,
                           VkSharedBaseObj<VulkanVideoEncoderContext>& outContext);

    uint32_t GetPhysicalDeviceCount() const {
        return (uint32_t)m_devices.size();
    }
    const DeviceEntry* GetDeviceEntry(uint32_t index) const {
        return (index < m_devices.size()) ? &m_devices[index] : nullptr;
    }
    const VulkanDeviceContext& GetDeviceContext() const { return *m_devCtx; }
    VkVideoEncoderContextMode GetMode() const { return m_mode; }

    virtual ~VulkanVideoEncoderContext() {
        // Nothing Vulkan is destroyed here, in either mode.
        //
        // ADOPT (rule 3): the instance is flagged imported inside
        // VulkanDeviceContext, so its destructor leaves it alone, and the
        // physical device was never ours to destroy. Rule 1: the loader
        // handle was retained in Build() before anything could fail, so the
        // destructor below does not unload it either.
        //
        // OWN (rule 2): an OWN-mode context never reaches this destructor at
        // all -- the process-wide floor reference holds its refcount above
        // zero for the process lifetime, precisely so a later create cannot
        // re-issue vkCreateInstance after a sandbox has locked down.
    }

private:
    VulkanVideoEncoderContext() = default;
    VkResult Build(const VkVideoEncoderContextCreateInfo& createInfo);

    VkVideoEncoderContextMode            m_mode =
        VK_VIDEO_ENCODER_CONTEXT_MODE_OWN;
    std::unique_ptr<VulkanDeviceContext> m_devCtx;
    std::vector<DeviceEntry>             m_devices;
    // This context's silence request, held for its whole life. Released by
    // the destructor, which is what makes silence end with the last owner
    // rather than with whoever assigned the flag most recently.
    VkEncoderStdioSilenceScope           m_stdioSilence;
};

namespace {

struct VkEncOwnContextEntry {
    uint8_t                                    gpuUUID[VK_UUID_SIZE];
    VkSharedBaseObj<VulkanVideoEncoderContext> context;
};

// The floor reference (design 3.1 rule 2), deliberately leaked.
//
// Destroying this vector at static-destruction time would release the last
// reference to every OWN-mode context and run vkDestroyInstance on the way
// out of main -- and, far worse, would let a create that happens afterwards
// stand up a second instance. Never destroyed means never re-created. The
// pointer is a function-local static, so there is also no static-init order
// to get wrong.
std::vector<VkEncOwnContextEntry>& VkEncOwnContextRegistry()
{
    static std::vector<VkEncOwnContextEntry>* const registry =
        new std::vector<VkEncOwnContextEntry>();
    return *registry;
}

// The construction lock. Guards the floor registry AND the snapshot build, so
// two sequences racing to create the same OWN context get one instance and
// one snapshot rather than two.
std::mutex& VkEncContextConstructionMutex()
{
    static std::mutex* const constructionMutex = new std::mutex();
    return *constructionMutex;
}

} // anonymous namespace

VkResult VulkanVideoEncoderContext::Build(
    const VkVideoEncoderContextCreateInfo& createInfo)
{
    m_mode = createInfo.mode;
    m_devCtx.reset(new VulkanDeviceContext());

    // Rule 1, and set BEFORE anything can fail: even a context whose bring-up
    // fails must leave the loader mapped, because the failure path still ran
    // dlopen and an embedder may already be bound to the same object.
    m_devCtx->RetainLoaderHandle();

    AddCapsProbeExtensions(*m_devCtx);

    const VkInstance adoptInstance =
        (m_mode == VK_VIDEO_ENCODER_CONTEXT_MODE_ADOPT)
            ? createInfo.adoptInstance : VK_NULL_HANDLE;

    // Phase 1. With a non-null instance this imports rather than creates:
    // VulkanDeviceContext records m_importedInstanceHandle and its destructor
    // will not destroy it.
    VkResult result = m_devCtx->InitVulkanDevice("VulkanVideoEncoderContext",
                                                 adoptInstance,
                                                 /*verbose*/ false);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Phase 2.
    std::vector<VkPhysicalDevice> candidates;
    if (m_mode == VK_VIDEO_ENCODER_CONTEXT_MODE_ADOPT) {
        // ADOPT never re-selects (D8): the caller already chose the device,
        // and a context only issues physical-device-level queries, so there
        // is no queue family to pick and no VkDevice to create.
        candidates.push_back(createInfo.adoptPhysicalDevice);
    } else {
        result = vk::enumerate(m_devCtx.get(), m_devCtx->getInstance(),
                               candidates);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    const bool pinToUuid = (m_mode == VK_VIDEO_ENCODER_CONTEXT_MODE_OWN) &&
                           !VkEncCtxUuidIsZero(createInfo.gpuUUID);

    for (size_t candidate = 0; candidate < candidates.size(); candidate++) {
        VkPhysicalDevice physDevice = candidates[candidate];
        if (physDevice == VK_NULL_HANDLE) {
            continue;
        }

        VkPhysicalDeviceVulkan11Properties vulkan11Props = {};
        vulkan11Props.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
        VkPhysicalDeviceProperties2 props2 = {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &vulkan11Props;
        m_devCtx->GetPhysicalDeviceProperties2(physDevice, &props2);

        if (pinToUuid && (memcmp(vulkan11Props.deviceUUID, createInfo.gpuUUID,
                                 VK_UUID_SIZE) != 0)) {
            continue;
        }

        // Populates the device-extension list this device's probes read
        // through DeviceHasExtension(). PopulateDeviceExtensions() RESIZES
        // that list, so it is per candidate rather than accumulated -- unlike
        // m_reqDeviceExtensions, which HasAllDeviceExtensions() appends to and
        // which this loop deliberately never touches.
        result = m_devCtx->AdoptPhysicalDevice(physDevice);
        if (result != VK_SUCCESS) {
            if (m_mode == VK_VIDEO_ENCODER_CONTEXT_MODE_ADOPT) {
                return result;
            }
            continue;
        }

        DeviceEntry entry;
        memcpy(entry.identity.deviceUUID, vulkan11Props.deviceUUID,
               VK_UUID_SIZE);
        memcpy(entry.identity.driverUUID, vulkan11Props.driverUUID,
               VK_UUID_SIZE);
        memcpy(entry.identity.deviceName, props2.properties.deviceName,
               sizeof(entry.identity.deviceName));
        entry.identity.deviceName[sizeof(entry.identity.deviceName) - 1] = 0;
        entry.identity.vendorID = props2.properties.vendorID;
        entry.identity.deviceID = props2.properties.deviceID;
        entry.physDevice = physDevice;

        entry.hasRequiredVideoExtensions =
            (m_devCtx->FindDeviceExtension(
                 VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) != nullptr) &&
            (m_devCtx->FindDeviceExtension(
                 VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME) != nullptr);
        entry.hasDrmFormatModifierExtension =
            (m_devCtx->FindDeviceExtension(
                 VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) != nullptr);
        entry.hasFormatFeatureFlags2 =
            (props2.properties.apiVersion >= VK_API_VERSION_1_3) ||
            (m_devCtx->FindDeviceExtension(
                 VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) != nullptr);

        // Which codecs the device's encode-capable queue families carry.
        std::vector<VkQueueFamilyProperties2> queues;
        std::vector<VkQueueFamilyVideoPropertiesKHR> videoQueues;
        std::vector<VkQueueFamilyQueryResultStatusPropertiesKHR> queryStatus;
        vk::get(m_devCtx.get(), physDevice, queues, videoQueues, queryStatus);
        // The three arrays are filled in parallel, one entry per family;
        // indexing one against another's length would be an out-of-bounds
        // read past a check that looked like it covered it.
        if (videoQueues.size() == queues.size()) {
            for (size_t family = 0; family < queues.size(); family++) {
                if ((queues[family].queueFamilyProperties.queueFlags &
                     VK_QUEUE_VIDEO_ENCODE_BIT_KHR) != 0) {
                    entry.encodeCodecOps |=
                        videoQueues[family].videoCodecOperations;
                }
            }
        }

        // The capability snapshot itself -- the whole reason the context
        // exists. Every (codec, profile) pair the library can probe is
        // resolved here, once, so a later query is a table read.
        for (uint32_t c = 0; c < VK_ENC_CTX_CODEC_COUNT; c++) {
            const VkVideoCodecOperationFlagBitsKHR codec =
                VkEncCtxCodecAtIndex(c);
            for (uint32_t p = 0; p < VK_ENC_CTX_PROFILE_SLOTS; p++) {
                entry.snapshot[c][p]   = {};
                entry.probed[c][p]     = false;
                entry.capsResult[c][p] = VK_ERROR_EXTENSION_NOT_PRESENT;

                // A slot this codec does not have. The array is rectangular
                // and the lists are not, so the surplus rows exist and must
                // say why they hold nothing.
                VkEncCtxProbeEntry slotProbe = {};
                if (!VkEncCtxProbeAtSlot(codec, p, slotProbe)) {
                    entry.capsResult[c][p] =
                        VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
                    continue;
                }

                // vkGetPhysicalDeviceVideoCapabilitiesKHR is an entry point
                // of VK_KHR_video_queue; asking a device that does not
                // advertise it is invalid usage, not a cheap "no". ADOPT is
                // exempt because the caller named the device explicitly and
                // the pre-context entry point probed it unconditionally
                // -- changing that would be a behaviour change, not a fix.
                if (!entry.hasRequiredVideoExtensions &&
                    (m_mode != VK_VIDEO_ENCODER_CONTEXT_MODE_ADOPT)) {
                    continue;
                }

                VkEncProfileCapabilitySnapshot probeSnapshot = {};
                const VkResult probeResult = QueryEncoderCapsInternal(
                    *m_devCtx, codec, slotProbe.profile, slotProbe.bitDepth,
                    &probeSnapshot);
                entry.capsResult[c][p] = probeResult;
                // QueryEncoderCapsInternal leaves *out untouched when it
                // rejects the (codec, profile) pair before probing; only a
                // pair it actually took to the driver has an out-struct
                // worth handing back.
                if ((probeResult != VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR) &&
                    (probeResult != VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR)) {
                    entry.probed[c][p]   = true;
                    entry.snapshot[c][p] = probeSnapshot;
                } else if (probeSnapshot.caps.codec == codec) {
                    // The driver, not the pair check, returned that code:
                    // QueryEncoderCapsInternal had already stamped the codec
                    // into the out-struct, so there IS a result to hand back.
                    entry.probed[c][p]   = true;
                    entry.snapshot[c][p] = probeSnapshot;
                }
            }
        }

        m_devices.push_back(entry);
    }

    if (m_devices.empty()) {
        // Same code the pre-context ephemeral path returned when no candidate
        // matched (VulkanDeviceContext::InitPhysicalDevice).
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return VK_SUCCESS;
}

VkResult VulkanVideoEncoderContext::Create(
    const VkVideoEncoderContextCreateInfo& createInfo,
    VkSharedBaseObj<VulkanVideoEncoderContext>& outContext)
{
    std::lock_guard<std::mutex> lock(VkEncContextConstructionMutex());

    // Latched before any output can be produced, including the device-
    // selection chatter inside VulkanDeviceContext. Only VK_TRUE acts: see
    // the header for why VK_FALSE must not un-silence.
    if (createInfo.silenceStdio == VK_TRUE) {
        SetVkEncoderStdioSilenced(true);
    }

    if (createInfo.mode == VK_VIDEO_ENCODER_CONTEXT_MODE_OWN) {
        std::vector<VkEncOwnContextEntry>& registry = VkEncOwnContextRegistry();
        for (size_t i = 0; i < registry.size(); i++) {
            if (memcmp(registry[i].gpuUUID, createInfo.gpuUUID,
                       VK_UUID_SIZE) == 0) {
                // Rule 2: hand back the existing context rather than issuing
                // a second vkCreateInstance.
                outContext = registry[i].context;
                return VK_SUCCESS;
            }
        }
    }

    VkSharedBaseObj<VulkanVideoEncoderContext> context(
        new VulkanVideoEncoderContext());
    // Move the request into the object: from here the context owns it, and it
    // ends when the context does rather than when this function returns. A
    // failed Build below leaves the token on the local, which releases it.
    context->m_stdioSilence = std::move(contextSilence);
    const VkResult result = context->Build(createInfo);
    if (result != VK_SUCCESS) {
        return result;
    }

    if (createInfo.mode == VK_VIDEO_ENCODER_CONTEXT_MODE_OWN) {
        VkEncOwnContextEntry entry;
        memcpy(entry.gpuUUID, createInfo.gpuUUID, VK_UUID_SIZE);
        entry.context = context;
        // The floor reference. Never removed.
        VkEncOwnContextRegistry().push_back(entry);
    }

    outContext = context;
    return VK_SUCCESS;
}

VK_VIDEO_ENCODER_EXPORT
VkResult CreateVulkanVideoEncoderContext(
    const VkVideoEncoderContextCreateInfo*      pCreateInfo,
    VkSharedBaseObj<VulkanVideoEncoderContext>& outContext)
{
    outContext = nullptr;
    if (pCreateInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // Structure-type gate, and the chain rule with it: this struct defines no
    // extension structs, so it refuses ANY chain rather than walking past
    // something it does not understand.
    if ((pCreateInfo->sType !=
             VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONTEXT_CREATE_INFO) ||
        (pCreateInfo->pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    switch (pCreateInfo->mode) {
        case VK_VIDEO_ENCODER_CONTEXT_MODE_OWN:
            if ((pCreateInfo->adoptInstance != VK_NULL_HANDLE) ||
                (pCreateInfo->adoptPhysicalDevice != VK_NULL_HANDLE)) {
                // Handles supplied to a mode that will not borrow them: the
                // caller asked for one thing and meant another, and silently
                // creating a second instance beside handles it already had is
                // the expensive half of that mistake.
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            break;
        case VK_VIDEO_ENCODER_CONTEXT_MODE_ADOPT:
            // Both required together; either alone is an error.
            if ((pCreateInfo->adoptInstance == VK_NULL_HANDLE) ||
                (pCreateInfo->adoptPhysicalDevice == VK_NULL_HANDLE)) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            if (!VkEncCtxUuidIsZero(pCreateInfo->gpuUUID)) {
                // gpuUUID is an OWN-mode selector. In ADOPT the device is
                // already chosen, so a non-zero one is a contradiction, and
                // honouring one of the two would be a guess.
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            break;
        default:
            return VK_ERROR_INITIALIZATION_FAILED;
    }

    return VulkanVideoEncoderContext::Create(*pCreateInfo, outContext);
}

// Defined HERE, below VulkanVideoEncoderContext, and not next to
// CreateVulkanVideoEncoderExt: it dereferences the context, which is an
// incomplete type at that point in this file.
VK_VIDEO_ENCODER_EXPORT
VkResult CreateVulkanVideoEncoderExtOnContext(
    const VkSharedBaseObj<VulkanVideoEncoderContext>& context,
    uint32_t                                          deviceIndex,
    VkSharedBaseObj<VulkanVideoEncoderExt>&           vulkanVideoEncoder)
{
    // |vulkanVideoEncoder| is deliberately NOT cleared here. Clearing it would
    // make a FAILING create destroy whatever session the caller already held in
    // that variable, and it would differ from CreateVulkanVideoEncoderExt,
    // which leaves its out-param untouched on failure. Assigned once, on
    // success, at the bottom.
    if (!context) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Resolve both handles now, while the type is complete, and hand them to
    // the session as plain values -- the session cannot call these accessors
    // from where it needs them.
    const VulkanVideoEncoderContext::DeviceEntry* entry =
        context->GetDeviceEntry(deviceIndex);
    if (entry == nullptr) {
        VkEncErr() << "[EncoderExt] deviceIndex " << deviceIndex
                   << " is out of range for this context ("
                   << context->GetPhysicalDeviceCount() << " device(s))"
                   << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkInstance instance = context->GetDeviceContext().getInstance();
    if ((instance == VK_NULL_HANDLE) || (entry->physDevice == VK_NULL_HANDLE)) {
        // A built context always has both. Checked anyway because everything
        // downstream treats them as valid without re-testing, and a null
        // instance reaching InitVulkanDevice reads as "create your own".
        VkEncErr() << "[EncoderExt] context holds no usable instance or "
                      "physical device at index " << deviceIndex << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VulkanVideoEncoderExtImpl* impl = new VulkanVideoEncoderExtImpl();
    VkSharedBaseObj<VulkanVideoEncoderExt> obj(impl);
    if (!obj) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Bind before publishing, so the session is never observable without its
    // context reference.
    impl->SetContext(context, instance, entry->physDevice);

    vulkanVideoEncoder = obj;
    return VK_SUCCESS;
}

VK_VIDEO_ENCODER_EXPORT
uint32_t VkEncGetPhysicalDeviceCount(VulkanVideoEncoderContext* ctx)
{
    return (ctx != nullptr) ? ctx->GetPhysicalDeviceCount() : 0u;
}

VK_VIDEO_ENCODER_EXPORT
VkResult VkEncGetPhysicalDeviceIdentity(VulkanVideoEncoderContext*    ctx,
                                        uint32_t                      deviceIndex,
                                        VkVideoEncoderDeviceIdentity* pOut)
{
    // Struct gate first, and before the context is even dereferenced: an
    // unstamped or chained out-struct is version skew.
    if ((pOut == nullptr) ||
        (pOut->sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_DEVICE_IDENTITY) ||
        (pOut->pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (ctx == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VulkanVideoEncoderContext::DeviceEntry* entry =
        ctx->GetDeviceEntry(deviceIndex);
    if (entry == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pOut = entry->identity;
    return VK_SUCCESS;
}

VK_VIDEO_ENCODER_EXPORT
VkResult VkEncGetEncodeCapabilities(VulkanVideoEncoderContext*       ctx,
                                    uint32_t                         deviceIndex,
                                    VkVideoCodecOperationFlagBitsKHR codec,
                                    uint32_t                         profile,
                                    VkVideoEncoderCapabilities*      pOut)
{
    // Same gate, same order, same reason as the free-function enumerators.
    if ((pOut == nullptr) ||
        (pOut->sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_CAPABILITIES) ||
        (pOut->pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (ctx == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VulkanVideoEncoderContext::DeviceEntry* entry =
        ctx->GetDeviceEntry(deviceIndex);
    if (entry == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const int32_t codecIndex = VkEncCtxCodecIndex(codec);
    if (codecIndex < 0) {
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }
    // Profile numbers are the standard's own, so the row is found by lookup
    // and not by using the number as an index. A number this library does not
    // probe for this codec -- including one that names a profile of a
    // DIFFERENT codec -- has no row, and says so.
    const int32_t profileSlot = VkEncCtxProfileSlotIndex(codec, profile);
    if (profileSlot < 0) {
        return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
    }
    if (!entry->probed[codecIndex][profileSlot]) {
        // No driver query was issued for this pair, so there is no out-struct
        // to hand back. Leave the caller's alone -- exactly what the
        // pre-context entry point did when it rejected a pair before probing.
        return entry->capsResult[codecIndex][profileSlot];
    }
    *pOut = entry->snapshot[codecIndex][profileSlot].caps;
    return entry->capsResult[codecIndex][profileSlot];
}

// The one place the snapshot row for a (codec, profile) pair is resolved.
// Returns nullptr and leaves |outResult| holding the code the caller must
// propagate when there is no row to read.
static const VkEncProfileCapabilitySnapshot* VkEncCtxSnapshotRow(
    VulkanVideoEncoderContext*       ctx,
    uint32_t                         deviceIndex,
    VkVideoCodecOperationFlagBitsKHR codec,
    uint32_t                         profile,
    VkResult&                        outResult)
{
    outResult = VK_ERROR_INITIALIZATION_FAILED;
    if (ctx == nullptr) {
        return nullptr;
    }
    const VulkanVideoEncoderContext::DeviceEntry* entry =
        ctx->GetDeviceEntry(deviceIndex);
    if (entry == nullptr) {
        return nullptr;
    }
    const int32_t codecIndex = VkEncCtxCodecIndex(codec);
    if (codecIndex < 0) {
        outResult = VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
        return nullptr;
    }
    const int32_t profileSlot = VkEncCtxProfileSlotIndex(codec, profile);
    if (profileSlot < 0) {
        outResult = VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
        return nullptr;
    }
    outResult = entry->capsResult[codecIndex][profileSlot];
    if (!entry->probed[codecIndex][profileSlot]) {
        return nullptr;
    }
    return &entry->snapshot[codecIndex][profileSlot];
}

// The two-call write shared by every list this context answers: copy at most
// |capacity| entries, report how many were written, and say VK_INCOMPLETE
// when the caller's buffer could not hold the answer.
//
// A null |pArray| is the counting call and is not an error: *pCount receives
// the full number and nothing is written.
template <typename T>
static VkResult VkEncCtxWriteList(const T* entries, uint32_t entryCount,
                                  uint32_t* pCount, T* pArray)
{
    if (pArray == nullptr) {
        *pCount = entryCount;
        return VK_SUCCESS;
    }
    const uint32_t capacity = *pCount;
    const uint32_t written  = (capacity < entryCount) ? capacity : entryCount;
    for (uint32_t i = 0; i < written; i++) {
        pArray[i] = entries[i];
    }
    *pCount = written;
    return (written < entryCount) ? VK_INCOMPLETE : VK_SUCCESS;
}

// The device's encode-source format list for |coreProfile| on |physDevice|.
//
// vkGetPhysicalDeviceVideoFormatPropertiesKHR is reached DIRECTLY rather than
// through VulkanVideoCapabilities::GetVideoFormats, and that is not a
// shortcut. GetVideoFormats reads the device context's CURRENT physical
// device; a context holds one entry per device and this query names WHICH, so
// routing through the context's current handle would answer about whichever
// device happened to be adopted last during construction. Re-adopting to fix
// that would mutate the very state the capability snapshot was built against,
// from a query documented as not moving anything.
static VkResult VkEncQueryDeviceEncodeSrcFormats(
    const VulkanDeviceContext& devCtx,
    VkPhysicalDevice           physDevice,
    const VkVideoCoreProfile&  coreProfile,
    VkFormat*                  outFormats,
    uint32_t&                  ioCount)
{
    const VkVideoProfileListInfoKHR profileList = {
        VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR, nullptr, 1,
        coreProfile.GetProfile() };
    const VkPhysicalDeviceVideoFormatInfoKHR formatInfo = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
        const_cast<VkVideoProfileListInfoKHR*>(&profileList),
        VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR };

    const uint32_t capacity = ioCount;
    ioCount = 0;

    uint32_t count = 0;
    VkResult result = devCtx.GetPhysicalDeviceVideoFormatPropertiesKHR(
        physDevice, &formatInfo, &count, nullptr);
    if (result != VK_SUCCESS) {
        // A profile this device has no encode support for answers here, with
        // the driver's own reason. That is the answer, not an error to hide.
        return result;
    }
    if (count == 0) {
        return VK_SUCCESS;
    }
    if (count > capacity) {
        count = capacity;
    }
    VkVideoFormatPropertiesKHR props[VK_ENC_MAX_DEVICE_INPUT_FORMATS] = {};
    for (uint32_t i = 0; i < count; i++) {
        props[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    }
    result = devCtx.GetPhysicalDeviceVideoFormatPropertiesKHR(
        physDevice, &formatInfo, &count, props);
    // VK_INCOMPLETE means the clamp above dropped entries, which is a short
    // buffer and not a failed query: what was written is still true.
    if ((result != VK_SUCCESS) && (result != VK_INCOMPLETE)) {
        return result;
    }
    for (uint32_t i = 0; i < count; i++) {
        outFormats[i] = props[i].format;
    }
    ioCount = count;
    return VK_SUCCESS;
}

// WHAT THIS DEVICE WILL TAKE, ASKED ONCE AND ANSWERED FOR THREE SURFACES.
//
// The enumerator, the point query and InitializeExt all have to give the same
// answer to "will this device encode this input for this profile", and until
// this function existed only the first two shared one. InitializeExt asked
// nothing: it built the config, created the session, and let the DRIVER refuse
// at vkGetPhysicalDeviceVideoCapabilitiesKHR -- past the point where a caller
// could still choose differently, with a message naming neither the format nor
// its subsampling. The advertised set was therefore narrower than the accepted
// set, and a caller had to consult two surfaces to learn what it could hand in.
//
// EXTRACTED RATHER THAN RESTATED, for the reason the resolver below states
// about its own halves: a second statement of this rule is what produced two
// surfaces that disagreed for identical arguments in the first place.
//
// THE VERDICT IS AN ENUM AND NOT A VkResult, and that is the whole point of
// the extraction. Every one of these is VK_ERROR_FORMAT_NOT_SUPPORTED to the
// caller of a query -- which is the right answer for a query, since the
// question was "yes or no". At an INITIALISATION boundary the same yes-or-no
// is a refusal a caller has to act on, and "no" without WHICH of these is what
// the driver already said.
static VkEncDeviceFormatVerdict VkEncResolveDeviceEncodeFormat(
    const VulkanDeviceContext&       devCtx,
    VkPhysicalDevice                 physDevice,
    VkVideoCodecOperationFlagBitsKHR codec,
    uint32_t                         codecProfile,
    uint32_t                         chromaSubsampling,
    uint32_t                         bitDepth,
    VkFormat                         inputFormat,
    bool                             viaFilter,
    VkFormat&                        outEncodeFormat)
{
    // Written only on ACCEPTED, and never partially, for the same reason the
    // resolver states about its own out-parameter: a half-answer a caller
    // cannot tell from a whole one.
    outEncodeFormat = VK_FORMAT_UNDEFINED;

    const VkVideoComponentBitDepthFlagBitsKHR depthFlag =
        GetComponentBitDepthFlagBits(bitDepth);
    if (depthFlag == VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR) {
        return VK_ENC_DEVICE_FORMAT_DEPTH_NOT_ENCODABLE;
    }

    // A LIVE QUERY, AT THE INPUT'S OWN GEOMETRY. Not the context's capability
    // snapshot: that probes every profile at 4:2:0 (MapProbeProfile), so
    // reading it here would answer "no" for every 4:4:4 input on every device.
    // The profile asked about is the one the binder ACTUALLY derived or bound,
    // and the subsampling and depth are the input's own.
    VkVideoCoreProfile coreProfile(
        codec, (VkVideoChromaSubsamplingFlagBitsKHR)chromaSubsampling,
        depthFlag, depthFlag, codecProfile);

    VkFormat deviceFormats[VK_ENC_MAX_DEVICE_INPUT_FORMATS] = {};
    uint32_t deviceFormatCount = VK_ENC_MAX_DEVICE_INPUT_FORMATS;
    if (VkEncQueryDeviceEncodeSrcFormats(devCtx, physDevice, coreProfile,
                                         deviceFormats,
                                         deviceFormatCount) != VK_SUCCESS) {
        return VK_ENC_DEVICE_FORMAT_PROFILE_ABSENT;
    }
    if (deviceFormatCount == 0) {
        // The query succeeded and named nothing, which is the same fact as a
        // failed query and is worth reporting as the same reason: this device
        // has no encode source for that profile.
        return VK_ENC_DEVICE_FORMAT_PROFILE_ABSENT;
    }

    // WHAT THE ENCODER IS HANDED is what the device has to accept, and for a
    // converted input that is the conversion's OUTPUT and not the caller's
    // format. Asking the device about the caller's format would refuse every
    // RGBA and packed-Y'CbCr input on a device that encodes them perfectly
    // well through the filter.
    const VkFormat encodeFormat =
        viaFilter ? VkEncConversionTargetFormat(inputFormat, deviceFormats,
                                                deviceFormatCount)
                  : inputFormat;
    if (encodeFormat == VK_FORMAT_UNDEFINED) {
        return VK_ENC_DEVICE_FORMAT_NO_CONVERSION_TARGET;
    }
    if (!VkEncFormatListContains(deviceFormats, deviceFormatCount,
                                 encodeFormat)) {
        return VK_ENC_DEVICE_FORMAT_NOT_AN_ENCODE_SOURCE;
    }

    outEncodeFormat = encodeFormat;
    return VK_ENC_DEVICE_FORMAT_ACCEPTED;
}

// THE ONE ANSWER BOTH THE POINT QUERY AND THE ENUMERATOR GIVE.
//
// Extracted rather than restated. Two statements of this rule is exactly what
// made an advertised encodeFormat and a queried one disagree for identical
// arguments on the same device in the same process: the enumerator read a
// capability snapshot probed at a fixed 4:2:0 envelope, the point query bound
// the caller's own configuration and asked the device at the profile that
// binding derived. Only one of those describes the session a caller would get.
//
// Writes |outProps| only on VK_SUCCESS, and never partially: a caller reading
// the struct after a refusal would be reading a half-answer it cannot tell from
// a whole one.
//
// Defined ABOVE the enumerator on purpose -- the enumerator is now one of its
// two callers.
static VkResult VkEncResolveInputFormatSupport(
    VulkanVideoEncoderContext*                    ctx,
    const VulkanVideoEncoderContext::DeviceEntry* entry,
    VkVideoCodecOperationFlagBitsKHR              codec,
    uint32_t                                      profile,
    VkFormat                                      format,
    VkVideoEncoderColorModel                      colorModel,
    VkVideoEncoderInputFormatProperties*          outProps)
{
    // ---- The library half, answered BY the binder rather than beside it. ----
    //
    // Running VkEncBuildAndProbeConfig is what makes this query and
    // InitializeExt one answer instead of two. Every rule that decides
    // acceptance -- the input taxonomy, the colour-model declaration, and the
    // profile's own bit-depth and chroma-subsampling limits -- is applied
    // there, once. A reimplementation here would be a second statement of the
    // same rules, and what that eventually produces is a query promising what
    // init refuses.
    VkVideoEncoderConfig probeConfig = {};
    probeConfig.sType           = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
    probeConfig.codec           = codec;
    probeConfig.profile         = profile;
    probeConfig.inputFormat     = format;
    probeConfig.inputColorModel = colorModel;
    // Geometry the binder needs to produce a config at all, and that no part
    // of this answer depends on: a format's routing and its encode profile are
    // both independent of the frame size. It is stated here rather than taken
    // from the caller for exactly that reason -- an extent parameter on this
    // entry point would be a knob with no effect on what it returns.
    probeConfig.encodeWidth     = 1920;
    probeConfig.encodeHeight    = 1080;
    probeConfig.inputWidth      = 1920;
    probeConfig.inputHeight     = 1080;
    probeConfig.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    probeConfig.averageBitrate  = 4000000;
    probeConfig.frameRateNum    = 30;
    probeConfig.frameRateDen    = 1;
    probeConfig.gopLength       = 30;
    // No file is opened for a query: the binder opens one only when an
    // outputPath is set AND file output is wanted, and neither is, here.
    probeConfig.disableFileOutput = VK_TRUE;

    VkEncBoundConfigProbe probe = {};
    if (VkEncBuildAndProbeConfig(probeConfig, codec, &probe) != VK_SUCCESS) {
        // The binder refused: either the pair is not an input this library
        // routes, or the named profile cannot carry it, or it is a profile
        // number this library does not bind. All three are "you cannot feed me
        // this on this profile", which is the question that was asked.
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    // ---- The device half, and it is the SAME CALL InitializeExt makes. ----
    //
    // VkEncResolveDeviceEncodeFormat above holds it, so a query that says yes
    // and a session that refuses cannot be written without changing one
    // function. The profile handed to it is the one the binder ACTUALLY
    // derived, and the subsampling and depth are the ENCODE side's -- the
    // geometry the video profile is built from at session creation, which is
    // what makes this query predict that session rather than a different one.
    // Both are read back off the probe rather than assumed.
    //
    // THE REASON IS DISCARDED HERE, deliberately. A point query answers yes or
    // no; the reason is what an initialisation boundary owes its caller, and
    // that is where it is spent.
    const bool viaFilter = (probe.preprocessComputeFilter != 0);
    VkFormat encodeFormat = VK_FORMAT_UNDEFINED;
    if (VkEncResolveDeviceEncodeFormat(
            ctx->GetDeviceContext(), entry->physDevice, codec,
            probe.codecProfile, probe.encodeChromaSubsampling,
            probe.encodeBitDepthLuma,
            format, viaFilter, encodeFormat) !=
        VK_ENC_DEVICE_FORMAT_ACCEPTED) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    if (outProps != nullptr) {
        outProps->format       = format;
        outProps->encodeFormat = encodeFormat;
        outProps->optimality =
            viaFilter ? VK_VIDEO_ENCODER_INPUT_FORMAT_SUBOPTIMAL
                      : VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL;
    }
    return VK_SUCCESS;
}

// The enumerator's admission: one live resolve per candidate. |userData| is a
// VkEncLiveAdmitContext naming the device and the key being enumerated.
struct VkEncLiveAdmitContext {
    VulkanVideoEncoderContext*                    ctx;
    const VulkanVideoEncoderContext::DeviceEntry* entry;
    VkVideoCodecOperationFlagBitsKHR              codec;
    uint32_t                                      profile;
};

static bool VkEncAdmitByLiveResolve(
    void* userData, VkFormat candidate,
    VkVideoEncoderInputFormatProperties* outEntry)
{
    VkEncLiveAdmitContext* const live =
        static_cast<VkEncLiveAdmitContext*>(userData);
    return VkEncResolveInputFormatSupport(
               live->ctx, live->entry, live->codec, live->profile, candidate,
               VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, outEntry) ==
           VK_SUCCESS;
}

VK_VIDEO_ENCODER_EXPORT
VkResult VkEncEnumerateStdFlags(VulkanVideoEncoderContext*       ctx,
                                uint32_t                         deviceIndex,
                                VkVideoCodecOperationFlagBitsKHR codec,
                                uint32_t                         profile,
                                uint32_t*                        pCount,
                                VkVideoEncoderStdFlags*          pFlags)
{
    if (pCount == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult rowResult = VK_ERROR_INITIALIZATION_FAILED;
    const VkEncProfileCapabilitySnapshot* row =
        VkEncCtxSnapshotRow(ctx, deviceIndex, codec, profile, rowResult);
    if (row == nullptr) {
        // No row to read. The count is still answered, because a caller that
        // treats a refused profile as "advertise nothing" needs a zero rather
        // than a stale number.
        *pCount = 0;
        return rowResult;
    }
    const VkResult writeResult =
        VkEncCtxWriteList(row->stdFlags, row->stdFlagCount, pCount, pFlags);
    // A short buffer outranks the probe's own result: the caller must know it
    // did not receive everything before it acts on what it did receive.
    const VkResult result =
        (writeResult == VK_SUCCESS) ? rowResult : writeResult;
    if ((result != VK_SUCCESS) && (result != VK_INCOMPLETE)) {
        // An entry can be probed and still carry a refusal. A list read out of
        // one is not an answer, so it is reported as the count every other
        // error reports.
        *pCount = 0;
    }
    return result;
}

VK_VIDEO_ENCODER_EXPORT
VkResult VkEncEnumerateInputFormats(
    VulkanVideoEncoderContext*           ctx,
    uint32_t                             deviceIndex,
    VkVideoCodecOperationFlagBitsKHR     codec,
    uint32_t                             profile,
    uint32_t*                            pCount,
    VkVideoEncoderInputFormatProperties* pFormats)
{
    if (pCount == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // THE SNAPSHOT IS THE KEY GATE AND NOTHING ELSE. It is what returns
    // VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR for a profile number
    // this context does not carry, and what carries the probe's own capsResult
    // for a pair the driver refused. It is NOT where the list comes from: the
    // probe keys every profile at a fixed 4:2:0 envelope, and a list built from
    // that describes a session no caller asked for.
    VkResult rowResult = VK_ERROR_INITIALIZATION_FAILED;
    const VkEncProfileCapabilitySnapshot* row =
        VkEncCtxSnapshotRow(ctx, deviceIndex, codec, profile, rowResult);
    if (row == nullptr) {
        *pCount = 0;
        return rowResult;
    }
    const VulkanVideoEncoderContext::DeviceEntry* const entry =
        ctx->GetDeviceEntry(deviceIndex);
    if (entry == nullptr) {
        // Unreachable while the snapshot row exists -- the row was resolved
        // through the same entry -- and stated rather than assumed, because
        // what follows dereferences it.
        *pCount = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // BUILT LIVE, per candidate, through the SAME resolver the point query
    // answers from. The cost is one binder run and one device format query per
    // routable candidate, which is what the point query already pays per call;
    // what it buys is that the two surfaces cannot disagree, because there is
    // only one of them.
    VkEncLiveAdmitContext live = { ctx, entry, codec, profile };
    VkVideoEncoderInputFormatProperties entries[
        VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};

    // A REFUSED CANDIDATE IS THE ANSWER HERE, NOT AN ERROR. The binder explains
    // every refusal on the gated error stream, which is right for a caller that
    // declared one configuration and wrong for a sweep that deliberately offers
    // every routable format to a profile most of them cannot reach. An
    // enumeration that printed twenty refusals per successful call would be
    // unusable in the sandboxed process the latch exists for, and would say
    // nothing a caller could act on.
    //
    // Saved and restored rather than set: a caller that had already silenced
    // the streams stays silenced, and one that had not is unaffected the moment
    // this returns.
    // A scoped request rather than a read/save/restore. The restore could
    // write back a value another thread had changed while the enumeration
    // ran, un-silencing an owner that still needed silence. A token adds this
    // query's request and removes exactly that one.
    uint32_t entryCount = 0;
    {
        const VkEncoderStdioSilenceScope quietQuery(true);
        entryCount = VkEncAdvertiseInputFormats(
            &VkEncAdmitByLiveResolve, &live, entries,
            VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
    }

    const VkResult writeResult =
        VkEncCtxWriteList(entries, entryCount, pCount, pFormats);
    const VkResult result =
        (writeResult == VK_SUCCESS) ? rowResult : writeResult;
    if ((result != VK_SUCCESS) && (result != VK_INCOMPLETE)) {
        *pCount = 0;
    }
    return result;
}

VK_VIDEO_ENCODER_EXPORT
VkResult VkEncEnumerateDrmModifiers(VulkanVideoEncoderContext* ctx,
                                    uint32_t                   deviceIndex,
                                    VkFormat                   format,
                                    VkImageUsageFlags          usage,
                                    uint32_t*                  pCount,
                                    uint64_t*                  pModifiers)
{
    // A null pCount is the one failure with nowhere to put the count. Every
    // other return below leaves one behind.
    if (pCount == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (ctx == nullptr) {
        *pCount = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VulkanVideoEncoderContext::DeviceEntry* entry =
        ctx->GetDeviceEntry(deviceIndex);
    if (entry == nullptr) {
        *pCount = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkFormatFeatureFlags2 requiredFeatures = 0;
    if (!VkEncCtxUsageToFormatFeatures(usage, requiredFeatures)) {
        *pCount = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!entry->hasDrmFormatModifierExtension) {
        // No modifiers is a true answer on a device without the extension
        // (every Windows device, for one), not a failure.
        *pCount = 0;
        return VK_SUCCESS;
    }
    if (!entry->hasFormatFeatureFlags2) {
        // The 32-bit modifier list cannot express VIDEO_ENCODE_INPUT, the one
        // feature this entry point exists to filter on, so an answer built
        // from it would be wrong rather than partial.
        *pCount = 0;
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    const VulkanDeviceContext& devCtx = ctx->GetDeviceContext();

    VkDrmFormatModifierPropertiesList2EXT modifierList = {};
    modifierList.sType =
        VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT;
    VkFormatProperties2 formatProps = {};
    formatProps.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    formatProps.pNext = &modifierList;

    devCtx.GetPhysicalDeviceFormatProperties2(entry->physDevice, format,
                                              &formatProps);
    if (modifierList.drmFormatModifierCount == 0) {
        *pCount = 0;
        return VK_SUCCESS;
    }
    std::vector<VkDrmFormatModifierProperties2EXT> modifierProps(
        modifierList.drmFormatModifierCount);
    modifierList.pDrmFormatModifierProperties = modifierProps.data();
    devCtx.GetPhysicalDeviceFormatProperties2(entry->physDevice, format,
                                              &formatProps);
    if (modifierList.drmFormatModifierCount < modifierProps.size()) {
        modifierProps.resize(modifierList.drmFormatModifierCount);
    }

    const uint32_t capacity = (pModifiers != nullptr) ? *pCount : 0u;
    uint32_t matched = 0;
    uint32_t written = 0;
    for (size_t i = 0; i < modifierProps.size(); i++) {
        if ((modifierProps[i].drmFormatModifierTilingFeatures &
             requiredFeatures) != requiredFeatures) {
            continue;
        }
        matched++;
        if ((pModifiers != nullptr) && (written < capacity)) {
            pModifiers[written++] = modifierProps[i].drmFormatModifier;
        }
    }

    if (pModifiers == nullptr) {
        *pCount = matched;
        return VK_SUCCESS;
    }
    *pCount = written;
    return (written < matched) ? VK_INCOMPLETE : VK_SUCCESS;
}


VK_VIDEO_ENCODER_EXPORT
VkResult VkEncQueryInputFormatSupport(
    VulkanVideoEncoderContext*           ctx,
    uint32_t                             deviceIndex,
    VkVideoCodecOperationFlagBitsKHR     codec,
    uint32_t                             profile,
    VkFormat                             format,
    VkVideoEncoderColorModel             colorModel,
    VkVideoEncoderInputFormatProperties* pProperties)
{
    // Same gate, same order, same reason as the enumerators above -- except
    // that pProperties is OPTIONAL here, because a caller that wants only the
    // verdict should not have to supply somewhere to put an answer it will not
    // read.
    if (ctx == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VulkanVideoEncoderContext::DeviceEntry* entry =
        ctx->GetDeviceEntry(deviceIndex);
    if (entry == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (VkEncCtxCodecIndex(codec) < 0) {
        return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }

    // Everything below the argument gate is the shared resolver, which the
    // enumerator next door runs over every routable candidate. This entry point
    // is that resolver applied to one.
    return VkEncResolveInputFormatSupport(ctx, entry, codec, profile, format,
                                          colorModel, pProperties);
}

//=============================================================================
// The four pre-context capability entry points are thin wrappers over a
// context they build and throw away, so that a caller holding no Vulkan
// handles reaches the same code as one that supplies its own.
//
// They are the reason the context is on the executed path from day one: the
// Chromium enumerator calls the ephemeral per-profile variant once per
// candidate profile, and every one of those calls now goes through
// CreateVulkanVideoEncoderContext.
//=============================================================================

VK_VIDEO_ENCODER_EXPORT
VkResult EnumerateVulkanVideoEncoderProfileCapabilities(
    VkInstance                       instance,
    VkPhysicalDevice                 physicalDevice,
    VkVideoCodecOperationFlagBitsKHR codec,
    uint32_t                         profile,
    VkVideoEncoderCapabilities*      outCaps)
{
    // Struct gate first, before any Vulkan work: an unstamped or chained
    // outCaps is version skew, and must be refused before the caller's
    // handles are touched.
    if ((outCaps == nullptr) ||
        (outCaps->sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_CAPABILITIES) ||
        (outCaps->pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // An ADOPT-mode context over the caller's instance and physical
    // device. It creates nothing and destroys nothing of the caller's, and it
    // adopts rather than re-selects -- D8's repair, now living in the context
    // instead of beside it.
    VkVideoEncoderContextCreateInfo createInfo = {};
    createInfo.mode                = VK_VIDEO_ENCODER_CONTEXT_MODE_ADOPT;
    createInfo.adoptInstance       = instance;
    createInfo.adoptPhysicalDevice = physicalDevice;

    VkSharedBaseObj<VulkanVideoEncoderContext> context;
    const VkResult result = CreateVulkanVideoEncoderContext(&createInfo,
                                                            context);
    if (result != VK_SUCCESS) {
        return result;
    }
    // ADOPT enumerates exactly the one device it was handed.
    return VkEncGetEncodeCapabilities(context.get(), /*deviceIndex*/ 0,
                                      codec, profile, outCaps);
}

VK_VIDEO_ENCODER_EXPORT
VkResult EnumerateVulkanVideoEncoderProfileCapabilitiesEphemeral(
    int32_t                          deviceId,
    VkVideoCodecOperationFlagBitsKHR codec,
    uint32_t                         profile,
    VkVideoEncoderCapabilities*      outCaps)
{
    // Struct gate first -- same rule and same reason as the caller-handles
    // variant above, and it still runs before any context is built.
    if ((outCaps == nullptr) ||
        (outCaps->sType != VK_VIDEO_ENCODER_STRUCTURE_TYPE_CAPABILITIES) ||
        (outCaps->pNext != nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // "Ephemeral" now names the API contract, not the implementation: the
    // caller still supplies no handles and still gets none back. What is no
    // longer ephemeral is the VkInstance -- an OWN-mode context is built once
    // and floor-referenced, so the five calls the Chromium enumerator makes
    // per cache computation cost one loader load and one vkCreateInstance in
    // total instead of five of each. That is D1, and it is the whole reason
    // the context exists.
    VkVideoEncoderContextCreateInfo createInfo = {};
    createInfo.mode = VK_VIDEO_ENCODER_CONTEXT_MODE_OWN;
    // All-zero gpuUUID: enumerate everything, and select below.

    VkSharedBaseObj<VulkanVideoEncoderContext> context;
    const VkResult result = CreateVulkanVideoEncoderContext(&createInfo,
                                                            context);
    if (result != VK_SUCCESS) {
        return result;
    }

    // The selection predicate InitPhysicalDevice() applied when this entry
    // point drove it directly, term for term: the deviceID filter, the
    // required video extensions, and an encode-capable queue family carrying
    // the requested codec. It is reproduced here rather than pushed into the
    // context because it is THIS entry point's contract -- a context
    // enumerates, it does not choose.
    const uint32_t deviceCount = VkEncGetPhysicalDeviceCount(context.get());
    for (uint32_t index = 0; index < deviceCount; index++) {
        const VulkanVideoEncoderContext::DeviceEntry* entry =
            context->GetDeviceEntry(index);
        if ((deviceId != -1) &&
            (entry->identity.deviceID != (uint32_t)deviceId)) {
            continue;
        }
        if (!entry->hasRequiredVideoExtensions) {
            continue;
        }
        if ((entry->encodeCodecOps & codec) == 0) {
            continue;
        }
        return VkEncGetEncodeCapabilities(context.get(), index, codec, profile,
                                          outCaps);
    }

    // The code InitPhysicalDevice() returned when no candidate matched.
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
