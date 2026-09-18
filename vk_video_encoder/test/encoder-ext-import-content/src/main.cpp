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
 * The dma-buf IMPORT CONTENT PROBE: the detection predicate, the scorer that
 * feeds it, the per-registration latch, and the carrier the verdict travels
 * on. Device-free.
 *
 * WHAT THIS COVERS, AND -- FIRST -- WHAT IT DOES NOT.
 *
 * NOT COVERED, and it is the interesting half: whether the probe fires on a
 * REAL buffer the driver actually poisoned. That needs a driver that
 * exhibits the defect, a GBM exporter and a real dma-buf import, and no
 * device-free test
 * can manufacture one. A green here is NOT evidence the workaround works on
 * hardware. It is evidence that, GIVEN the bytes a poisoned import produces,
 * this code reaches the right verdict and reports it -- which is the half
 * that can be regression-tested on every host, and the half that would
 * otherwise be provable only on one machine.
 *
 * WHAT IS COVERED:
 *
 *   1. THE PREDICATE, over the whole 2x2x2 quadrant space of (luma dead,
 *      U dead, V dead) plus both sides of the threshold. This is the
 *      requirement that has been got wrong twice on this defect: a
 *      CHROMA-ONLY scorer conflates a chroma-dead buffer with an all-dead
 *      one, so the union is asserted quadrant by quadrant rather than by
 *      spot-check.
 *
 *   2. THE SCORER, over SYNTHETIC NV12 buffers laid out the way a real
 *      HOST_VISIBLE LINEAR image is -- non-zero plane offsets, row pitch
 *      wider than the width, and DIFFERENT VALUES in the rows the row-stride
 *      skips and in the padding past the width. A scorer that used the wrong
 *      pitch, forgot an offset, walked every row, or read into the padding
 *      produces a different mean and turns these red. The four buffers are
 *      the two measured damage modes, ordinary content, and -- the one that
 *      matters most for false positives -- a FULLY LEGAL BLACK FRAME, which
 *      is Y=16 U=V=128 and must score CLEAN.
 *
 *   3. THE LATCH: one verdict per REGISTRATION and not per frame, the
 *      oldest-damaged-first report ordering, and the drain -- retiring a
 *      damaged registration is what lets the next one surface, and is the
 *      whole reason a consumer can act on this channel without losing a
 *      verdict between two polls.
 *
 *   4. THE CARRIER: VkVideoEncoderImportContentInfo chained onto
 *      RegisterImageResource (where chaining it IS the opt-in) and onto
 *      GetCompletionInfo (where the verdict comes back), the widened chain
 *      gate that now accepts TWO known link types, and the writer proof.
 *
 * WHY A NULL BACKEND, for group 4. VkEncInstallNullBackend gives a session
 * that reports initialized with no device, no worker threads and no
 * VkVideoEncoder at all, on which a VK_IMAGE registration runs the real
 * RegisterImageResource -- the real pStatus gate, the real chain walk, the
 * real arming. That the probe object is owned by the EXT layer rather than by
 * the encoder is what makes this reachable; see the note on m_contentProbe.
 */

#include "vulkan_video_encoder_ext_internal.h"
#include "VkVideoEncoder/VkVideoEncoderContentProbe.h"

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

using Probe = VkVideoEncoderContentProbe;

const char* StateName(Probe::State s)
{
    switch (s) {
        case Probe::STATE_NOT_EVALUATED:  return "NOT_EVALUATED";
        case Probe::STATE_NOT_APPLICABLE: return "NOT_APPLICABLE";
        case Probe::STATE_ARMED:          return "ARMED";
        case Probe::STATE_CLEAN:          return "CLEAN";
        case Probe::STATE_DAMAGED_CHROMA: return "DAMAGED_CHROMA";
        case Probe::STATE_DAMAGED_ALL:    return "DAMAGED_ALL";
    }
    return "?";
}

// Q8 helper: a plane whose every sampled byte is |v|.
constexpr uint32_t Q8(uint32_t v) { return v * 256u; }

// ===========================================================================
// GROUP 1 -- THE PREDICATE
// ===========================================================================
//
// The full quadrant space. DEAD is any mean strictly below 2.0/255; ALIVE is
// at or above it. The two damage modes this driver defect produces are
// (alive, dead, dead) and (dead, dead, dead); the single-channel chroma cases
// are included because a partially-written chroma plane is the shape a
// half-completed import would take and the requirement names U OR V, not
// U AND V.
void CasePredicateQuadrants()
{
    g_currentCase = "predicate: every (Y,U,V) liveness quadrant";

    struct Row {
        uint32_t y, u, v;
        Probe::State want;
        const char* why;
    };
    const uint32_t DEAD  = 0;
    const uint32_t ALIVE = Q8(100);
    const Row rows[] = {
        { ALIVE, ALIVE, ALIVE, Probe::STATE_CLEAN,
          "ordinary content" },
        { ALIVE, DEAD,  DEAD,  Probe::STATE_DAMAGED_CHROMA,
          "CHROMA_ZERO: the measured mode -- luma correct, chroma plane dead" },
        { ALIVE, DEAD,  ALIVE, Probe::STATE_DAMAGED_CHROMA,
          "U alone dead: the requirement says U OR V, not U AND V" },
        { ALIVE, ALIVE, DEAD,  Probe::STATE_DAMAGED_CHROMA,
          "V alone dead" },
        { DEAD,  DEAD,  DEAD,  Probe::STATE_DAMAGED_ALL,
          "ALL_ZERO: the other measured mode. A CHROMA-ONLY scorer reports "
          "this identically to the row above, which is the conflation that "
          "has produced a wrong conclusion here twice" },
        { DEAD,  ALIVE, ALIVE, Probe::STATE_CLEAN,
          "luma dead over LIVE chroma is deliberately NOT in the union: it "
          "is not a mode this defect produces, and a black-luma frame is a "
          "thing a producer can legitimately send" },
        { DEAD,  DEAD,  ALIVE, Probe::STATE_CLEAN,
          "same quadrant family: no all-dead, and the chroma clause requires "
          "live luma" },
        { DEAD,  ALIVE, DEAD,  Probe::STATE_CLEAN,
          "same" },
    };
    for (const Row& r : rows) {
        const Probe::State got = Probe::ClassifyPlaneMeansQ8(r.y, r.u, r.v);
        Check(got == r.want, r.why,
              std::string("Y=") + U64(r.y) + " U=" + U64(r.u) + " V=" +
                  U64(r.v) + " -> " + StateName(got) + ", expected " +
                  StateName(r.want));
    }
}

