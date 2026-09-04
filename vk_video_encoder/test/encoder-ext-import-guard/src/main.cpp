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
 * The dma-buf import-ordinal guard's REPORTING CHANNEL, device-free half.
 *
 * WHAT THIS COVERS, AND -- FIRST -- WHAT IT DOES NOT.
 *
 * The guard is RETIRED BY DEFAULT: the library builds with
 * kVkEncImportOrdinalGuardCount = 0, takes no sacrificial dma-buf import and
 * reports DISABLED. Armed, it takes that many imports on an NVIDIA VkDevice.
 * Nothing here can produce one either way: a dma-buf import needs a real
 * exporter, a real driver and a real device, and no encoder-ext test in this
 * tree reaches VkEncImportExternalImage with HANDLE_TYPE_DMA_BUF except the
 * hardware sibling. So COMPLETE and INCOMPLETE -- the verdicts only an armed
 * build can produce -- are NOT covered by this file, and neither is DISABLED
 * on a real import. They are covered by a hardware run or not at all. Read
 * the honest gap statement in the commit message before treating a green
 * here as "the guard works"; on the shipped build the guard does nothing,
 * deliberately, and that is what the hardware sibling asserts.
 *
 * WHAT IS COVERED is the carrier the guard's verdict now travels on, which
 * is the part that was missing entirely and which is fully device-free:
 *
 *   * VkVideoEncoderStatus::pNext accepts exactly one
 *     VkVideoEncoderImportGuardInfo, and the library WRITES it. Before this
 *     change a chained pStatus was refused outright, so a caller could not
 *     ask the question at all.
 *   * An unknown chained sType, and a REPEATED known one, are still refused
 *     -- the relaxation must not have turned into "ignore the chain".
 *   * A chain hanging off a MIS-STAMPED pStatus is refused AND is not
 *     written through. Writing into a struct whose gate rejected the
 *     request is the version-skew failure the gate exists to prevent.
 *   * VkVideoEncoderCompletionInfo::pNext accepts the same struct, and its
 *     own unknown-sType refusal is unchanged.
 *
 * WHY EVERY GUARD-INFO CASE POISONS ITS STRUCT FIRST. Every field's
 * "nothing happened" value is 0, which is exactly what a caller's
 * zero-initialised struct already holds -- so asserting 0 proves nothing
 * about whether the library wrote anything. requestedCount escapes that only
 * while it is non-zero: it is a BUILD constant stamped on every path, so a
 * non-zero count read back proves the writer ran. This build's count is 0, so
 * that proof is not available here, and the poison below is what supplies it.
 *
 * What replaces it does not depend on the constant at all, and is stronger:
 * hand the library values it MUST overwrite (C1) or MUST leave alone
 * (C2-C4), and assert which happened. A library that dropped the chain, or
 * that wrote through a chain its own gate refused, leaves the poison
 * standing or clears it -- and either way one of these cases goes red. C1
 * additionally pins the build count itself, so the retirement is a claim
 * this suite makes rather than one it merely tolerates.
 *
 * WHY A NULL BACKEND. VkEncInstallNullBackend gives a session that reports
 * initialized with no device, no worker threads and no VkVideoEncoder, on
 * which a VK_IMAGE registration runs the real RegisterImageResource --
 * including the real pStatus gate and the real chain walk -- and stops
 * before the one step that needs a device. No hardware dependence in either
 * direction, which is what makes it deterministic.
 */

#include "vulkan_video_encoder_ext_internal.h"
// C9 needs the CONCRETE VulkanDeviceContext, not the forward declaration the
// internal header is content with -- it constructs one and writes a dispatch
// member. Same include, same position, as the sibling ordinal-guard suite.
#include "VkCodecUtils/VulkanDeviceContext.h"

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
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

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

// A session with no device. VK_IMAGE registrations skip the device-touching
// view build; everything ahead of it -- the ownership echo, the structure
// gate, the chain walk, the guard verdict -- is the production code.
class Session {
public:
    bool Open()
    {
        if ((CreateVulkanVideoEncoderExt(m_encoder) != VK_SUCCESS) ||
            !m_encoder) {
            std::printf("  ERROR: CreateVulkanVideoEncoderExt failed\n");
            return false;
        }
        if (VkEncInstallNullBackend(m_encoder.get(), &m_backend) !=
            VK_SUCCESS) {
            std::printf("  ERROR: VkEncInstallNullBackend failed\n");
            return false;
        }
        return true;
    }

    VulkanVideoEncoderExt* Get() const { return m_encoder.get(); }

