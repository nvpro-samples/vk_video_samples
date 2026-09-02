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

// H.264 Baseline must not emit CABAC.
//
// Baseline and Constrained Baseline are both profile_idc 66 and neither admits
// CABAC: entropy_coding_mode_flag must be 0. Main (77) and the High profiles do
// admit it. Before the guard this suite covers, an explicit --profile baseline
// session took its entropy coder from the device preferredStdEntropyCodingMode
// flag, which is CABAC on this vendor hardware, and emitted profile_idc 66 with
// entropy_coding_mode_flag 1 -- a non-conformant bitstream, reachable from the
// WebRTC default profile.
//
// Device-free by construction. All three parts drive the profile decision and
// the parameter-set writers, neither of which needs a VkDevice.
//
// Exit codes match the sibling library suites: 0 every assertion held, 1 an
// assertion failed, 2 the session could not be stood up at all.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "VkVideoEncoder/VkEncoderConfigH264.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what)
{
    ++g_checks;
    if (ok) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

const char* EntropyName(EncoderConfigH264::EntropyCodingMode m)
{
    return (m == EncoderConfigH264::ENTROPY_CODING_MODE_CABAC) ? "CABAC" : "CAVLC";
}

// High 10 is profile_idc 110, which the Vulkan std enum does not name; the
// library itself reaches it by the same cast in InitProfileLevel().
const StdVideoH264ProfileIdc kProfileHigh10 = static_cast<StdVideoH264ProfileIdc>(110);

// PART A -- the rule itself, over the whole profile x requested matrix.
//
// Asserted exhaustively rather than spot-checked on the one interesting cell,
// because the failure mode that matters second-most here is over-reach: a clamp
// that also stripped CABAC from Main or High would silently cost every
// non-Baseline session its entropy coder, and that regression is invisible to a
// test that only checks the Baseline cell.
void PartA_Rule()
{
    printf("PART A -- ConformantEntropyCodingMode(profile, requested)\n");

    struct Case {
        StdVideoH264ProfileIdc               profile;
        EncoderConfigH264::EntropyCodingMode requested;
        EncoderConfigH264::EntropyCodingMode expected;
        const char*                          name;
    };

    const Case cases[] = {
        // The defect: Baseline may not carry CABAC, so it is clamped.
        { STD_VIDEO_H264_PROFILE_IDC_BASELINE, EncoderConfigH264::ENTROPY_CODING_MODE_CABAC,
          EncoderConfigH264::ENTROPY_CODING_MODE_CAVLC, "Baseline(66) + CABAC -> CAVLC" },
        { STD_VIDEO_H264_PROFILE_IDC_BASELINE, EncoderConfigH264::ENTROPY_CODING_MODE_CAVLC,
          EncoderConfigH264::ENTROPY_CODING_MODE_CAVLC, "Baseline(66) + CAVLC -> CAVLC" },
        // Every profile that DOES admit CABAC must keep it.
        { STD_VIDEO_H264_PROFILE_IDC_MAIN, EncoderConfigH264::ENTROPY_CODING_MODE_CABAC,
          EncoderConfigH264::ENTROPY_CODING_MODE_CABAC, "Main(77) + CABAC -> CABAC" },
        { STD_VIDEO_H264_PROFILE_IDC_MAIN, EncoderConfigH264::ENTROPY_CODING_MODE_CAVLC,
          EncoderConfigH264::ENTROPY_CODING_MODE_CAVLC, "Main(77) + CAVLC -> CAVLC" },
        { STD_VIDEO_H264_PROFILE_IDC_HIGH, EncoderConfigH264::ENTROPY_CODING_MODE_CABAC,
          EncoderConfigH264::ENTROPY_CODING_MODE_CABAC, "High(100) + CABAC -> CABAC" },
        { kProfileHigh10, EncoderConfigH264::ENTROPY_CODING_MODE_CABAC,
          EncoderConfigH264::ENTROPY_CODING_MODE_CABAC, "High10(110) + CABAC -> CABAC" },
        { STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE, EncoderConfigH264::ENTROPY_CODING_MODE_CABAC,
          EncoderConfigH264::ENTROPY_CODING_MODE_CABAC, "High444P(244) + CABAC -> CABAC" },
    };

    for (const Case& c : cases) {
        const EncoderConfigH264::EntropyCodingMode got =
            EncoderConfigH264::ConformantEntropyCodingMode(c.profile, c.requested);
        if (got != c.expected) {
            printf("        got %s, expected %s\n", EntropyName(got), EntropyName(c.expected));
        }
        Check(got == c.expected, c.name);
    }
}

// Stands up a config far enough to drive the parameter-set writers, with no
// device. Only the fields InitSpsPpsParameters() and InitProfileLevel() read
// are set; everything else keeps its constructed default.
void PrimeConfig(EncoderConfigH264& cfg)
{
    // 176x144 is mb-aligned in both dimensions, so no cropping arithmetic runs
    // and the level search lands on the lowest entry.
    cfg.encodeWidth  = 176;
    cfg.encodeHeight = 144;
    cfg.pic_width_in_mbs        = 11;
    cfg.pic_height_in_map_units = 9;
    cfg.encodeChromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    cfg.input.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    cfg.input.bpp = 8;
    cfg.dpbCount  = 2;
    cfg.numRefL0  = 1;
    cfg.numRefL1  = 0;
    // Rate control DISABLED with hrdBitrate 0 makes DetermineLevel() skip its
    // bitrate and CPB tests, so the level search cannot fall off the table on
    // an unrelated default.
    cfg.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
    cfg.hrdBitrate      = 0;
    cfg.vbvBufferSize   = 0;
    cfg.tuningMode      = VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR;
    // Lossless would force qpprime_y_zero_transform_bypass_flag and pull the
    // profile to High 4:4:4; keep this suite off that path.
    cfg.qpprime_y_zero_transform_bypass_flag = 0;
}

// PART B -- what actually lands in the emitted SPS/PPS pair.
//
// This is the part that answers the conformance question, because
// entropy_coding_mode_flag in the PPS and profile_idc in the SPS are the two
// syntax elements a decoder reads. Part A can pass while this fails, if the
// writer stops consulting the rule.
void PartB_EmittedParameterSets()
{
    printf("PART B -- emitted SPS/PPS\n");

    struct Case {
        StdVideoH264ProfileIdc               profile;
        EncoderConfigH264::EntropyCodingMode entropy;
        bool                                 expectFlag;
        const char*                          name;
    };

    const Case cases[] = {
        { STD_VIDEO_H264_PROFILE_IDC_BASELINE, EncoderConfigH264::ENTROPY_CODING_MODE_CABAC,
          false, "Baseline + CABAC requested -> entropy_coding_mode_flag 0" },
        { STD_VIDEO_H264_PROFILE_IDC_BASELINE, EncoderConfigH264::ENTROPY_CODING_MODE_CAVLC,
          false, "Baseline + CAVLC requested -> entropy_coding_mode_flag 0" },
        { STD_VIDEO_H264_PROFILE_IDC_MAIN, EncoderConfigH264::ENTROPY_CODING_MODE_CABAC,
          true,  "Main + CABAC requested -> entropy_coding_mode_flag 1" },
        { STD_VIDEO_H264_PROFILE_IDC_HIGH, EncoderConfigH264::ENTROPY_CODING_MODE_CABAC,
          true,  "High + CABAC requested -> entropy_coding_mode_flag 1" },
    };

    for (const Case& c : cases) {
        VkSharedBaseObj<EncoderConfigH264> cfg(new EncoderConfigH264());
        if (!cfg) {
            printf("  FAIL  could not allocate EncoderConfigH264\n");
            ++g_failures;
            return;
        }
        PrimeConfig(*cfg);
        cfg->profileIdc        = c.profile;
        cfg->entropyCodingMode = c.entropy;

        StdVideoH264SequenceParameterSet sps;
        StdVideoH264PictureParameterSet  pps;
        memset(&sps, 0, sizeof(sps));
        memset(&pps, 0, sizeof(pps));

        if (!cfg->InitSpsPpsParameters(&sps, &pps, nullptr)) {
            printf("  FAIL  InitSpsPpsParameters returned false for %s\n", c.name);
            ++g_failures;
            continue;
        }

        const bool flag = (pps.flags.entropy_coding_mode_flag != 0);
        if (flag != c.expectFlag) {
            printf("        entropy_coding_mode_flag=%d, expected %d\n",
                   flag ? 1 : 0, c.expectFlag ? 1 : 0);
        }
        Check(flag == c.expectFlag, c.name);

        // The profile is NOT rewritten to buy the entropy coder back. A
        // downgrade is only honest if the SPS still says 66, so that what the
        // decoder is told matches what the encoder emitted.
        Check(sps.profile_idc == c.profile,
              "  sps.profile_idc is the requested profile, unchanged");

        if (c.profile == STD_VIDEO_H264_PROFILE_IDC_BASELINE) {
            // 66 with constraint_set1_flag set is Constrained Baseline. Both
            // readings of the stream forbid CABAC, and both are satisfied.
            Check(sps.flags.constraint_set0_flag != 0,
                  "  Baseline: constraint_set0_flag set");
            Check(sps.flags.constraint_set1_flag != 0,
                  "  Baseline: constraint_set1_flag set (Constrained Baseline)");
            // Control on the sibling per-profile tool restriction this fix is
            // modelled on: it must still be enforced, and unchanged.
            Check(pps.flags.transform_8x8_mode_flag == 0,
                  "  Baseline: transform_8x8_mode_flag still 0 (sibling rule intact)");
        }
    }
}

// PART C -- composition with the profile auto-upgrade in InitProfileLevel().
//
// The auto-upgrade turns an UNSPECIFIED profile into Main when B-frames or
// CABAC are in play. The two cases below pin why that upgrade neither replaces
// this fix nor collides with it:
//
//   C1  An EXPLICIT Baseline request is not upgraded -- the upgrade block is
//       guarded by profileIdc == INVALID, so it never runs on a request. This
//       is the hole: without the clamp, this config reaches the PPS writer as
//       profile_idc 66 with the device CABAC preference still set.
//   C2  An UNSPECIFIED profile never resolves to Baseline, so the clamp and the
//       upgrade operate on disjoint inputs and cannot both fire.
//
// C2 also records a live oddity: InitProfileLevel() runs at config
// construction, strictly BEFORE InitDeviceCapabilities() assigns
// entropyCodingMode from the device, so the CABAC term of the upgrade condition
// reads the constructed default -- which is CABAC. The upgrade therefore always
// leaves Baseline, making the profileIdc = BASELINE line that opens that block
// dead. That ordering is also why the clamp has to sit at the device-capability
// assignment: placed with the profile decision it would be overwritten, and
// inert.
void PartC_AutoUpgradeComposition()
{
    printf("PART C -- composition with the InitProfileLevel auto-upgrade\n");

    {
        VkSharedBaseObj<EncoderConfigH264> cfg(new EncoderConfigH264());
        PrimeConfig(*cfg);
        cfg->profileIdc        = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
        cfg->entropyCodingMode = EncoderConfigH264::ENTROPY_CODING_MODE_CABAC;
        cfg->InitProfileLevel();
        if (cfg->profileIdc != STD_VIDEO_H264_PROFILE_IDC_BASELINE) {
            printf("        profileIdc became %u\n", static_cast<uint32_t>(cfg->profileIdc));
        }
        Check(cfg->profileIdc == STD_VIDEO_H264_PROFILE_IDC_BASELINE,
              "C1 explicit Baseline request survives InitProfileLevel (not auto-upgraded)");
    }

    {
        VkSharedBaseObj<EncoderConfigH264> cfg(new EncoderConfigH264());
        PrimeConfig(*cfg);
        cfg->profileIdc        = STD_VIDEO_H264_PROFILE_IDC_INVALID;
        cfg->entropyCodingMode = EncoderConfigH264::ENTROPY_CODING_MODE_CABAC;
        cfg->InitProfileLevel();
        printf("        unspecified profile resolved to %u\n",
               static_cast<uint32_t>(cfg->profileIdc));
        Check(cfg->profileIdc != STD_VIDEO_H264_PROFILE_IDC_BASELINE,
              "C2 unspecified profile never resolves to Baseline");
        Check(cfg->profileIdc != STD_VIDEO_H264_PROFILE_IDC_INVALID,
              "C2 unspecified profile is resolved to something concrete");
    }
}

}  // namespace

int main(int, char**)
{
    printf("encoder_h264_baseline_entropy_test -- H.264 Baseline must not emit CABAC\n\n");

    PartA_Rule();
    printf("\n");
    PartB_EmittedParameterSets();
    printf("\n");
    PartC_AutoUpgradeComposition();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("RESULT: nothing ran\n");
        return 2;
    }
    printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
