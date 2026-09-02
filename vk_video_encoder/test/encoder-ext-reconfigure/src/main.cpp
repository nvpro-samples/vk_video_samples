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
 * Device-free coverage for WHAT Reconfigure() CARRIES, WHAT IT REFUSES, AND
 * WHAT IT STILL IGNORES ON PURPOSE.
 *
 * THE CONTRACT UNDER TEST, stated by Reconfigure itself: "Everything this
 * call cannot carry is REFUSED rather than discarded. Returning VK_SUCCESS
 * for a field that was ignored is how a stream ends up encoded one way and
 * described another." VkVideoEncoderConfig carries 44 members, and they
 * sort four ways:
 *
 *   GATES (2) -- sType and pNext.
 *
 *   APPLIED (9) -- averageBitrate, maxBitrate, frameRateNum, frameRateDen,
 *   constQpI/P/B, and minQp/maxQp. Each is asserted below as a CHANGE the
 *   session then holds, never as a return code: a VK_SUCCESS that altered
 *   nothing is the defect this file exists to pin, so asserting the return
 *   code alone would assert nothing at all.
 *
 *   REFUSED ON A CHANGE (21) -- the sequence-header and input-routing
 *   fields, the input extent, vbvBufferSize, the GOP structure and the
 *   quality controls. Every one is settled at InitializeExt and can reach
 *   the bitstream, so accepting a change is the misdescription the contract
 *   forbids.
 *
 *   NEITHER (12) -- the seven session-creation-only members and the five
 *   diagnostic ones. None can change an encoded bit, so none can
 *   misdescribe the stream. They stay accepted, and the group below is the
 *   control that says so: a fix that refused these too would break a caller
 *   that builds a fresh minimal config for the reconfigure rather than
 *   copying its stored one, and would buy nothing.
 *
 * THE RECORD IS ALSO UNDER TEST, and it is the one thing here that is not
 * about a config field at all. Reconfigure keeps a copy of the
 * configuration in force, both to compare a later call against and as the
 * session statement of what it is running. Two members are COERCED on the
 * way in and a third can be DROPPED -- a zero maxBitrate becomes the
 * average bitrate, a zero frameRateDen becomes 1, and a zero frameRateNum
 * leaves the frame rate untouched -- so a record of what was PASSED is not
 * a record of what is in force. The cases below read the record and the
 * live rate-control layer through two separate seams and assert they agree,
 * which is the only form of that claim that is not just inspection.
 *
 * WHY THE COMPARISON IS AGAINST THE INIT CONFIG rather than an absolute
 * refusal: a caller hands Reconfigure a WHOLE config, not a minimal one.
 * Two shapes of that are known: retaining a baseline and editing only its
 * rate fields in place, and copying the init config and editing a couple of
 * fields -- the second is what the sibling format-matrix test does. Refusing
 * on a CHANGE rather than on presence is what keeps both shapes working, and
 * the first group below is the control that proves an unchanged config is
 * still accepted.
 *
 * WHY THIS RUNS WITHOUT A GPU. Reconfigure gates on
 * (m_initialized && m_encoder) before it compares anything.
 * VkEncInstallNullBackend sets the first; VkEncPushCapture installs the
 * second device-free. Both are required -- with only the null backend every
 * call here would stop at that gate and answer NOT_PERMITTED, and the file
 * would pass for the wrong reason. The session is never InitializeExt-ed, so
 * m_initConfig holds a default-constructed VkVideoEncoderConfig and a
 * default-constructed config in this file agrees with it on every compared
 * field. That is what makes each case single-variable: the only thing that
 * differs is the one field the case sets.
 *
 * CTest semantics, matching the sibling library tests: 0 means every
 * assertion held, 1 means an assertion failed, 2 means the session could not
 * be stood up at all -- deliberately a FAILURE and not a skip. There is no
 * GPU, driver or display dependence here.
 */

#include "vulkan_video_encoder_ext_internal.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int         g_checks      = 0;
int         g_failures    = 0;
const char* g_currentCase = "";

std::string I64(int64_t v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%lld", (long long)v);
    return b;
}

void Check(bool ok, const char* what, const std::string& detail)
{
    g_checks++;
    if (ok) {
        std::printf("  ok   %s\n", what);
    } else {
        g_failures++;
        std::printf("  FAIL %s [%s] (%s)\n", what, g_currentCase,
                    detail.c_str());
    }
}

// A null-backend session that Reconfigure can actually reach. See the file
// comment: the capture push is what installs m_encoder, and without it every
// assertion below would be answered by the gate instead of by the field
// comparison under test.
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
        if (VkEncPushCapture(m_encoder.get(), 1, VK_SUCCESS) != VK_SUCCESS) {
            std::printf("  ERROR: VkEncPushCapture failed\n");
            return false;
        }
        return true;
    }

    VulkanVideoEncoderExt* Get() const { return m_encoder.get(); }

private:
    VkEncNullBackendState                  m_backend{};
    VkSharedBaseObj<VulkanVideoEncoderExt> m_encoder;
};

// Agrees with the never-initialized m_initConfig on every compared field.
// averageBitrate must be non-zero or RequestRateControlUpdate refuses the
// call for a reason that has nothing to do with what is being tested.
VkVideoEncoderConfig Base()
{
    VkVideoEncoderConfig cfg{};
    cfg.averageBitrate = 5000000;
    // NEGATIVE is how this API spells "this config names no quantizer", and
    // it is the spelling InitializeExt reads too. ZERO would NOT mean that:
    // 0 is a valid lossless QP, so a zero-initialized config asks for QP 0
    // rather than asking for nothing. A default-constructed config therefore
    // does NOT mean "leave the quantizers alone", which is why this says so
    // explicitly.
    cfg.constQpI = -1;
    cfg.constQpP = -1;
    cfg.constQpB = -1;
    return cfg;
}

void ExpectRefused(Session& s, const char* field, VkVideoEncoderConfig cfg)
{
    const VkResult r = s.Get()->Reconfigure(cfg);
    Check(r == VK_ERROR_INITIALIZATION_FAILED, field,
          "Reconfigure returned " + I64((int64_t)r) +
              ", want VK_ERROR_INITIALIZATION_FAILED (-3)");
}

void ExpectAccepted(Session& s, const char* what, VkVideoEncoderConfig cfg)
{
    const VkResult r = s.Get()->Reconfigure(cfg);
    Check(r == VK_SUCCESS, what,
          "Reconfigure returned " + I64((int64_t)r) + ", want VK_SUCCESS (0)");
}

//=============================================================================
// 1. THE CONTROL. An unchanged config is still accepted.
//=============================================================================
//
// Without this, a change that refused everything would look like a fix. Both
// in-tree callers pass a whole config that differs from the init config in
// the rate fields alone, so this is the shape that actually ships.
void CaseUnchangedConfigIsAccepted(Session& s)
{
    g_currentCase = "an unchanged config is accepted";
    ExpectAccepted(s, "an unchanged config is accepted", Base());
}

