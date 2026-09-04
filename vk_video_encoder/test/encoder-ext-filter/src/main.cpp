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
 * Device-free coverage for the input-format taxonomy and the config binder's
 * disposition of enablePreprocessFilter.
 *
 * WHAT THIS PINS. The adaptation ladder has three
 * rungs -- hardware conversion, compute filter, transfer copy -- in that
 * order, and states that falling to a copy where the device could have
 * converted is a defect. Two library-side decisions implement the second
 * rung, and both are pure functions of their arguments:
 *
 *   1. the FORMAT taxonomy: is a given input directly encodable, encodable
 *      only after a conversion, or neither;
 *   2. the CONFIG binder: does a caller that asks for the preprocess filter
 *      get it, and does a caller whose input NEEDS it get told when it did
 *      not ask.
 *
 * Neither needs a device, an instance or a queue, so both are asserted here
 * from a plain process. What this CANNOT assert is that the filter then
 * produces correct pixels, or that the queue-family acquire it now records
 * is accepted by a real driver -- both need the GPU and are deferred to
 * vk_filter_test and vk-video-enc-test.
 *
 * WHY THIS FILE EXISTS AT ALL. The binder conformance suite that would
 * normally carry these assertions (VulkanVideoEncoderConfigBinderTest) lives
 * in Chromium, not in this tree, so a change to the field table's
 * dispositions had no in-tree test that could fail. It does now.
 *
 * CTest semantics, matching the sibling library tests: 0 means every
 * assertion held, 1 means an assertion failed, 2 means the harness could not
 * run at all -- deliberately a FAILURE and not a skip. There is no GPU,
 * driver or display dependence here.
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

std::string U32(uint32_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u", v);
    return buf;
}

// Is the preprocess compute filter compiled into the library this test links?
// The library's own answer, not a guess: the binder refuses the flag when the
// filter is absent, and every expectation below that depends on the flag
// being honourable has to follow the same build gate the library was built
// with. The test and the library are one build here (the test links the
// static archive), so the macro is the same macro.
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
constexpr bool kFilterCompiledIn = true;
#else
constexpr bool kFilterCompiledIn = false;
#endif

VkVideoEncoderConfig BaseConfig()
{
    VkVideoEncoderConfig cfg{};
    cfg.sType        = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
    cfg.pNext        = nullptr;
    cfg.codec        = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    cfg.encodeWidth  = 1920;
    cfg.encodeHeight = 1080;
    cfg.inputWidth   = 1920;
    cfg.inputHeight  = 1080;
    cfg.inputFormat  = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    cfg.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    cfg.averageBitrate  = 4000000;
    cfg.frameRateNum    = 30;
    cfg.frameRateDen    = 1;
    cfg.gopLength       = 30;
    return cfg;
}

//=============================================================================
// 1. The format taxonomy (VkEncClassifyInputFormat / VkEncSupportsInputFormat)
//=============================================================================

void CaseSemiPlanarIsDirect()
{
    g_currentCase = "semi-planar 4:2:0 is encodable DIRECTLY";
    const VkFormat direct[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                    // NV12
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,   // P010
        VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,   // P012
    };
    for (VkFormat f : direct) {
        Check(VkEncClassifyInputFormat(f) ==
                  VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
              "classified DIRECT", "format " + U32((uint32_t)f));
        Check(VkEncSupportsInputFormat(f) == VK_TRUE,
              "supported", "format " + U32((uint32_t)f));
        Check(VkEncInputFormatPlaneCount(f) == 2,
              "2 planes", "format " + U32((uint32_t)f) + " -> " +
                              U32(VkEncInputFormatPlaneCount(f)));
    }
}

void CaseThreePlaneIsViaFilter()
{
    g_currentCase = "3-plane 4:2:0 is encodable only VIA THE FILTER";
    // The distinction the old single VkBool32 could not carry. A copy cannot
    // stand in for the conversion: a 3-plane input through a filter-off
    // library is VK_ERROR_DEVICE_LOST with a 0-byte bitstream.
    const VkFormat viaFilter[] = {
        VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,                   // I420
        VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16,
    };
    for (VkFormat f : viaFilter) {
        Check(VkEncClassifyInputFormat(f) ==
                  VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER,
              "classified VIA_FILTER", "format " + U32((uint32_t)f));
        Check(VkEncClassifyInputFormat(f) !=
                  VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
              "NOT classified DIRECT -- a direct encode of this hangs the GPU",
              "format " + U32((uint32_t)f));
        Check(VkEncInputFormatPlaneCount(f) == 3,
              "3 planes", "format " + U32((uint32_t)f) + " -> " +
                              U32(VkEncInputFormatPlaneCount(f)));
    }
}