// The threshold itself, from both sides. 512 is 2.0 in Q8; the predicate is
// STRICTLY below, so 511 is dead and 512 is alive. A scorer that used <= or
// that scaled by 255 instead of 256 lands on the wrong side of one of these.
void CasePredicateThresholdBoundary()
{
    g_currentCase = "predicate: the dead-plane threshold, both sides";

    Check(Probe::kDeadPlaneMeanQ8 == 512u,
          "the threshold is 2.0/255 expressed in Q8",
          "kDeadPlaneMeanQ8 = " + U64(Probe::kDeadPlaneMeanQ8));
    Check(Probe::ClassifyPlaneMeansQ8(Q8(100), 511u, 511u) ==
              Probe::STATE_DAMAGED_CHROMA,
          "a chroma mean of 511 (just under 2.0) is DEAD",
          "got " + std::string(StateName(
                       Probe::ClassifyPlaneMeansQ8(Q8(100), 511u, 511u))));
    Check(Probe::ClassifyPlaneMeansQ8(Q8(100), 512u, 512u) ==
              Probe::STATE_CLEAN,
          "a chroma mean of exactly 512 (2.0) is ALIVE -- strictly below",
          "got " + std::string(StateName(
                       Probe::ClassifyPlaneMeansQ8(Q8(100), 512u, 512u))));
    Check(Probe::ClassifyPlaneMeansQ8(511u, 511u, 511u) ==
              Probe::STATE_DAMAGED_ALL,
          "all three just under the threshold is ALL, not CHROMA",
          "got " + std::string(StateName(
                       Probe::ClassifyPlaneMeansQ8(511u, 511u, 511u))));
}

// ===========================================================================
// GROUP 2 -- THE SCORER, over synthetic NV12 images
// ===========================================================================

// A host-visible LINEAR NV12 image the way a driver actually hands one back:
// a non-zero plane offset, a row pitch wider than the width, and the chroma
// plane after the luma one at its own offset. The three "poison" values below
// are what make this a test of the ADDRESSING and not just of the arithmetic.
struct SyntheticNv12 {
    static constexpr uint32_t kWidth  = 64;
    static constexpr uint32_t kHeight = 32;
    static constexpr uint32_t kLumaPitch   = kWidth + 37;        // padded, odd
    static constexpr uint32_t kChromaPitch = (kWidth / 2) * 2 + 23;
    static constexpr uint64_t kLumaOffset   = 64;                // not 0
    static constexpr uint64_t kChromaOffset =
        kLumaOffset + (uint64_t)kLumaPitch * kHeight;

    std::vector<uint8_t> bytes;
    VkSubresourceLayout luma{};
    VkSubresourceLayout chroma{};

    // |sampledY| is written into the rows the kRowStride=8 walk READS;
    // |skippedY| into the rows it must SKIP; padding past the width gets
    // 0xFF. Same for chroma. If the scorer walks every row, or misreads the
    // pitch, or runs off the end of a row, it picks up |skippedY| or 0xFF and
    // the asserted mean moves.
    SyntheticNv12(uint8_t sampledY, uint8_t skippedY,
                  uint8_t sampledU, uint8_t sampledV,
                  uint8_t skippedU, uint8_t skippedV)
    {
        const uint32_t cw = kWidth / 2;
        const uint32_t ch = kHeight / 2;
        bytes.assign((size_t)kChromaOffset + (size_t)kChromaPitch * ch + 64,
                     0xFFu);
        for (uint32_t y = 0; y < kHeight; y++) {
            uint8_t* r = bytes.data() + kLumaOffset + (size_t)y * kLumaPitch;
            const uint8_t v = ((y % 8u) == 0u) ? sampledY : skippedY;
            memset(r, v, kWidth);
            // Row padding stays 0xFF from the fill above.
        }
        for (uint32_t y = 0; y < ch; y++) {
            uint8_t* r = bytes.data() + kChromaOffset + (size_t)y * kChromaPitch;
            const bool sampled = ((y % 8u) == 0u);
            for (uint32_t x = 0; x < cw; x++) {
                r[(2 * x) + 0] = sampled ? sampledU : skippedU;
                r[(2 * x) + 1] = sampled ? sampledV : skippedV;
            }
        }
        luma.offset = kLumaOffset;
        luma.rowPitch = kLumaPitch;
        chroma.offset = kChromaOffset;
        chroma.rowPitch = kChromaPitch;
    }

    Probe::State Score(uint32_t& y, uint32_t& u, uint32_t& v) const
    {
        std::vector<uint8_t> scratch;
        Probe::ScorePlanesQ8(bytes.data(), luma, chroma, kWidth, kHeight,
                             scratch, y, u, v);
        return Probe::ClassifyPlaneMeansQ8(y, u, v);
    }
};

void CaseScorerOnSyntheticBuffers()
{
    g_currentCase = "scorer: synthetic NV12, padded pitch and plane offsets";

    struct Buf {
        const char* name;
        uint8_t sy, ky, su, sv, ku, kv;
        uint32_t wantY, wantU, wantV;
        Probe::State want;
    };
    const Buf bufs[] = {
        // Ordinary content. Skipped rows and padding carry values that would
        // move every one of these means if they were read.
        { "ordinary content", 100, 200, 110, 120, 10, 20,
          Q8(100), Q8(110), Q8(120), Probe::STATE_CLEAN },
        // THE FALSE-POSITIVE CASE. A fully legal black NV12 frame: Y=16,
        // U=V=128. Zeroed is NOT black, and this is the assertion that says
        // so -- if it ever goes red, the workaround has started rerouting
        // buffers that were working.
        { "fully legal BLACK frame (Y=16, U=V=128)", 16, 16, 128, 128, 128, 128,
          Q8(16), Q8(128), Q8(128), Probe::STATE_CLEAN },
        // CHROMA_ZERO: measured mode 1. Luma alive, chroma plane dead. The
        // skipped chroma rows are ALIVE, so a scorer that walked every row
        // would find live chroma and miss the damage entirely.
        { "CHROMA_ZERO (measured mode 1)", 100, 200, 0, 0, 200, 200,
          Q8(100), 0, 0, Probe::STATE_DAMAGED_CHROMA },
        // ALL_ZERO: measured mode 2.
        { "ALL_ZERO (measured mode 2)", 0, 200, 0, 0, 200, 200,
          0, 0, 0, Probe::STATE_DAMAGED_ALL },
    };
    for (const Buf& b : bufs) {
        SyntheticNv12 img(b.sy, b.ky, b.su, b.sv, b.ku, b.kv);
        uint32_t y = 0, u = 0, v = 0;
        const Probe::State got = img.Score(y, u, v);
        Check(y == b.wantY,
              "luma mean is exact (pitch, offset and row stride all right)",
              std::string(b.name) + ": meanY=" + U64(y) + ", expected " +
                  U64(b.wantY));
        Check(u == b.wantU, "U mean is exact",
              std::string(b.name) + ": meanU=" + U64(u) + ", expected " +
                  U64(b.wantU));
        Check(v == b.wantV, "V mean is exact",
              std::string(b.name) + ": meanV=" + U64(v) + ", expected " +
                  U64(b.wantV));
        Check(got == b.want, "the verdict for this buffer",
              std::string(b.name) + ": " + StateName(got) + ", expected " +
                  StateName(b.want));
    }
}