// The four fields the call exists to carry. Changing them must stay accepted.
void CaseCarriedRateFieldsAreAccepted(Session& s)
{
    g_currentCase = "the four carried rate fields are accepted";
    VkVideoEncoderConfig cfg = Base();
    cfg.averageBitrate = 9000000;
    cfg.maxBitrate     = 12000000;
    cfg.frameRateNum   = 60;
    cfg.frameRateDen   = 1;
    ExpectAccepted(s, "a rate-control change is carried", cfg);
}

//=============================================================================
// 2. THE RED LEG. Encoding-affecting fields are REFUSED, not ignored.
//=============================================================================
//
// Every case here answered VK_SUCCESS before the fix while the encoder went
// on using the value it was initialized with.
void CaseEncodingAffectingFieldsAreRefused(Session& s)
{
    g_currentCase = "an encoding-affecting change is refused";

    // The input extent: what the session conversion was sized around.
    { VkVideoEncoderConfig c = Base(); c.inputWidth  = 1920;
      ExpectRefused(s, "inputWidth is refused", c); }
    { VkVideoEncoderConfig c = Base(); c.inputHeight = 1080;
      ExpectRefused(s, "inputHeight is refused", c); }

    // The rate-control lever this call still does not carry. NOT for the
    // reason minQp and maxQp were once refused beside it: the codec fill
    // can be re-invoked, and for the clamps it is. vbvBufferSize is
    // refused because it is not an independent input to that fill -- what
    // reaches the command is a duration in milliseconds computed from it
    // AND from a vbvInitialDelay derived from the OLD buffer size in a
    // finalize step this call does not re-run, both divided by a config
    // bitrate a mid-stream bitrate change deliberately leaves alone.
    { VkVideoEncoderConfig c = Base(); c.vbvBufferSize = 4000000;
      ExpectRefused(s, "vbvBufferSize is refused", c); }

    // The GOP structure the session sequences to.
    { VkVideoEncoderConfig c = Base(); c.gopLength = 60;
      ExpectRefused(s, "gopLength is refused", c); }
    { VkVideoEncoderConfig c = Base(); c.consecutiveBFrames = 2;
      ExpectRefused(s, "consecutiveBFrames is refused", c); }
    { VkVideoEncoderConfig c = Base(); c.idrPeriod = 120;
      ExpectRefused(s, "idrPeriod is refused", c); }
    { VkVideoEncoderConfig c = Base(); c.closedGop = VK_TRUE;
      ExpectRefused(s, "closedGop is refused", c); }

    // The encode-quality controls.
    { VkVideoEncoderConfig c = Base(); c.qualityLevel = 2;
      ExpectRefused(s, "qualityLevel is refused", c); }
    { VkVideoEncoderConfig c = Base();
      c.tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR;
      ExpectRefused(s, "tuningMode is refused", c); }
}


//=============================================================================
// 2b. THE APPLIED GROUP. constQp is CHANGED, not merely accepted.
//=============================================================================
//
// The sharpest case in the whole config, and the one field here that is
// APPLIED rather than refused. On a DISABLED (constant-QP) session the
// constant-QP defaults are the only session-level rate lever there is: the
// per-layer bitrates a rate-control command carries are dropped outright on
// such a session, because that mode commands layerCount 0. So before this
// change a constant-QP caller had NO working session-level control at all,
// and Reconfigure answered VK_SUCCESS to every attempt.
//
// WHAT THIS ASSERTS, AND WHAT IT DOES NOT. It reads back the value the
// session now holds after folding the update -- the exact member
// EncodeFrameCommon copies into the next frame it processes,
// unconditionally, on the encoder thread. A VK_SUCCESS that changed nothing
// is the defect this file exists to pin, so asserting the return code alone
// would assert nothing. What it CANNOT assert is that the driver then
// honours the value in the produced bitstream: that needs a GPU and a decode
// comparison, and is deliberately not claimed here.
//
// IT ALSO CANNOT SEE WHICH FRAME THE VALUE LANDS ON, and that is a
// structural limit of the seam rather than a gap in this case. The seam
// forces a fold and then reads m_encoderConfig->constQp; the ordering that
// decides the landing frame is between that fold and the per-frame copy at
// the top of EncodeFrameCommon, which no device-free session reaches -- the
// null backend stubs out EncodeFrame, the frame-info pool and the image
// resources EncodeFrameCommon asserts on. A build with the fold moved back
// after the copy reads green here and encodes the change one frame late.
// The witness for that is a device run: encode a DISABLED-mode stream,
// Reconfigure before frame N, and read SliceQPy back out of frame N.
void CaseConstQpIsAppliedNotIgnored(Session& s)
{
    g_currentCase = "constQp is applied to the session";

    int32_t qpI = -1, qpP = -1, qpB = -1;

    // The control FIRST, so the observable is calibrated against a known
    // no-change input before it is trusted on a changing one: a config that
    // names no QP at all must leave the session exactly as it was.
    ExpectAccepted(s, "a config naming no constQp is accepted", Base());
    Check(VkEncApplyAndGetSessionConstQp(s.Get(), &qpI, &qpP, &qpB) ==
              VK_SUCCESS,
          "the session constQp is readable", "seam refused");
    Check((qpI == 0) && (qpP == 0) && (qpB == 0),
          "a config naming no constQp leaves the session untouched",
          "got " + I64(qpI) + "/" + I64(qpP) + "/" + I64(qpB) + ", want 0/0/0");

    // The red leg: a named QP must actually reach the session.
    VkVideoEncoderConfig c = Base();
    c.constQpI = 33;
    c.constQpP = 34;
    c.constQpB = 35;
    ExpectAccepted(s, "a constQp change is accepted", c);
    Check(VkEncApplyAndGetSessionConstQp(s.Get(), &qpI, &qpP, &qpB) ==
              VK_SUCCESS,
          "the session constQp is readable after the update", "seam refused");
    Check((qpI == 33) && (qpP == 34) && (qpB == 35),
          "the constQp change reached the session",
          "got " + I64(qpI) + "/" + I64(qpP) + "/" + I64(qpB) +
              ", want 33/34/35");

    // A partial declaration touches only what it names. -1 means "not
    // specified" at InitializeExt and must keep meaning that here, or a
    // caller that copies a config built for a non-CQP session -- where the
    // three are left at -1 -- would have its QPs silently rewritten.
    VkVideoEncoderConfig p = Base();
    p.constQpI = 40;   // named
    p.constQpP = -1;   // not named -- must survive untouched
    p.constQpB = -1;   // not named -- must survive untouched
    ExpectAccepted(s, "a partial constQp change is accepted", p);
    Check(VkEncApplyAndGetSessionConstQp(s.Get(), &qpI, &qpP, &qpB) ==
              VK_SUCCESS,
          "the session constQp is readable after the partial update",
          "seam refused");
    Check((qpI == 40) && (qpP == 34) && (qpB == 35),
          "an unnamed constQp member is left alone",
          "got " + I64(qpI) + "/" + I64(qpP) + "/" + I64(qpB) +
              ", want 40/34/35");
}