void CaseRgbaIsViaFilterAndSinglePlane()
{
    g_currentCase = "8-bit RGBA is encodable VIA THE FILTER, and is 1 plane";
    // The other half of A2, now that the filter has a sampled-read arm: it
    // binds the RGBA source as a SAMPLED_IMAGE and reads it with texelFetch on
    // a `texture2D`, which declares no GLSL format qualifier and therefore
    // resolves the component order from the view's VkFormat. That is what lets
    // ONE shader take all three spellings below -- notably B8G8R8A8_UNORM,
    // which has no `bgra8` storage qualifier to declare.
    const VkFormat viaFilter[] = {
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,
    };
    for (VkFormat f : viaFilter) {
        Check(VkEncClassifyInputFormat(f) ==
                  VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER,
              "classified VIA_FILTER", "format " + U32((uint32_t)f));
        Check(VkEncIsRgbaInputFormat(f) == VK_TRUE,
              "identified as RGBA", "format " + U32((uint32_t)f));
        // PLANE COUNT COMES FROM THE FORMAT, NOT FROM ITS CLASS. Answering it
        // from the class gives every VIA_FILTER format 3, because the class
        // holds only the 3-plane family. RGBA
        // is ONE plane, and this number is what EncoderConfig::input.numPlanes
        // -- and therefore the input geometry -- is written from.
        Check(VkEncInputFormatPlaneCount(f) == 1,
              "1 plane", "format " + U32((uint32_t)f) + " -> " +
                             U32(VkEncInputFormatPlaneCount(f)));
    }
}

void CaseSrgbAndJunkAreUnsupported()
{
    g_currentCase = "sRGB, wide/deep RGB and unrelated formats are UNSUPPORTED";
    // Each of these is a POSITIVE refusal, not a gap.
    //
    //  *_SRGB: the filter applies the colour matrix only, with no transfer
    //    function, which is correct exactly because Y'CbCr is defined on
    //    gamma-encoded R'G'B'. A sampled read of an _SRGB view is linearised
    //    by the implementation before the shader sees it, so accepting these
    //    would feed linear RGB to a matrix expecting R'G'B' -- output that
    //    looks almost right, which is the kind that survives review.
    //  A2B10G10R10 / R16G16B16A16_UNORM: VulkanFilterYuvCompute already uses
    //    these same enumerants to mean PACKED YCbCr (Y410, Y416) on its output
    //    side. One enumerant, two colour models, is a trap.
    //  R16G16B16A16_SFLOAT: scRGB -- linear, and not confined to [0,1].
    const VkFormat unsupported[] = {
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_A8B8G8R8_SRGB_PACK32,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_R16G16B16A16_UNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8_UNORM,            // 3-component, no alpha
        VK_FORMAT_G8B8G8R8_422_UNORM,      // 4:2:2, not 4:2:0
        VK_FORMAT_UNDEFINED,
    };
    for (VkFormat f : unsupported) {
        Check(VkEncClassifyInputFormat(f) == VK_ENC_INPUT_FORMAT_UNSUPPORTED,
              "classified UNSUPPORTED", "format " + U32((uint32_t)f));
        Check(VkEncSupportsInputFormat(f) == VK_FALSE,
              "not supported", "format " + U32((uint32_t)f));
        Check(VkEncInputFormatPlaneCount(f) == 0,
              "plane count 0", "format " + U32((uint32_t)f));
        Check(VkEncIsRgbaInputFormat(f) == VK_FALSE,
              "not claimed as RGBA", "format " + U32((uint32_t)f));
    }
}

void CaseYcbcrIsNeverClaimedAsRgba()
{
    g_currentCase = "no YCbCr input is mistaken for RGBA";
    // The two halves of ENCODABLE_VIA_FILTER must stay disjoint: the RGBA
    // predicate drives both the plane count and whether VerifyInputs()
    // re-derives input.vkFormat, so a YCbCr format leaking into it would be
    // described as a single 4-byte-per-pixel plane.
    const VkFormat ycbcr[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16,
    };
    for (VkFormat f : ycbcr) {
        Check(VkEncIsRgbaInputFormat(f) == VK_FALSE,
              "not claimed as RGBA", "format " + U32((uint32_t)f));
    }
}