// ===========================================================================
// GROUP 3 -- THE LATCH, THE ORDERING, THE DRAIN
// ===========================================================================

VkSharedBaseObj<Probe> MakeProbe()
{
    VkSharedBaseObj<Probe> p;
    Probe::Create(p);
    return p;
}

void CaseArmingAndApplicability()
{
    g_currentCase = "latch: arming, applicability, once-per-registration";

    VkSharedBaseObj<Probe> p = MakeProbe();
    Check(p != nullptr, "the probe object is creatable", "Create returned null");
    if (!p) {
        return;
    }
    Check(p->GetRegistrationState(1) == Probe::STATE_NOT_EVALUATED,
          "an unknown registration is NOT_EVALUATED",
          StateName(p->GetRegistrationState(1)));

    p->ArmRegistration(1, Probe::CaptureSite::kReachable);
    Check(p->GetRegistrationState(1) == Probe::STATE_ARMED,
          "a registration whose capture site is reachable arms",
          StateName(p->GetRegistrationState(1)));
    Check(p->NeedsCapture(1), "an armed registration wants a capture", "it did not");

    // A DIRECTLY ENCODABLE REGISTRATION IS NOT EXCUSED FROM PROBING, and is
    // deliberately not NOT_APPLICABLE.
    // Directly-encodable registrations are the BLOCK-LINEAR ones, i.e. the
    // measured damage class, so excusing them is precisely the blindness the
    // probe exists to avoid. What IS NOT_APPLICABLE is a registration the probe
    // genuinely cannot ride: no legal copy out of the image, or a FILTER route
    // that samples rather than copies. Arming one of those would leave it
    // ARMED forever, reporting NOT_EVALUATED -- an observable that cannot
    // fail, which is worse than an honest refusal.
    p->ArmRegistration(2, Probe::CaptureSite::kUnreachable);
    Check(p->GetRegistrationState(2) == Probe::STATE_NOT_APPLICABLE,
          "a registration with no reachable capture site is NOT_APPLICABLE",
          StateName(p->GetRegistrationState(2)));
    Check(!p->NeedsCapture(2), "and it is never asked for a capture", "it was");

    // ONCE PER BUFFER. The second measurement is the one that would arrive if
    // the capture were per frame instead of per registration; it must not
    // move the state or the counters.
    p->ApplyVerdict(1, Q8(100), 0, 0);
    Check(p->GetRegistrationState(1) == Probe::STATE_DAMAGED_CHROMA,
          "the first measurement latches",
          StateName(p->GetRegistrationState(1)));
    Check(!p->NeedsCapture(1), "and the registration stops asking",
          "it still wants a capture");
    p->ApplyVerdict(1, Q8(100), Q8(110), Q8(120));
    Check(p->GetRegistrationState(1) == Probe::STATE_DAMAGED_CHROMA,
          "a SECOND measurement for the same registration is ignored",
          StateName(p->GetRegistrationState(1)));

    Probe::Verdict verdict;
    uint32_t probed = 0, damaged = 0, armedNow = 0;
    p->GetSnapshot(verdict, probed, damaged, armedNow);
    Check(probed == 1,
          "the session counts one PROBED registration, not two measurements",
          "probed=" + U64(probed));
    Check(damaged == 1, "and one damaged", "damaged=" + U64(damaged));
    // Registration 1 was scored and registration 2 was refused honestly, so
    // nothing is still waiting. This is the value the case below shows is
    // NOT automatic.
    Check(armedNow == 0,
          "and nothing is left waiting for a verdict",
          "armed=" + U64(armedNow));
}

// F1/F2. THE ASSERTION THAT MAKES THE PROBE ITSELF FAIL-ABLE.
//
// The verifier's finding, restated so it stays fixed: arming a registration
// whose capture never runs leaves it STATE_ARMED forever, and the session
// then reports probed=0 damaged=0 with a NOT_EVALUATED verdict -- BYTE-FOR-
// BYTE what a session that armed nothing reports, and one reading away from
// "every buffer was checked and every buffer was clean". That is the same
// disguised-inertness shape this whole probe exists to expose, reproduced one
// layer down inside the probe.
//
// It matters most for exactly the class D2 added. A DIRECT registration only
// reaches the capture site through the one-frame staged detour in
// VkVideoEncoder::SetExternalInputFrameWithNode; if that detour stops firing,
// every block-linear buffer arms and none is ever scored, and WITHOUT THIS
// COUNTER the session reports clean. With it, the session reports "N buffers
// were promised a verdict and never got one", which is a failure a caller can
// see and a test can assert.
void CaseArmedButNeverScoredIsCounted()
{
    g_currentCase = "latch: ARMED-and-never-scored is counted, not silent";

    VkSharedBaseObj<Probe> p = MakeProbe();
    if (!p) {
        Check(false, "probe creatable", "null");
        return;
    }

    Probe::Verdict v;
    uint32_t probed = 0, damaged = 0, armed = 0;

    // Three buffers imported and armed. No capture has landed yet -- which is
    // the honest mid-session state, and also exactly what a permanently
    // broken capture site looks like.
    p->ArmRegistration(21, Probe::CaptureSite::kReachable);
    p->ArmRegistration(22, Probe::CaptureSite::kReachable);
    p->ArmRegistration(23, Probe::CaptureSite::kReachable);

    p->GetSnapshot(v, probed, damaged, armed);
    Check(probed == 0 && damaged == 0,
          "with nothing yet scored the two session totals are zero -- which is "
          "ALSO what a session that armed nothing reports, and is why they "
          "cannot carry this question",
          "probed=" + U64(probed) + " damaged=" + U64(damaged));
    Check(v.state == Probe::STATE_NOT_EVALUATED,
          "and the reported verdict is the NOT_EVALUATED default",
          StateName(v.state));
    Check(armed == 3,
          "but THREE registrations are recorded as still waiting for a "
          "verdict, which is the fact the totals above cannot express",
          "armed=" + U64(armed));

    // A capture lands for one of them. The count falls -- it is a live
    // outstanding count, not a session total, and that direction is the
    // useful one: it is what lets 'still non-zero at the end' mean something.
    p->ApplyVerdict(22, Q8(90), Q8(115), Q8(125));
    p->GetSnapshot(v, probed, damaged, armed);
    Check(probed == 1, "scoring one moves the probed total",
          "probed=" + U64(probed));
    Check(armed == 2, "and drops the outstanding count to two",
          "armed=" + U64(armed));

    p->ApplyVerdict(21, Q8(100), 0, 0);
    p->ApplyVerdict(23, 0, 0, 0);
    p->GetSnapshot(v, probed, damaged, armed);
    Check(armed == 0,
          "a session in which every armed buffer was scored ends with nothing "
          "outstanding -- this zero is the discriminating value, and the 3 "
          "above is what a dead capture site would leave here instead",
          "armed=" + U64(armed));
    Check(damaged == 2, "and the damage it found is still reported",
          "damaged=" + U64(damaged));

    // A REGISTRATION REFUSED HONESTLY IS NOT OUTSTANDING. NOT_APPLICABLE is
    // an answer; it must not be counted as a buffer awaiting one, or the
    // number would never reach zero on any session with a FILTER route in it
    // and would stop discriminating.
    p->ArmRegistration(24, Probe::CaptureSite::kUnreachable);
    p->GetSnapshot(v, probed, damaged, armed);
    Check(armed == 0,
          "a NOT_APPLICABLE registration is an ANSWER, not an outstanding "
          "promise, and does not inflate the count",
          "armed=" + U64(armed));

    // AND RETIREMENT DRAINS IT. A consumer that reroutes a buffer and
    // unregisters it must not leave a permanent phantom in this count, or a
    // long session accumulates one and the signal decays to noise.
    p->ArmRegistration(25, Probe::CaptureSite::kReachable);
    p->GetSnapshot(v, probed, damaged, armed);
    Check(armed == 1, "a freshly armed buffer is outstanding",
          "armed=" + U64(armed));
    p->ForgetRegistration(25);
    p->GetSnapshot(v, probed, damaged, armed);
    Check(armed == 0,
          "and retiring it drains the count rather than leaving a phantom",
          "armed=" + U64(armed));
}