//=============================================================================
//=============================================================================
// 2c. THE RECORD AGREES WITH WHAT IS IN FORCE.
//=============================================================================
//
// Reconfigure keeps a copy of the configuration so a later call compares
// against what is actually running. It was updated from the RAW config, and
// the raw config is not always what the session ends up using: a zero
// maxBitrate is coerced to the average bitrate, a zero frameRateDen becomes
// 1, and a zero frameRateNum leaves the frame rate alone entirely. The
// record said 0 in each case while the session ran on something else.
//
// HOW THIS IS ASSERTED. Not by re-deriving the coercion in the test -- that
// would only check the test against itself. Both halves are read back
// through separate seams: VkEncGetRecordedConfig for the record, and
// VkEncApplyAndGetRateControl for the LIVE rate-control layer, which is the
// struct HandleCtrlCmd copies verbatim into the next control command. The
// assertion is that the two agree.
//
// WHAT THIS DOES NOT CLAIM. Nothing compared by Reconfigure reads these
// members, so no refusal decision moves either way -- see the note in
// Reconfigure. This is a truthfulness fix to the record, not a behaviour
// change, and the cases are written to show exactly that.
void CaseRecordAgreesWithWhatIsInForce()
{
    Session s;
    if (!s.Open()) {
        Check(false, "record-vs-in-force session opens", "session setup");
        return;
    }
    VkEncRateControlObservation live{};
    VkVideoEncoderConfig         rec{};

    auto Read = [&](const char* what) -> bool {
        const bool ok =
            (VkEncApplyAndGetRateControl(s.Get(), &live) == VK_SUCCESS) &&
            (VkEncGetRecordedConfig(s.Get(), &rec) == VK_SUCCESS);
        Check(ok, what, "an observation seam refused");
        return ok;
    };

    // THE CONTROL, FIRST. A config where nothing is coerced: every value
    // passed is the value applied. The record and the live layer must agree
    // here too, and that agreement is the point: an
    // observable that only ever reported "they agree" would prove nothing
    // below, and one that reported "they differ" on THIS input would be
    // measuring something other than the coercion.
    g_currentCase = "an uncoerced config: record and session already agree";
    {
        VkVideoEncoderConfig c = Base();
        c.averageBitrate = 9000000;
        c.maxBitrate     = 12000000;
        c.frameRateNum   = 60;
        c.frameRateDen   = 1;
        ExpectAccepted(s, "an uncoerced rate change is accepted", c);
        if (Read("the record and the live layer are both readable")) {
            Check((live.layerMaxBitrate == 12000000) &&
                      (live.layerFrameRateNumerator == 60) &&
                      (live.layerFrameRateDenominator == 1),
                  "the uncoerced values are in force",
                  "live " + I64((int64_t)live.layerMaxBitrate) + " " +
                      I64(live.layerFrameRateNumerator) + "/" +
                      I64(live.layerFrameRateDenominator));
            Check((rec.maxBitrate == live.layerMaxBitrate) &&
                      (rec.frameRateNum == live.layerFrameRateNumerator) &&
                      (rec.frameRateDen == live.layerFrameRateDenominator),
                  "the record agrees with the session on an uncoerced config",
                  "record " + I64(rec.maxBitrate) + " " +
                      I64(rec.frameRateNum) + "/" + I64(rec.frameRateDen));
        }
    }

    // RED LEG 1. maxBitrate 0 means "track averageBitrate". The session
    // runs at 7000000, so the record must not say 0.
    g_currentCase = "a coerced maxBitrate is recorded as what is in force";
    {
        VkVideoEncoderConfig c = Base();
        c.averageBitrate = 7000000;
        c.maxBitrate     = 0;
        c.frameRateNum   = 60;
        c.frameRateDen   = 1;
        ExpectAccepted(s, "a zero maxBitrate is accepted", c);
        if (Read("the seams are readable after the coerced maxBitrate")) {
            Check(live.layerMaxBitrate == 7000000,
                  "a zero maxBitrate runs at the average bitrate",
                  "live max " + I64((int64_t)live.layerMaxBitrate));
            Check(rec.maxBitrate == live.layerMaxBitrate,
                  "the record holds the coerced maxBitrate, not the zero",
                  "record " + I64(rec.maxBitrate) + ", live " +
                      I64((int64_t)live.layerMaxBitrate));
        }
    }

    // RED LEG 2. frameRateNum 0 leaves the frame rate alone -- BOTH halves
    // of it. The session is still at 60/1, so the record must not say 0/99.
    g_currentCase = "a dropped frame rate is not recorded as passed";
    {
        VkVideoEncoderConfig c = Base();
        c.averageBitrate = 7000000;
        c.maxBitrate     = 7000000;
        c.frameRateNum   = 0;
        c.frameRateDen   = 99;
        ExpectAccepted(s, "a zero frameRateNum is accepted", c);
        if (Read("the seams are readable after the dropped frame rate")) {
            Check((live.layerFrameRateNumerator == 60) &&
                      (live.layerFrameRateDenominator == 1),
                  "a zero frameRateNum leaves the frame rate in force",
                  "live " + I64(live.layerFrameRateNumerator) + "/" +
                      I64(live.layerFrameRateDenominator));
            Check((rec.frameRateNum == live.layerFrameRateNumerator) &&
                      (rec.frameRateDen == live.layerFrameRateDenominator),
                  "the record keeps the frame rate that is in force",
                  "record " + I64(rec.frameRateNum) + "/" +
                      I64(rec.frameRateDen) + ", live " +
                      I64(live.layerFrameRateNumerator) + "/" +
                      I64(live.layerFrameRateDenominator));
        }
    }

    // RED LEG 3. frameRateDen 0 beside a non-zero numerator becomes 1. The
    // session runs 24/1, so the record must not say 24/0, which is not a frame
    // rate at all.
    g_currentCase = "a coerced frameRateDen is recorded as what is in force";
    {
        VkVideoEncoderConfig c = Base();
        c.averageBitrate = 7000000;
        c.maxBitrate     = 7000000;
        c.frameRateNum   = 24;
        c.frameRateDen   = 0;
        ExpectAccepted(s, "a zero frameRateDen is accepted", c);
        if (Read("the seams are readable after the coerced frameRateDen")) {
            Check((live.layerFrameRateNumerator == 24) &&
                      (live.layerFrameRateDenominator == 1),
                  "a zero frameRateDen runs as 1",
                  "live " + I64(live.layerFrameRateNumerator) + "/" +
                      I64(live.layerFrameRateDenominator));
            Check(rec.frameRateDen == live.layerFrameRateDenominator,
                  "the record holds the coerced denominator, not the zero",
                  "record den " + I64(rec.frameRateDen));
        }
    }
}