//=============================================================================
// 2. The binder's disposition of inputFormat and enablePreprocessFilter
//=============================================================================

void CaseSemiPlanarBindsTwoPlanes()
{
    g_currentCase = "an NV12 session describes its input as 2-plane";
    // EncoderConfig does not store the input format; it reconstructs
    // input.vkFormat from subsampling, bit depth and numPlanes, so numPlanes
    // has to carry the caller's actual count. A default of 3 makes every ext
    // session describe its own input as 3-plane I420 whatever the caller
    // passed -- invisible until the compute filter is built from that
    // description.
    VkVideoEncoderConfig cfg = BaseConfig();
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "binder accepted an NV12 config",
          "VkResult " + U32((uint32_t)r));
    Check(probe.inputNumPlanes == 2,
          "input.numPlanes follows inputFormat",
          "got " + U32(probe.inputNumPlanes) + ", want 2");
    Check(probe.inputBpp == 8, "8-bit", "got " + U32(probe.inputBpp));
}

void CaseTenBitBindsBitDepthAndPlanes()
{
    g_currentCase = "a P010 session binds both bit depth and plane count";
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "binder accepted a P010 config",
          "VkResult " + U32((uint32_t)r));
    Check(probe.inputBpp == 10, "10-bit", "got " + U32(probe.inputBpp));
    Check(probe.inputNumPlanes == 2, "2 planes",
          "got " + U32(probe.inputNumPlanes));
}

void CaseFilterOffByDefault()
{
    g_currentCase = "a caller that did not ask gets no filter";
    // EncoderConfig's own default for enablePreprocessComputeFilter is TRUE,
    // so this is a positive write-down and not an inherited value. Without
    // it, InitEncoder would swap the input command-buffer pool for the
    // filter's compute-family pool behind a caller that never asked.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.enablePreprocessFilter = VK_FALSE;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "binder accepted the config",
          "VkResult " + U32((uint32_t)r));
    Check(probe.preprocessComputeFilter == 0,
          "enablePreprocessComputeFilter written DOWN, not inherited",
          "got " + U32(probe.preprocessComputeFilter));
}

void CaseFilterRequestIsHonouredNotRefused()
{
    g_currentCase = "enablePreprocessFilter is FORWARDED, not rejected";
    // THE CONTRACT. Forwarding this request is only admissible because the
    // conversion behind it is real for YCbCr plane-count mismatches; a flag
    // that converted nothing would encode unconverted input as if converted,
    // which is why the alternative is a distinct refusal and not silent
    // acceptance. Exactly two end states are admissible -- forwarded, or
    // rejected with an error that says so -- and this asserts which one is in
    // force.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.enablePreprocessFilter = VK_TRUE;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    if (kFilterCompiledIn) {
        Check(r == VK_SUCCESS, "the request is accepted",
              "VkResult " + U32((uint32_t)r));
        Check(probe.preprocessComputeFilter == 1,
              "it lands on enablePreprocessComputeFilter",
              "got " + U32(probe.preprocessComputeFilter));
    } else {
        // A build without the filter must still refuse rather than drop the
        // flag: "accepted and ignored" is the state ARCH_REVIEW:115 calls the
        // worst option, and it is the state this whole exercise removes.
        Check(r != VK_SUCCESS,
              "a build without the filter refuses rather than ignoring",
              "VkResult " + U32((uint32_t)r));
    }
}

void CaseThreePlaneWithoutFilterIsRefused()
{
    g_currentCase = "a 3-plane input without the filter is refused at INIT";
    // The gate that keeps a plane-count mismatch away from the transfer copy.
    // Refusing at init rather than at submit is the point: init is the last
    // moment at which the producer can still allocate its pool differently
    // (API_V2:622 -- answer before the producer allocates).
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
    cfg.enablePreprocessFilter = VK_FALSE;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r != VK_SUCCESS,
          "refused: nothing on this session could convert it",
          "VkResult " + U32((uint32_t)r));
}