// F1. THE DETOUR'S CONTROLLING PREDICATE, PINNED.
//
// This does not execute the detour -- that needs a device, and
// test/encoder-ext-format-encode --content-probe does it on real hardware.
// What it pins is the ONE fact the detour reads, NeedsCapture(), including
// the property that makes the detour affordable at all: it is TRUE for
// exactly one frame per registration.
//
// VkVideoEncoder::SetExternalInputFrameWithNode routes a directly-encodable
// frame down the staged path iff `directlyEncodable && probeStillOwesA-
// Capture`, and probeStillOwesACapture IS NeedsCapture(). So "the detour
// fires once and then stops" and "NeedsCapture goes true then false" are the
// same statement, and the second one is testable on any host.
void CaseDetourBudgetIsOneFramePerRegistration()
{
    g_currentCase = "latch: the staged detour is one frame per registration";

    VkSharedBaseObj<Probe> p = MakeProbe();
    if (!p) {
        Check(false, "probe creatable", "null");
        return;
    }

    // Not armed at all: a caller that never chained the struct must never be
    // detoured. This is the "costs nothing when you did not ask" property.
    Check(!p->NeedsCapture(31),
          "an unregistered buffer never owes a capture, so an un-opted-in "
          "session takes no detour at all",
          "it claimed to");

    p->ArmRegistration(31, Probe::CaptureSite::kReachable);
    Check(p->NeedsCapture(31),
          "frame 1 of an armed DIRECT registration owes a capture, which is "
          "what sends it down the staged path",
          "it did not");

    // The verdict is what the scored capture produces. After it, the budget
    // is spent and every later frame of this buffer goes DIRECT again.
    p->ApplyVerdict(31, Q8(90), Q8(115), Q8(125));
    Check(!p->NeedsCapture(31),
          "and frame 2 does NOT -- the detour is bounded at one frame per "
          "registration, so a 5125-frame session over 5 buffers pays 5 "
          "detours and not 5125",
          "it still owes one");
    Check(p->GetRegistrationState(31) == Probe::STATE_CLEAN,
          "with the verdict latched",
          StateName(p->GetRegistrationState(31)));

    // A registration the probe cannot ride must never detour even once --
    // otherwise every FILTER-routed frame in a session pays a staged copy for
    // a capture that can never happen.
    p->ArmRegistration(32, Probe::CaptureSite::kUnreachable);
    Check(!p->NeedsCapture(32),
          "a registration with no reachable capture site is never detoured",
          "it was");
}

void CaseOldestDamagedFirstAndDrain()
{
    g_currentCase = "latch: oldest-damaged-first, and the drain on retirement";

    VkSharedBaseObj<Probe> p = MakeProbe();
    if (!p) {
        Check(false, "probe creatable", "null");
        return;
    }
    for (uint64_t id = 10; id <= 14; id++) {
        p->ArmRegistration(id, Probe::CaptureSite::kReachable);
    }
    // Scored in this order: clean, chroma-dead, clean, all-dead, clean.
    p->ApplyVerdict(10, Q8(100), Q8(110), Q8(120));
    p->ApplyVerdict(11, Q8(100), 0, 0);
    p->ApplyVerdict(12, Q8(90),  Q8(115), Q8(125));
    p->ApplyVerdict(13, 0, 0, 0);
    p->ApplyVerdict(14, Q8(80),  Q8(100), Q8(100));

    Probe::Verdict v;
    uint32_t probed = 0, damaged = 0, armedNow = 0;
    p->GetSnapshot(v, probed, damaged, armedNow);
    Check(probed == 5, "five registrations reached a verdict",
          "probed=" + U64(probed));
    Check(damaged == 2, "two of them were damaged", "damaged=" + U64(damaged));
    Check(v.registrationId == 11,
          "the report names the OLDEST damaged registration, not the newest -- "
          "a consumer polls between frames and must not lose the older one",
          "reported id " + U64(v.registrationId));
    Check(v.state == Probe::STATE_DAMAGED_CHROMA, "with its own verdict",
          StateName(v.state));
    Check((v.meanY == Q8(100)) && (v.meanU == 0) && (v.meanV == 0),
          "and the arithmetic behind it",
          "Y=" + U64(v.meanY) + " U=" + U64(v.meanU) + " V=" + U64(v.meanV));

    // IDEMPOTENT: reading does not consume. A consumer that reads twice
    // before acting must see the same answer both times.
    Probe::Verdict again;
    p->GetSnapshot(again, probed, damaged, armedNow);
    Check(again.registrationId == 11, "reading the report does not consume it",
          "second read gave id " + U64(again.registrationId));

    // THE DRAIN. Retiring the damaged registration is what surfaces the next
    // one -- this is the mechanism that lets a consumer work through several
    // damaged buffers one poll at a time.
    p->ForgetRegistration(11);
    Check(p->GetRegistrationState(11) == Probe::STATE_NOT_EVALUATED,
          "retirement forgets the registration itself, not just its place in "
          "the damaged order -- the two erasures inside ForgetRegistration "
          "are redundant for the REPORT (either alone drains it) and this is "
          "the assertion that pins both",
          StateName(p->GetRegistrationState(11)));
    p->GetSnapshot(v, probed, damaged, armedNow);
    Check(v.registrationId == 13,
          "retiring the reported registration surfaces the NEXT damaged one",
          "reported id " + U64(v.registrationId));
    Check(v.state == Probe::STATE_DAMAGED_ALL, "with its own verdict",
          StateName(v.state));
    Check(damaged == 2,
          "the SESSION total does not fall when a damaged buffer is retired -- "
          "'two of this session's buffers were damaged' stays true",
          "damaged=" + U64(damaged));

    p->ForgetRegistration(13);
    p->GetSnapshot(v, probed, damaged, armedNow);
    Check(v.state == Probe::STATE_CLEAN,
          "with no damaged registration outstanding the report falls back to "
          "the most recent CLEAN verdict",
          StateName(v.state));
    Check(v.registrationId == 14, "which is the last one scored clean",
          "reported id " + U64(v.registrationId));
}