//=============================================================================
// 2d. THE QP CLAMPS ARE APPLIED, not refused and not ignored.
//=============================================================================
//
// minQp and maxQp were refused on the grounds that they reach the driver
// only through the codec-specific rate-control structs, which are filled
// once at codec-init. Half of that is true and half is not: the fill,
// EncoderConfig::GetRateControlParameters, is a pure function of config
// state, and CodecHandleRateControlCmd chains its output onto EVERY
// ENCODE_RATE_CONTROL command. So re-invoking it on the encoder thread puts
// a new clamp on the very command this call already causes.
//
// WHAT IS ASSERTED, AND AT WHAT DEPTH. Not the return code, and not the
// config field either -- a config field that moved while the codec struct
// did not would be a clamp that reaches nothing. The assertion is on
// resolvedUseMinQp / resolvedMinQpI, read out of the codec rate-control
// layer struct itself, which is the far end of the chain the library owns.
// Whether the driver then honours it needs a GPU and is not claimed.
void CaseQpClampIsAppliedNotIgnored()
{
    Session s;
    if (!s.Open()) {
        Check(false, "QP-clamp session opens", "session setup");
        return;
    }
    VkEncRateControlObservation live{};
    auto Read = [&](const char* what) -> bool {
        const bool ok =
            (VkEncApplyAndGetRateControl(s.Get(), &live) == VK_SUCCESS);
        Check(ok, what, "the observation seam refused");
        return ok;
    };

    // THE CONTROL, FIRST, so the observable is calibrated on a known
    // no-change input before it is trusted on a changing one. A config
    // naming no clamp must leave the resolved struct reporting no clamp AND
    // must not re-invoke the codec fill at all.
    g_currentCase = "a config naming no QP clamp changes nothing";
    VkVideoEncoderConfig base = Base();
    base.averageBitrate = 8000000;
    base.maxBitrate     = 10000000;
    base.frameRateNum   = 50;
    base.frameRateDen   = 1;
    ExpectAccepted(s, "a config naming no QP clamp is accepted", base);
    if (Read("the rate-control observation is readable")) {
        Check((live.configMinQpSet == 0) && (live.configMaxQpSet == 0),
              "no clamp is requested on the session",
              "set flags " + I64(live.configMinQpSet) + "/" +
                  I64(live.configMaxQpSet));
        Check((live.resolvedUseMinQp == 0) && (live.resolvedUseMaxQp == 0),
              "the resolved codec struct carries no clamp",
              "use flags " + I64(live.resolvedUseMinQp) + "/" +
                  I64(live.resolvedUseMaxQp));
        Check(live.codecRefreshCount == 0,
              "no clamp change means no codec refresh",
              "refresh count " + I64(live.codecRefreshCount));
    }

    // THE RED LEG. A named clamp must reach the codec struct a control
    // command is built from.
    //
    // THE FRAME RATE IS DELIBERATELY LEFT UNNAMED HERE. The codec fill
    // rewrites the live layer frame rate from the CONFIG, which still holds
    // the value the session was built with -- and with frameRateNum 0 the
    // update carries no frame rate to repair it afterwards. So the frame
    // rate surviving this call at 50/1 is the assertion that the refresh
    // did not reach past what it is for.
    g_currentCase = "a QP clamp change reaches the codec rate-control struct";
    VkVideoEncoderConfig clamped = base;
    clamped.frameRateNum = 0;
    clamped.frameRateDen = 0;
    clamped.minQp        = 20;
    clamped.maxQp        = 44;
    ExpectAccepted(s, "a QP clamp change is accepted", clamped);
    if (Read("the observation is readable after the clamp change")) {
        Check((live.configMinQp == 20) && (live.configMinQpSet == 1) &&
                  (live.configMaxQp == 44) && (live.configMaxQpSet == 1),
              "the clamp request reached the session config",
              "config " + I64(live.configMinQp) + "/" +
                  I64(live.configMaxQp));
        Check((live.resolvedUseMinQp == 1) && (live.resolvedMinQpI == 20),
              "the minQp clamp resolved into the codec struct",
              "use " + I64(live.resolvedUseMinQp) + ", qpI " +
                  I64(live.resolvedMinQpI) + ", want 1 and 20");
        Check((live.resolvedUseMaxQp == 1) && (live.resolvedMaxQpI == 44),
              "the maxQp clamp resolved into the codec struct",
              "use " + I64(live.resolvedUseMaxQp) + ", qpI " +
                  I64(live.resolvedMaxQpI) + ", want 1 and 44");
        Check(live.codecRefreshCount == 1,
              "the codec fill was re-invoked exactly once",
              "refresh count " + I64(live.codecRefreshCount));
        Check((live.layerFrameRateNumerator == 50) &&
                  (live.layerFrameRateDenominator == 1),
              "the refresh did not revert the live frame rate",
              "live " + I64(live.layerFrameRateNumerator) + "/" +
                  I64(live.layerFrameRateDenominator) + ", want 50/1");
        Check((live.layerAverageBitrate == 8000000) &&
                  (live.layerMaxBitrate == 10000000),
              "the refresh did not revert the live bitrates",
              "live " + I64((int64_t)live.layerAverageBitrate) + "/" +
                  I64((int64_t)live.layerMaxBitrate));
    }

    // A CLAMP THAT CAN BE SET MUST BE CLEARABLE. Zero means "no clamp" at
    // InitializeExt, and it has to keep meaning that here or these two
    // fields would carry two different contracts.
    g_currentCase = "a zero QP clamp clears the clamp";
    VkVideoEncoderConfig cleared = clamped;
    cleared.minQp = 0;
    ExpectAccepted(s, "clearing the minQp clamp is accepted", cleared);
    if (Read("the observation is readable after the clear")) {
        Check((live.configMinQpSet == 0) && (live.resolvedUseMinQp == 0),
              "the cleared clamp no longer reaches the codec struct",
              "set " + I64(live.configMinQpSet) + ", use " +
                  I64(live.resolvedUseMinQp));
        Check((live.resolvedUseMaxQp == 1) && (live.resolvedMaxQpI == 44),
              "clearing one clamp leaves the other alone",
              "use " + I64(live.resolvedUseMaxQp) + ", qpI " +
                  I64(live.resolvedMaxQpI));
    }

    // THE SCOPE CONTROL. The refresh is not a general recompute: an update
    // that changes no clamp must not run it. Without this a fix that
    // re-invoked the fill on every rate change would look identical.
    g_currentCase = "a bitrate-only change does not re-invoke the codec fill";
    const uint32_t refreshBefore = live.codecRefreshCount;
    VkVideoEncoderConfig rateOnly = cleared;
    rateOnly.averageBitrate = 6000000;
    ExpectAccepted(s, "a bitrate-only change is accepted", rateOnly);
    if (Read("the observation is readable after the bitrate-only change")) {
        Check(live.codecRefreshCount == refreshBefore,
              "no clamp change means no codec refresh",
              "count " + I64(live.codecRefreshCount) + ", want " +
                  I64(refreshBefore));
        Check((live.resolvedUseMaxQp == 1) && (live.resolvedMaxQpI == 44),
              "the standing clamp survives a bitrate-only change",
              "use " + I64(live.resolvedUseMaxQp) + ", qpI " +
                  I64(live.resolvedMaxQpI));
        Check(live.layerAverageBitrate == 6000000,
              "the bitrate-only change is still in force",
              "live " + I64((int64_t)live.layerAverageBitrate));
    }
}

