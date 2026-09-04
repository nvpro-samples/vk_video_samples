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
 * THE PRE-SESSION INPUT-FORMAT POINT QUERY, on a real device.
 *
 * WHAT THIS ANSWERS. VkEncQueryInputFormatSupport is the only surface that
 * takes a (format, colour model) PAIR and a (codec, profile) and answers
 * before a session exists. QueryImageSupport reads a colour model but is a
 * session method; VkEncEnumerateInputFormats is context-level and carries no
 * colour model, so it answers the same question for the whole routable set at
 * once. This file measures both on hardware.
 *
 * THE CASE THIS EXISTS FOR. An A4000 encodes NV24 at H.264 High 4:4:4
 * Predictive and S410 at H.265 Range Extensions. A session declared in either
 * one works -- the codec config derives the 4:4:4 profile from the input's own
 * chroma subsampling -- so those are ACCEPTED, and the assertions below are
 * that the point query says so before a frame pool is allocated. They were
 * once also UNDISCOVERABLE, because the enumerator answered from a capability
 * snapshot probed at a fixed 4:2:0 envelope; section 3 now asserts the
 * opposite, and --cross-route asserts that the two surfaces give the same
 * answer rather than merely both saying yes.
 *
 * --cross-route IS A SEPARATE MODE AND A SEPARATE CTEST ROW. It sweeps every
 * publishable (codec, profile) key, puts each advertised entry back through
 * the point query, and asserts equality of encodeFormat and optimality -- with
 * the OPTIMAL arm as its calibration and a refusal control that keeps
 * "accurate" distinguishable from "permissive".
 *
 * --init-gate IS A THIRD MODE AND A THIRD ROW, and it asserts the proposition
 * neither of the other two can: that what InitializeExt ACCEPTS is what the
 * enumerator ADVERTISES. Both surfaces above are answers ABOUT a session; this
 * one builds the session. It is the only mode here that does -- which used to
 * be offered as the reason this binary could decline a validation gate, and
 * is not: the gate is per binary, and one mode that creates twenty-five
 * sessions is the whole binary creating sessions. All three rows are gated.
 *
 * CALIBRATION ORDER, AND IT IS NOT COSMETIC. The known-good 4:2:0 rows run
 * FIRST. An instrument that answered "no" to everything would pass every
 * refusal assertion in this file, and a bare refusal proves nothing until the
 * same instrument has been seen to say yes to something. So the 4:2:0 rows
 * are a precondition for reading anything below them, and they are asserted,
 * not merely printed.
 *
 * WHAT IT CAN FAIL ON:
 *   - The device leg could be inert -- a query that consulted the 4:2:0
 *     capability snapshot instead of asking the device at the input's own
 *     subsampling answers "no" for NV24 and fails the acceptance rows.
 *   - The library leg could over-promise -- a query that skipped the profile
 *     guard would accept H.264 High (100) over NV24, which the standard
 *     forbids and this device refuses. That row is here.
 *   - The guard could be indiscriminate -- a library that refused every
 *     explicit profile would fail the negative control, which asks for High
 *     (100) over NV12 and requires a yes.
 *
 * DEVICE IDENTITY IS ASSERTED, NOT ASSUMED. Both lab machines carry a second
 * Vulkan device (llvmpipe needs no render node), and a capability verdict read
 * off a software rasteriser is worthless. The identity block of every device
 * the context enumerated is printed, and the run refuses to proceed on
 * anything but an NVIDIA one.
 *
 * Exits 77 (CTest SKIP) with no encode-capable NVIDIA device, 0 when every
 * assertion held, 1 when one did not.
 */

#include "vulkan_video_encoder_ext.h"

// The public header reaches the Xlib platform headers, whose macros collide
// with ordinary identifiers. Same scrub, same reason, as the sibling tests.
#undef Status
#undef None
#undef Bool
#undef Window

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;
int g_checks   = 0;

void Check(bool ok, const std::string& what, const std::string& detail)
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
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u", v);
    return buf;
}

const char* ResultName(VkResult r)
{
    switch (r) {
        case VK_SUCCESS:                       return "VK_SUCCESS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:    return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_INITIALIZATION_FAILED:   return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR:
            return "VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR";
        case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR:
            return "VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR";
        case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR:
            return "VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR";
        case VK_ERROR_FEATURE_NOT_PRESENT:     return "VK_ERROR_FEATURE_NOT_PRESENT";
        default: {
            // NAMED BY NUMBER RATHER THAN "other". A code this switch does not
            // spell is exactly the interesting case -- the late driver refusal
            // this file's --init-gate mode exists to move -- and "other" makes
            // two different late refusals read identically in a failure line.
            static char buf[32];
            std::snprintf(buf, sizeof(buf), "VkResult %d", (int)r);
            return buf;
        }
    }
}

// Vulkan enumerants the rows below name. Spelled once so a row reads as the
// format a producer would recognise it by.
const VkFormat kNv12 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
const VkFormat kP010 = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
const VkFormat kNv24 = VK_FORMAT_G8_B8R8_2PLANE_444_UNORM;
const VkFormat kS410 = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16;
const VkFormat kNv16 = VK_FORMAT_G8_B8R8_2PLANE_422_UNORM;
const VkFormat kP012 = VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;

const VkVideoCodecOperationFlagBitsKHR kH264 =
    VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
const VkVideoCodecOperationFlagBitsKHR kH265 =
    VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
const VkVideoCodecOperationFlagBitsKHR kAV1 =
    VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;

// Spelling for the report only. A format this list does not name prints as its
// enumerant, which is what makes an unexpected entry readable rather than
// silently anonymous.
const char* FormatName(VkFormat f)
{
    switch ((uint32_t)f) {
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:                 return "NV12";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: return "P010";
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16: return "P012";
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:              return "P016";
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:                 return "NV16";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16: return "P210";
        case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM:                 return "NV24";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16: return "S410";
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:                return "I420";
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:                return "I422";
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:                return "I444";
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
            return "I420-10";
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
            return "I444-10";
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
            return "I420-12";
        case VK_FORMAT_R8G8B8A8_UNORM:                           return "RGBA8";
        case VK_FORMAT_B8G8R8A8_UNORM:                           return "BGRA8";
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:                    return "ABGR8";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:                 return "A2BGR10";
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:                 return "A2RGB10";
        case VK_FORMAT_UNDEFINED:                                return "UNDEFINED";
        default:                                                 return "(other)";
    }
}