// ===========================================================================
// GROUP 4 -- THE CARRIER
// ===========================================================================

class Session {
public:
    bool Open()
    {
        if ((CreateVulkanVideoEncoderExt(m_encoder) != VK_SUCCESS) || !m_encoder) {
            std::printf("  ERROR: CreateVulkanVideoEncoderExt failed\n");
            return false;
        }
        if (VkEncInstallNullBackend(m_encoder.get(), &m_backend) != VK_SUCCESS) {
            std::printf("  ERROR: VkEncInstallNullBackend failed\n");
            return false;
        }
        return true;
    }
    VulkanVideoEncoderExt* Get() const { return m_encoder.get(); }

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

VkVideoEncoderStatus FreshStatus()
{
    VkVideoEncoderStatus status = {};
    status.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS;
    return status;
}

VkVideoEncoderImportContentInfo FreshContentInfo()
{
    VkVideoEncoderImportContentInfo info = {};
    info.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_CONTENT_INFO;
    return info;
}

VkVideoEncoderImportGuardInfo FreshGuardInfo()
{
    VkVideoEncoderImportGuardInfo info = {};
    info.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_GUARD_INFO;
    return info;
}

// Values no library path produces, so "the library wrote this" and "the
// library did not" stay distinguishable. probeGeneration is deliberately
// poisoned too: it is the writer proof, and a writer proof that is only
// checked against 0 cannot tell a stamped 0 from an untouched one.
void PoisonContentInfo(VkVideoEncoderImportContentInfo& info)
{
    info.state = VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_DAMAGED_ALL;
    info.resource = 0xDEADBEEFu;
    info.probeGeneration = 0xBADC0DEu;
    info.meanY = 0xAAAAu;
    info.meanU = 0xBBBBu;
    info.meanV = 0xCCCCu;
    info.probedRegistrationCount = 0x1234u;
    info.damagedRegistrationCount = 0x5678u;
}

// C1. Chaining the struct onto a registration is accepted, arms the probe,
// and the echo reports ARMED with the writer proof stamped.
void CaseRegistrationEchoArmsAndReports(Session& s)
{
    g_currentCase = "carrier: chaining onto RegisterImageResource arms it";

    VkVideoEncoderImportContentInfo content = FreshContentInfo();
    PoisonContentInfo(content);
    VkVideoEncoderStatus status = FreshStatus();
    status.pNext = &content;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
        Session::Descriptor(), 0, &resource, &status);

    Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "a chained VkVideoEncoderImportContentInfo is ACCEPTED",
          "status " + U64((uint64_t)reg));
    Check(content.probeGeneration ==
              VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION,
          "probeGeneration is stamped: the writer proof, and it is NON-ZERO "
          "by contract so it cannot decay into 'same as an untouched struct' "
          "the way the guard's requestedCount did when the guard was retired",
          "probeGeneration " + U64(content.probeGeneration));
    Check(content.state == VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_ARMED,
          "the registration echo reports ARMED -- at registration time the "
          "producer has written nothing, so there is no verdict to give yet",
          "state " + U64((uint64_t)content.state));
    Check(content.resource == resource,
          "and names the registration it armed",
          "echo said " + U64(content.resource) + ", id was " + U64(resource));
    Check(content.meanY == 0 && content.meanU == 0 && content.meanV == 0,
          "the echo carries no measurement, and says so with zeros (the "
          "poison was cleared)",
          "Y=" + U64(content.meanY) + " U=" + U64(content.meanU) + " V=" +
              U64(content.meanV));
}

// C1b. THE BLOCK-LINEAR REGISTRATION, which is the shape a probe gated on
// encodeCapable is structurally blind to.
//
// encodeCapable is `encodableFormat && (tiling != VK_IMAGE_TILING_LINEAR) &&
// (usage & VIDEO_ENCODE_SRC)`. Deciding the arm on it means a BLOCK-LINEAR
// NV12 import that also declares
// VIDEO_ENCODE_SRC -- i.e. the exact descriptor a compositor hands over on a
// modifier that carries encode-src -- came out encodeCapable and was latched
// NOT_APPLICABLE: never probed, on a class of buffer that is not
// hypothetical. The periodicity harness measured block-linear GBM buffers
// (modifier 0x0300000000606014) poisoned on 8 of 64 import ordinals, four
// ALL_ZERO and four CHROMA_ZERO.
//
// This case pins the fix at the ONE line that decides it. Against the
// previous library it reports state 1 (NOT_APPLICABLE) and fails; the only
// thing that changes it is dropping tiling/encodeCapable out of the arm
// decision.
void CaseBlockLinearDirectRegistrationArms(Session& s)
{
    g_currentCase = "carrier: a BLOCK-LINEAR encode-src registration ARMS";

    VkVideoEncoderExternalImageDescriptor desc = Session::Descriptor();
    // Block-linear. VK_IMAGE_TILING_OPTIMAL is 0, so this is also what the
    // base descriptor already says -- named explicitly because it is the
    // clause under test and a reader must not have to know that 0 is OPTIMAL.
    desc.tiling = VK_IMAGE_TILING_OPTIMAL;
    // ...and the usage that tips encodeCapable to true. TRANSFER_SRC is
    // present because the probe's capture is a copy out of the image and the
    // new predicate requires exactly that bit; VIDEO_ENCODE_SRC is what makes
    // the registration DIRECT.
    desc.imageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;

    VkVideoEncoderImportContentInfo content = FreshContentInfo();
    PoisonContentInfo(content);
    VkVideoEncoderStatus status = FreshStatus();
    status.pNext = &content;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode reg =
        s.Get()->RegisterImageResource(desc, 0, &resource, &status);

    Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "a block-linear VIDEO_ENCODE_SRC registration is accepted",
          "status " + U64((uint64_t)reg));
    Check(content.state == VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_ARMED,
          "and it ARMS -- the probe's arm decision no longer reads tiling, so "
          "a block-linear import is covered instead of being latched "
          "NOT_APPLICABLE for being directly encodable",
          "state " + U64((uint64_t)content.state));
    Check(content.resource == resource, "and names the registration it armed",
          "echo said " + U64(content.resource) + ", id was " + U64(resource));
}