    // The descriptor every case registers. A VK_IMAGE slot with a sentinel
    // handle: stored and compared, never dereferenced, because this session
    // has no device to dereference it with.
    static VkVideoEncoderExternalImageDescriptor Descriptor()
    {
        VkVideoEncoderExternalImageDescriptor desc = {};
        desc.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
        desc.handleType = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
        desc.format     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        desc.width      = 640;
        desc.height     = 360;
        desc.residency  = VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
        desc.existingImage = (VkImage)(uintptr_t)0xA110C8ED;
        return desc;
    }

private:
    VkSharedBaseObj<VulkanVideoEncoderExt> m_encoder;
    VkEncNullBackendState                  m_backend{};
};

VkVideoEncoderImportGuardInfo FreshGuardInfo()
{
    VkVideoEncoderImportGuardInfo info = {};
    info.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_GUARD_INFO;
    return info;
}

// The library's kVkEncImportOrdinalGuardCount, mirrored -- it lives in an
// anonymous namespace and cannot be read from here. 0 is the retired default:
// the guard takes no sacrificial import and reports DISABLED on the imports
// it would once have moved. Pinning it is deliberate. The count is a claim
// about a driver defect (it buys a phase shift of the damage pattern and not
// a repair, measured), so a build that changes it is changing that claim and
// has to come here and say so. The hardware sibling takes --guard-count=N for
// the same reason, and is where a non-zero count is actually exercised.
constexpr uint32_t kExpectedRequestedCount = 0;

// Values no library path can produce, written into a caller's struct before
// the call so that "the library wrote this" and "the library did not" are
// distinguishable at all. See WHY EVERY GUARD-INFO CASE POISONS ITS STRUCT
// FIRST at the top of this file: with the guard retired, every honest field
// value on every path this suite can reach is 0, which is also what an
// untouched struct holds.
void PoisonGuardInfo(VkVideoEncoderImportGuardInfo& info)
{
    info.state          = VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_INCOMPLETE;
    info.requestedCount = 0xBADC0DEu;
    info.retainedCount  = 99u;
    info.failureStatus  = VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED;
    info.failureErrno   = -4242;
}

VkVideoEncoderStatus FreshStatus()
{
    VkVideoEncoderStatus status = {};
    status.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS;
    return status;
}

// ---------------------------------------------------------------------------
// C1. The channel exists at all: a chained guard-info is accepted and FILLED.
//
// This is the case the whole change is for. Before it, the same call answered
// STRUCTURE_TYPE_UNKNOWN -- a caller with silenceStdio set had no way, at
// all, to learn what the workaround had done.
// ---------------------------------------------------------------------------
void CaseChainedGuardInfoIsAcceptedAndWritten(Session& s)
{
    g_currentCase = "chained guard-info is accepted and written";

    VkVideoEncoderImportGuardInfo guardInfo = FreshGuardInfo();
    PoisonGuardInfo(guardInfo);
    VkVideoEncoderStatus status = FreshStatus();
    status.pNext = &guardInfo;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
        Session::Descriptor(), 0, &resource, &status);

    Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "a chained VkVideoEncoderImportGuardInfo is ACCEPTED",
          "status " + U64((uint64_t)reg));
    Check(resource != VK_VIDEO_ENCODER_RESOURCE_NULL,
          "the registration still produced an id", "id was NULL");
    Check(status.handlesConsumed == VK_FALSE,
          "the ownership echo still answers for a VK_IMAGE registration",
          "handlesConsumed was VK_TRUE");

#if defined(__linux__)
    // THE BUILD-COUNT PIN. requestedCount is stamped by the library on every
    // path, and it is the guard's build constant. The library ships 0 -- the
    // guard is retired, because retaining imports was measured to move which
    // imports the driver damages and not how many. A build that re-arms it
    // fails here, which is the point: the count is a claim about a driver
    // defect and it does not get to change silently. This is ALSO half of the
    // writer gate, since the struct was poisoned with 0xBADC0DE and only the
    // library can have put a 0 there.
    Check(guardInfo.requestedCount == kExpectedRequestedCount,
          "requestedCount is this build's guard count (retired default 0)",
          "requestedCount was " + U64(guardInfo.requestedCount) +
              ", expected " + U64(kExpectedRequestedCount));