const char* OptimalityName(VkVideoEncoderInputFormatOptimality o)
{
    return (o == VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL) ? "OPTIMAL"
                                                        : "SUBOPTIMAL";
}

// One row of the sweep. |wantEncodeFormat| is checked only when the row is
// expected to succeed; VK_FORMAT_UNDEFINED means "do not check it".
struct Row {
    VkVideoCodecOperationFlagBitsKHR codec;
    const char*                      codecName;
    uint32_t                         profile;
    VkFormat                         format;
    const char*                      formatName;
    VkResult                         want;
    VkFormat                         wantEncodeFormat;
    VkVideoEncoderInputFormatOptimality wantOptimality;
    const char*                      why;
};

// Ask the device, through the same enumeration surface a caller would use,
// whether |format| is encodable for (codec, profile), and hand back the
// advertised properties when it is.
//
// The 4:2:2 rows below used to assert a refusal unconditionally. That is not a
// library rule -- it is a per-device capability: Blackwell and later encode
// 4:2:2, earlier parts do not. Hardcoding the refusal makes this test report a
// hardware CAPABILITY as a library DEFECT on every part that has it. Derive the
// expectation instead, and take the expected properties from the enumerated
// entry, so the point query is still held to agreeing with the list.
static bool DeviceAdvertisesFormat(VulkanVideoEncoderContext* ctx,
                                   uint32_t deviceIndex,
                                   VkVideoCodecOperationFlagBitsKHR codec,
                                   uint32_t profile,
                                   VkFormat format,
                                   VkVideoEncoderInputFormatProperties* pProps)
{
    uint32_t count = 0;
    if (VkEncEnumerateInputFormats(ctx, deviceIndex, codec, profile,
                                   &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    if (count == 0) {
        return false;
    }
    VkVideoEncoderInputFormatProperties entries[64] = {};
    uint32_t fetched = (count < 64u) ? count : 64u;
    const VkResult r = VkEncEnumerateInputFormats(ctx, deviceIndex, codec,
                                                  profile, &fetched, entries);
    if ((r != VK_SUCCESS) && (r != VK_INCOMPLETE)) {
        return false;
    }
    for (uint32_t i = 0; i < fetched; i++) {
        if (entries[i].format == format) {
            if (pProps != nullptr) {
                *pProps = entries[i];
            }
            return true;
        }
    }
    return false;
}

void RunRows(VulkanVideoEncoderContext* ctx, uint32_t deviceIndex,
             const Row* rows, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const Row& row = rows[i];
        VkVideoEncoderInputFormatProperties props = {};
        const VkResult r = VkEncQueryInputFormatSupport(
            ctx, deviceIndex, row.codec, row.profile, row.format,
            VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, &props);
        const std::string what =
            std::string(row.codecName) + " profile " + U32(row.profile) +
            " + " + row.formatName + " -- " + row.why;
        Check(r == row.want, what,
              std::string("got ") + ResultName(r) + ", wanted " +
                  ResultName(row.want));
        if ((r != VK_SUCCESS) || (row.want != VK_SUCCESS)) {
            continue;
        }
        Check(props.format == row.format,
              what + ": echoes the format asked about",
              "got " + U32((uint32_t)props.format));
        if (row.wantEncodeFormat != VK_FORMAT_UNDEFINED) {
            Check(props.encodeFormat == row.wantEncodeFormat,
                  what + ": names what the bitstream is coded from",
                  "got " + U32((uint32_t)props.encodeFormat));
        }
        Check(props.optimality == row.wantOptimality,
              what + ": reports the right optimality",
              "got " + U32((uint32_t)props.optimality));
    }
}

// ---------------------------------------------------------------------------
// THE CROSS-ROUTE SWEEP (--cross-route).
//
// Two entry points answer the same question by different routes:
// VkEncEnumerateInputFormats hands back a list, VkEncQueryInputFormatSupport
// answers a point. For one context, one device and one (codec, profile) they
// must agree, and the field that matters most is encodeFormat -- the header
// makes it load-bearing because it, and not `format`, is what decides the
// chroma subsampling and bit depth of the bitstream.
//
// WHY IT LIVES IN THIS BINARY. This file already calls both entry points
// against one context, one device and one bring-up. A second binary would
// duplicate the context creation, the device choice and the SKIP-77 harness to
// assert something neither half of the duplication owns.
//
// THE CALIBRATION IS THE OPTIMAL ARM AND IT IS NOT COSMETIC. On an OPTIMAL
// entry both surfaces set encodeFormat to the format itself, by construction,
// through code they do not share. So every OPTIMAL row must agree BEFORE any
// SUBOPTIMAL row is read: a sweep that is red on OPTIMAL rows too is not
// measuring a cross-route gap, it is measuring a broken harness, and the run
// has to be discarded rather than interpreted.
//
// THE SECOND CONTROL IS A REFUSAL. 4:2:2 and 12-bit must be on NO list, before
// or after any change here. Without it, "every format the caller hoped for is
// now advertised" cannot be told apart from "the enumerator became permissive".

struct Key {
    VkVideoCodecOperationFlagBitsKHR codec;
    const char*                      codecName;
    uint32_t                         profile;
    const char*                      profileName;
};

// The eight PUBLISHABLE keys -- the (codec, profile) pairs the context probes
// and a caller can name. AV1 Main appears once: the context probes it at two
// depths but a profile number resolves to the first row, so 8-bit Main is the
// only AV1 answer any public entry point can return.
const Key kPublishableKeys[] = {
    { kH264, "H.264", VK_VIDEO_ENCODER_PROFILE_DEFAULT,       "DEFAULT"     },
    { kH264, "H.264", VK_VIDEO_ENCODER_PROFILE_H264_BASELINE, "66 Baseline" },
    { kH264, "H.264", VK_VIDEO_ENCODER_PROFILE_H264_MAIN,     "77 Main"     },
    { kH264, "H.264", VK_VIDEO_ENCODER_PROFILE_H264_HIGH,     "100 High"    },
    { kH265, "H.265", VK_VIDEO_ENCODER_PROFILE_DEFAULT,       "DEFAULT"     },
    { kH265, "H.265", VK_VIDEO_ENCODER_PROFILE_H265_MAIN,     "1 Main"      },
    { kH265, "H.265", VK_VIDEO_ENCODER_PROFILE_H265_MAIN10,   "2 Main 10"   },
    { kAV1,  "AV1",   VK_VIDEO_ENCODER_PROFILE_AV1_MAIN,      "0 Main"      },
};
const size_t kPublishableKeyCount =
    sizeof(kPublishableKeys) / sizeof(kPublishableKeys[0]);

// Wide enough for every routable format plus slack, so a list that grew is
// reported as it is rather than clipped into agreement.
enum { kMaxEntries = 64 };

struct KeyList {
    VkVideoEncoderInputFormatProperties entries[kMaxEntries];
    uint32_t                            count;
    VkResult                            result;
    bool                                answered;
};

void FetchList(VulkanVideoEncoderContext* ctx, uint32_t deviceIndex,
               const Key& key, KeyList& out)
{
    out.count    = 0;
    out.answered = false;
    uint32_t count = 0;
    out.result = VkEncEnumerateInputFormats(ctx, deviceIndex, key.codec,
                                            key.profile, &count, nullptr);
    if (out.result != VK_SUCCESS) {
        return;
    }
    out.answered = true;
    if (count == 0) {
        return;
    }
    uint32_t fetched = (count < (uint32_t)kMaxEntries) ? count
                                                       : (uint32_t)kMaxEntries;
    const VkResult r =
        VkEncEnumerateInputFormats(ctx, deviceIndex, key.codec, key.profile,
                                   &fetched, out.entries);
    if ((r != VK_SUCCESS) && (r != VK_INCOMPLETE)) {
        out.result   = r;
        out.answered = false;
        return;
    }
    out.count = fetched;
}

std::string KeyName(const Key& key)
{
    return std::string(key.codecName) + " " + key.profileName;
}

// One advertised entry, put back through the point query. Three assertions,
// each its own Check because they fail for different reasons: acceptance,
// encodeFormat equality (the one the header makes load-bearing), and
// optimality.
void CrossCheckEntry(VulkanVideoEncoderContext* ctx, uint32_t deviceIndex,
                     const Key& key,
                     const VkVideoEncoderInputFormatProperties& entry)
{
    VkVideoEncoderInputFormatProperties props = {};
    const VkResult r = VkEncQueryInputFormatSupport(
        ctx, deviceIndex, key.codec, key.profile, entry.format,
        VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, &props);
    const std::string what = KeyName(key) + " + " + FormatName(entry.format) +
                             " [" + OptimalityName(entry.optimality) + "]";
    Check(r == VK_SUCCESS, what + ": an advertised format is accepted by the "
                                  "point query",
          std::string("got ") + ResultName(r));
    if (r != VK_SUCCESS) {
        return;
    }
    Check(props.encodeFormat == entry.encodeFormat,
          what + ": both routes name the same encodeFormat",
          std::string("enumerator says ") + FormatName(entry.encodeFormat) +
              " (" + U32((uint32_t)entry.encodeFormat) + "), point query says " +
              FormatName(props.encodeFormat) + " (" +
              U32((uint32_t)props.encodeFormat) + ")");
    Check(props.optimality == entry.optimality,
          what + ": both routes name the same optimality",
          std::string("enumerator says ") + OptimalityName(entry.optimality) +
              ", point query says " + OptimalityName(props.optimality));
}

bool ListContains(const KeyList& list, VkFormat format)
{
    for (uint32_t i = 0; i < list.count; i++) {
        if (list.entries[i].format == format) {
            return true;
        }
    }
    return false;
}

int RunCrossRoute(VulkanVideoEncoderContext* ctx, uint32_t deviceIndex)
{
    std::printf("== cross-route agreement: enumeration vs point query ==\n");

    static KeyList lists[kPublishableKeyCount];
    uint32_t answeredKeys = 0;
    uint32_t optimalRows  = 0;
    uint32_t suboptimalRows = 0;

    std::printf("-- advertised lists, per publishable key --\n");
    for (size_t k = 0; k < kPublishableKeyCount; k++) {
        FetchList(ctx, deviceIndex, kPublishableKeys[k], lists[k]);
        if (!lists[k].answered) {
            std::printf("  %-18s : no list (%s)\n",
                        KeyName(kPublishableKeys[k]).c_str(),
                        ResultName(lists[k].result));
            continue;
        }
        answeredKeys++;
        uint32_t opt = 0;
        uint32_t sub = 0;
        for (uint32_t i = 0; i < lists[k].count; i++) {
            if (lists[k].entries[i].optimality ==
                VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL) {
                opt++;
            } else {
                sub++;
            }
        }
        optimalRows    += opt;
        suboptimalRows += sub;
        std::printf("  %-18s : %u entries (%u OPTIMAL, %u SUBOPTIMAL) :",
                    KeyName(kPublishableKeys[k]).c_str(), lists[k].count, opt,
                    sub);
        for (uint32_t i = 0; i < lists[k].count; i++) {
            std::printf(" %s->%s", FormatName(lists[k].entries[i].format),
                        FormatName(lists[k].entries[i].encodeFormat));
        }
        std::printf("\n");
    }
    std::printf("  totals: %u keys answered, %u OPTIMAL rows, %u SUBOPTIMAL "
                "rows\n", answeredKeys, optimalRows, suboptimalRows);

    Check(answeredKeys >= 2u,
          "at least two publishable keys advertise a list, so the sweep has "
          "something to read",
          "answeredKeys=" + U32(answeredKeys));

    // ---- The two-call idiom's stability, which is now a DIFFERENT claim ----
    //
    // The header used to rest this list's stability on the context's snapshot
    // being immutable. It is not read out of the snapshot any more -- it is
    // recomputed live, per candidate, against the device, on every call --
    // so the rule survives for a different reason: the resolve is a
    // deterministic function of (context, device, codec, profile). That is a
    // published caller contract and nothing asserted it, which is exactly how
    // a rationale outlives its mechanism.
    //
    // CALIBRATION FIRST. "Every call agrees" is worth nothing from an
    // observable that reads the same for everything, so the counts are shown
    // to VARY ACROSS KEYS before they are required to be STABLE ACROSS CALLS.
    {
        uint32_t distinctCounts = 0;
        for (size_t k = 0; k < kPublishableKeyCount; k++) {
            if (!lists[k].answered) {
                continue;
            }
            bool seen = false;
            for (size_t j = 0; j < k; j++) {
                if (lists[j].answered && (lists[j].count == lists[k].count)) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                distinctCounts++;
            }
        }
        Check(distinctCounts >= 2u,
              "calibration: the advertised counts differ across keys, so "
              "'the same on every call' is an observation and not a constant",
              "distinct counts " + U32(distinctCounts));

        for (size_t k = 0; k < kPublishableKeyCount; k++) {
            if (!lists[k].answered) {
                continue;
            }
            const Key& key = kPublishableKeys[k];
            uint32_t countA = 0;
            const VkResult rA = VkEncEnumerateInputFormats(
                ctx, deviceIndex, key.codec, key.profile, &countA, nullptr);
            uint32_t countB = 0;
            const VkResult rB = VkEncEnumerateInputFormats(
                ctx, deviceIndex, key.codec, key.profile, &countB, nullptr);
            Check((rA == VK_SUCCESS) && (rB == VK_SUCCESS) &&
                      (countA == countB) && (countA == lists[k].count),
                  KeyName(key) + ": two counting calls and the fetch agree, "
                                 "on a list recomputed live each time",
                  "counts " + U32(countA) + ", " + U32(countB) + ", " +
                      U32(lists[k].count));

            static VkVideoEncoderInputFormatProperties again[kMaxEntries];
            uint32_t fetched = (countA < (uint32_t)kMaxEntries)
                                   ? countA : (uint32_t)kMaxEntries;
            const VkResult rC = VkEncEnumerateInputFormats(
                ctx, deviceIndex, key.codec, key.profile, &fetched, again);
            bool identical = ((rC == VK_SUCCESS) || (rC == VK_INCOMPLETE)) &&
                             (fetched == lists[k].count);
            for (uint32_t i = 0; identical && (i < fetched); i++) {
                identical = (again[i].format == lists[k].entries[i].format) &&
                            (again[i].encodeFormat ==
                             lists[k].entries[i].encodeFormat) &&
                            (again[i].optimality ==
                             lists[k].entries[i].optimality);
            }
            Check(identical,
                  KeyName(key) + ": and a second fetch is entry-for-entry the "
                                 "same list, ordering included",
                  "fetched " + U32(fetched) + " of " + U32(lists[k].count));
        }
    }
    if (g_failures != 0) {
        std::printf("\nNO LISTS -- nothing below can be read. checks: %d, "
                    "failures: %d\nRESULT: FAIL\n", g_checks, g_failures);
        return 1;
    }

    // ---- CONTROL 1. The OPTIMAL arm, where the two surfaces must agree by
    // ---- construction. Read before any SUBOPTIMAL row.
    std::printf("-- control 1: the OPTIMAL arm, which must already agree --\n");
    for (size_t k = 0; k < kPublishableKeyCount; k++) {
        for (uint32_t i = 0; i < lists[k].count; i++) {
            if (lists[k].entries[i].optimality !=
                VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL) {
                continue;
            }
            CrossCheckEntry(ctx, deviceIndex, kPublishableKeys[k],
                            lists[k].entries[i]);
        }
    }
    if (g_failures != 0) {
        std::printf("\nCALIBRATION FAILED on the OPTIMAL arm -- this run is "
                    "measuring a broken harness and not a cross-route gap. "
                    "Discard it. checks: %d, failures: %d\nRESULT: FAIL\n",
                    g_checks, g_failures);
        return 1;
    }

    // ---- CONTROL 2. The refusal. 12-bit is refused by every part this
    // ---- library targets, at every key including DEFAULT, so it belongs on no
    // ---- list -- before or after any change to how the list is built, and it
    // ---- is what keeps this control able to catch an enumerator that simply
    // ---- became permissive.
    // ----
    // ---- 4:2:2 is NOT such an absolute. Every NAMED profile here is 4:2:0
    // ---- only, so NV16 must be absent from those lists on any device. But
    // ---- DEFAULT derives the profile from the input's own subsampling, and
    // ---- whether 4:2:2 appears there is a per-device capability -- Blackwell
    // ---- and later encode it. Asserting its absence made this control report
    // ---- a hardware capability as an enumerator defect.
    std::printf("-- control 2: what must be on no list at all --\n");
    for (size_t k = 0; k < kPublishableKeyCount; k++) {
        if (!lists[k].answered) {
            continue;
        }
        if (kPublishableKeys[k].profile != VK_VIDEO_ENCODER_PROFILE_DEFAULT) {
            Check(!ListContains(lists[k], kNv16),
                  KeyName(kPublishableKeys[k]) +
                      ": NV16 (4:2:2) is not advertised, because this profile "
                      "is 4:2:0 only",
                  "it was");
        } else {
            // Derive it: the list must say exactly what the point query says.
            VkVideoEncoderInputFormatProperties props = {};
            const VkResult r = VkEncQueryInputFormatSupport(
                ctx, deviceIndex, kPublishableKeys[k].codec,
                kPublishableKeys[k].profile, kNv16,
                VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, &props);
            const bool listed = ListContains(lists[k], kNv16);
            Check(listed == (r == VK_SUCCESS),
                  KeyName(kPublishableKeys[k]) +
                      ": NV16 (4:2:2) is listed exactly when the point query "
                      "accepts it",
                  std::string("the list says ") + (listed ? "yes" : "no") +
                      " and the point query says " + ResultName(r));
        }
        Check(!ListContains(lists[k], kP012),
              KeyName(kPublishableKeys[k]) + ": P012 (12-bit) is not advertised",
              "it was");
    }
    if (g_failures != 0) {
        std::printf("\nREFUSAL CONTROL FAILED -- the enumerator is advertising "
                    "what the point query refuses, so no presence assertion "
                    "below distinguishes accurate from permissive. checks: %d, "
                    "failures: %d\nRESULT: FAIL\n", g_checks, g_failures);
        return 1;
    }

    // ---- THE ASSERTION. The SUBOPTIMAL arm, where the enumerator computes
    // ---- encodeFormat from the DEVICE's list and the point query computes it
    // ---- from the SESSION's own bound configuration.
    std::printf("-- cross-route equality on the SUBOPTIMAL arm --\n");
    for (size_t k = 0; k < kPublishableKeyCount; k++) {
        for (uint32_t i = 0; i < lists[k].count; i++) {
            if (lists[k].entries[i].optimality ==
                VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL) {
                continue;
            }
            CrossCheckEntry(ctx, deviceIndex, kPublishableKeys[k],
                            lists[k].entries[i]);
        }
    }

    // ---- PRESENCE. A different observable from equality, so it is asserted
    // ---- separately: these are formats this library encodes on this device
    // ---- and the point query accepts, which no advertised list reported.
    std::printf("-- presence: what the library encodes and must advertise --\n");
    struct Presence {
        size_t   keyIndex;
        VkFormat format;
        const char* why;
    };
    static const Presence kPresence[] = {
        { 0u, kNv24, "8-bit 4:4:4 derives H.264 High 4:4:4 Predictive" },
        { 4u, kNv24, "8-bit 4:4:4 derives H.265 Range Extensions" },
        { 4u, kS410, "10-bit 4:4:4 derives H.265 Range Extensions" },
        { 4u, kP010, "10-bit 4:2:0 derives H.265 Main 10" },
        { 7u, kP010, "10-bit 4:2:0 is AV1 Main at ten bits" },
    };
    for (size_t i = 0; i < sizeof(kPresence) / sizeof(kPresence[0]); i++) {
        const Presence& p = kPresence[i];
        if (!lists[p.keyIndex].answered) {
            std::printf("  (skipped: %s has no list on this device)\n",
                        KeyName(kPublishableKeys[p.keyIndex]).c_str());
            continue;
        }
        Check(ListContains(lists[p.keyIndex], p.format),
              KeyName(kPublishableKeys[p.keyIndex]) + " advertises " +
                  FormatName(p.format) + " -- " + p.why,
              "it does not");
    }

    std::printf("\nchecks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}


// ---------------------------------------------------------------------------
// THE INITIALISATION GATE (--init-gate).
//
// WHAT THIS ASSERTS, AND IT IS ONE SENTENCE: what InitializeExt accepts is what
// VkEncEnumerateInputFormats advertises. Not a superset of it.
//
// WHY THAT IS NOT A RESTATEMENT OF --cross-route. That sweep puts the two
// PRE-SESSION surfaces against each other -- the enumerator and the point query
// -- and both of them are answers ABOUT a session. This one puts the answer
// against the SESSION ITSELF. A library whose two queries agreed perfectly with
// each other and disagreed with what init does would pass --cross-route on
// every row and fail every row here.
//
// THE TWO HALVES ARE DIFFERENT PROPOSITIONS AND FAIL FOR DIFFERENT REASONS, so
// they are asserted separately and in this order:
//
//   1. EVERY ADVERTISED FORMAT STILL INITIALISES. This is the negative control
//      and it runs first, because a gate that refused everything would satisfy
//      half 2 completely. It is also the half that costs a real session per
//      entry, which is the price of asserting the proposition rather than a
//      proxy for it.
//   2. A FORMAT THE DEVICE REFUSES IS REFUSED AT InitializeExt. 4:2:2 and
//      12-bit are on no advertised list and the point query refuses them at
//      every profile, so they are the pairs where "advertised" and "accepted"
//      used to differ: the binder took them, the session was created, and the
//      driver refused at video-session creation -- with a message naming
//      neither the format nor its subsampling.
//
// AND THE CALIBRATION COMES BEFORE BOTH. NV12 must initialise. An instrument
// that could not create a session at all would report every row of half 1 as a
// failure and every row of half 2 as a pass, and the second reading is the
// dangerous one: it looks exactly like a working gate.
//
// THE REFUSAL CODE IS THE POINT QUERY'S. VK_ERROR_FORMAT_NOT_SUPPORTED is what
// VkEncQueryInputFormatSupport returns for a pair this device will not take, so
// the session boundary returning anything else would be a second vocabulary for
// one verdict.

struct InitVerdict {
    VkResult result;
    bool     created;
};

// One session, built exactly as a shipping caller builds one for that format,
// and destroyed before the next: the sweep walks tens of formats and a process
// that held them all open would measure the driver's session limit instead of
// the library's acceptance.
InitVerdict RunInit(VkVideoCodecOperationFlagBitsKHR codec, uint32_t profile,
                    VkFormat format)
{
    InitVerdict v = { VK_ERROR_INITIALIZATION_FAILED, false };
    VkSharedBaseObj<VulkanVideoEncoderExt> enc;
    if ((CreateVulkanVideoEncoderExt(enc) != VK_SUCCESS) || !enc) {
        return v;
    }
    v.created = true;

    VkVideoEncoderConfig config = {};
    config.sType             = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
    config.codec             = codec;
    config.profile           = profile;
    config.encodeWidth       = 1920;
    config.encodeHeight      = 1080;
    config.inputFormat       = format;
    config.inputWidth        = 1920;
    config.inputHeight       = 1080;
    config.rateControlMode   = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    config.averageBitrate    = 4000000;
    config.maxBitrate        = 4000000;
    config.gopLength         = 30;
    config.idrPeriod         = 30;
    config.frameRateNum      = 30;
    config.frameRateDen      = 1;
    config.deviceId          = -1;
    config.disableFileOutput = VK_TRUE;
    v.result = enc->InitializeExt(config);
    return v;
}

// The keys the gate sweeps. DEFAULT on each codec, which is what a caller that
// has not named a profile gets and the only key whose derivation reaches the
// 4:4:4 and 10-bit profiles at all.
const Key kInitGateKeys[] = {
    { kH264, "H.264", VK_VIDEO_ENCODER_PROFILE_DEFAULT, "DEFAULT" },
    { kH265, "H.265", VK_VIDEO_ENCODER_PROFILE_DEFAULT, "DEFAULT" },
    { kAV1,  "AV1",   VK_VIDEO_ENCODER_PROFILE_AV1_MAIN, "0 Main" },
};
const size_t kInitGateKeyCount =
    sizeof(kInitGateKeys) / sizeof(kInitGateKeys[0]);

// What the DEVICE refuses, on both measured architectures, at every profile:
// the 4:2:2 family and the 12-bit family. These are exactly the formats the
// library ROUTES -- the taxonomy admits them and the binder builds a config for
// them -- and that the device then will not encode. They are on no advertised
// list, which --cross-route's control 2 already asserts for two of them; what
// is asserted here is that InitializeExt agrees.
struct RefusalRow {
    VkFormat    format;
    const char* name;
    const char* why;
};
const RefusalRow kMustRefuse[] = {
    { VK_FORMAT_G8_B8R8_2PLANE_422_UNORM,                    "NV16",
      "8-bit 4:2:2, semi-planar" },
    { VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM,                   "I422",
      "8-bit 4:2:2, 3-plane, converted rung" },
    { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16,   "P210",
      "10-bit 4:2:2, semi-planar" },
    { VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,   "P012",
      "12-bit 4:2:0, semi-planar" },
    { VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16,  "I420-12",
      "12-bit 4:2:0, 3-plane, converted rung" },
    { VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16,   "S412",
      "12-bit 4:4:4, semi-planar" },
};
const size_t kMustRefuseCount =
    sizeof(kMustRefuse) / sizeof(kMustRefuse[0]);

int RunInitGate(VulkanVideoEncoderContext* ctx, uint32_t deviceIndex)
{
    std::printf("== initialisation gate: accepted == advertised ==\n");

    // ---- CALIBRATION. One session on the format every encoder takes. Read
    // ---- before anything below, because a harness that can create no session
    // ---- reports every refusal assertion as a pass.
    std::printf("-- calibration: a session the device certainly takes --\n");
    {
        const InitVerdict v =
            RunInit(kH264, VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv12);
        Check(v.created, "an encoder object is creatable at all", "it is not");
        Check(v.result == VK_SUCCESS,
              "H.264 DEFAULT + NV12 initialises -- the instrument can produce "
              "an ACCEPTANCE, not only a refusal",
              std::string("got ") + ResultName(v.result));
    }
    if (g_failures != 0) {
        std::printf("\nCALIBRATION FAILED -- this harness cannot create a "
                    "session, so every refusal below would pass for the wrong "
                    "reason. checks: %d, failures: %d\nRESULT: FAIL\n",
                    g_checks, g_failures);
        return 1;
    }

    // ---- HALF 1, THE NEGATIVE CONTROL. Every advertised entry, at the key it
    // ---- was advertised for, must still initialise.
    std::printf("-- half 1 (negative control): every ADVERTISED format still "
                "initialises --\n");
    static KeyList lists[kInitGateKeyCount];
    uint32_t swept = 0;
    for (size_t k = 0; k < kInitGateKeyCount; k++) {
        FetchList(ctx, deviceIndex, kInitGateKeys[k], lists[k]);
        if (!lists[k].answered) {
            std::printf("  %-14s : no list on this device (%s) -- skipped\n",
                        KeyName(kInitGateKeys[k]).c_str(),
                        ResultName(lists[k].result));
            continue;
        }
        for (uint32_t i = 0; i < lists[k].count; i++) {
            const VkFormat f = lists[k].entries[i].format;
            const InitVerdict v =
                RunInit(kInitGateKeys[k].codec, kInitGateKeys[k].profile, f);
            swept++;
            Check(v.result == VK_SUCCESS,
                  KeyName(kInitGateKeys[k]) + " + " + FormatName(f) + " [" +
                      OptimalityName(lists[k].entries[i].optimality) +
                      "]: an ADVERTISED format still initialises",
                  std::string("got ") + ResultName(v.result));
        }
    }
    std::printf("  %u advertised entries put through InitializeExt\n", swept);
    Check(swept >= 10u,
          "the negative control swept a list worth reading",
          "only " + U32(swept) + " entries");
    if (g_failures != 0) {
        std::printf("\nNEGATIVE CONTROL FAILED -- acceptance has been narrowed "
                    "past the advertised set, which is worse than the gap it "
                    "was closing. checks: %d, failures: %d\nRESULT: FAIL\n",
                    g_checks, g_failures);
        return 1;
    }

    // ---- HALF 2, THE ASSERTION. What the device refuses is refused here,
    // ---- with the same code the point query gives it.
    std::printf("-- half 2: a DEVICE-REFUSED format is refused at "
                "InitializeExt --\n");
    for (size_t k = 0; k < kInitGateKeyCount; k++) {
        if (!lists[k].answered) {
            continue;
        }
        for (size_t r = 0; r < kMustRefuseCount; r++) {
            const RefusalRow& row = kMustRefuse[r];
            const std::string what = KeyName(kInitGateKeys[k]) + " + " +
                                     row.name + " (" + row.why + ")";
            // The point query first, so the row is only read as evidence about
            // InitializeExt when the pre-session surfaces already say no. A
            // device that DID encode 4:2:2 would make this row vacuous rather
            // than false, and it would say so here.
            const VkResult q = VkEncQueryInputFormatSupport(
                ctx, deviceIndex, kInitGateKeys[k].codec,
                kInitGateKeys[k].profile, row.format,
                VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, nullptr);
            if (q == VK_SUCCESS) {
                std::printf("  (vacuous: %s is ACCEPTED by the point query on "
                            "this device, so it is not a refusal row here)\n",
                            what.c_str());
                continue;
            }
            Check(!ListContains(lists[k], row.format),
                  what + ": is on no advertised list", "it was advertised");
            std::printf("  [InitializeExt refusal message follows for %s]\n",
                        what.c_str());
            std::fflush(stdout);
            const InitVerdict v =
                RunInit(kInitGateKeys[k].codec, kInitGateKeys[k].profile,
                        row.format);
            Check(v.result != VK_SUCCESS,
                  what + ": InitializeExt REFUSES what no list advertises",
                  "InitializeExt returned VK_SUCCESS -- the accepted set is "
                  "wider than the advertised one");
            Check(v.result == VK_ERROR_FORMAT_NOT_SUPPORTED,
                  what + ": refused with the point query's own code",
                  std::string("got ") + ResultName(v.result) +
                      ", wanted VK_ERROR_FORMAT_NOT_SUPPORTED");
        }
    }

    std::printf("\nchecks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    // Two flags, following the sibling binaries' pattern: --cross-route runs
    // the enumeration-versus-point-query sweep and --init-gate the
    // advertisement-versus-InitializeExt sweep, each and nothing else, each
    // registered as its own ctest row. Without either the behaviour is
    // unchanged.
    bool crossRoute = false;
    bool initGate   = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--cross-route") == 0) {
            crossRoute = true;
        } else if (std::strcmp(argv[i], "--init-gate") == 0) {
            initGate = true;
        }
    }

    std::printf("== pre-session input-format point query ==\n");

    VkVideoEncoderContextCreateInfo ci = {};
    ci.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONTEXT_CREATE_INFO;
    ci.mode  = VK_VIDEO_ENCODER_CONTEXT_MODE_OWN;

    VkSharedBaseObj<VulkanVideoEncoderContext> context;
    if (CreateVulkanVideoEncoderContext(&ci, context) != VK_SUCCESS) {
        std::printf("SKIP: no Vulkan encoder context could be created\n");
        return 77;
    }
    VulkanVideoEncoderContext* const ctx = context.get();

    // ---- Device identity, printed for every device and asserted for the one
    // ---- used. A capability answer off llvmpipe is not a capability answer.
    const uint32_t deviceCount = VkEncGetPhysicalDeviceCount(ctx);
    std::printf("  devices enumerated: %u\n", deviceCount);

    uint32_t chosen      = UINT32_MAX;
    for (uint32_t i = 0; i < deviceCount; i++) {
        VkVideoEncoderDeviceIdentity id = {};
        id.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_DEVICE_IDENTITY;
        if (VkEncGetPhysicalDeviceIdentity(ctx, i, &id) != VK_SUCCESS) {
            continue;
        }
        std::printf("  device[%u]: name='%s' vendorID=0x%04x deviceID=0x%04x\n",
                    i, id.deviceName, id.vendorID, id.deviceID);
        // 0x10DE is NVIDIA. Mesa's llvmpipe reports 0x10005 and is excluded
        // by the same test that would exclude any other vendor's device.
        if ((chosen == UINT32_MAX) && (id.vendorID == 0x10DEu)) {
            // Only if it can actually answer for an encode profile: a
            // display-only NVIDIA device would pass the vendor test and
            // report nothing.
            VkVideoEncoderCapabilities caps = {};
            caps.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CAPABILITIES;
            if (VkEncGetEncodeCapabilities(ctx, i, kH264,
                                           VK_VIDEO_ENCODER_PROFILE_DEFAULT,
                                           &caps) == VK_SUCCESS) {
                chosen = i;
            }
        }
    }
    if (chosen == UINT32_MAX) {
        std::printf("SKIP: no encode-capable NVIDIA device enumerated\n");
        return 77;
    }
    std::printf("  using device[%u]\n", chosen);

    if (crossRoute) {
        return RunCrossRoute(ctx, chosen);
    }
    if (initGate) {
        return RunInitGate(ctx, chosen);
    }

    // ---- 1. CALIBRATION. Known-good 4:2:0 rows, run before anything else is
    // ---- believed. If these do not say yes, no refusal below means anything.
    std::printf("-- calibration: known-good 4:2:0 --\n");
    static const Row kCalibration[] = {
        { kH264, "H.264", VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv12, "NV12",
          VK_SUCCESS, kNv12, VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "8-bit 4:2:0, the format every encoder takes" },
        { kH265, "H.265", VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv12, "NV12",
          VK_SUCCESS, kNv12, VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "8-bit 4:2:0" },
        { kH265, "H.265", VK_VIDEO_ENCODER_PROFILE_DEFAULT, kP010, "P010",
          VK_SUCCESS, kP010, VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "10-bit 4:2:0 derives Main 10" },
    };
    RunRows(ctx, chosen, kCalibration,
            sizeof(kCalibration) / sizeof(kCalibration[0]));
    if (g_failures != 0) {
        std::printf("\nCALIBRATION FAILED -- nothing below this line can be "
                    "read. checks: %d, failures: %d\nRESULT: FAIL\n",
                    g_checks, g_failures);
        return 1;
    }

    // ---- 2. THE ACCEPTANCE CASE. 4:4:4, which no enumeration reports.
    std::printf("-- acceptance: 4:4:4, accepted and previously "
                "undiscoverable --\n");
    static const Row kAcceptance[] = {
        { kH264, "H.264", VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv24, "NV24",
          VK_SUCCESS, kNv24, VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "8-bit 4:4:4 derives High 4:4:4 Predictive (244)" },
        { kH265, "H.265", VK_VIDEO_ENCODER_PROFILE_DEFAULT, kS410, "S410",
          VK_SUCCESS, kS410, VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "10-bit 4:4:4 derives Range Extensions (4)" },
        { kH265, "H.265", VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv24, "NV24",
          VK_SUCCESS, kNv24, VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "8-bit 4:4:4 derives Range Extensions (4)" },
    };
    RunRows(ctx, chosen, kAcceptance,
            sizeof(kAcceptance) / sizeof(kAcceptance[0]));

    // ---- 3. THE GAP IS CLOSED, MEASURED. The same NV24 the point query just
    // ---- accepted is on the advertised list too.
    // ----
    // ---- THE INVERSE OF THIS ASSERTION -- "NV24 is NOT on the advertised
    // ---- H.264 list" -- holds only when the enumerator builds its
    // ---- answer from a capability snapshot that probes every profile at
    // ---- 4:2:0, so no 4:4:4 encode source could reach any advertised list on
    // ---- any device however capable. Both surfaces now resolve through one
    // ---- function, so a format this library encodes on this device is
    // ---- discoverable before a session exists, which is what the entry point
    // ---- was for.
    // ----
    // ---- MEMBERSHIP ONLY, HERE. That the two surfaces AGREE -- same
    // ---- encodeFormat, same optimality, for every entry of every publishable
    // ---- key -- is a different proposition and is asserted in --cross-route,
    // ---- where it has the calibration it needs.
    std::printf("-- the gap this closes --\n");
    {
        uint32_t count = 0;
        VkResult r = VkEncEnumerateInputFormats(
            ctx, chosen, kH264, VK_VIDEO_ENCODER_PROFILE_DEFAULT, &count,
            nullptr);
        bool advertised = false;
        if ((r == VK_SUCCESS) && (count > 0)) {
            VkVideoEncoderInputFormatProperties entries[64] = {};
            uint32_t fetched = (count < 64u) ? count : 64u;
            r = VkEncEnumerateInputFormats(ctx, chosen, kH264,
                                           VK_VIDEO_ENCODER_PROFILE_DEFAULT,
                                           &fetched, entries);
            if ((r == VK_SUCCESS) || (r == VK_INCOMPLETE)) {
                for (uint32_t i = 0; i < fetched; i++) {
                    if (entries[i].format == kNv24) {
                        advertised = true;
                    }
                }
            }
        }
        std::printf("  VkEncEnumerateInputFormats(H.264, DEFAULT) advertises "
                    "%u formats\n", count);
        Check(advertised,
              "NV24 IS on the advertised H.264 list, so what the point query "
              "accepts is discoverable without asking about it by name",
              "NV24 was not advertised");
    }

    // ---- 4. REFUSALS THE DEVICE OWNS. The sweep on this part refuses 4:2:2
    // ---- at every profile and 12-bit at every profile.
    std::printf("-- refusals the device owns --\n");
    // 12-bit is refused by every part this library targets, so it stays a
    // fixed expectation.
    static const Row kDeviceRefusals[] = {
        { kH265, "H.265", VK_VIDEO_ENCODER_PROFILE_DEFAULT, kP012, "P012",
          VK_ERROR_FORMAT_NOT_SUPPORTED, VK_FORMAT_UNDEFINED,
          VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "12-bit, which this device does not encode at any profile" },
    };
    RunRows(ctx, chosen, kDeviceRefusals,
            sizeof(kDeviceRefusals) / sizeof(kDeviceRefusals[0]));

    // 4:2:2 is a per-device capability. Ask, then assert what the answer
    // obliges: a refusal where the device has no 4:2:2, and agreement with the
    // advertised entry where it does.
    {
        static const struct {
            VkVideoCodecOperationFlagBitsKHR codec;
            const char*                      codecName;
            const char*                      refusedWhy;
            const char*                      acceptedWhy;
        } k422Rows[] = {
            { kH264, "H.264",
              "4:2:2 derives High 4:2:2 (122), which this device does not encode",
              "4:2:2 derives High 4:2:2 (122), which this device encodes, and "
              "the point query agrees with the advertised list" },
            { kH265, "H.265",
              "4:2:2 at Range Extensions, which this device does not encode",
              "4:2:2 at Range Extensions, which this device encodes, and the "
              "point query agrees with the advertised list" },
        };
        for (size_t i = 0; i < sizeof(k422Rows) / sizeof(k422Rows[0]); i++) {
            VkVideoEncoderInputFormatProperties advertised = {};
            const bool supported = DeviceAdvertisesFormat(
                ctx, chosen, k422Rows[i].codec,
                VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv16, &advertised);
            const Row row = {
                k422Rows[i].codec, k422Rows[i].codecName,
                VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv16, "NV16",
                supported ? VK_SUCCESS : VK_ERROR_FORMAT_NOT_SUPPORTED,
                supported ? advertised.encodeFormat : VK_FORMAT_UNDEFINED,
                supported ? advertised.optimality
                          : VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
                supported ? k422Rows[i].acceptedWhy : k422Rows[i].refusedWhy
            };
            RunRows(ctx, chosen, &row, 1);
        }
    }

    // ---- 5. REFUSALS THE LIBRARY OWNS, and the negative control that says
    // ---- the guard discriminates rather than refusing everything.
    std::printf("-- refusals the library owns, and the negative control --\n");
    static const Row kLibraryRows[] = {
        { kH264, "H.264", VK_VIDEO_ENCODER_PROFILE_H264_HIGH, kNv24, "NV24",
          VK_ERROR_FORMAT_NOT_SUPPORTED, VK_FORMAT_UNDEFINED,
          VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "High (100) is a 4:2:0 profile and cannot carry 4:4:4" },
        { kH265, "H.265", VK_VIDEO_ENCODER_PROFILE_H265_MAIN, kNv24, "NV24",
          VK_ERROR_FORMAT_NOT_SUPPORTED, VK_FORMAT_UNDEFINED,
          VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "Main (1) is a 4:2:0 profile and cannot carry 4:4:4" },
        // EXPRESSIBLE, DERIVABLE, AND NOW BINDABLE. 244 is the profile the
        // DEFAULT derivation reaches for this very input and this device
        // encodes it, so naming it explicitly must not be refused. This query
        // answers what the binder
        // answers. The bindable set has widened to every number the standard's
        // limits table states, so the two now agree: what the derivation
        // selects, a caller may also name. What the DEVICE will encode is a
        // separate question and the rows above and below are where it is put.
        { kH264, "H.264",
          (uint32_t)VK_VIDEO_ENCODER_PROFILE_H264_HIGH_444_PREDICTIVE,
          kNv24, "NV24",
          VK_SUCCESS, kNv24,
          VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "244 is expressible, derivable and now bindable" },
        // THE NEGATIVE CONTROL. A guard that refused every explicitly named
        // profile would pass all three rows above. This one requires a yes.
        { kH264, "H.264", VK_VIDEO_ENCODER_PROFILE_H264_HIGH, kNv12, "NV12",
          VK_SUCCESS, kNv12, VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "High (100) over 4:2:0 is still accepted -- the guard "
          "discriminates" },
        { kH264, "H.264", VK_VIDEO_ENCODER_PROFILE_H264_BASELINE, kNv12,
          "NV12", VK_SUCCESS, kNv12, VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
          "Baseline (66) over 4:2:0 is still accepted" },
    };
    RunRows(ctx, chosen, kLibraryRows,
            sizeof(kLibraryRows) / sizeof(kLibraryRows[0]));

    // ---- 6. ARGUMENT GATES, and the optional out-parameter.
    std::printf("-- argument gates --\n");
    {
        VkVideoEncoderInputFormatProperties props = {};
        Check(VkEncQueryInputFormatSupport(
                  nullptr, 0u, kH264, VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv12,
                  VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, &props) ==
                  VK_ERROR_INITIALIZATION_FAILED,
              "a null context is VK_ERROR_INITIALIZATION_FAILED", "");
        Check(VkEncQueryInputFormatSupport(
                  ctx, deviceCount + 7u, kH264,
                  VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv12,
                  VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, &props) ==
                  VK_ERROR_INITIALIZATION_FAILED,
              "an out-of-range deviceIndex is VK_ERROR_INITIALIZATION_FAILED",
              "");
        Check(VkEncQueryInputFormatSupport(
                  ctx, chosen, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
                  VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv12,
                  VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, &props) ==
                  VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR,
              "a decode codec is VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR",
              "");
        // The out-parameter is optional and the verdict must not depend on it.
        const VkResult withOut = VkEncQueryInputFormatSupport(
            ctx, chosen, kH264, VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv24,
            VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, &props);
        const VkResult withoutOut = VkEncQueryInputFormatSupport(
            ctx, chosen, kH264, VK_VIDEO_ENCODER_PROFILE_DEFAULT, kNv24,
            VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, nullptr);
        Check(withOut == withoutOut,
              "a NULL pProperties gives the same verdict",
              std::string("with ") + ResultName(withOut) + ", without " +
                  ResultName(withoutOut));
    }

    std::printf("\nchecks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