// C1c. THE NEGATIVE CONTROL, and it is what keeps C1b from degenerating into
// "arm everything". A registration the probe genuinely cannot ride must still
// answer NOT_APPLICABLE, because arming it would leave it ARMED forever and
// reporting NOT_EVALUATED -- an observable that cannot fail.
//
// The case chosen is an import with no TRANSFER_SRC. The capture is a
// vkCmdCopyImage out of the imported image; without that usage bit the copy
// is a VUID violation, so "the probe can ride it" is false as a matter of
// spec, not of policy.
void CaseUncopyableRegistrationStaysNotApplicable(Session& s)
{
    g_currentCase = "carrier: an import with no TRANSFER_SRC is NOT_APPLICABLE";

    VkVideoEncoderExternalImageDescriptor desc = Session::Descriptor();
    desc.tiling = VK_IMAGE_TILING_OPTIMAL;
    desc.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;

    VkVideoEncoderImportContentInfo content = FreshContentInfo();
    PoisonContentInfo(content);
    VkVideoEncoderStatus status = FreshStatus();
    status.pNext = &content;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode reg =
        s.Get()->RegisterImageResource(desc, 0, &resource, &status);
    Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "the registration itself is fine", "status " + U64((uint64_t)reg));
    Check(content.state == VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_APPLICABLE,
          "but the probe refuses it honestly: no TRANSFER_SRC means no legal "
          "copy out of the image, so there is no capture site and "
          "NOT_APPLICABLE is the true answer rather than a permanent ARMED",
          "state " + U64((uint64_t)content.state));
}

// C2. NOT opting in must cost nothing and must leave the channel silent.
// This is the assertion that the default-off path is really off.
// C1d. A FORMAT THE SCORER CANNOT READ IS REFUSED AT REGISTRATION, NOT A
// FRAME LATER.
//
// FOUND BY THE HARDWARE TEST, NOT BY REVIEW. encoder-ext-format-encode
// --content-probe row [2/13] P010 on an RTX A4000 reported, before the fix:
//
//   armEcho=ARMED(gen=1) final=NOT_EVALUATED probed=0 damaged=0 armedOutstanding=0
//
// The arm predicate asked whether a copy could be RECORDED out of the image
// and never whether the bytes it produced could be READ. RecordCapture asked
// the second question on the first frame and latched NOT_APPLICABLE quietly,
// so the session ended reporting the same three zeros a fully-probed, fully-
// clean session reports.
//
// The rule this restores is the one C1c states: an ARMED echo is a PROMISE of
// a verdict. A promise that is withdrawn a frame later is worse than an
// honest refusal, because armedRegistrationCount -- the count that exists to
// catch exactly this -- cannot see a registration that has already been
// downgraded.
void CaseUnscorableFormatIsRefusedAtRegistration(Session& s)
{
    g_currentCase = "carrier: a 10-bit import is NOT_APPLICABLE at REGISTRATION";

    VkVideoEncoderExternalImageDescriptor desc = Session::Descriptor();
    // Everything else is the shape that DOES arm -- block-linear, encode-src,
    // transfer-src -- so the ONLY thing this case varies is the format. Same
    // discipline as C1b/C1c: one clause at a time.
    desc.tiling = VK_IMAGE_TILING_OPTIMAL;
    desc.imageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
    desc.format = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;

    VkVideoEncoderImportContentInfo content = FreshContentInfo();
    PoisonContentInfo(content);
    VkVideoEncoderStatus status = FreshStatus();
    status.pNext = &content;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode reg =
        s.Get()->RegisterImageResource(desc, 0, &resource, &status);
    Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "the registration itself is fine -- the probe's opinion is not a "
          "registration error",
          "status " + U64((uint64_t)reg));
    Check(content.state ==
              VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_APPLICABLE,
          "a 10-bit import is refused AT REGISTRATION, where the caller can "
          "still see it, rather than echoing ARMED and being downgraded on "
          "the first frame",
          "state " + U64((uint64_t)content.state));

    // AND THE 8-BIT CONTROL, so this is not "refuse everything". Without it a
    // predicate hardwired to false would pass the assertion above.
    VkVideoEncoderExternalImageDescriptor ok = desc;
    ok.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    VkVideoEncoderImportContentInfo okContent = FreshContentInfo();
    PoisonContentInfo(okContent);
    VkVideoEncoderStatus okStatus = FreshStatus();
    okStatus.pNext = &okContent;
    VkVideoEncoderResource okResource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    s.Get()->RegisterImageResource(ok, 0, &okResource, &okStatus);
    Check(okContent.state == VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_ARMED,
          "while the same descriptor in 8-bit 420 still ARMS -- the clause "
          "added is the format, not a blanket refusal",
          "state " + U64((uint64_t)okContent.state));
}

void CaseNoOptInLeavesTheChannelSilent()
{
    g_currentCase = "carrier: a session that never chains it reports nothing";

    Session s;
    if (!s.Open()) {
        Check(false, "session", "setup failed");
        return;
    }
    VkVideoEncoderStatus status = FreshStatus();
    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
        Session::Descriptor(), 0, &resource, &status);
    Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "an unchained registration still works", "status " + U64((uint64_t)reg));

    VkVideoEncoderImportContentInfo content = FreshContentInfo();
    PoisonContentInfo(content);
    VkVideoEncoderCompletionInfo info = {};
    info.pNext = &content;
    const VkResult r = s.Get()->GetCompletionInfo(&info);
    Check(r == VK_SUCCESS, "GetCompletionInfo accepts the struct",
          "VkResult " + U64((uint64_t)r));
    Check(content.probeGeneration ==
              VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION,
          "probeGeneration is stamped even with nothing armed -- 'the library "
          "has the observable and has nothing to say' must be readable apart "
          "from 'the library predates the observable'",
          "probeGeneration " + U64(content.probeGeneration));
    Check(content.state == VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED,
          "and the verdict is NOT_EVALUATED: nothing was armed, so nothing "
          "was probed",
          "state " + U64((uint64_t)content.state));
    Check(content.probedRegistrationCount == 0 &&
              content.damagedRegistrationCount == 0,
          "with zero probed and zero damaged (the poison was cleared)",
          "probed " + U64(content.probedRegistrationCount) + " damaged " +
              U64(content.damagedRegistrationCount));
}