#endif
    // THE REST OF THE WRITER GATE. Each of these fields was poisoned above
    // with a value no library path produces, so every one of them is now a
    // proof that the library WROTE the caller's struct -- not merely that
    // the struct still holds zeroes. Before the guard was retired that job
    // belonged to requestedCount alone, and a zeroed build constant can no
    // longer do it.
    //
    // The values themselves are the honest answer for a VK_IMAGE
    // registration: it performs no import, so the guard never ran for it,
    // which is a distinct answer from "it ran and did nothing" (DISABLED).
    Check(guardInfo.state == VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_EVALUATED,
          "a VK_IMAGE registration reports NOT_EVALUATED (poison cleared)",
          "state " + U64((uint64_t)guardInfo.state));
    Check(guardInfo.retainedCount == 0,
          "no sacrificial import is claimed on a registration that did none",
          "retainedCount " + U64(guardInfo.retainedCount));
    Check(guardInfo.failureStatus == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "no failure is claimed where nothing failed",
          "failureStatus " + U64((uint64_t)guardInfo.failureStatus));
    Check(guardInfo.failureErrno == 0,
          "no errno is claimed where nothing failed",
          "failureErrno " + U64((uint64_t)(int64_t)guardInfo.failureErrno));
}

// ---------------------------------------------------------------------------
// C2. The relaxation did not become "ignore the chain".
// ---------------------------------------------------------------------------
void CaseUnknownChainedSTypeIsRefused(Session& s)
{
    g_currentCase = "unknown chained sType is refused";

    VkVideoEncoderImportGuardInfo alien = FreshGuardInfo();
    alien.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FILTER_INFO;  // not ours
    // Poisoned so the "not written through" assertion below can fail. At the
    // retired guard count the library's own written value for requestedCount
    // is 0, so asserting 0 on a refusal path stopped discriminating.
    PoisonGuardInfo(alien);
    VkVideoEncoderStatus status = FreshStatus();
    status.pNext = &alien;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
        Session::Descriptor(), 0, &resource, &status);

    Check(reg == VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN,
          "an extension this build does not understand is REFUSED",
          "status " + U64((uint64_t)reg));
    Check(resource == VK_VIDEO_ENCODER_RESOURCE_NULL,
          "a refused registration mints no id",
          "id " + U64((uint64_t)resource));
    Check((alien.requestedCount == 0xBADC0DEu) &&
              (alien.retainedCount == 99u) &&
              (alien.state ==
               VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_INCOMPLETE),
          "a refused link is not written through (the poison SURVIVES)",
          "requestedCount " + U64(alien.requestedCount) + ", retainedCount " +
              U64(alien.retainedCount) + ", state " +
              U64((uint64_t)alien.state));
}

// ---------------------------------------------------------------------------
// C3. Two links of one type mean the caller believes it is getting two
// different things. Refused, as everywhere else in this file's chain walks.
// ---------------------------------------------------------------------------
void CaseRepeatedGuardInfoLinkIsRefused(Session& s)
{
    g_currentCase = "repeated guard-info link is refused";

    VkVideoEncoderImportGuardInfo second = FreshGuardInfo();
    VkVideoEncoderImportGuardInfo first  = FreshGuardInfo();
    // Poisoned for the same reason as C2: 0 is what the library itself would
    // write for these fields now, so only a value it cannot write can prove
    // it wrote nothing.
    PoisonGuardInfo(first);
    PoisonGuardInfo(second);
    first.pNext = &second;
    VkVideoEncoderStatus status = FreshStatus();
    status.pNext = &first;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
        Session::Descriptor(), 0, &resource, &status);

    Check(reg == VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN,
          "a repeated known sType is REFUSED",
          "status " + U64((uint64_t)reg));
    Check((first.requestedCount == 0xBADC0DEu) &&
              (second.requestedCount == 0xBADC0DEu),
          "neither link of a refused chain is written through (poison "
          "SURVIVES in both)",
          "first " + U64(first.requestedCount) + ", second " +
              U64(second.requestedCount));
}

