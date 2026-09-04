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
 * Per-direction semaphore-override coverage for SubmitRegisteredFrame's
 * chained-descriptor walk.
 *
 * THE DEFECT THIS PINS. The walk resolves a frame's registered wait ids and
 * signal ids into two lists, then decides whether the submit gets those or the
 * caller's own raw VkSemaphore arrays. THE DECISION IS PER DIRECTION. Deciding
 * once for both together -- `if (!resolvedWait.empty() || !resolvedSignal.empty())`
 * -- and overwriting BOTH makes a chain naming only waits set
 * signalSemaphoreCount to zero and throw the caller's signal semaphores away.
 *
 * That is not a corner. It is the shape Chromium submits on every fenced
 * frame: media/gpu/vulkan/vulkan_video_encode_accelerator.cc chains a
 * VkVideoEncoderFrameFenceDescriptor carrying an acquire fence and nothing
 * else, and separately fills the submit info's pSignalSemaphores from its own
 * end_semaphores. Whoever waits on those end semaphores waits forever, which
 * presents as a hung tab rather than an error.
 *
 * WHY A NULL BACKEND. The rule is a decision about two pointers and two
 * counts, taken before any Vulkan object is touched, and the public surface
 * cannot see it: SubmitRegisteredFrame consumes the arrays and answers a
 * status. So this drives the REAL walk on a null-backend session
 * (vulkan_video_encoder_ext_internal.h) and reads back what the submit was
 * handed. No device, no queue, no encoder -- and therefore no hardware
 * dependence in either direction, which is what makes it deterministic.
 *
 * A registered semaphore is what makes a chain resolve to something, and the
 * public RegisterSemaphore needs a device to create and import one, so the
 * ids here come from VkEncInstallTestSemaphore -- sentinel handles that are
 * stored, compared and submitted, never dereferenced. Each is uninstalled
 * before the session drops, because Deinitialize would otherwise try to
 * destroy it through a dispatch table this session never populated.
 */

#include "vulkan_video_encoder_ext_internal.h"

// The public header reaches the Xlib platform headers, whose macros collide
// with ordinary identifiers. Scrub them before anything else sees them -- the
// same block, for the same reason, as the sibling library test TUs.
#undef Status
#undef None
#undef Bool
#undef Window

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;
const char* g_currentCase = "";

void Check(bool ok, const char* what, const std::string& detail)
{
    g_checks++;
    if (ok) {
        return;
    }
    g_failures++;
    std::printf("  FAIL [%s] %s : %s\n", g_currentCase, what, detail.c_str());
}

std::string U64(uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return buf;
}

void CheckEqU32(uint32_t got, uint32_t want, const char* what)
{
    Check(got == want, what, "got " + U64(got) + ", want " + U64(want));
}

// A NON-DISPATCHABLE HANDLE IS NOT A POINTER, and only looks like one on
// 64-bit. VK_DEFINE_NON_DISPATCHABLE_HANDLE resolves to a pointer type there
// and to uint64_t on a 32-bit build, so formatting one with %p compiles on the
// one ABI and fails on the other. Print the 64-bit value, which is what the
// handle is on both.
std::string Handle(uint64_t h)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)h);
    return buf;
}

void CheckEqSem(VkSemaphore got, VkSemaphore want, const char* what)
{
    Check(got == want, what,
          "got " + Handle((uint64_t)got) + ", want " + Handle((uint64_t)want));
}

void CheckEqU64(uint64_t got, uint64_t want, const char* what)
{
    Check(got == want, what, "got " + U64(got) + ", want " + U64(want));
}

// Sentinel VkSemaphore values. Never dereferenced: the submit terminates at
// the null backend, which records the handles and enqueues.
VkSemaphore Sentinel(uintptr_t v)
{
    return (VkSemaphore)v;
}