void CaseThreePlaneWithFilterIsAccepted()
{
    g_currentCase = "a 3-plane input WITH the filter is accepted";
    // The case this whole change exists for: an I420 dma-buf producer that
    // the library adapts, rather than an embedder that converts with libyuv
    // (CONTEXT_DESIGN:577-579, :607-612).
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
    cfg.enablePreprocessFilter = VK_TRUE;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    if (kFilterCompiledIn) {
        Check(r == VK_SUCCESS, "accepted", "VkResult " + U32((uint32_t)r));
        Check(probe.preprocessComputeFilter == 1, "the filter is enabled",
              "got " + U32(probe.preprocessComputeFilter));
        Check(probe.inputNumPlanes == 3,
              "the session describes its input as 3-plane, which is what "
              "input.vkFormat -- and therefore the filter's input format -- "
              "is derived from",
              "got " + U32(probe.inputNumPlanes));
    } else {
        Check(r != VK_SUCCESS,
              "a build without the filter refuses a format only it could take",
              "VkResult " + U32((uint32_t)r));
    }
}

void CaseUnsupportedFormatStillRefused()
{
    g_currentCase = "an sRGB RGBA input is refused, filter or no filter";
    // Widening the taxonomy to the 8-bit UNORM RGBA family must NOT have
    // widened it to the _SRGB spellings. The conversion is matrix-only and an
    // sRGB view is linearised before the shader reads it, so accepting these
    // would be the silent, plausible-looking wrongness that is worse than a
    // refusal. Asking for the filter must not buy them either.
    for (VkBool32 wantFilter : {VK_FALSE, VK_TRUE}) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.inputFormat = VK_FORMAT_B8G8R8A8_SRGB;
        cfg.enablePreprocessFilter = wantFilter;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        Check(r != VK_SUCCESS, "refused",
              "enablePreprocessFilter=" + U32((uint32_t)wantFilter) +
                  " gave VkResult " + U32((uint32_t)r));
    }
}

void CaseRgbaWithoutFilterIsRefused()
{
    g_currentCase = "an RGBA input without the filter is refused at INIT";
    // Same gate the 3-plane family gets, for a stronger reason: a transfer
    // copy cannot perform a colour-space conversion at all, so accepting an
    // RGBA session with no filter would encode raw RGB bytes as if they were
    // luma and chroma.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_B8G8R8A8_UNORM;
    cfg.enablePreprocessFilter = VK_FALSE;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r != VK_SUCCESS,
          "refused: nothing on this session could convert it",
          "VkResult " + U32((uint32_t)r));
}

void CaseRgbaWithFilterIsAcceptedAsOnePlane()
{
    g_currentCase = "a BGRA8 input WITH the filter is accepted, as 1 plane";
    // The case the sampled-read arm exists for: a BGRA8 compositor buffer the
    // library adapts itself, rather than an embedder converting with libyuv.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_B8G8R8A8_UNORM;
    cfg.enablePreprocessFilter = VK_TRUE;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    if (kFilterCompiledIn) {
        Check(r == VK_SUCCESS, "accepted", "VkResult " + U32((uint32_t)r));
        Check(probe.preprocessComputeFilter == 1, "the filter is enabled",
              "got " + U32(probe.preprocessComputeFilter));
        // The session must describe its input as ONE plane. input.vkFormat --
        // which the compute filter's input format is taken from -- is derived
        // alongside this, and a 3 here would have built the filter to read
        // planes a BGRA import does not have.
        Check(probe.inputNumPlanes == 1,
              "the session describes its input as single-plane",
              "got " + U32(probe.inputNumPlanes));
        Check(probe.inputBpp == 8, "8-bit", "got " + U32(probe.inputBpp));
    } else {
        Check(r != VK_SUCCESS,
              "a build without the filter refuses a format only it could take",
              "VkResult " + U32((uint32_t)r));
    }
}

//=============================================================================
// 2c. The profile number
//=============================================================================

struct ProfileRow {
    VkVideoCodecOperationFlagBitsKHR codec;
    const char*                      codecName;
    uint32_t                         profile;
    const char*                      name;
};