// ---------------------------------------------------------------------------
// C4. A chain behind a MIS-STAMPED pStatus. The status gate refuses first,
// and nothing may be written through either struct: a consumer built against
// a different header is exactly who owns that memory.
// ---------------------------------------------------------------------------
void CaseMisStampedStatusRefusesAndWritesNothing(Session& s)
{
    g_currentCase = "mis-stamped pStatus refuses and writes nothing";

    VkVideoEncoderImportGuardInfo guardInfo = FreshGuardInfo();
    // Poisoned, or this case cannot fail: the library's own written value for
    // requestedCount is 0 on every path this build takes, so "still 0"
    // does not separate "not written" from "written".
    PoisonGuardInfo(guardInfo);
    VkVideoEncoderStatus status = FreshStatus();
    status.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_UNDEFINED;
    status.pNext = &guardInfo;
    status.handlesConsumed = VK_TRUE;  // a sentinel the library must not clear

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
        Session::Descriptor(), 0, &resource, &status);

    Check(reg == VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN,
          "a mis-stamped pStatus is still version skew",
          "status " + U64((uint64_t)reg));
    Check(status.handlesConsumed == VK_TRUE,
          "a mis-stamped pStatus is not written through",
          "handlesConsumed was cleared");
    Check((guardInfo.requestedCount == 0xBADC0DEu) &&
              (guardInfo.retainedCount == 99u),
          "a chain behind a mis-stamped pStatus is not written through (the "
          "poison SURVIVES)",
          "requestedCount " + U64(guardInfo.requestedCount) +
              ", retainedCount " + U64(guardInfo.retainedCount));
}

// ---------------------------------------------------------------------------
// C5. The pre-existing shape -- pStatus with no chain -- is untouched. This
// is the ABI-compatibility case: every caller built against the header that
// had nothing to chain here passes exactly this.
// ---------------------------------------------------------------------------
void CaseUnchainedStatusStillWorks(Session& s)
{
    g_currentCase = "unchained pStatus is unaffected";

    VkVideoEncoderStatus status = FreshStatus();
    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
        Session::Descriptor(), 0, &resource, &status);

    Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "an unchained pStatus still registers",
          "status " + U64((uint64_t)reg));
    Check(status.handlesConsumed == VK_FALSE,
          "the ownership echo is unchanged",
          "handlesConsumed was VK_TRUE");
}

// ---------------------------------------------------------------------------
// C6. The second read site: GetCompletionInfo. This is the surface a consumer
// already calls, so it is the one that can report the verdict without the
// consumer changing its registration code at all.
// ---------------------------------------------------------------------------
void CaseCompletionInfoChainAcceptsGuardInfo(Session& s)
{
    g_currentCase = "GetCompletionInfo accepts the guard-info link";

    VkVideoEncoderImportGuardInfo guardInfo = FreshGuardInfo();
    guardInfo.state = VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_COMPLETE;  // sentinel
    guardInfo.retainedCount = 99;                                    // sentinel
    VkVideoEncoderCompletionInfo info = {};
    info.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_COMPLETION_INFO;
    info.pNext = &guardInfo;

    const VkResult r = s.Get()->GetCompletionInfo(&info);
    Check(r == VK_SUCCESS,
          "the snapshot call accepts a chained guard-info",
          "VkResult " + U64((uint64_t)r));
    // The sentinels must have been OVERWRITTEN. No registration on this
    // session was ever evaluated by the guard, so the honest answer is
    // NOT_EVALUATED / 0 -- and a library that ignored the link would leave
    // COMPLETE / 99 standing, which is the exact shape of the over-claim
    // this channel exists to make impossible.
    Check(guardInfo.state == VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_EVALUATED,
          "the snapshot overwrote the caller's sentinel state",
          "state " + U64((uint64_t)guardInfo.state));
    Check(guardInfo.retainedCount == 0,
          "the snapshot overwrote the caller's sentinel count",
          "retainedCount " + U64(guardInfo.retainedCount));
}

void CaseCompletionInfoStillRefusesUnknownSType(Session& s)
{
    g_currentCase = "GetCompletionInfo still refuses an unknown sType";

    VkVideoEncoderImportGuardInfo alien = FreshGuardInfo();
    alien.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS;  // not chainable here
    VkVideoEncoderCompletionInfo info = {};
    info.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_COMPLETION_INFO;
    info.pNext = &alien;

    const VkResult r = s.Get()->GetCompletionInfo(&info);
    Check(r == VK_ERROR_INITIALIZATION_FAILED,
          "an unknown chained sType is still refused, not ignored",
          "VkResult " + U64((uint64_t)r));
}

void CaseCompletionInfoRefusesRepeatedGuardInfo(Session& s)
{
    g_currentCase = "GetCompletionInfo refuses a repeated guard-info";

    VkVideoEncoderImportGuardInfo second = FreshGuardInfo();
    VkVideoEncoderImportGuardInfo first  = FreshGuardInfo();
    first.pNext = &second;
    VkVideoEncoderCompletionInfo info = {};
    info.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_COMPLETION_INFO;
    info.pNext = &first;

    const VkResult r = s.Get()->GetCompletionInfo(&info);
    Check(r == VK_ERROR_INITIALIZATION_FAILED,
          "two guard-info links are refused",
          "VkResult " + U64((uint64_t)r));
}

}  // namespace