// One null-backend session with a VK_IMAGE registration, torn down in the
// order the internal header requires.
class Session {
public:
    bool Open()
    {
        if ((CreateVulkanVideoEncoderExt(m_encoder) != VK_SUCCESS) ||
            !m_encoder) {
            std::printf("  ERROR: CreateVulkanVideoEncoderExt failed\n");
            return false;
        }
        if (VkEncInstallNullBackend(m_encoder.get(), &m_backend) != VK_SUCCESS) {
            std::printf("  ERROR: VkEncInstallNullBackend failed\n");
            return false;
        }
        VkVideoEncoderExternalImageDescriptor desc = {};
        desc.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
        desc.handleType = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
        desc.format     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        desc.width      = 640;
        desc.height     = 360;
        desc.residency  = VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
        desc.existingImage = (VkImage)(uintptr_t)0xA110C8ED;
        if ((m_encoder->RegisterImageResource(desc, 0, &m_resource, nullptr) !=
             VK_VIDEO_ENCODER_STATUS_SUCCESS) ||
            (m_resource == VK_VIDEO_ENCODER_RESOURCE_NULL)) {
            std::printf("  ERROR: RegisterImageResource failed\n");
            return false;
        }
        return true;
    }

    ~Session()
    {
        for (size_t i = 0; i < m_testSemaphores.size(); i++) {
            VkEncUninstallTestSemaphore(m_encoder.get(), m_testSemaphores[i]);
        }
    }

    // A registered-semaphore id a FrameSyncDescriptor can name.
    VkVideoEncoderResource InstallSemaphore(VkSemaphore sentinel)
    {
        VkVideoEncoderResource id = VK_VIDEO_ENCODER_RESOURCE_NULL;
        if (VkEncInstallTestSemaphore(m_encoder.get(), sentinel, &id) != VK_SUCCESS) {
            std::printf("  ERROR: VkEncInstallTestSemaphore failed\n");
            return VK_VIDEO_ENCODER_RESOURCE_NULL;
        }
        m_testSemaphores.push_back(id);
        return id;
    }

    VkVideoEncoderFrameSubmitInfo SubmitInfoFor(uint64_t frameId) const
    {
        VkVideoEncoderFrameSubmitInfo info = {};
        info.sType      = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS;
        info.resource   = m_resource;
        info.frameId    = frameId;
        info.qpOverride = -1;
        return info;
    }

    VulkanVideoEncoderExt* Get() const { return m_encoder.get(); }

private:
    VkEncNullBackendState                  m_backend;
    VkSharedBaseObj<VulkanVideoEncoderExt> m_encoder;
    VkVideoEncoderResource m_resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    std::vector<VkVideoEncoderResource> m_testSemaphores;
};

// Submit |info| and hand back what the submit was actually given.
bool SubmitAndProbe(Session& s,
                    VkVideoEncoderFrameSubmitInfo& info,
                    VkEncSubmitSyncProbe* outProbe)
{
    const VkVideoEncoderStatusCode status =
        s.Get()->SubmitRegisteredFrame(info, nullptr);
    if (status != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
        Check(false, "SubmitRegisteredFrame",
              "status " + U64((uint64_t)status));
        return false;
    }
    if (VkEncProbeLastSubmitSync(s.Get(), outProbe) !=
        VK_VIDEO_ENCODER_STATUS_SUCCESS) {
        Check(false, "VkEncProbeLastSubmitSync", "call failed");
        return false;
    }
    Check(outProbe->recorded == VK_TRUE, "probe.recorded",
          "no submit reached the null backend");
    return outProbe->recorded == VK_TRUE;
}

// Caller-supplied raw arrays, shared by the cases below.
VkSemaphore g_callerWait[2]         = {Sentinel(0x1001), Sentinel(0x1002)};
uint64_t    g_callerWaitValues[2]   = {11, 12};
VkSemaphore g_callerSignal[2]       = {Sentinel(0x2001), Sentinel(0x2002)};
uint64_t    g_callerSignalValues[2] = {21, 22};

void FillCallerArrays(VkVideoEncoderFrameSubmitInfo& info)
{
    info.waitSemaphoreCount     = 2;
    info.pWaitSemaphores        = g_callerWait;
    info.pWaitSemaphoreValues   = g_callerWaitValues;
    info.signalSemaphoreCount   = 2;
    info.pSignalSemaphores      = g_callerSignal;
    info.pSignalSemaphoreValues = g_callerSignalValues;
}