//=============================================================================
// 2e. WHERE A QP CLAMP CHANGE IS STILL REFUSED.
//=============================================================================
//
// Three cases, and on each of them the codec fill would run and carry
// nothing -- which is the accepted-and-ignored shape the contract forbids,
// not a milder version of it. Plus the two validations InitializeExt
// already applies, so a value refused at init cannot arrive here instead.
void CaseQpClampRefusals()
{
    Session s;
    if (!s.Open()) {
        Check(false, "QP-clamp refusal session opens", "session setup");
        return;
    }

    g_currentCase = "an invalid QP clamp is refused";
    { VkVideoEncoderConfig c = Base(); c.minQp = 60;
      ExpectRefused(s, "a minQp outside 0..51 is refused", c); }
    { VkVideoEncoderConfig c = Base(); c.maxQp = 52;
      ExpectRefused(s, "a maxQp outside 0..51 is refused", c); }
    { VkVideoEncoderConfig c = Base(); c.minQp = 40; c.maxQp = 20;
      ExpectRefused(s, "an inverted clamp window is refused", c); }

    // THE DEVICE QP WINDOW. With useMinQp raised the spec requires the
    // value inside the window the device reports, and the init path
    // refuses one that is not. A mid-stream write that skipped the check
    // would be a hole straight past it.
    g_currentCase = "a clamp outside the device QP window is refused";
    Check(VkEncSetDeviceQpWindow(s.Get(), 10, 40) == VK_SUCCESS,
          "the device QP window is declarable", "seam refused");
    { VkVideoEncoderConfig c = Base(); c.minQp = 5;
      ExpectRefused(s, "a minQp below the device window is refused", c); }
    { VkVideoEncoderConfig c = Base(); c.maxQp = 45;
      ExpectRefused(s, "a maxQp above the device window is refused", c); }
    // The control for that check: inside the window it is still accepted,
    // so the refusals above are the window firing and not the clamp path
    // being broken outright.
    { VkVideoEncoderConfig c = Base(); c.minQp = 12; c.maxQp = 38;
      ExpectAccepted(s, "a clamp inside the device window is accepted", c); }

    // AV1 HAS NO QP-UNIT CLAMP AT ALL. Its fill reads quantizer indices
    // derived from the device capability limits and never these fields, so
    // a clamp here would reach nothing. InitializeExt rejects one rather
    // than ignoring it, and so does this.
    g_currentCase = "a QP clamp is refused on an AV1 session";
    {
        Session av1;
        if (av1.Open()) {
            VkVideoEncoderConfig seed = Base();
            seed.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
            Check(VkEncSeedRecordedConfig(av1.Get(), &seed) == VK_SUCCESS,
                  "an AV1 session record is seedable", "seam refused");
            VkVideoEncoderConfig c = seed;
            c.minQp = 30;
            ExpectRefused(av1, "minQp is refused on an AV1 session", c);
            // The control: the same session still takes a rate change, so
            // the refusal is the AV1 rule and not a dead session.
            ExpectAccepted(av1, "an AV1 session still takes a rate change",
                           seed);
        } else {
            Check(false, "AV1 session opens", "session setup");
        }
    }

    // A CONSTANT-QP SESSION IGNORES THE CLAMPS BY CONSTRUCTION. The
    // DISABLED arm of the H.26x fill sets the codec clamp from the
    // quality-level constant QP and never reads the caller request, so a
    // clamp change there is ignored however it is delivered. constQp is
    // that session's lever, and it is applied.
    g_currentCase = "a QP clamp is refused on a constant-QP session";
    {
        Session cqp;
        if (cqp.Open()) {
            VkVideoEncoderConfig seed = Base();
            seed.rateControlMode =
                VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
            Check(VkEncSeedRecordedConfig(cqp.Get(), &seed) == VK_SUCCESS,
                  "a constant-QP session record is seedable", "seam refused");
            VkVideoEncoderConfig c = seed;
            c.minQp = 30;
            ExpectRefused(cqp, "minQp is refused on a constant-QP session", c);
            // The control, and the point of refusing rather than accepting:
            // the lever that session DOES have still works.
            VkVideoEncoderConfig q = seed;
            q.constQpI = 28;
            ExpectAccepted(cqp, "a constant-QP session still takes constQp", q);
            int32_t qpI = -1, qpP = -1, qpB = -1;
            Check(VkEncApplyAndGetSessionConstQp(cqp.Get(), &qpI, &qpP,
                                                 &qpB) == VK_SUCCESS,
                  "the constant-QP session is readable", "seam refused");
            Check(qpI == 28,
                  "the constant-QP change reached the constant-QP session",
                  "got " + I64(qpI) + ", want 28");
        } else {
            Check(false, "constant-QP session opens", "session setup");
        }
    }
}

//=============================================================================
// 2f. THE CONSTANT QUANTIZER'S RANGE, WHICH IS CODEC-DEPENDENT.
//=============================================================================
//
// constQpI/P/B are stated in the CODEC'S OWN units -- a QP on 0..51 for
// H.264/H.265, a quantizer INDEX on 0..255 for AV1 -- and nothing between the
// ext boundary and the bitstream narrows the value: the fields are int32_t
// and ConstQpSettings holds uint32_t. So an out-of-range value was refused
// nowhere. It was TRUNCATED at the (uint8_t) cast in
// VkVideoEncoderAV1::EncodeFrame, modulo 256, with VK_SUCCESS answered to the
// caller: constQpI 300 encoded at quantizer index 44.
//
// EVERY REFUSAL BELOW IS PAIRED WITH A NEGATIVE CONTROL AT THE BOUNDARY --
// 51 for H.26x, 255 for AV1 -- which must still be ACCEPTED and must still
// READ BACK AS THE VALUE GIVEN. A guard that refused everything would pass a
// refusal-only test, and a guard that CLAMPED rather than refused would pass
// one that read the return code alone. Both halves therefore assert the value
// in force, not the VkResult.
//
// AND THE CODEC-DEPENDENCE IS ITSELF ASSERTED, from the same harness: 52 is
// refused on H.264 and accepted on AV1. One range applied to both codecs
// cannot pass this file.
//
// BOTH ENTRY POINTS ARE COVERED, because a refusal that guarded only one is
// worse than none: it teaches the caller the value is validated. The binder
// half below is the InitializeExt path, reached device-free through
// VkEncBuildAndProbeConfig, which builds a config and reads it back with no
// device anywhere.