void CaseProfileNumbersReachTheCodecConfigUnchanged()
{
    g_currentCase = "a profile number reaches the codec config unchanged";
    // THE CONTRACT. VkVideoEncoderConfig::profile carries the codec
    // standard's own number -- H.264 profile_idc, H.265 general_profile_idc,
    // AV1 seq_profile -- and the codec-typed config is what session creation
    // reads. Asserting equality end to end is what makes "the values come from
    // the standard" a property of the code rather than of the comment: a
    // translation table reintroduced between the two would show up here as a
    // number that changed on the way through.
    static const ProfileRow rows[] = {
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "H.264",
          VK_VIDEO_ENCODER_PROFILE_H264_BASELINE, "Baseline (66)" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "H.264",
          VK_VIDEO_ENCODER_PROFILE_H264_MAIN,     "Main (77)" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "H.264",
          VK_VIDEO_ENCODER_PROFILE_H264_HIGH,     "High (100)" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, "H.265",
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN,     "Main (1)" },
    };
    for (const ProfileRow& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec   = row.codec;
        cfg.profile = row.profile;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(cfg, row.codec, &probe);
        const std::string what =
            std::string(row.codecName) + " " + row.name;
        Check(r == VK_SUCCESS, (what + " is accepted").c_str(),
              "VkResult " + U32((uint32_t)r));
        Check(probe.codecProfile == row.profile,
              (what + " reaches the codec config as its own number").c_str(),
              "got " + U32(probe.codecProfile) + ", asked for " +
                  U32(row.profile));
    }
}

void CaseProfileDefaultIsDerivedPerCodec()
{
    g_currentCase = "DEFAULT derives a profile rather than binding one";
    // DEFAULT binds nothing; the codec config derives from the input depth.
    // On AV1 the number is also seq_profile 0, which is Main -- the overlap
    // the public header states -- so the derivation and the named profile
    // land on the same value for 8-bit input.
    static const ProfileRow rows[] = {
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "H.264",
          VK_VIDEO_ENCODER_PROFILE_DEFAULT, "DEFAULT" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, "H.265",
          VK_VIDEO_ENCODER_PROFILE_DEFAULT, "DEFAULT" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,  "AV1",
          VK_VIDEO_ENCODER_PROFILE_DEFAULT, "DEFAULT" },
    };
    for (const ProfileRow& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec   = row.codec;
        cfg.profile = row.profile;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(cfg, row.codec, &probe);
        Check(r == VK_SUCCESS,
              (std::string(row.codecName) + " accepts DEFAULT").c_str(),
              "VkResult " + U32((uint32_t)r));
    }
    // And AV1's overlap, asserted rather than described: 0 selects seq_profile
    // 0 on 8-bit input, which is Main.
    VkVideoEncoderConfig av1 = BaseConfig();
    av1.codec   = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    av1.profile = VK_VIDEO_ENCODER_PROFILE_DEFAULT;
    VkEncBoundConfigProbe aprobe{};
    Check(VkEncBuildAndProbeConfig(
              av1, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, &aprobe) ==
              VK_SUCCESS,
          "AV1 DEFAULT session builds", "init failed");
    Check(aprobe.codecProfile == VK_VIDEO_ENCODER_PROFILE_AV1_MAIN,
          "AV1 0 selects seq_profile 0 (Main) on 8-bit input",
          "got " + U32(aprobe.codecProfile));
}

void CaseProfileNumbersAreReadAgainstTheCodec()
{
    g_currentCase = "a profile number is read against the codec, not alone";
    // Standard profile numbers repeat across codecs: 1 is H.265 Main and is
    // not an H.264 profile_idc; 100 is H.264 High and is not an assigned H.265
    // general_profile_idc. A number that belongs to another codec must be
    // REFUSED, not bound and not ignored -- binding it would emit a bitstream
    // declaring a profile the caller never asked for, and ignoring it would
    // emit the library's default under the caller's label.
    struct Row { VkVideoCodecOperationFlagBitsKHR codec; uint32_t profile;
                 const char* why; };
    static const Row rows[] = {
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN,
          "H.265 Main (1) on an H.264 session" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H264_HIGH,
          "H.264 High (100) on an H.265 session" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, 122,
          "H.264 High 4:2:2 (122), which this library does not bind" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, 4,
          "H.265 Range Extensions (4), which this library does not bind" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, 1,
          "AV1 High (1), which this library does not bind" },
    };
    for (const Row& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec   = row.codec;
        cfg.profile = row.profile;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(cfg, row.codec, &probe);
        Check(r == VK_ERROR_INITIALIZATION_FAILED,
              (std::string("refused: ") + row.why).c_str(),
              "VkResult " + U32((uint32_t)r));
    }
}