void ExpectCallerWaitSurvived(const VkEncSubmitSyncProbe& p)
{
    CheckEqU32(p.waitCount, 2, "wait count is the caller's");
    CheckEqSem(p.waitSemaphores[0], g_callerWait[0], "wait[0] is the caller's");
    CheckEqSem(p.waitSemaphores[1], g_callerWait[1], "wait[1] is the caller's");
    CheckEqU64(p.waitValues[0], g_callerWaitValues[0], "waitValue[0]");
    CheckEqU64(p.waitValues[1], g_callerWaitValues[1], "waitValue[1]");
}

void ExpectCallerSignalSurvived(const VkEncSubmitSyncProbe& p)
{
    CheckEqU32(p.signalCount, 2, "signal count is the caller's");
    CheckEqSem(p.signalSemaphores[0], g_callerSignal[0],
               "signal[0] is the caller's");
    CheckEqSem(p.signalSemaphores[1], g_callerSignal[1],
               "signal[1] is the caller's");
    CheckEqU64(p.signalValues[0], g_callerSignalValues[0], "signalValue[0]");
    CheckEqU64(p.signalValues[1], g_callerSignalValues[1], "signalValue[1]");
}

//=============================================================================
// Case 1 -- THE REGRESSION. A chain that names only waits must not touch the
// signal direction. This is the shape Chromium submits on every fenced frame,
// and it is the case the combined test broke.
//=============================================================================
void CaseAcquireOnlyChainKeepsCallerSignals(Session& s)
{
    g_currentCase = "AcquireOnlyChainKeepsCallerSignals";
    const VkSemaphore kRegistered = Sentinel(0x3001);
    const VkVideoEncoderResource id = s.InstallSemaphore(kRegistered);
    if (id == VK_VIDEO_ENCODER_RESOURCE_NULL) {
        Check(false, "InstallSemaphore", "setup failed");
        return;
    }
    const uint64_t kWaitValue = 77;

    VkVideoEncoderFrameSyncDescriptor sync;
    sync.waitCount       = 1;
    sync.pWaitSemaphores = &id;
    sync.pWaitValues     = &kWaitValue;
    // signalCount stays 0: the chain says nothing about the signal direction.

    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(101);
    FillCallerArrays(info);
    info.pNext = &sync;

    VkEncSubmitSyncProbe p;
    if (!SubmitAndProbe(s, info, &p)) {
        return;
    }
    // The wait direction WAS named, so the registered id replaces the raw one.
    CheckEqU32(p.waitCount, 1, "wait count is the resolved one");
    CheckEqSem(p.waitSemaphores[0], kRegistered, "wait[0] is the resolved one");
    CheckEqU64(p.waitValues[0], kWaitValue, "waitValue[0] is the resolved one");
    // The signal direction was NOT named. The caller's list must survive
    // intact -- this is the assertion the defect failed.
    ExpectCallerSignalSurvived(p);
}

//=============================================================================
// Case 2 -- the mirror image. A chain naming only signals must not touch the
// wait direction.
//=============================================================================
void CaseSignalOnlyChainKeepsCallerWaits(Session& s)
{
    g_currentCase = "SignalOnlyChainKeepsCallerWaits";
    const VkSemaphore kRegistered = Sentinel(0x3002);
    const VkVideoEncoderResource id = s.InstallSemaphore(kRegistered);
    if (id == VK_VIDEO_ENCODER_RESOURCE_NULL) {
        Check(false, "InstallSemaphore", "setup failed");
        return;
    }
    const uint64_t kSignalValue = 88;

    VkVideoEncoderFrameSyncDescriptor sync;
    sync.signalCount       = 1;
    sync.pSignalSemaphores = &id;
    sync.pSignalValues     = &kSignalValue;

    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(102);
    FillCallerArrays(info);
    info.pNext = &sync;

    VkEncSubmitSyncProbe p;
    if (!SubmitAndProbe(s, info, &p)) {
        return;
    }
    CheckEqU32(p.signalCount, 1, "signal count is the resolved one");
    CheckEqSem(p.signalSemaphores[0], kRegistered,
               "signal[0] is the resolved one");
    CheckEqU64(p.signalValues[0], kSignalValue,
               "signalValue[0] is the resolved one");
    ExpectCallerWaitSurvived(p);
}