VkVideoEncoderConfig InitBase(VkVideoCodecOperationFlagBitsKHR codec)
{
    VkVideoEncoderConfig cfg{};
    cfg.sType           = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
    cfg.codec           = codec;
    cfg.encodeWidth     = 1920;
    cfg.encodeHeight    = 1080;
    cfg.inputWidth      = 1920;
    cfg.inputHeight     = 1080;
    cfg.inputFormat     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    cfg.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
    cfg.averageBitrate  = 4000000;
    cfg.frameRateNum    = 30;
    cfg.frameRateDen    = 1;
    cfg.gopLength       = 30;
    cfg.constQpI        = -1;
    cfg.constQpP        = -1;
    cfg.constQpB        = -1;
    return cfg;
}

// Accepted AND carried through unaltered. Reading the triple back is the
// whole point of the control: a guard that clamped an out-of-range value
// would answer VK_SUCCESS here too, and so would one that quietly rewrote an
// in-range one.
void BinderCarries(const std::string& what, VkVideoEncoderConfig cfg,
                   VkVideoCodecOperationFlagBitsKHR codecOp, uint32_t wantI,
                   uint32_t wantP, uint32_t wantB)
{
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(cfg, codecOp, &probe);
    Check(r == VK_SUCCESS, what.c_str(),
          "the binder returned " + I64((int64_t)r) + ", want VK_SUCCESS (0)");
    if (r != VK_SUCCESS) {
        return;
    }
    Check((probe.constQpIntra == wantI) && (probe.constQpInterP == wantP) &&
              (probe.constQpInterB == wantB) && (probe.constQpSet == 1u),
          (what + " -- and is carried unaltered").c_str(),
          "got " + I64(probe.constQpIntra) + "/" + I64(probe.constQpInterP) +
              "/" + I64(probe.constQpInterB) + " constQpSet " +
              I64(probe.constQpSet) + ", want " + I64((int64_t)wantI) + "/" +
              I64((int64_t)wantP) + "/" + I64((int64_t)wantB) +
              " constQpSet 1");
}

void BinderRefuses(const std::string& what, VkVideoEncoderConfig cfg,
                   VkVideoCodecOperationFlagBitsKHR codecOp)
{
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(cfg, codecOp, &probe);
    Check(r == VK_ERROR_INITIALIZATION_FAILED, what.c_str(),
          "the binder returned " + I64((int64_t)r) +
              ", want VK_ERROR_INITIALIZATION_FAILED (-3)");
}

void CaseConstQpRangeAtTheBinder()
{
    const VkVideoCodecOperationFlagBitsKHR kH264 =
        VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    const VkVideoCodecOperationFlagBitsKHR kH265 =
        VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    const VkVideoCodecOperationFlagBitsKHR kAv1 =
        VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;

    //-------------------------------------------------------------------
    // H.26x. The boundary is 51.
    //-------------------------------------------------------------------
    g_currentCase = "the H.26x constant-QP range is 0..51 at the binder";
    // CALIBRATED ON A KNOWN-CLEAN INPUT FIRST. Without a legal value that
    // passes, every refusal below could be the binder failing for a reason
    // that has nothing to do with the range.
    { VkVideoEncoderConfig c = InitBase(kH264);
      c.constQpI = 51; c.constQpP = 51; c.constQpB = 51;
      BinderCarries("H.264 constQp 51 is accepted", c, kH264, 51, 51, 51); }
    { VkVideoEncoderConfig c = InitBase(kH264);
      c.constQpI = 0; c.constQpP = 0; c.constQpB = 0;
      BinderCarries("H.264 constQp 0 is accepted", c, kH264, 0, 0, 0); }
    { VkVideoEncoderConfig c = InitBase(kH264); c.constQpI = 52;
      BinderRefuses("H.264 constQpI 52 is refused", c, kH264); }
    { VkVideoEncoderConfig c = InitBase(kH264); c.constQpP = 52;
      BinderRefuses("H.264 constQpP 52 is refused", c, kH264); }
    { VkVideoEncoderConfig c = InitBase(kH264); c.constQpB = 52;
      BinderRefuses("H.264 constQpB 52 is refused", c, kH264); }
    // The pinned value: 300 & 0xFF is 44, a perfectly plausible QP, which is
    // why nothing downstream could ever have noticed.
    { VkVideoEncoderConfig c = InitBase(kH264); c.constQpI = 300;
      BinderRefuses("H.264 constQpI 300 is refused, not truncated to 44", c,
                    kH264); }
    // H.265 takes the same range through the same rule.
    { VkVideoEncoderConfig c = InitBase(kH265); c.constQpI = 51;
      BinderCarries("H.265 constQpI 51 is accepted", c, kH265, 51, 51, 51); }
    { VkVideoEncoderConfig c = InitBase(kH265); c.constQpI = 52;
      BinderRefuses("H.265 constQpI 52 is refused", c, kH265); }

    //-------------------------------------------------------------------
    // AV1. The boundary is 255, and 52 is a legal input.
    //-------------------------------------------------------------------
    g_currentCase = "the AV1 quantizer-index range is 0..255 at the binder";
    // THE CODEC-DEPENDENCE, asserted directly: the value refused two blocks
    // above is accepted here, because the unit is not the same unit.
    { VkVideoEncoderConfig c = InitBase(kAv1);
      c.constQpI = 52; c.constQpP = 52; c.constQpB = 52;
      BinderCarries("AV1 constQp 52 is accepted -- 52 is a legal quantizer "
                    "index", c, kAv1, 52, 52, 52); }
    { VkVideoEncoderConfig c = InitBase(kAv1);
      c.constQpI = 255; c.constQpP = 255; c.constQpB = 255;
      BinderCarries("AV1 constQp 255 is accepted at the boundary", c, kAv1,
                    255, 255, 255); }
    { VkVideoEncoderConfig c = InitBase(kAv1); c.constQpI = 256;
      BinderRefuses("AV1 constQpI 256 is refused, not truncated to 0", c,
                    kAv1); }
    { VkVideoEncoderConfig c = InitBase(kAv1); c.constQpP = 256;
      BinderRefuses("AV1 constQpP 256 is refused", c, kAv1); }
    { VkVideoEncoderConfig c = InitBase(kAv1); c.constQpB = 256;
      BinderRefuses("AV1 constQpB 256 is refused", c, kAv1); }
    { VkVideoEncoderConfig c = InitBase(kAv1); c.constQpI = 300;
      BinderRefuses("AV1 constQpI 300 is refused, not truncated to 44", c,
                    kAv1); }

    //-------------------------------------------------------------------
    // AV1 quantizer index 0: what the library guarantees about it.
    //-------------------------------------------------------------------
    g_currentCase = "an explicit AV1 quantizer index 0 is carried, not "
                    "substituted";
    // 0 is a LEGAL AV1 quantizer index -- the lossless one -- so it is
    // neither refused nor normalised, and constQpSet must be raised so the
    // driver-preference substitution in
    // EncoderConfigAV1::InitDeviceCapabilities stays out. This is the
    // library side of the contract, and it is the half the library actually
    // controls: what the DRIVER then does with base_q_idx 0 is not asserted
    // here and cannot be, device-free.
    { VkVideoEncoderConfig c = InitBase(kAv1);
      c.constQpI = 0; c.constQpP = 0; c.constQpB = 0;
      BinderCarries("AV1 constQp 0 is carried and marked resolved", c, kAv1,
                    0, 0, 0); }
    // THE CONTROL FOR THAT, and the reason constQpSet is not decoration: a
    // config that names NOTHING must leave it clear, so the substitution
    // does run. If constQpSet read 1 in both cases the assertion above
    // would be asserting nothing.
    {
        VkVideoEncoderConfig c = InitBase(kAv1);
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(c, kAv1, &probe);
        Check(r == VK_SUCCESS, "an AV1 config naming no quantizer is accepted",
              "the binder returned " + I64((int64_t)r));
        Check(probe.constQpSet == 0u,
              "and leaves constQpSet clear, so the substitution still runs",
              "constQpSet " + I64(probe.constQpSet) + ", want 0");
    }
}