void CaseProfileMustAdmitTheInputDepth()
{
    g_currentCase = "a profile the input depth does not admit is refused";
    // The standard's rule, enforced instead of worked around. Honouring an
    // 8-bit-only profile over 10-bit input would emit an out-of-spec
    // bitstream; overriding the request with a deeper profile would be the
    // same ignored request in the other direction.
    struct Row { VkVideoCodecOperationFlagBitsKHR codec; uint32_t profile;
                 VkFormat fmt; const char* why; };
    static const Row rows[] = {
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H264_HIGH,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
          "H.264 High (100) over 10-bit input" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
          "H.265 Main (1) over 10-bit input" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN10,
          VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,
          "H.265 Main 10 (2) over 12-bit input" },
    };
    for (const Row& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec       = row.codec;
        cfg.profile     = row.profile;
        cfg.inputFormat = row.fmt;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(cfg, row.codec, &probe);
        Check(r == VK_ERROR_INITIALIZATION_FAILED,
              (std::string("refused: ") + row.why).c_str(),
              "VkResult " + U32((uint32_t)r));
    }
    // The control: the same depth WITH a profile that admits it is accepted,
    // so the rows above measure the profile rule and not the bit depth.
    VkVideoEncoderConfig ok = BaseConfig();
    ok.codec       = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    ok.profile     = VK_VIDEO_ENCODER_PROFILE_H265_MAIN10;
    ok.inputFormat = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    VkEncBoundConfigProbe okProbe{};
    const VkResult okR = VkEncBuildAndProbeConfig(
        ok, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &okProbe);
    Check(okR == VK_SUCCESS,
          "H.265 Main 10 (2) over 10-bit input is accepted",
          "VkResult " + U32((uint32_t)okR));
    Check(okProbe.codecProfile == VK_VIDEO_ENCODER_PROFILE_H265_MAIN10,
          "and binds general_profile_idc 2",
          "got " + U32(okProbe.codecProfile));
}

//=============================================================================
// 3. The field table itself
//=============================================================================

void CaseFieldTableClassifiesTheFilterFlag()
{
    g_currentCase = "the field table records the new disposition";
    // Layer 1 of design section 4.2: every public field appears exactly once
    // with a disposition saying what happens to it. The table is what a
    // reviewer reads; a table that still said REJECTED while the binder
    // forwarded the field would be the same lie the previous row was written
    // to correct.
    bool found = false;
    for (const VkVideoEncoderConfigFieldInfo& f : kVkVideoEncoderConfigFields) {
        if (std::strcmp(f.name, "enablePreprocessFilter") != 0) {
            continue;
        }
        found = true;
        Check(f.disposition == VK_ENC_FIELD_BOUND,
              "enablePreprocessFilter is BOUND",
              "disposition " + U32((uint32_t)f.disposition));
    }
    Check(found, "enablePreprocessFilter appears in the field table", "absent");

    // And the shape assertion the table exists for: exactly one entry per
    // field, every offset inside the struct.
    Check((size_t)kVkEncCfgFieldCount ==
              sizeof(kVkVideoEncoderConfigFields) /
                  sizeof(kVkVideoEncoderConfigFields[0]),
          "table length matches the field enum", "mismatch");
    for (const VkVideoEncoderConfigFieldInfo& f : kVkVideoEncoderConfigFields) {
        Check(f.offset < sizeof(VkVideoEncoderConfig),
              "field offset is inside VkVideoEncoderConfig",
              std::string(f.name) + " at " + U32((uint32_t)f.offset));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    std::printf("Encoder-ext input-format taxonomy and filter disposition\n");
    std::printf("-------------------------------------------------------\n");
    std::printf("compute filter compiled in: %s\n",
                kFilterCompiledIn ? "yes" : "no");

    CaseSemiPlanarIsDirect();
    CaseThreePlaneIsViaFilter();
    CaseRgbaIsViaFilterAndSinglePlane();
    CaseSrgbAndJunkAreUnsupported();
    CaseYcbcrIsNeverClaimedAsRgba();

    CaseSemiPlanarBindsTwoPlanes();
    CaseTenBitBindsBitDepthAndPlanes();
    CaseFilterOffByDefault();
    CaseFilterRequestIsHonouredNotRefused();
    CaseThreePlaneWithoutFilterIsRefused();
    CaseThreePlaneWithFilterIsAccepted();
    CaseRgbaWithoutFilterIsRefused();
    CaseRgbaWithFilterIsAcceptedAsOnePlane();
    CaseUnsupportedFormatStillRefused();

    CaseProfileNumbersReachTheCodecConfigUnchanged();
    CaseProfileDefaultIsDerivedPerCodec();
    CaseProfileNumbersAreReadAgainstTheCodec();
    CaseProfileMustAdmitTheInputDepth();

    CaseFieldTableClassifiesTheFilterFlag();

    std::printf("-------------------------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