//=============================================================================
// Case 3 -- both directions named: both are replaced outright. The registered
// ids win; nothing is merged with the raw arrays.
//=============================================================================
void CaseBothSuppliedReplacesBothDirections(Session& s)
{
    g_currentCase = "BothSuppliedReplacesBothDirections";
    const VkSemaphore kRegWait   = Sentinel(0x3003);
    const VkSemaphore kRegSignal = Sentinel(0x3004);
    const VkVideoEncoderResource waitId   = s.InstallSemaphore(kRegWait);
    const VkVideoEncoderResource signalId = s.InstallSemaphore(kRegSignal);
    if ((waitId == VK_VIDEO_ENCODER_RESOURCE_NULL) ||
        (signalId == VK_VIDEO_ENCODER_RESOURCE_NULL)) {
        Check(false, "InstallSemaphore", "setup failed");
        return;
    }
    const uint64_t kWaitValue   = 55;
    const uint64_t kSignalValue = 66;

    VkVideoEncoderFrameSyncDescriptor sync;
    sync.waitCount         = 1;
    sync.pWaitSemaphores   = &waitId;
    sync.pWaitValues       = &kWaitValue;
    sync.signalCount       = 1;
    sync.pSignalSemaphores = &signalId;
    sync.pSignalValues     = &kSignalValue;

    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(103);
    FillCallerArrays(info);
    info.pNext = &sync;

    VkEncSubmitSyncProbe p;
    if (!SubmitAndProbe(s, info, &p)) {
        return;
    }
    CheckEqU32(p.waitCount, 1, "wait count is the resolved one");
    CheckEqSem(p.waitSemaphores[0], kRegWait, "wait[0] is the resolved one");
    CheckEqU64(p.waitValues[0], kWaitValue, "waitValue[0]");
    CheckEqU32(p.signalCount, 1, "signal count is the resolved one");
    CheckEqSem(p.signalSemaphores[0], kRegSignal,
               "signal[0] is the resolved one");
    CheckEqU64(p.signalValues[0], kSignalValue, "signalValue[0]");
}

//=============================================================================
// Case 4 -- control. No chain at all: both raw arrays reach the submit
// untouched. Without this, cases 1 and 2 could pass on a build that ignored
// the chain entirely.
//=============================================================================
void CaseNoChainKeepsBothCallerArrays(Session& s)
{
    g_currentCase = "NoChainKeepsBothCallerArrays";
    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(104);
    FillCallerArrays(info);

    VkEncSubmitSyncProbe p;
    if (!SubmitAndProbe(s, info, &p)) {
        return;
    }
    ExpectCallerWaitSurvived(p);
    ExpectCallerSignalSurvived(p);
}

//=============================================================================
// Case 5 -- an acquire-only chain with NO caller signals must not invent one.
// The per-direction rule leaves the untouched direction exactly as it found
// it, empty included.
//=============================================================================
void CaseAcquireOnlyChainWithNoCallerSignals(Session& s)
{
    g_currentCase = "AcquireOnlyChainWithNoCallerSignals";
    const VkSemaphore kRegistered = Sentinel(0x3005);
    const VkVideoEncoderResource id = s.InstallSemaphore(kRegistered);
    if (id == VK_VIDEO_ENCODER_RESOURCE_NULL) {
        Check(false, "InstallSemaphore", "setup failed");
        return;
    }
    const uint64_t kWaitValue = 99;

    VkVideoEncoderFrameSyncDescriptor sync;
    sync.waitCount       = 1;
    sync.pWaitSemaphores = &id;
    sync.pWaitValues     = &kWaitValue;

    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(105);
    info.pNext = &sync;

    VkEncSubmitSyncProbe p;
    if (!SubmitAndProbe(s, info, &p)) {
        return;
    }
    CheckEqU32(p.waitCount, 1, "wait count is the resolved one");
    CheckEqU32(p.signalCount, 0, "signal count stays empty");
}