//=============================================================================
// 2g. THE SAME RANGE, ON THE OTHER ENTRY POINT.
//=============================================================================
void CaseConstQpRangeOnReconfigure()
{
    //-------------------------------------------------------------------
    // H.26x.
    //-------------------------------------------------------------------
    g_currentCase = "the H.26x constant-QP range is 0..51 on Reconfigure";
    {
        Session h26x;
        if (h26x.Open()) {
            VkVideoEncoderConfig seed = Base();
            seed.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
            seed.rateControlMode =
                VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
            Check(VkEncSeedRecordedConfig(h26x.Get(), &seed) == VK_SUCCESS,
                  "an H.264 constant-QP session record is seedable",
                  "seam refused");
            // Calibration: the boundary value must be ACCEPTED and must be
            // the value in force afterwards.
            { VkVideoEncoderConfig c = seed;
              c.constQpI = 51; c.constQpP = 51; c.constQpB = 51;
              ExpectAccepted(h26x, "H.264 constQp 51 is accepted", c); }
            int32_t qpI = -1, qpP = -1, qpB = -1;
            Check(VkEncApplyAndGetSessionConstQp(h26x.Get(), &qpI, &qpP,
                                                 &qpB) == VK_SUCCESS,
                  "the H.264 session quantizers are readable", "seam refused");
            Check((qpI == 51) && (qpP == 51) && (qpB == 51),
                  "constQp 51 is the value in force",
                  "got " + I64(qpI) + "/" + I64(qpP) + "/" + I64(qpB) +
                      ", want 51/51/51");
            { VkVideoEncoderConfig c = seed; c.constQpI = 52;
              ExpectRefused(h26x, "H.264 constQpI 52 is refused", c); }
            { VkVideoEncoderConfig c = seed; c.constQpP = 52;
              ExpectRefused(h26x, "H.264 constQpP 52 is refused", c); }
            { VkVideoEncoderConfig c = seed; c.constQpB = 52;
              ExpectRefused(h26x, "H.264 constQpB 52 is refused", c); }
            { VkVideoEncoderConfig c = seed; c.constQpI = 300;
              ExpectRefused(h26x,
                            "H.264 constQpI 300 is refused, not truncated",
                            c); }
            // A REFUSAL MUST NOT HALF-APPLY. The four calls above must have
            // left the session on the last value it accepted.
            qpI = qpP = qpB = -1;
            Check(VkEncApplyAndGetSessionConstQp(h26x.Get(), &qpI, &qpP,
                                                 &qpB) == VK_SUCCESS,
                  "the H.264 session is still readable after the refusals",
                  "seam refused");
            Check((qpI == 51) && (qpP == 51) && (qpB == 51),
                  "a refused quantizer left the last accepted one in force",
                  "got " + I64(qpI) + "/" + I64(qpP) + "/" + I64(qpB) +
                      ", want 51/51/51");
            // And the session is not dead: it still takes a legal change.
            { VkVideoEncoderConfig c = seed; c.constQpI = 30;
              ExpectAccepted(h26x, "the H.264 session still takes constQp 30",
                             c); }
        } else {
            Check(false, "H.264 constant-QP session opens", "session setup");
        }
    }

    //-------------------------------------------------------------------
    // AV1.
    //-------------------------------------------------------------------
    g_currentCase = "the AV1 quantizer-index range is 0..255 on Reconfigure";
    {
        Session av1;
        if (av1.Open()) {
            VkVideoEncoderConfig seed = Base();
            seed.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
            seed.rateControlMode =
                VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
            Check(VkEncSeedRecordedConfig(av1.Get(), &seed) == VK_SUCCESS,
                  "an AV1 constant-QP session record is seedable",
                  "seam refused");
            // The codec-dependence again, on this entry point: the value the
            // H.264 session above refused is accepted here and applied.
            { VkVideoEncoderConfig c = seed;
              c.constQpI = 52; c.constQpP = 52; c.constQpB = 52;
              ExpectAccepted(av1, "AV1 constQp 52 is accepted", c); }
            int32_t qpI = -1, qpP = -1, qpB = -1;
            Check(VkEncApplyAndGetSessionConstQp(av1.Get(), &qpI, &qpP,
                                                 &qpB) == VK_SUCCESS,
                  "the AV1 session quantizers are readable", "seam refused");
            Check((qpI == 52) && (qpP == 52) && (qpB == 52),
                  "AV1 constQp 52 is the value in force",
                  "got " + I64(qpI) + "/" + I64(qpP) + "/" + I64(qpB) +
                      ", want 52/52/52");
            { VkVideoEncoderConfig c = seed;
              c.constQpI = 255; c.constQpP = 255; c.constQpB = 255;
              ExpectAccepted(av1, "AV1 constQp 255 is accepted at the "
                                  "boundary", c); }
            qpI = qpP = qpB = -1;
            Check(VkEncApplyAndGetSessionConstQp(av1.Get(), &qpI, &qpP,
                                                 &qpB) == VK_SUCCESS,
                  "the AV1 session is readable at the boundary",
                  "seam refused");
            Check((qpI == 255) && (qpP == 255) && (qpB == 255),
                  "AV1 constQp 255 is the value in force, not wrapped",
                  "got " + I64(qpI) + "/" + I64(qpP) + "/" + I64(qpB) +
                      ", want 255/255/255");
            { VkVideoEncoderConfig c = seed; c.constQpI = 256;
              ExpectRefused(av1,
                            "AV1 constQpI 256 is refused, not truncated to 0",
                            c); }
            { VkVideoEncoderConfig c = seed; c.constQpP = 256;
              ExpectRefused(av1, "AV1 constQpP 256 is refused", c); }
            { VkVideoEncoderConfig c = seed; c.constQpB = 256;
              ExpectRefused(av1, "AV1 constQpB 256 is refused", c); }
            { VkVideoEncoderConfig c = seed; c.constQpI = 300;
              ExpectRefused(av1,
                            "AV1 constQpI 300 is refused, not truncated to 44",
                            c); }
            qpI = qpP = qpB = -1;
            Check(VkEncApplyAndGetSessionConstQp(av1.Get(), &qpI, &qpP,
                                                 &qpB) == VK_SUCCESS,
                  "the AV1 session is still readable after the refusals",
                  "seam refused");
            Check((qpI == 255) && (qpP == 255) && (qpB == 255),
                  "a refused AV1 quantizer left the last accepted one in "
                  "force",
                  "got " + I64(qpI) + "/" + I64(qpP) + "/" + I64(qpB) +
                      ", want 255/255/255");
        } else {
            Check(false, "AV1 constant-QP session opens", "session setup");
        }
    }
}