#if defined(__linux__)
// ---------------------------------------------------------------------------
// C9. The guard must not require dispatch the import never needed.
//
// REGRESSION, and it shipped: the guard calls VkEncIsNvidiaDevice on the way
// in to EVERY dma-buf import, and that probe called
// vkDevCtx.GetPhysicalDeviceProperties with no null check. Until the guard
// existed VkEncImportExternalImage touched no INSTANCE-level dispatch at all,
// so a consumer that populated only the device-level entries the import
// actually uses was fine -- and then was not. It segfaulted all ten of
// Chromium's VulkanVideoEncoderImportOwnershipTest cases (SEGV_MAPERR, ip=0),
// which is precisely that shape: a bare VulkanDeviceContext with device-level
// stubs and no instance table. The library's own CI could not see it, because
// nothing here had ever called the import with a partly-populated context.
//
// So this case builds the minimal such context -- ONE dispatch entry, a
// CreateImage that refuses -- and asserts the import RETURNS. The vendor probe
// must answer "not NVIDIA" when it cannot tell, which skips the guard and
// leaves the import behaving exactly as it did before the guard existed.
//
// MUTATION: drop either clause of the null check in VkEncIsNvidiaDevice and
// this case does not fail, it CRASHES -- ctest reports the whole binary dead,
// which is a louder red than a FAIL line and is the correct one here.
VkResult VKAPI_PTR RefusingCreateImage(VkDevice,
                                       const VkImageCreateInfo*,
                                       const VkAllocationCallbacks*,
                                       VkImage*)
{
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

void CaseImportSurvivesAContextWithNoInstanceDispatch()
{
    g_currentCase = "import survives a context with no instance dispatch";

    // Everything null except the one entry the import reaches after the
    // guard declines. This is the point: it is SUPPOSED to be under-populated.
    VulkanDeviceContext ctx{};
    ctx.CreateImage = &RefusingCreateImage;

    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        Check(false, "pipe(2) for the import fd", "pipe failed");
        return;
    }
    close(fds[1]);
    const int fd = fds[0];

    VkVideoEncoderExternalImageDescriptor desc{};
    desc.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
    desc.handleType = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF;
    desc.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    desc.width = 64;
    desc.height = 64;
    desc.tiling = VK_IMAGE_TILING_LINEAR;
    desc.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
    desc.allocationSize = 8192;
    desc.memoryTypeBits = 0;
    desc.memoryTypeIndex = UINT32_MAX;

    VkEncImportedImage imported{};
    const VkVideoEncoderStatusCode status =
        VkEncImportExternalImage(ctx, desc, static_cast<uint64_t>(fd),
                                 &imported);

    // Reaching this line at all is the assertion that matters; the rest
    // pins the behaviour so the crash cannot be "fixed" by changing it.
    Check(status == VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED,
          "a dma-buf import on an instance-less context returns, not crashes",
          "status " + U64((uint64_t)status));
    Check(imported.result == VK_ENC_IMPORT_FAILED_BEFORE_ALLOCATE,
          "the refusal is classified as pre-handoff",
          "result " + U64((uint64_t)imported.result));
    // Design section 2.3: nothing reached the driver, so the LIBRARY owns the
    // close. fcntl on a closed fd is the only way to ask without racing.
    Check(fcntl(fd, F_GETFD) == -1,
          "the library closed the fd it never handed over",
          "fd " + U64((uint64_t)fd) + " is still open");
}
#endif  // __linux__

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    std::printf("Encoder-ext import-ordinal guard reporting channel\n");
    std::printf("--------------------------------------------------\n");

    Session session;
    if (!session.Open()) {
        std::printf("RESULT: COULD-NOT-RUN (session setup failed)\n");
        return 2;
    }

    CaseChainedGuardInfoIsAcceptedAndWritten(session);
    CaseUnknownChainedSTypeIsRefused(session);
    CaseRepeatedGuardInfoLinkIsRefused(session);
    CaseMisStampedStatusRefusesAndWritesNothing(session);
    CaseUnchainedStatusStillWorks(session);
    CaseCompletionInfoChainAcceptsGuardInfo(session);
    CaseCompletionInfoStillRefusesUnknownSType(session);
    CaseCompletionInfoRefusesRepeatedGuardInfo(session);
#if defined(__linux__)
    CaseImportSurvivesAContextWithNoInstanceDispatch();
#endif

    std::printf("--------------------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