// C3. The chain gate, WIDENED not relaxed. Two known link types are now
// accepted, in either order; everything the narrower rule refused it still
// refuses.
void CaseChainGateWidenedNotRelaxed(Session& s)
{
    g_currentCase = "carrier: the widened chain gate still refuses skew";

    // Both known links, content first.
    {
        VkVideoEncoderImportGuardInfo guard = FreshGuardInfo();
        VkVideoEncoderImportContentInfo content = FreshContentInfo();
        content.pNext = &guard;
        VkVideoEncoderStatus status = FreshStatus();
        status.pNext = &content;
        VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
            Session::Descriptor(), 0, &resource, &status);
        Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
              "content-then-guard is accepted",
              "status " + U64((uint64_t)reg));
        Check(content.probeGeneration ==
                  VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION,
              "and both links are written: content", "content not written");
        Check(guard.state == VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_EVALUATED,
              "and both links are written: guard",
              "guard state " + U64((uint64_t)guard.state));
    }
    // Both known links, guard first.
    {
        VkVideoEncoderImportContentInfo content = FreshContentInfo();
        VkVideoEncoderImportGuardInfo guard = FreshGuardInfo();
        guard.pNext = &content;
        VkVideoEncoderStatus status = FreshStatus();
        status.pNext = &guard;
        VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
            Session::Descriptor(), 0, &resource, &status);
        Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
              "guard-then-content is accepted too (order does not matter)",
              "status " + U64((uint64_t)reg));
        Check(content.state == VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_ARMED,
              "and the content link is still armed and written",
              "state " + U64((uint64_t)content.state));
    }
    // A REPEATED content link is version skew and is refused.
    {
        VkVideoEncoderImportContentInfo a = FreshContentInfo();
        VkVideoEncoderImportContentInfo b = FreshContentInfo();
        a.pNext = &b;
        VkVideoEncoderStatus status = FreshStatus();
        status.pNext = &a;
        VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
            Session::Descriptor(), 0, &resource, &status);
        Check(reg == VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN,
              "a REPEATED content link is refused -- two links of one type "
              "mean the caller believes it is getting two different things",
              "status " + U64((uint64_t)reg));
    }
    // An unknown sType behind a known one is still refused.
    {
        VkVideoEncoderImportContentInfo content = FreshContentInfo();
        VkVideoEncoderImportGuardInfo bogus = FreshGuardInfo();
        bogus.sType = (VkVideoEncoderStructureType)0x56450FFF;
        content.pNext = &bogus;
        VkVideoEncoderStatus status = FreshStatus();
        status.pNext = &content;
        VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
            Session::Descriptor(), 0, &resource, &status);
        Check(reg == VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN,
              "an unknown sType chained behind a known one is still refused",
              "status " + U64((uint64_t)reg));
    }
    // A chain hanging off a MIS-STAMPED status is refused AND not written.
    {
        VkVideoEncoderImportContentInfo content = FreshContentInfo();
        PoisonContentInfo(content);
        VkVideoEncoderStatus status = FreshStatus();
        status.sType = (VkVideoEncoderStructureType)0x56450FFE;
        status.pNext = &content;
        VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        const VkVideoEncoderStatusCode reg = s.Get()->RegisterImageResource(
            Session::Descriptor(), 0, &resource, &status);
        Check(reg == VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN,
              "a chain on a mis-stamped status is refused",
              "status " + U64((uint64_t)reg));
        Check(content.probeGeneration == 0xBADC0DEu,
              "and is NOT written through -- writing into a struct whose gate "
              "rejected the request is the exact version-skew failure the "
              "gate exists to prevent",
              "probeGeneration " + U64(content.probeGeneration));
    }
}

// C4. The completion channel: the same struct, the verdict side, and its own
// refusal rules unchanged.
void CaseCompletionChannel(Session& s)
{
    g_currentCase = "carrier: GetCompletionInfo is the verdict channel";

    // The session |s| has armed several registrations by now (C1 and C3), but
    // nothing has been SCORED -- a null-backend session runs no frames. That
    // is exactly the ARMED-but-unscored state, and NOT_EVALUATED is its
    // honest report.
    {
        VkVideoEncoderImportContentInfo content = FreshContentInfo();
        PoisonContentInfo(content);
        VkVideoEncoderCompletionInfo info = {};
        info.pNext = &content;
        const VkResult r = s.Get()->GetCompletionInfo(&info);
        Check(r == VK_SUCCESS, "the completion chain accepts the struct",
              "VkResult " + U64((uint64_t)r));
        Check(content.probeGeneration ==
                  VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION,
              "probeGeneration stamped here too",
              "probeGeneration " + U64(content.probeGeneration));
        Check(content.state ==
                  VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED,
              "armed but never scored reports NOT_EVALUATED, not ARMED: the "
              "verdict channel answers about VERDICTS",
              "state " + U64((uint64_t)content.state));
        Check(content.probedRegistrationCount == 0,
              "and nothing has been probed",
              "probed " + U64(content.probedRegistrationCount));
    }
    // Repeated link, refused, exactly as every other known link on this call.
    {
        VkVideoEncoderImportContentInfo a = FreshContentInfo();
        VkVideoEncoderImportContentInfo b = FreshContentInfo();
        a.pNext = &b;
        VkVideoEncoderCompletionInfo info = {};
        info.pNext = &a;
        Check(s.Get()->GetCompletionInfo(&info) == VK_ERROR_INITIALIZATION_FAILED,
              "a repeated content link on GetCompletionInfo is refused",
              "it was accepted");
    }
    // Alongside the other known links, all of which must still work.
    {
        VkVideoEncoderImportContentInfo content = FreshContentInfo();
        VkVideoEncoderImportGuardInfo guard = FreshGuardInfo();
        VkVideoEncoderDiagnosticInfo diag = {};
        diag.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_DIAGNOSTIC_INFO;
        diag.pNext = &guard;
        guard.pNext = &content;
        VkVideoEncoderCompletionInfo info = {};
        info.pNext = &diag;
        Check(s.Get()->GetCompletionInfo(&info) == VK_SUCCESS,
              "diagnostic + guard + content chained together are all accepted",
              "the chain was refused");
        Check(content.probeGeneration ==
                  VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION,
              "and the content link is written", "it was not");
    }
}