//=============================================================================
// 3. THE OVER-REFUSAL CONTROL. What cannot change the stream stays accepted.
//=============================================================================
//
// The counterweight to group 2. These are the twelve of the twenty-six that
// are deliberately still ignored: refusing them would buy no correctness --
// none can change an encoded bit -- and would break a caller that builds a
// fresh minimal config for the reconfigure. A blanket refusal of all
// twenty-six is what this group exists to catch.
void CaseNonEncodingFieldsAreStillAccepted(Session& s)
{
    g_currentCase = "a field that cannot change the stream is still accepted";

    // Diagnostic.
    { VkVideoEncoderConfig c = Base(); c.verbose = VK_TRUE;
      ExpectAccepted(s, "verbose is still accepted", c); }
    { VkVideoEncoderConfig c = Base(); c.validate = VK_TRUE;
      ExpectAccepted(s, "validate is still accepted", c); }
    { VkVideoEncoderConfig c = Base(); c.outputPath = "/tmp/reconfigure.h264";
      ExpectAccepted(s, "outputPath is still accepted", c); }
    { VkVideoEncoderConfig c = Base(); c.disableFileOutput = VK_TRUE;
      ExpectAccepted(s, "disableFileOutput is still accepted", c); }
    { VkVideoEncoderConfig c = Base(); c.silenceStdio = VK_TRUE;
      ExpectAccepted(s, "silenceStdio is still accepted", c); }

    // Session-creation-only.
    { VkVideoEncoderConfig c = Base(); c.deviceId = 3;
      ExpectAccepted(s, "deviceId is still accepted", c); }
    { VkVideoEncoderConfig c = Base(); c.gpuUUID[0] = 0xAB;
      ExpectAccepted(s, "gpuUUID is still accepted", c); }
    { VkVideoEncoderConfig c = Base();
      c.externalEncodeQueueFamilyIndex = 2;
      ExpectAccepted(s, "externalEncodeQueueFamilyIndex is still accepted",
                     c); }
    { VkVideoEncoderConfig c = Base();
      c.externalComputeQueueFamilyIndex = 3;
      ExpectAccepted(s, "externalComputeQueueFamilyIndex is still accepted",
                     c); }
}

//=============================================================================
// 4. REGRESSION GUARD on the twelve that were already refused.
//=============================================================================
void CasePreexistingRefusalsStillHold(Session& s)
{
    g_currentCase = "the already-refused fields are still refused";
    { VkVideoEncoderConfig c = Base(); c.encodeWidth = 1920;
      ExpectRefused(s, "encodeWidth is still refused", c); }
    { VkVideoEncoderConfig c = Base(); c.encodeHeight = 1080;
      ExpectRefused(s, "encodeHeight is still refused", c); }
    { VkVideoEncoderConfig c = Base(); c.videoFullRange = VK_TRUE;
      ExpectRefused(s, "videoFullRange is still refused", c); }
    { VkVideoEncoderConfig c = Base(); c.colourPrimaries = 9;
      ExpectRefused(s, "colourPrimaries is still refused", c); }

    g_currentCase = "the structure-type gate still fires";
    { VkVideoEncoderConfig c = Base();
      c.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_FRAME;
      ExpectRefused(s, "a wrong sType is refused", c); }
    { VkVideoEncoderConfig c = Base(); c.pNext = (const void*)&g_checks;
      ExpectRefused(s, "a chained pNext is refused", c); }
    // A RECOGNISED sType IS STILL A pNext. VkVideoEncoderInputColourInfo is
    // chainable at InitializeExt and every axis it carries is immutable for
    // the life of the session, so a caller that reuses one config for both
    // entry points must clear pNext for this one -- and finding out by having
    // the declaration silently ignored is exactly the accepted-and-dropped
    // class this call refuses. The case above uses a junk pointer, which
    // cannot tell "any pNext" from "an unrecognised one".
    { VkVideoEncoderConfig c = Base();
      VkVideoEncoderInputColourInfo ic = {};
      ic.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_COLOUR_INFO;
      c.pNext = &ic;
      ExpectRefused(s, "a chained VkVideoEncoderInputColourInfo is refused "
                       "too -- Reconfigure refuses ANY pNext, recognised or "
                       "not", c); }
}

}  // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    std::printf("Encoder-ext Reconfigure field dispositions\n");
    std::printf("------------------------------------------\n");

    Session session;
    if (!session.Open()) {
        std::printf("RESULT: COULD-NOT-RUN (session setup failed)\n");
        return 2;
    }

    CaseUnchangedConfigIsAccepted(session);
    CaseCarriedRateFieldsAreAccepted(session);
    CaseEncodingAffectingFieldsAreRefused(session);
    CaseConstQpIsAppliedNotIgnored(session);
    CaseRecordAgreesWithWhatIsInForce();
    CaseQpClampIsAppliedNotIgnored();
    CaseQpClampRefusals();
    CaseConstQpRangeAtTheBinder();
    CaseConstQpRangeOnReconfigure();
    CaseNonEncodingFieldsAreStillAccepted(session);
    CasePreexistingRefusalsStillHold(session);

    std::printf("------------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