//=============================================================================
// Case 6 -- the Chromium chain SHAPE: a fence descriptor ahead of the sync
// descriptor. The walk must traverse past the fence descriptor and still apply
// the per-direction rule to what follows. The fence carries acquireFenceFd =
// -1 (no fence) because importing a real one needs a device this session does
// not have; what this pins is the walk and the override, not the import.
//=============================================================================
void CaseFenceThenSyncChainKeepsCallerSignals(Session& s)
{
    g_currentCase = "FenceThenSyncChainKeepsCallerSignals";
    const VkSemaphore kRegistered = Sentinel(0x3006);
    const VkVideoEncoderResource id = s.InstallSemaphore(kRegistered);
    if (id == VK_VIDEO_ENCODER_RESOURCE_NULL) {
        Check(false, "InstallSemaphore", "setup failed");
        return;
    }
    const uint64_t kWaitValue = 123;

    VkVideoEncoderFrameSyncDescriptor sync;
    sync.waitCount       = 1;
    sync.pWaitSemaphores = &id;
    sync.pWaitValues     = &kWaitValue;

    int releaseFenceFd = 12345;  // must be overwritten by the library
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.pNext           = &sync;
    fence.acquireFenceFd  = -1;
    fence.pReleaseFenceFd = &releaseFenceFd;

    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(106);
    FillCallerArrays(info);
    info.pNext = &fence;

    VkEncSubmitSyncProbe p;
    if (!SubmitAndProbe(s, info, &p)) {
        return;
    }
    CheckEqU32(p.waitCount, 1, "wait count is the resolved one");
    CheckEqSem(p.waitSemaphores[0], kRegistered, "wait[0] is the resolved one");
    ExpectCallerSignalSurvived(p);
    // Not the subject of this file, but the walk reached the fence descriptor
    // only if this was answered.
    //
    // -1 here is the CORRECT answer and stays correct now that the release
    // export is implemented: this is a null-backend session, which has no
    // queue, so nothing can carry the signal a SYNC_FD export needs to be
    // pending. The library declines to create a release semaphore at all on
    // that arm. What proves the export itself works is the real-device
    // sibling, vk_video_encoder/test/encoder-ext-release-fence, which asserts
    // fd >= 0 and that it becomes signalled -- a claim no device-free test
    // can make.
    Check(releaseFenceFd == -1, "pReleaseFenceFd answered",
          "got " + U64((uint64_t)(int64_t)releaseFenceFd) + ", want -1");
}

//=============================================================================
// Case 7 -- THE CHAINING CONTRACT, TRAILING POSITION. The fence descriptor
// hangs off a VkVideoEncoderFrameSyncDescriptor's pNext. That is the shape the
// header names, and this is its only in-tree coverage:
// case 6 puts the fence FIRST, the hardware sibling puts it alone.
//
// The assertion is deliberately an EFFECT and not a status. A descriptor the
// sType gate accepts and the walk then never reads is dead code that looks
// wired, and it would pass any check that only asked whether the submit
// succeeded. The store through pReleaseFenceFd happens in the fence branch of
// the walk and nowhere else, so an unchanged sentinel means this node was
// skipped.
//=============================================================================
void CaseSyncThenFenceChainIsRead(Session& s)
{
    g_currentCase = "SyncThenFenceChainIsRead";
    const VkSemaphore kRegistered = Sentinel(0x3007);
    const VkVideoEncoderResource id = s.InstallSemaphore(kRegistered);
    if (id == VK_VIDEO_ENCODER_RESOURCE_NULL) {
        Check(false, "InstallSemaphore", "setup failed");
        return;
    }
    const uint64_t kWaitValue = 321;

    int releaseFenceFd = 12345;  // must be overwritten by the library
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = -1;
    fence.pReleaseFenceFd = &releaseFenceFd;

    VkVideoEncoderFrameSyncDescriptor sync;
    sync.pNext           = &fence;
    sync.waitCount       = 1;
    sync.pWaitSemaphores = &id;
    sync.pWaitValues     = &kWaitValue;

    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(107);
    FillCallerArrays(info);
    info.pNext = &sync;

    VkEncSubmitSyncProbe p;
    if (!SubmitAndProbe(s, info, &p)) {
        return;
    }
    // The sync descriptor was honoured in the LEADING position ...
    CheckEqU32(p.waitCount, 1, "wait count is the resolved one");
    CheckEqSem(p.waitSemaphores[0], kRegistered, "wait[0] is the resolved one");
    CheckEqU64(p.waitValues[0], kWaitValue, "waitValue[0] is the resolved one");
    ExpectCallerSignalSurvived(p);
    // ... and the fence descriptor BEHIND it was read, not walked past. -1 is
    // the correct answer on a session with no queue, for the reason spelled
    // out in case 6; what matters here is that 12345 did not survive.
    Check(releaseFenceFd == -1, "the fence behind the sync descriptor was read",
          "pReleaseFenceFd left at " + U64((uint64_t)(int64_t)releaseFenceFd) +
              " -- the walk never read this node");
}