// C5. THE WHOLE EXT-SIDE PATH, END TO END, minus the GPU readback: register
// with the struct chained, feed the probe the measurement a poisoned import
// produces, and read the verdict back off GetCompletionInfo -- then retire the
// buffer, which is the consumer's reaction, and watch the report drain.
//
// The measurement is injected through VkEncInjectImportContentMeasurement
// (the internal seam) because the capture that would otherwise produce it
// needs a dma-buf import, a staging command buffer and a fence. What is
// injected is a MEASUREMENT and never a verdict: every state asserted below
// is the production predicate's own answer.
void CaseVerdictReachesTheCompletionChannel()
{
    g_currentCase = "carrier: a damaged verdict reaches GetCompletionInfo";

    Session s;
    if (!s.Open()) {
        Check(false, "session", "setup failed");
        return;
    }
    // Two armed registrations, so the ordering and the drain are visible
    // across the ext boundary and not only inside the probe object.
    VkVideoEncoderResource first = VK_VIDEO_ENCODER_RESOURCE_NULL;
    VkVideoEncoderResource second = VK_VIDEO_ENCODER_RESOURCE_NULL;
    for (VkVideoEncoderResource* out : { &first, &second }) {
        VkVideoEncoderImportContentInfo content = FreshContentInfo();
        VkVideoEncoderStatus status = FreshStatus();
        status.pNext = &content;
        s.Get()->RegisterImageResource(Session::Descriptor(), 0, out, &status);
        Check(content.state == VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_ARMED,
              "armed", "state " + U64((uint64_t)content.state));
    }

    // CHROMA_ZERO on the first buffer, ALL_ZERO on the second -- the two
    // damage modes the driver produces, in that order.
    Check(VkEncInjectImportContentMeasurement(s.Get(), first, Q8(100), 0, 0) ==
              VK_SUCCESS,
          "the measurement seam reaches the armed probe", "it did not");
    Check(VkEncInjectImportContentMeasurement(s.Get(), second, 0, 0, 0) ==
              VK_SUCCESS,
          "and the second one too", "it did not");

    VkVideoEncoderImportContentInfo content = FreshContentInfo();
    PoisonContentInfo(content);
    VkVideoEncoderCompletionInfo info = {};
    info.pNext = &content;
    Check(s.Get()->GetCompletionInfo(&info) == VK_SUCCESS,
          "the completion call succeeds", "it did not");
    Check(content.state ==
              VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_DAMAGED_CHROMA,
          "the CHROMA_ZERO verdict crosses the ext boundary intact",
          "state " + U64((uint64_t)content.state));
    Check(content.resource == first,
          "naming the registration a consumer has to reroute -- WHICH BUFFER "
          "is the entire actionable content of this channel",
          "reported " + U64(content.resource) + ", expected " + U64(first));
    Check(content.meanY == Q8(100) && content.meanU == 0 && content.meanV == 0,
          "with the arithmetic behind it",
          "Y=" + U64(content.meanY) + " U=" + U64(content.meanU) + " V=" +
              U64(content.meanV));
    Check(content.probedRegistrationCount == 2 &&
              content.damagedRegistrationCount == 2,
          "and the session totals",
          "probed " + U64(content.probedRegistrationCount) + " damaged " +
              U64(content.damagedRegistrationCount));

    // THE CONSUMER'S REACTION, and the assertion that it is wired: retiring
    // the buffer named above must surface the NEXT damaged one. Without
    // UnregisterImageResource reaching the probe, this call reports |first|
    // forever and a consumer can never work past the first damaged buffer.
    Check(s.Get()->UnregisterImageResource(first) ==
              VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "the damaged registration retires", "unregister refused");
    content = FreshContentInfo();
    info = VkVideoEncoderCompletionInfo{};
    info.pNext = &content;
    s.Get()->GetCompletionInfo(&info);
    Check(content.state == VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_DAMAGED_ALL,
          "retiring it surfaces the NEXT damaged buffer, with ITS verdict",
          "state " + U64((uint64_t)content.state));
    Check(content.resource == second, "and its id",
          "reported " + U64(content.resource) + ", expected " + U64(second));

    Check(s.Get()->UnregisterImageResource(second) ==
              VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "the second one retires too", "unregister refused");
    content = FreshContentInfo();
    info = VkVideoEncoderCompletionInfo{};
    info.pNext = &content;
    s.Get()->GetCompletionInfo(&info);
    Check(content.state ==
              VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED,
          "with every damaged buffer retired and none ever scored clean, the "
          "channel goes quiet again",
          "state " + U64((uint64_t)content.state));
    Check(content.damagedRegistrationCount == 2,
          "but the SESSION total still remembers both -- 'two of this "
          "session's buffers were damaged' does not stop being true when they "
          "are retired",
          "damaged " + U64(content.damagedRegistrationCount));
}

// C6. The once-per-registration rule holds ACROSS the ext boundary too, and
// a measurement for a retired registration is dropped rather than resurrecting
// a verdict the consumer has already acted on.
void CaseMeasurementForARetiredRegistrationIsDropped()
{
    g_currentCase = "carrier: a measurement for a retired registration is dropped";

    Session s;
    if (!s.Open()) {
        Check(false, "session", "setup failed");
        return;
    }
    VkVideoEncoderImportContentInfo armed = FreshContentInfo();
    VkVideoEncoderStatus status = FreshStatus();
    status.pNext = &armed;
    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    s.Get()->RegisterImageResource(Session::Descriptor(), 0, &resource, &status);
    s.Get()->UnregisterImageResource(resource);

    // A capture recorded before the retirement can still land after it: the
    // fence wait that scores it is on another thread. It must not resurrect a
    // verdict for a buffer the consumer has already finished with.
    VkEncInjectImportContentMeasurement(s.Get(), resource, Q8(100), 0, 0);

    VkVideoEncoderImportContentInfo content = FreshContentInfo();
    PoisonContentInfo(content);
    VkVideoEncoderCompletionInfo info = {};
    info.pNext = &content;
    s.Get()->GetCompletionInfo(&info);
    Check(content.state ==
              VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED,
          "a late measurement for a retired registration is dropped",
          "state " + U64((uint64_t)content.state));
    Check(content.probedRegistrationCount == 0,
          "and does not move the session totals",
          "probed " + U64(content.probedRegistrationCount));
}

}  // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    std::printf("Encoder-ext dma-buf import CONTENT probe\n");
    std::printf("----------------------------------------\n");

    CasePredicateQuadrants();
    CasePredicateThresholdBoundary();
    CaseScorerOnSyntheticBuffers();
    CaseArmingAndApplicability();
    CaseArmedButNeverScoredIsCounted();
    CaseDetourBudgetIsOneFramePerRegistration();
    CaseOldestDamagedFirstAndDrain();

    Session session;
    if (!session.Open()) {
        std::printf("RESULT: COULD-NOT-RUN (session setup failed)\n");
        return 2;
    }
    CaseRegistrationEchoArmsAndReports(session);
    CaseBlockLinearDirectRegistrationArms(session);
    CaseUncopyableRegistrationStaysNotApplicable(session);
    CaseUnscorableFormatIsRefusedAtRegistration(session);
    CaseChainGateWidenedNotRelaxed(session);
    CaseCompletionChannel(session);
    CaseNoOptInLeavesTheChannelSilent();
    CaseVerdictReachesTheCompletionChannel();
    CaseMeasurementForARetiredRegistrationIsDropped();

    std::printf("----------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