//=============================================================================
// Case 8 -- the shape the only real caller sends: a fence descriptor ALONE on
// the submit info's pNext, no sync descriptor anywhere in the chain. That is
// media/gpu/vulkan/vulkan_video_encode_accelerator.cc, which sets
// input.pNext = &fence_desc. Read literally, the header's old wording made
// this illegal. It is not, and neither raw array may be disturbed by it.
//=============================================================================
void CaseFenceAloneChainIsRead(Session& s)
{
    g_currentCase = "FenceAloneChainIsRead";
    int releaseFenceFd = 12345;  // must be overwritten by the library
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = -1;
    fence.pReleaseFenceFd = &releaseFenceFd;

    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(108);
    FillCallerArrays(info);
    info.pNext = &fence;

    VkEncSubmitSyncProbe p;
    if (!SubmitAndProbe(s, info, &p)) {
        return;
    }
    Check(releaseFenceFd == -1, "the lone fence descriptor was read",
          "pReleaseFenceFd left at " + U64((uint64_t)(int64_t)releaseFenceFd) +
              " -- the walk never read this node");
    // A chain that names NEITHER direction is not a chain that names both as
    // empty: both caller arrays reach the submit intact.
    ExpectCallerWaitSurvived(p);
    ExpectCallerSignalSurvived(p);
}

//=============================================================================
// Case 9 -- the fence node's pNext is genuinely FOLLOWED, and the ABI's
// refuse-don't-skip rule holds in the trailing position. An unknown sType put
// behind the fence descriptor is reachable only through fence->pNext, so the
// refusal proves that field was read rather than the walk stopping at the
// fence. The store through pReleaseFenceFd must still have happened: the
// header promises a defined value on every exit path, the failing ones
// included, so that a caller cannot mistake stack garbage for an fd.
//=============================================================================
void CaseUnknownSTypeBehindFenceIsRefused(Session& s)
{
    g_currentCase = "UnknownSTypeBehindFenceIsRefused";
    // Every public struct opens with {sType, pNext}, which is what the walk
    // reads a link through -- so an unknown link needs nothing more than that.
    struct BogusLink {
        VkVideoEncoderStructureType sType;
        const void*                 pNext;
    };
    BogusLink bogus;
    bogus.sType = (VkVideoEncoderStructureType)0x5645FFFF;  // in the private
    bogus.pNext = nullptr;                                  // 'VE' block, unused

    int releaseFenceFd = 12345;  // must be overwritten by the library
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.pNext           = &bogus;
    fence.acquireFenceFd  = -1;
    fence.pReleaseFenceFd = &releaseFenceFd;

    VkVideoEncoderFrameSyncDescriptor sync;
    sync.pNext = &fence;

    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(109);
    FillCallerArrays(info);
    info.pNext = &sync;

    const VkVideoEncoderStatusCode status =
        s.Get()->SubmitRegisteredFrame(info, nullptr);
    Check(status == VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN,
          "unknown sType behind the fence descriptor is refused",
          "status " + U64((uint64_t)status) + ", want " +
              U64((uint64_t)VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN));
    Check(releaseFenceFd == -1,
          "the fence node was read before that refusal",
          "pReleaseFenceFd left at " + U64((uint64_t)(int64_t)releaseFenceFd));
}

//=============================================================================
// Case 10 -- the MIRROR of case 9, and the ordering in which the header's
// promise is easiest to break. Case 9 puts the bogus link BEHIND the fence descriptor,
// which is the one ordering in which "pReleaseFenceFd holds a defined value on
// every exit path" happened to hold by accident: the walk reached the fence
// node, stored -1, and only then refused. Put the bogus link AHEAD of the
// fence -- a shape this header explicitly blesses, since the chain is flat and
// order-independent -- and the walk refuses before it ever sees the fence
// node. A caller who declared `int fd;` on the strength of the header then
// close(2)s stack garbage, shutting an unrelated descriptor in its own
// process. The store is a pre-pass over the chain for exactly this reason.
//=============================================================================
void CaseUnknownSTypeAheadOfFenceStillWritesTheFd(Session& s)
{
    g_currentCase = "UnknownSTypeAheadOfFenceStillWritesTheFd";
    struct BogusLink {
        VkVideoEncoderStructureType sType;
        const void*                 pNext;
    };

    int releaseFenceFd = 12345;  // stands in for an uninitialised stack local
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = -1;
    fence.pReleaseFenceFd = &releaseFenceFd;

    BogusLink bogus;
    bogus.sType = (VkVideoEncoderStructureType)0x5645FFFE;
    bogus.pNext = &fence;

    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(110);
    FillCallerArrays(info);
    info.pNext = &bogus;

    const VkVideoEncoderStatusCode status =
        s.Get()->SubmitRegisteredFrame(info, nullptr);
    Check(status == VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN,
          "unknown sType ahead of the fence descriptor is refused",
          "status " + U64((uint64_t)status) + ", want " +
              U64((uint64_t)VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN));
    Check(releaseFenceFd == -1,
          "pReleaseFenceFd was written despite refusing before the fence node",
          "pReleaseFenceFd left at " + U64((uint64_t)(int64_t)releaseFenceFd) +
              " -- a caller following the header would close(2) that");
}

//=============================================================================
// Case 11 -- an UNRESOLVABLE registered id ahead of the fence descriptor. Same
// promise, a different refusal: this one is raised inside the sync node's own
// resolve loop rather than by the sType gate, so it proves the pre-pass covers
// the RESOURCE_UNKNOWN exit too and not just the STRUCTURE_TYPE_UNKNOWN one.
//=============================================================================
void CaseUnresolvableIdAheadOfFenceStillWritesTheFd(Session& s)
{
    g_currentCase = "UnresolvableIdAheadOfFenceStillWritesTheFd";
    int releaseFenceFd = 12345;
    VkVideoEncoderFrameFenceDescriptor fence;
    fence.acquireFenceFd  = -1;
    fence.pReleaseFenceFd = &releaseFenceFd;

    // Never installed, so it cannot resolve. Carries the semaphore tag bit so
    // it is rejected by the registry lookup rather than by the tag check.
    const VkVideoEncoderResource bogusId =
        (VkVideoEncoderResource)(0x8000000000000000ull | 0x0000000100000777ull);
    const uint64_t kWaitValue = 5;

    VkVideoEncoderFrameSyncDescriptor sync;
    sync.pNext           = &fence;
    sync.waitCount       = 1;
    sync.pWaitSemaphores = &bogusId;
    sync.pWaitValues     = &kWaitValue;

    VkVideoEncoderFrameSubmitInfo info = s.SubmitInfoFor(111);
    FillCallerArrays(info);
    info.pNext = &sync;

    const VkVideoEncoderStatusCode status =
        s.Get()->SubmitRegisteredFrame(info, nullptr);
    Check(status == VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN,
          "an unresolvable wait id is refused",
          "status " + U64((uint64_t)status) + ", want " +
              U64((uint64_t)VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN));
    Check(releaseFenceFd == -1,
          "pReleaseFenceFd was written despite refusing in the node ahead",
          "pReleaseFenceFd left at " + U64((uint64_t)(int64_t)releaseFenceFd));
}

}  // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    std::printf("Encoder-ext per-direction sync override\n");
    std::printf("---------------------------------------\n");

    Session session;
    if (!session.Open()) {
        std::printf("RESULT: COULD-NOT-RUN (session setup failed)\n");
        return 2;
    }

    CaseAcquireOnlyChainKeepsCallerSignals(session);
    CaseSignalOnlyChainKeepsCallerWaits(session);
    CaseBothSuppliedReplacesBothDirections(session);
    CaseNoChainKeepsBothCallerArrays(session);
    CaseAcquireOnlyChainWithNoCallerSignals(session);
    CaseFenceThenSyncChainKeepsCallerSignals(session);
    CaseSyncThenFenceChainIsRead(session);
    CaseFenceAloneChainIsRead(session);
    CaseUnknownSTypeBehindFenceIsRefused(session);
    CaseUnknownSTypeAheadOfFenceStillWritesTheFd(session);
    CaseUnresolvableIdAheadOfFenceStillWritesTheFd(session);

    std::printf("---------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
