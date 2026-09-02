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
 * Device-free coverage for the input-format taxonomy, the preprocess-
 * conversion decision the config binder derives from it, the transfer-function
 * declaration, and the COLOUR DESCRIPTION the binder and the preprocess filter
 * agree on.
 *
 * The colour half (section 3) was added with the four colour-description
 * defects it pins: a partially-supplied declaration that fabricated the
 * fields it was not given, an AV1 colour config that dropped full range on
 * the floor, an RGBA matrix contract that converted as BT.709 under someone
 * else's label, and chroma siting that was declared in one place and
 * signalled nowhere. It lives HERE rather than in a new harness because
 * three of the four are decided by this same binder, and the fourth is the
 * preprocess filter's own contract -- the subject of this file.
 *
 * WHAT THIS PINS. The adaptation ladder has three
 * rungs -- hardware conversion, compute filter, transfer copy -- in that
 * order, and states that falling to a copy where the device could have
 * converted is a defect. Two library-side decisions implement the second
 * rung, and both are pure functions of their arguments:
 *
 *   1. the FORMAT taxonomy: is a given input directly encodable, encodable
 *      only after a conversion, or neither;
 *   2. the CONFIG binder: does an input that NEEDS a conversion get one
 *      without asking, does an input that does not need one stay clear of it,
 *      and is a declared transfer-function mismatch refused rather than
 *      quietly encoded.
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
#include "VkVideoEncoder/VkEncoderConfig.h"

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
#include <iostream>
#include <sstream>
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
// 1. The input taxonomy (VkEncClassifyInput / VkEncSupportsInput)
//=============================================================================

void CaseSemiPlanarIsDirect()
{
    g_currentCase = "8- and 10-bit semi-planar is encodable DIRECTLY";
    // Both subsamplings, because the rung is the LAYOUT and the DEPTH and not
    // the subsampling: a driver reports semi-planar 4:4:4 as an encode source
    // for its 4:4:4 profiles exactly as it reports 4:2:0 for its 4:2:0 ones,
    // and the encode profile follows the input rather than the other way
    // round. A 4:4:4 input classified anything but DIRECT would be converted
    // by the filter into a format the encoder already took.
    const VkFormat direct[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                    // NV12
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,   // P010
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,                    // NV24
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16,   // S410
    };
    for (VkFormat f : direct) {
        Check(VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
                  VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
              "classified DIRECT", "format " + U32((uint32_t)f));
        Check(VkEncSupportsInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) == VK_TRUE,
              "supported", "format " + U32((uint32_t)f));
        Check(VkEncInputFormatPlaneCount(f) == 2,
              "2 planes", "format " + U32((uint32_t)f) + " -> " +
                              U32(VkEncInputFormatPlaneCount(f)));
    }
}
void CaseTwelveBitSemiPlanarIsViaFilter()
{
    g_currentCase = "12-bit semi-planar 4:2:0 is encodable only VIA THE FILTER";
    // P012 already has the encode format's plane layout and subsampling, so
    // the plane-count argument that puts the 3-plane family on the filter does
    // not apply to it. What does apply is bit depth: no encode-source table
    // carries a 12-bit row, so the depth has to be converted before the
    // encoder sees the frame, and conversion is the filter's work.
    const VkFormat f = VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
    Check(VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
              VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER,
          "classified VIA_FILTER", "P012");
    Check(VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) !=
              VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
          "NOT classified DIRECT", "P012");
    Check(VkEncInputFormatPlaneCount(f) == 2,
          "2 planes -- the layout was never the mismatch", "P012");
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
        Check(VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
                  VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER,
              "classified VIA_FILTER", "format " + U32((uint32_t)f));
        Check(VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) !=
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
    // The filter binds the RGBA source as ONE combined view, declared
    // VK_DESCRIPTOR_TYPE_STORAGE_IMAGE and read with imageLoad. The component
    // order comes from the view's VkFormat rather than from a GLSL format
    // qualifier, which is what lets ONE shader take all three spellings below
    // -- notably B8G8R8A8_UNORM, which has no `bgra8` qualifier to declare.
    const VkFormat viaFilter[] = {
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,
    };
    for (VkFormat f : viaFilter) {
        Check(VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
                  VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER,
              "classified VIA_FILTER", "format " + U32((uint32_t)f));
        Check(VkEncResolveColorModel(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
                  VK_VIDEO_ENCODER_COLOR_MODEL_RGB,
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

void CaseDeclaredColorModelDecidesTheClass()
{
    g_currentCase = "the declared colour model decides the class";
    // A VkFormat names a component layout. For every format but the packed
    // 4:4:4 Y'CbCr layouts the layout implies exactly one colour model, and
    // FROM_FORMAT -- what a zero-initialised structure says -- asks for it.
    // Declaring the model the format already implies changes nothing.
    const VkFormat nv12 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    Check(VkEncClassifyInput(nv12, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
              VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
          "NV12 undeclared is DIRECT", "NV12");
    Check(VkEncClassifyInput(nv12, VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR) ==
              VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
          "NV12 declared Y'CbCr is DIRECT -- the declaration agrees", "NV12");
    const VkFormat rgba = VK_FORMAT_R8G8B8A8_UNORM;
    Check(VkEncClassifyInput(rgba, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
              VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER,
          "RGBA8 undeclared is VIA_FILTER", "R8G8B8A8_UNORM");
    Check(VkEncClassifyInput(rgba, VK_VIDEO_ENCODER_COLOR_MODEL_RGB) ==
              VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER,
          "RGBA8 declared RGB is VIA_FILTER -- the declaration agrees",
          "R8G8B8A8_UNORM");

    // A DECLARATION THE FORMAT CANNOT CARRY IS REFUSED, NOT RECONCILED. This
    // is the whole of what the declaration buys: without it the library reads
    // the format and cannot be told it is wrong, so a caller holding luma and
    // chroma in an RGBA-spelled image has no way to say so and no way to be
    // refused when it says something impossible.
    Check(VkEncClassifyInput(nv12, VK_VIDEO_ENCODER_COLOR_MODEL_RGB) ==
              VK_ENC_INPUT_FORMAT_UNSUPPORTED,
          "RGB declared over a Y'CbCr format is refused", "NV12 as RGB");
    Check(VkEncClassifyInput(VK_FORMAT_B8G8R8A8_UNORM,
                             VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR) ==
              VK_ENC_INPUT_FORMAT_UNSUPPORTED,
          "Y'CbCr declared over an RGBA layout with no packed reading is "
          "refused", "BGRA8 as Y'CbCr");
    Check(VkEncResolveColorModel(VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
                                 VK_VIDEO_ENCODER_COLOR_MODEL_RGB) ==
              VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT,
          "an unresolvable pair resolves to no model at all", "I420 as RGB");

    // THE PACKED 4:4:4 LAYOUTS ARE THE ONE PLACE A DISAGREEMENT IS REAL. AYUV,
    // Y410 and Y416 have no Vulkan enumerant of their own and ride the RGBA
    // ones, so a Y'CbCr declaration over them is a statement of fact and is
    // resolved as such. Whether the library can then ROUTE that input is a
    // separate question, asked of VkEncClassifyInput.
    Check(VkEncResolveColorModel(rgba, VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR) ==
              VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR,
          "AYUV declared Y'CbCr resolves to Y'CbCr", "R8G8B8A8_UNORM");
    Check(VkEncResolveColorModel(rgba,
                                 VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
              VK_VIDEO_ENCODER_COLOR_MODEL_RGB,
          "the same enumerant undeclared is still RGB", "R8G8B8A8_UNORM");
    Check(VkEncResolveColorModel(VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                 VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR) ==
              VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR,
          "Y410 declared Y'CbCr resolves to Y'CbCr",
          "A2B10G10R10_UNORM_PACK32");
    Check(VkEncResolveColorModel(VK_FORMAT_R16G16B16A16_UNORM,
                                 VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR) ==
              VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR,
          "Y416 declared Y'CbCr resolves to Y'CbCr", "R16G16B16A16_UNORM");
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
    //  A2B10G10R10 / R16G16B16A16_UNORM: these same enumerants are how Y410
    //    and Y416 are spelled. Undeclared they read as RGB, and as RGB the
    //    library does not route them; a Y'CbCr declaration over them says
    //    something else entirely and is judged separately.
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
        Check(VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) == VK_ENC_INPUT_FORMAT_UNSUPPORTED,
              "classified UNSUPPORTED", "format " + U32((uint32_t)f));
        Check(VkEncSupportsInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) == VK_FALSE,
              "not supported", "format " + U32((uint32_t)f));
        Check(VkEncInputFormatPlaneCount(f) == 0,
              "plane count 0", "format " + U32((uint32_t)f));
        // Declaring the model does not widen the set. Naming RGB over an sRGB
        // or scRGB layout states what the layout already says and is still
        // refused; naming it over a Y'CbCr format is a contradiction and is
        // refused for that reason instead. Either way the answer is the same,
        // which is what makes the refusal a decision rather than a gap.
        Check(VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_RGB) ==
                  VK_ENC_INPUT_FORMAT_UNSUPPORTED,
              "still UNSUPPORTED when RGB is declared over it",
              "format " + U32((uint32_t)f));
    }
}

// The advertised input-format list, which is the DEVICE's list reduced to the
// library's answer. Driven with a synthetic device list because the reduction
// is a pure function and the interesting inputs -- a format the library
// refuses, one format reported twice -- are not what every device reports.
void CaseAdvertisedListDropsWhatTheLibraryRefuses()
{
    g_currentCase = "the advertised list drops what the library refuses";
    const VkFormat deviceList[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,     // routed
        VK_FORMAT_G8B8G8R8_422_UNORM,           // packed 4:2:2, refused
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,     // routed
        VK_FORMAT_R16G16B16A16_SFLOAT,          // refused
    };
    VkFormat out[VK_VIDEO_ENCODER_MAX_INPUT_FORMATS] = {};
    const uint32_t n = VkEncFilterAdvertisedInputFormats(
        deviceList, 4, out, VK_VIDEO_ENCODER_MAX_INPUT_FORMATS);
    Check(n == 2, "two of four survive", "got " + U32(n));
    Check((n > 0) && (out[0] == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM),
          "and the device order is kept", "first entry");
    Check((n > 1) && (out[1] == VK_FORMAT_G8_B8R8_2PLANE_444_UNORM),
          "and the device order is kept", "second entry");
    // The refusals are the point: neither may appear anywhere in the answer.
    for (uint32_t i = 0; i < n; i++) {
        Check((out[i] != VK_FORMAT_G8B8G8R8_422_UNORM) &&
                  (out[i] != VK_FORMAT_R16G16B16A16_SFLOAT),
              "and no refused format is advertised",
              "entry " + U32(i) + " = " + U32((uint32_t)out[i]));
    }
}

void CaseAdvertisedListPassesWhatTheLibraryRoutes()
{
    g_currentCase = "the advertised list keeps every format the library routes";
    // The positive control for the case above: a reduction that answered
    // nothing would pass it just as well.
    const VkFormat deviceList[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16,
    };
    VkFormat out[VK_VIDEO_ENCODER_MAX_INPUT_FORMATS] = {};
    const uint32_t n = VkEncFilterAdvertisedInputFormats(
        deviceList, 4, out, VK_VIDEO_ENCODER_MAX_INPUT_FORMATS);
    Check(n == 4, "all four survive", "got " + U32(n));
    for (uint32_t i = 0; (i < n) && (i < 4); i++) {
        Check(out[i] == deviceList[i], "unchanged, in order",
              "entry " + U32(i));
    }
}

void CaseAdvertisedListReportsOneFormatOnce()
{
    g_currentCase = "one format reported at two tilings is advertised once";
    const VkFormat deviceList[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
    };
    VkFormat out[VK_VIDEO_ENCODER_MAX_INPUT_FORMATS] = {};
    const uint32_t n = VkEncFilterAdvertisedInputFormats(
        deviceList, 4, out, VK_VIDEO_ENCODER_MAX_INPUT_FORMATS);
    Check(n == 2, "two distinct formats", "got " + U32(n));
    Check((n > 1) && (out[0] == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) &&
              (out[1] ==
               VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16),
          "and the surviving entries are the distinct ones", "");
}

void CaseAdvertisedListStopsAtCapacity()
{
    g_currentCase = "the advertised list never writes past its capacity";
    const VkFormat deviceList[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
    };
    VkFormat out[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                        VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
    const uint32_t n = VkEncFilterAdvertisedInputFormats(deviceList, 3, out, 2);
    Check(n == 2, "clamped to the capacity given", "got " + U32(n));
    Check(out[2] == VK_FORMAT_UNDEFINED, "and wrote nothing past it",
          "slot 2 = " + U32((uint32_t)out[2]));
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
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16,
    };
    for (VkFormat f : ycbcr) {
        Check(VkEncResolveColorModel(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) !=
                  VK_VIDEO_ENCODER_COLOR_MODEL_RGB,
              "not claimed as RGBA", "format " + U32((uint32_t)f));
    }
}

void CaseRgbaSessionSurvivesTheSinglePlaneGate()
{
    g_currentCase = "an RGBA session is accepted by VerifyInputs";
    // An RGBA input is ONE plane of four bytes per pixel and carries no chroma
    // subsampling of its own, so |chromaSubsampling| keeps the constructor
    // default of 4:2:0 on this path. VerifyInputs() also refuses a single-plane
    // input that is not 4:4:4, because a single-plane Y'CbCr image is packed
    // and packed is only defined for 4:4:4 here. The two are compatible only
    // while the RGBA arm is reached first: an RGBA image is not a packed
    // Y'CbCr image and the subsampling field does not describe it.
    //
    // This case pins that order. With the arms transposed the code still
    // compiles and links, and every RGBA input to the preprocess filter is
    // refused at configuration time.
    // The list carries A2B10G10R10_UNORM_PACK32 on purpose. It is an RGB
    // layout the library does not route, so the guard below skips it -- and
    // the guard is what this list is here to exercise: an RGB layout is not
    // by itself an RGBA input, and sizing a session from one that is refused
    // would describe a plane no allocation is made for.
    for (VkFormat f : { VK_FORMAT_R8G8B8A8_UNORM,
                        VK_FORMAT_B8G8R8A8_UNORM,
                        VK_FORMAT_A2B10G10R10_UNORM_PACK32 }) {
        if (VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_RGB) ==
            VK_ENC_INPUT_FORMAT_UNSUPPORTED) {
            continue;
        }
        EncoderInputImageParameters input;
        input.width      = 256;
        input.height     = 256;
        input.numPlanes  = VkEncInputFormatPlaneCount(f);
        input.vkFormat   = f;
        input.colorSpace = VkEncColorSpace::kRGB;

        Check(input.numPlanes == 1,
              "RGBA input is a single plane", "format " + U32((uint32_t)f));
        Check(input.chromaSubsampling == VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
              "no chroma subsampling is written on the RGBA path",
              "format " + U32((uint32_t)f));
        Check(input.VerifyInputs(),
              "VerifyInputs accepts the RGBA session",
              "format " + U32((uint32_t)f));
        Check(input.vkFormat == f,
              "the caller format is carried, not re-derived",
              "format " + U32((uint32_t)f));
        Check(input.planeLayouts[0].rowPitch >= (4u * input.width),
              "the row pitch is four bytes per pixel",
              "format " + U32((uint32_t)f));
    }

    // The control: a genuinely packed Y'CbCr single plane that is NOT 4:4:4 is
    // still refused, so the case above is not passing because the gate is inert.
    EncoderInputImageParameters packed422;
    packed422.width             = 256;
    packed422.height            = 256;
    packed422.numPlanes         = 1;
    packed422.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR;
    packed422.colorSpace        = VkEncColorSpace::kYCbCr;
    Check(!packed422.VerifyInputs(),
          "packed single-plane 4:2:2 is still refused", "control");
}


//=============================================================================
// 2. The binder's preprocess-conversion decision, derived from inputFormat
//=============================================================================

// What the library wrote to the gated error stream while |fn| ran. Two
// separate gates in the binder refuse the same config, and only the wording
// says which one did -- so the wording is the contract under test.
template <typename Fn>
std::string CaptureEncErr(Fn fn)
{
    std::ostringstream sink;
    std::streambuf* saved = std::cerr.rdbuf(sink.rdbuf());
    fn();
    std::cerr.rdbuf(saved);
    return sink.str();
}

void CaseContradictoryColorModelIsRefusedByTheBinder()
{
    g_currentCase = "a contradictory inputColorModel is refused by the binder";
    // The registration gate refuses a (format, colorModel) pair that cannot
    // be reconciled. Configuring a SESSION from one is refused for the same
    // reason and says the same thing, or the library answers one
    // contradiction in two voices -- and the voice is what the caller acts
    // on, because the FORMAT in these pairs is the half that is not wrong.
    struct Row {
        VkFormat                 format;
        VkVideoEncoderColorModel declared;
        const char*              why;
    };
    static const Row rows[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR,
          "YCbCr declared over BGRA8, which carries no packed reading" },
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_RGB,
          "RGB declared over NV12, which is directly encodable" },
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          (VkVideoEncoderColorModel)7,
          "a colorModel value the enumeration does not define" },
    };
    for (const Row& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.inputFormat     = row.format;
        cfg.inputColorModel = row.declared;
        VkEncBoundConfigProbe probe{};
        VkResult r = VK_SUCCESS;
        const std::string said = CaptureEncErr([&] {
            r = VkEncBuildAndProbeConfig(
                cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        });
        Check(r == VK_ERROR_INITIALIZATION_FAILED,
              (std::string("the binder refuses it: ") + row.why).c_str(),
              "VkResult " + U32((uint32_t)r));
        Check(said.find("inputColorModel") != std::string::npos,
              (std::string("and names the declaration as the reason: ")
               + row.why).c_str(),
              "said: " + said);
        // The control on the wording. The unencodable-format message lists
        // B8G8R8A8_UNORM among the formats it accepts, so answering the
        // first row with it names the submitted format as both refused and
        // accepted, and sends the caller to change the one field that is
        // correct.
        Check(said.find("is not encodable") == std::string::npos,
              (std::string("and not as an unencodable format: ")
               + row.why).c_str(),
              "said: " + said);
    }
}

void CaseAgreeingColorModelStillBinds()
{
    g_currentCase = "an unstated or agreeing inputColorModel still binds";
    // VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT is 0, so this is what every
    // caller that never set the field declares, and it must configure
    // exactly as it always did -- a gate that caught it would refuse the
    // entire installed base. Naming the model the format already implies
    // must bind too: what is refused is a CONTRADICTION, not a declaration.
    struct Row {
        VkFormat                 format;
        VkVideoEncoderColorModel declared;
        uint32_t                 wantPlanes;
        uint32_t                 wantFilter;
        bool                     needsFilterBuild;
        const char*              why;
    };
    static const Row rows[] = {
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, 2, 0, false,
          "NV12 declaring nothing" },
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR, 2, 0, false,
          "NV12 declared YCbCr" },
        { VK_FORMAT_B8G8R8A8_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT, 1, 1, true,
          "BGRA8 declaring nothing" },
        { VK_FORMAT_B8G8R8A8_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_RGB, 1, 1, true,
          "BGRA8 declared RGB" },
    };
    for (const Row& row : rows) {
        if (row.needsFilterBuild && !kFilterCompiledIn) {
            continue;
        }
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.inputFormat     = row.format;
        cfg.inputColorModel = row.declared;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        Check(r == VK_SUCCESS,
              (std::string("the binder accepts it: ") + row.why).c_str(),
              "VkResult " + U32((uint32_t)r));
        Check(probe.inputNumPlanes == row.wantPlanes,
              (std::string("and lays out the input it named: ")
               + row.why).c_str(),
              "planes " + U32(probe.inputNumPlanes) + ", want "
                  + U32(row.wantPlanes));
        Check(probe.preprocessComputeFilter == row.wantFilter,
              (std::string("and routes it the same way: ") + row.why).c_str(),
              "filter " + U32(probe.preprocessComputeFilter) + ", want "
                  + U32(row.wantFilter));
    }
}

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

void CaseFourFourFourBindsItsOwnSubsampling()
{
    g_currentCase = "a 4:4:4 session binds 4:4:4, 2 planes and no filter";
    // input.chromaSubsampling is what the codec arm derives the encode
    // profile from, and it is what an encoder writes into the bitstream's
    // chroma_format_idc. Left at the 4:2:0 default, a 4:4:4 request encodes
    // as 4:2:0 and reports success -- a wrong bitstream rather than a
    // refusal, which is why this is asserted rather than assumed from the
    // plane count. The layout is unchanged from 4:2:0 semi-planar, so the
    // plane count alone cannot tell the two apart.
    const struct { VkFormat format; uint32_t bpp; const char* name; } rows[] = {
        { VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,                   8, "NV24" },
        { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16, 10, "S410" },
    };
    for (const auto& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.inputFormat = row.format;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        Check(r == VK_SUCCESS, "binder accepted the config",
              std::string(row.name) + " VkResult " + U32((uint32_t)r));
        Check(probe.inputChromaSubsampling ==
                  (uint32_t)VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR,
              "input.chromaSubsampling follows inputFormat",
              std::string(row.name) + " got " +
                  U32(probe.inputChromaSubsampling) + ", want " +
                  U32((uint32_t)VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR));
        Check(probe.inputNumPlanes == 2, "2 planes",
              std::string(row.name) + " got " + U32(probe.inputNumPlanes));
        Check(probe.inputBpp == row.bpp, "bit depth follows inputFormat",
              std::string(row.name) + " got " + U32(probe.inputBpp));
        Check(probe.preprocessComputeFilter == 0,
              "no preprocess filter is built for a directly encodable input",
              std::string(row.name) + " got " +
                  U32(probe.preprocessComputeFilter));
    }
}

// The control for the case above: the 4:2:0 semi-planar set must still bind
// 4:2:0, so a pass there is not a projection that answers 4:4:4 for
// everything.
void CaseFourTwoZeroStillBindsFourTwoZero()
{
    g_currentCase = "a 4:2:0 session still binds 4:2:0";
    const VkFormat rows[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
    };
    for (VkFormat f : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.inputFormat = f;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        Check(r == VK_SUCCESS, "binder accepted the config",
              "format " + U32((uint32_t)f) + " VkResult " + U32((uint32_t)r));
        Check(probe.inputChromaSubsampling ==
                  (uint32_t)VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
              "input.chromaSubsampling is 4:2:0",
              "format " + U32((uint32_t)f) + " got " +
                  U32(probe.inputChromaSubsampling));
    }
}

void CaseDirectFormatGetsNoFilter()
{
    g_currentCase = "a directly encodable input gets NO filter";
    // EncoderConfig's own default for enablePreprocessComputeFilter is TRUE,
    // so this is a positive write-down and not an inherited value. Without
    // it, InitEncoder would swap the input command-buffer pool for the
    // filter's compute-family pool on a session with nothing to convert.
    VkVideoEncoderConfig cfg = BaseConfig();   // NV12
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "binder accepted the config",
          "VkResult " + U32((uint32_t)r));
    Check(probe.preprocessComputeFilter == 0,
          "enablePreprocessComputeFilter written DOWN, not inherited",
          "got " + U32(probe.preprocessComputeFilter));
}

void CaseThreePlaneGetsAFilterWithoutAsking()
{
    g_currentCase = "a 3-plane input gets a filter without asking for one";
    // The case the derivation exists for: an I420 dma-buf producer the
    // library adapts, rather than an embedder that converts before it submits.
    // Nothing on the config requests the conversion -- the format is the whole
    // input to the decision.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
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
        // A build with no filter must refuse rather than encode the frame
        // unconverted: with no conversion the frame falls to the staging
        // copy, whose two-region copy from a three-plane source is a GPU
        // hang and not a slower path.
        Check(r != VK_SUCCESS,
              "a build without the filter refuses a format only it could take",
              "VkResult " + U32((uint32_t)r));
    }
}

void CaseRgbaGetsAFilterWithoutAsking()
{
    g_currentCase = "a BGRA8 input gets a filter without asking, as 1 plane";
    // A BGRA8 compositor buffer the library adapts itself. The stronger of the
    // two conversion cases: a transfer copy cannot perform a colour-model
    // conversion at all, so encoding this without a filter would encode raw
    // RGB bytes as if they were luma and chroma.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_B8G8R8A8_UNORM;
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

void CaseUnsupportedFormatStillRefused()
{
    g_currentCase = "an sRGB RGBA input is refused";
    // The 8-bit UNORM RGBA family is convertible; the _SRGB spellings of the
    // same formats are not. The conversion is matrix-only and an sRGB view is
    // linearised by the sampler before the shader reads it, which is an input
    // transfer function this library cannot honour -- so accepting these would
    // be the silent, plausible-looking wrongness that is worse than a refusal.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r != VK_SUCCESS, "refused", "VkResult " + U32((uint32_t)r));
}

#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
const char* FilterArmName(VulkanFilterYuvCompute::FilterType arm)
{
    switch (arm) {
    case VulkanFilterYuvCompute::RGBA2YCBCR: return "RGBA2YCBCR";
    case VulkanFilterYuvCompute::YCBCR2RGBA: return "YCBCR2RGBA";
    case VulkanFilterYuvCompute::YCBCRCOPY:  return "YCBCRCOPY";
    case VulkanFilterYuvCompute::YCBCRCLEAR: return "YCBCRCLEAR";
    default: break;
    }
    return "unknown";
}
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED

void CaseFilterArmReadsTheDeclaredColourModel()
{
    g_currentCase = "the filter arm reads the declared colour model";
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    // THE CONTRACT. VkEncDeriveFilterType decides which conversion the
    // preprocess filter performs from the colour model the input is DECLARED
    // to carry and the format the device accepts as an encode source, and it
    // reads each side by what the surface MEANS rather than by which format
    // table places its enumerant.
    //
    // Both sides matter, and the packed 4:4:4 layouts are why. AYUV, Y410 and
    // Y416 have no Vulkan format of their own and ride R8G8B8A8_UNORM,
    // A2B10G10R10_UNORM_PACK32 and R16G16B16A16_UNORM, which are also the
    // formats an ordinary R'G'B' frame is spelled with. A derivation that
    // read the enumerant alone would call a packed encode source R'G'B' and
    // convert a Y'CbCr input through the inverse matrix, and would call a
    // packed INPUT R'G'B' and convert it through the forward one -- writing
    // R, G and B into the channels the encoder reads as Cr, Cb and Y. Neither
    // raises an error, because the inverse conversion emits the very
    // enumerant the packed encode source is spelled with.
    //
    // WHAT THIS PINS AND WHAT IT DOES NOT. This is the decision, not the
    // pixels: the arm this asserts is the arm the encoder builds, but whether
    // the built shader then writes the right samples is a hardware question,
    // and no device that advertises a packed 4:4:4 encode source is required
    // to exist for this case to run. It reads only its arguments.
    struct Row {
        VkEncColorSpace                    declared;
        VkFormat                           encodeSource;
        VulkanFilterYuvCompute::FilterType expected;
        const char*                        why;
    };
    static const Row rows[] = {
        { VkEncColorSpace::kYCbCr, VK_FORMAT_R8G8B8A8_UNORM,
          VulkanFilterYuvCompute::YCBCRCOPY,
          "Y'CbCr in, AYUV encode source: both sides are Y'CbCr, so it is a "
          "copy and no matrix is applied" },
        { VkEncColorSpace::kYCbCr, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
          VulkanFilterYuvCompute::YCBCRCOPY,
          "Y'CbCr in, Y410 encode source" },
        { VkEncColorSpace::kYCbCr, VK_FORMAT_R16G16B16A16_UNORM,
          VulkanFilterYuvCompute::YCBCRCOPY,
          "Y'CbCr in, Y416 encode source" },
        // The planar controls. They differ from the three rows above in the
        // encode-source format ONLY, so a packed row that passed because the
        // whole derivation had collapsed onto one answer would be caught by
        // the R'G'B' rows below rather than hidden by these.
        { VkEncColorSpace::kYCbCr, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VulkanFilterYuvCompute::YCBCRCOPY,
          "Y'CbCr in, semi-planar 4:2:0 encode source" },
        { VkEncColorSpace::kYCbCr, VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
          VulkanFilterYuvCompute::YCBCRCOPY,
          "Y'CbCr in, semi-planar 4:4:4 encode source" },
        // An R'G'B' input takes the matrix, and takes it whether the encode
        // source is planar or packed.
        { VkEncColorSpace::kRGB, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VulkanFilterYuvCompute::RGBA2YCBCR,
          "R'G'B' in, semi-planar 4:2:0 encode source: the matrix conversion" },
        { VkEncColorSpace::kRGB, VK_FORMAT_R8G8B8A8_UNORM,
          VulkanFilterYuvCompute::RGBA2YCBCR,
          "R'G'B' in, AYUV encode source: still the matrix conversion" },
    };
    for (const Row& row : rows) {
        const VulkanFilterYuvCompute::FilterType arm =
            VkEncDeriveFilterType(row.declared, row.encodeSource);
        Check(arm == row.expected, row.why,
              std::string("derived ") + FilterArmName(arm) + ", expected " +
                  FilterArmName(row.expected));
    }

    // THE CALIBRATION. Every row above is a positive expectation, and a
    // derivation that had collapsed onto a single answer would satisfy half
    // of them. This pair does not: it holds the encode-source format fixed
    // and changes ONLY the declared colour model, so it can be satisfied only
    // by a derivation that actually reads the declaration.
    Check(VkEncDeriveFilterType(VkEncColorSpace::kRGB,
                                VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) !=
          VkEncDeriveFilterType(VkEncColorSpace::kYCbCr,
                                VK_FORMAT_G8_B8R8_2PLANE_420_UNORM),
          "the declared colour model, and nothing else, decides the arm",
          "both declarations derived the same arm");

    // No encode source the library can select is R'G'B', so the inverse arm
    // is not an answer the encoder ever wants: it forces the filter output to
    // an RGBA format, which for AYUV is the encode source's own enumerant.
    for (const Row& row : rows) {
        Check(VkEncDeriveFilterType(row.declared, row.encodeSource) !=
                  VulkanFilterYuvCompute::YCBCR2RGBA,
              "no encode source derives the inverse arm", row.why);
    }
#else
    // With no filter compiled in there is no arm to derive; the binder
    // refuses every format that would need one, which the cases above assert.
    Check(true, "no preprocess filter is compiled into this build", "");
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
}

//=============================================================================
// 2b. The transfer-function declaration
//=============================================================================

void CaseUndeclaredInputOtfAssertsNothing()
{
    g_currentCase = "inputTransferCharacteristics 0 declares nothing";
    // 0 is "not declared". It must not be read as a code point and compared,
    // or every caller that never heard of the field would be refused the
    // moment it declared a bitstream transfer function.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.colourPrimaries              = 1;
    cfg.transferCharacteristics      = 16;   // PQ on the bitstream
    cfg.matrixCoefficients           = 1;
    cfg.inputTransferCharacteristics = 0;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "accepted", "VkResult " + U32((uint32_t)r));
    Check(probe.transferCharacteristics == 16,
          "and the bitstream still declares what it was given",
          "got " + U32(probe.transferCharacteristics));
}

void CaseAgreeingOtfDeclarationsAreAccepted()
{
    g_currentCase = "input and bitstream declaring the same OTF is accepted";
    // The shape a caller uses to state the requirement explicitly, which is
    // the whole point of the field: both ends named, and they agree.
    for (uint8_t tc : { (uint8_t)1, (uint8_t)16, (uint8_t)18 }) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.colourPrimaries              = 1;
        cfg.transferCharacteristics      = tc;
        cfg.matrixCoefficients           = 1;
        cfg.inputTransferCharacteristics = tc;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        Check(r == VK_SUCCESS,
              ("transfer function " + U32(tc) + " on both ends is accepted")
                  .c_str(),
              "VkResult " + U32((uint32_t)r));
    }
}

void CaseMismatchedOtfDeclarationIsRefused()
{
    g_currentCase = "a declared OTF mismatch is refused, not converted";
    // THE CONTRACT THIS PINS. The library applies a colour matrix and no
    // transfer function, so it cannot serve an input in one transfer function
    // and a bitstream in another. Converting anyway would emit pixels that are
    // close enough to look plausible and wrong everywhere; the refusal is the
    // only honest answer.
    struct Row { uint8_t in; uint8_t out; const char* why; };
    static const Row rows[] = {
        { 16, 1,  "PQ input, BT.709 bitstream" },
        { 1,  16, "BT.709 input, PQ bitstream" },
        { 18, 16, "HLG input, PQ bitstream" },
        { 1,  0,  "declared input OTF, undeclared bitstream OTF" },
    };
    for (const Row& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.colourPrimaries              = 1;
        cfg.transferCharacteristics      = row.out;
        cfg.matrixCoefficients           = 1;
        cfg.inputTransferCharacteristics = row.in;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        Check(r == VK_ERROR_INITIALIZATION_FAILED,
              (std::string("refused: ") + row.why).c_str(),
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

void CaseFieldTableClassifiesEveryField()
{
    g_currentCase = "the field table classifies every field, once";
    // Every public config field appears in the table exactly once, with a
    // disposition saying what happens to it. A disposition that disagrees
    // with the binder is the same defect as a field that is accepted and
    // ignored.
    //
    // The transfer-function declaration is the one VALIDATED field: it is read
    // as a requirement and checked, and it reaches EncoderConfig nowhere,
    // because this library applies no transfer function and so has nothing to
    // forward.
    bool foundOtf = false;
    bool foundColorModel = false;
    size_t nRemoved = 0;
    for (const VkVideoEncoderConfigFieldInfo& f : kVkVideoEncoderConfigFields) {
        if (std::strcmp(f.name, "inputTransferCharacteristics") == 0) {
            foundOtf = true;
            Check(f.disposition == VK_ENC_FIELD_VALIDATED,
                  "inputTransferCharacteristics is VALIDATED",
                  "disposition " + U32((uint32_t)f.disposition));
        }
        // The other half of the pair a session is declared in. BOUND, not
        // VALIDATED: the resolved model lands in EncoderConfig as
        // input.colorSpace and the input geometry follows from it, which is
        // what the binder cases above read back off the probe. A struct field
        // with no row here is the one thing the shape assertions below cannot
        // see, so the field the whole routing turns on is named explicitly.
        if (std::strcmp(f.name, "inputColorModel") == 0) {
            foundColorModel = true;
            Check(f.disposition == VK_ENC_FIELD_BOUND,
                  "inputColorModel is BOUND",
                  "disposition " + U32((uint32_t)f.disposition));
        }
        // The preprocess conversion is not a caller-settable field any more.
        // Stated as a count over the whole table rather than as an absence, so
        // a table that stopped being iterated would not read as a pass.
        if (std::strcmp(f.name, "enablePreprocessFilter") == 0) {
            nRemoved++;
        }
    }
    Check(foundOtf, "inputTransferCharacteristics appears in the field table",
          "absent");
    Check(foundColorModel, "inputColorModel appears in the field table",
          "absent");
    Check(nRemoved == 0,
          "no field asks the caller to decide the preprocess conversion",
          U32((uint32_t)nRemoved) + " of " +
              U32((uint32_t)kVkEncCfgFieldCount) + " fields still do");

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


//=============================================================================
// 3. The colour description: what the binder writes, and what the RGBA arm's
//    matrix contract does with a code point it cannot produce.
//
// Every case here is device-free for the same reason the ones above are: the
// binder, EncoderConfig::ResolveRgbToYcbcrMatrix and
// EncoderConfigAV1::InitSequenceHeader read only their arguments. What they
// CANNOT prove is that the driver then writes those values into the VUI or
// the sequence header; that is the GPU suite's job
// (test/encoder-ext-format-encode, which reads them back with ffprobe).
//=============================================================================

// ISO/IEC 23091-4 code points used below, named so the expectations read as
// claims about colour rather than about integers.
enum {
    kCpUnspecified = 2,
    kCpBt709       = 1,
    kCpBt2020      = 9,
    kTcBt709       = 1,
    kTcPq          = 16,   // SMPTE ST 2084
    kTcHlg         = 18,   // ARIB STD-B67
    kMcBt709       = 1,
    kMcBt2020Ncl   = 9,
    kMcSmpte240M   = 7,
};

void CasePartialColourSupplyDoesNotFabricateTheRest()
{
    g_currentCase = "declaring ONLY a transfer function declares only that";
    // THE HDR PATH, EXACTLY. A caller that knows its content is PQ and
    // nothing else sets transferCharacteristics = 16. The binder's gate used
    // to be "any of the four is set" and its body copied ALL FOUR, so this
    // config also emitted colour_primaries 0 (Reserved) and
    // matrix_coefficients 0 (Identity/GBR -- "the samples are RGB"). Two
    // fabricated declarations from one honest one, both wrong, both on the
    // HDR path.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    cfg.transferCharacteristics = kTcPq;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "binder accepted a transfer-only declaration",
          "VkResult " + U32((uint32_t)r));
    Check(probe.transferCharacteristics == kTcPq, "PQ survived",
          "got " + U32(probe.transferCharacteristics));
    Check(probe.colourPrimaries == kCpUnspecified,
          "unsupplied primaries are Unspecified, NOT Reserved(0)",
          "got " + U32(probe.colourPrimaries));
    Check(probe.matrixCoefficients == kCpUnspecified,
          "unsupplied matrix is Unspecified, NOT Identity/GBR(0)",
          "got " + U32(probe.matrixCoefficients));
    Check(probe.colorDescriptionPresent == 1, "the description is present",
          "got " + U32(probe.colorDescriptionPresent));
    Check(probe.videoSignalTypePresent == 1, "video_signal_type is present",
          "got " + U32(probe.videoSignalTypePresent));
    Check(probe.videoFullRangeFlag == 0,
          "an undeclared range stays studio", "got " + U32(probe.videoFullRangeFlag));
}

void CaseFullRangeOnlyDeclaresRangeAndNoColour()
{
    g_currentCase = "declaring ONLY full range raises no colour description";
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.videoFullRange = VK_TRUE;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "binder accepted a range-only declaration",
          "VkResult " + U32((uint32_t)r));
    Check(probe.videoFullRangeFlag == 1, "full range bound",
          "got " + U32(probe.videoFullRangeFlag));
    Check(probe.videoSignalTypePresent == 1,
          "video_signal_type carries it", "got " + U32(probe.videoSignalTypePresent));
    Check(probe.colorDescriptionPresent == 0,
          "no colour description is invented for it",
          "got " + U32(probe.colorDescriptionPresent));
}

void CaseAv1SignalsRangeWithoutAColourDescription()
{
    g_currentCase = "AV1 signals full range with no colour description";
    // AV1's color_config carries color_range, BitDepth and subsampling in
    // ADDITION to the colour description, and none of those is conditioned on
    // color_description_present_flag in the AV1 syntax. The whole struct used
    // to sit behind that one flag, so this configuration -- which signals
    // range fine on H.264 and H.265, where the range lives under a DIFFERENT
    // presence flag -- reached AV1 as nothing at all.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    cfg.videoFullRange = VK_TRUE;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "binder accepted the AV1 config",
          "VkResult " + U32((uint32_t)r));
    Check(probe.av1ColorConfigPresent == 1,
          "the sequence header carries a color_config at all",
          "got " + U32(probe.av1ColorConfigPresent));
    Check(probe.av1ColorRange == 1, "color_range is full",
          "got " + U32(probe.av1ColorRange));
    Check(probe.av1ColorDescriptionPresent == 0,
          "and no colour description is invented",
          "got " + U32(probe.av1ColorDescriptionPresent));
    // The three idc fields must be UNSPECIFIED, not 0: in AV1's enums 0 is
    // BT.709 / BT.709 / IDENTITY, and IDENTITY additionally asserts RGB.
    Check(probe.av1ColorPrimaries == kCpUnspecified,
          "primaries default to Unspecified, not BT.709(0)",
          "got " + U32(probe.av1ColorPrimaries));
    Check(probe.av1MatrixCoefficients == kCpUnspecified,
          "matrix defaults to Unspecified, not Identity(0)",
          "got " + U32(probe.av1MatrixCoefficients));
    Check(probe.av1BitDepth == 8, "BitDepth still describes the session",
          "got " + U32(probe.av1BitDepth));
}

void CaseHdr10CodePointsReachBothArms()
{
    g_currentCase = "BT.2020 + PQ + BT.2020-ncl survive to H.265 and AV1";
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    cfg.colourPrimaries         = kCpBt2020;
    cfg.transferCharacteristics = kTcPq;
    cfg.matrixCoefficients      = kMcBt2020Ncl;

    cfg.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    VkEncBoundConfigProbe h265{};
    Check(VkEncBuildAndProbeConfig(
              cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &h265) ==
              VK_SUCCESS,
          "H.265 accepted HDR10 code points", "init failed");
    Check(h265.colourPrimaries == kCpBt2020 &&
              h265.transferCharacteristics == kTcPq &&
              h265.matrixCoefficients == kMcBt2020Ncl &&
              h265.colorDescriptionPresent == 1,
          "H.265 VUI carries 9 / 16 / 9",
          U32(h265.colourPrimaries) + " / " + U32(h265.transferCharacteristics) +
              " / " + U32(h265.matrixCoefficients) + " present=" +
              U32(h265.colorDescriptionPresent));

    cfg.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    VkEncBoundConfigProbe av1{};
    Check(VkEncBuildAndProbeConfig(
              cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, &av1) ==
              VK_SUCCESS,
          "AV1 accepted HDR10 code points", "init failed");
    Check(av1.av1ColorPrimaries == kCpBt2020 &&
              av1.av1TransferCharacteristics == kTcPq &&
              av1.av1MatrixCoefficients == kMcBt2020Ncl &&
              av1.av1ColorDescriptionPresent == 1,
          "AV1 color_config carries 9 / 16 / 9",
          U32(av1.av1ColorPrimaries) + " / " +
              U32(av1.av1TransferCharacteristics) + " / " +
              U32(av1.av1MatrixCoefficients) + " present=" +
              U32(av1.av1ColorDescriptionPresent));
    Check(av1.av1BitDepth == 10, "at 10 bits", "got " + U32(av1.av1BitDepth));

    // HLG is the other half of the HDR pair and takes a different code point
    // through the same field; it is here so a transfer-specific clamp could
    // not pass by hardcoding 16.
    cfg.transferCharacteristics = kTcHlg;
    cfg.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    VkEncBoundConfigProbe hlg{};
    Check(VkEncBuildAndProbeConfig(
              cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &hlg) ==
              VK_SUCCESS,
          "H.265 accepted HLG", "init failed");
    Check(hlg.transferCharacteristics == kTcHlg, "HLG survived",
          "got " + U32(hlg.transferCharacteristics));
}

void CaseUnexpressibleMatrixIsRefusedOnAnRgbaSession()
{
    g_currentCase = "an RGBA session refuses a matrix the filter cannot produce";
    if (!kFilterCompiledIn) {
        return;  // an RGBA session cannot be built at all in this build.
    }
    // SMPTE 240M is a real, different matrix. The filter has no encoding for
    // it, and the old behaviour was to log a line, convert as BT.709 and
    // leave matrix_coefficients saying 7 -- BT.709 pixels under a 240M label,
    // on the SUCCESS path.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_R8G8B8A8_UNORM;
    cfg.colourPrimaries         = kCpBt709;
    cfg.transferCharacteristics = kTcBt709;
    cfg.matrixCoefficients      = kMcSmpte240M;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r == VK_ERROR_INITIALIZATION_FAILED,
          "matrix 7 on an RGBA session is refused, not converted as BT.709",
          "VkResult " + U32((uint32_t)r));

    // Identity/GBR is refused for a different reason -- it asserts the
    // samples ARE RGB -- and it is reachable here only because
    // matrixCoefficients 0 means "not supplied", so it is driven through the
    // config the binder produces rather than through the public field.
    cfg.matrixCoefficients = 11;   // reserved, outside every expressible set
    const VkResult r2 = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r2 == VK_ERROR_INITIALIZATION_FAILED,
          "a reserved matrix code point is refused too",
          "VkResult " + U32((uint32_t)r2));
}

void CaseUnspecifiedMatrixIsDerivedFromPrimaries()
{
    g_currentCase = "an RGBA session signals the matrix it actually applied";
    if (!kFilterCompiledIn) {
        return;
    }
    // The caller declined to NAME a matrix, so there is no request to ignore.
    // The filter must pick one, and after it does the samples really ARE the
    // matrix it picked -- so the label is written to say so. Leaving it at 2
    // would leave a decoder guessing at something the encoder knows.
    //
    // WHAT CHANGED, and it is the whole of CC-1's disposition for code point
    // 2: this arm used to write 1 unconditionally. That threw away the one
    // piece of information the caller DID supply -- the primaries -- and it
    // did so on precisely the caller that has them and nothing else. The
    // BT.2020 sub-case below is the same configuration an HDR caller
    // produces, and it used to emit BT.709 chroma under BT.2020 primaries.
    struct Row { uint8_t primaries; uint8_t expectMatrix; const char* why; };
    static const Row rows[] = {
        { kCpBt709,  kMcBt709,     "BT.709 primaries -> BT.709 matrix" },
        { kCpBt2020, kMcBt2020Ncl, "BT.2020 primaries -> BT.2020 NCL matrix" },
        { 6,         6,            "SMPTE 170M primaries -> BT.601 matrix" },
        { 5,         6,            "BT.470BG primaries -> BT.601 matrix" },
        { kCpUnspecified, kMcBt709,
          "Unspecified primaries -> BT.709, the we-were-not-told default" },
        { 12,        kMcBt709,     "Display-P3 primaries -> BT.709" },
    };
    for (const Row& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.inputFormat = VK_FORMAT_R8G8B8A8_UNORM;
            cfg.colourPrimaries         = row.primaries;
        cfg.transferCharacteristics = kTcBt709;
        cfg.matrixCoefficients      = kCpUnspecified;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        Check(r == VK_SUCCESS, (std::string("accepted: ") + row.why).c_str(),
              "VkResult " + U32((uint32_t)r));
        Check(probe.matrixCoefficients == row.expectMatrix,
              (std::string("signalled: ") + row.why).c_str(),
              "got " + U32(probe.matrixCoefficients) + ", wanted " +
                  U32(row.expectMatrix));
    }
}

//-----------------------------------------------------------------------------
// CC-1's table, walked. THE POINT IS NOT COVERAGE, IT IS DRIFT DETECTION.
//
// The same table is implemented on the Chromium side
// (media/gpu/vulkan/vulkan_video_encode_accelerator.cc, and walked by
// VulkanVeaMatrixTableTest). Code cannot be shared across the two
// repositories, so a divergence can only be caught by both sides asserting
// the same rows. If you change a disposition here and the review does not
// also carry a matching change to VulkanVeaMatrixTableTest, one of the two
// is wrong.
//
// READ THIS BEFORE ADDING A ROW: the input here is the PUBLIC ext field, on
// which 0 means "not supplied". A 0 written below is rewritten to 2 by the
// binder before ResolveRgbToYcbcrMatrix sees it, so row 0 measures the
// UNNAMED disposition and NOT the `case 0` refusal arm -- that arm is
// unreachable through every producer in this tree and is asserted by reading
// it, not by running it.
//-----------------------------------------------------------------------------
void CaseMatrixDispositionTable()
{
    g_currentCase = "CC-1 matrix disposition table (FILTER lane)";
    if (!kFilterCompiledIn) {
        return;
    }
    enum Disp { HONOUR, DERIVE, REFUSE };
    struct Row { uint8_t code; Disp disp; uint8_t signalled; const char* name; };
    // Primaries are pinned to BT.709 throughout, so DERIVE rows expect 1.
    static const Row table[] = {
        {   0, DERIVE, 1, "Identity/GBR -- \"not supplied\" on this surface" },
        {   1, HONOUR, 1, "BT.709" },
        {   2, DERIVE, 1, "Unspecified" },
        {   3, REFUSE, 0, "reserved" },
        {   4, REFUSE, 0, "FCC" },
        {   5, HONOUR, 5, "BT.470BG" },
        {   6, HONOUR, 6, "SMPTE 170M" },
        {   7, REFUSE, 0, "SMPTE 240M" },
        {   8, REFUSE, 0, "YCoCg" },
        {   9, HONOUR, 9, "BT.2020 NCL" },
        {  10, HONOUR, 10, "BT.2020 CL -- accepted, approximated as NCL" },
        {  11, REFUSE, 0, "SMPTE 2085" },
        {  12, REFUSE, 0, "chroma-derived NCL" },
        {  13, REFUSE, 0, "chroma-derived CL" },
        {  14, REFUSE, 0, "ICtCp" },
        { 255, REFUSE, 0, "unknown / INVALID" },
    };
    for (const Row& row : table) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.inputFormat = VK_FORMAT_R8G8B8A8_UNORM;
            cfg.colourPrimaries         = kCpBt709;
        cfg.transferCharacteristics = kTcBt709;
        cfg.matrixCoefficients      = row.code;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        const std::string label =
            "matrix " + U32(row.code) + " (" + row.name + ")";
        if (row.disp == REFUSE) {
            Check(r == VK_ERROR_INITIALIZATION_FAILED,
                  (label + " is REFUSED").c_str(),
                  "VkResult " + U32((uint32_t)r));
        } else {
            Check(r == VK_SUCCESS, (label + " is ACCEPTED").c_str(),
                  "VkResult " + U32((uint32_t)r));
            if (r == VK_SUCCESS) {
                Check(probe.matrixCoefficients == row.signalled,
                      (label + " signals " + U32(row.signalled)).c_str(),
                      "got " + U32(probe.matrixCoefficients));
            }
        }
    }

    // THE DIRECT LANE IS NOT SUBJECT TO THE TABLE, asserted here rather than
    // left to CaseYcbcrSessionKeepsAMatrixTheFilterCannotProduce alone, so
    // the scope travels with the table it scopes.
    for (uint8_t code : { (uint8_t)4, (uint8_t)7, (uint8_t)8, (uint8_t)11 }) {
        VkVideoEncoderConfig direct = BaseConfig();  // NV12, DIRECT
        direct.colourPrimaries         = kCpBt709;
        direct.transferCharacteristics = kTcBt709;
        direct.matrixCoefficients      = code;
        VkEncBoundConfigProbe dprobe{};
        const VkResult dr = VkEncBuildAndProbeConfig(
            direct, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &dprobe);
        Check(dr == VK_SUCCESS,
              ("DIRECT lane accepts matrix " + U32(code) +
               " -- the VUI describes the caller's own samples").c_str(),
              "VkResult " + U32((uint32_t)dr));
        Check(dprobe.matrixCoefficients == code,
              ("DIRECT lane carries matrix " + U32(code) +
               " unaltered").c_str(),
              "got " + U32(dprobe.matrixCoefficients));
    }
}

void CaseYcbcrSessionKeepsAMatrixTheFilterCannotProduce()
{
    g_currentCase = "a YCbCr->YCbCr session is not subject to the RGB contract";
    if (!kFilterCompiledIn) {
        return;
    }
    // THE SCOPING, AS A MEASUREMENT. A 3-plane I420 session also builds the
    // preprocess filter, but that filter COPIES chroma -- it applies no
    // matrix at all. Refusing SMPTE 240M there would reject a configuration
    // that is entirely correct, so the refusal is scoped to the arm that
    // actually converts. Nothing else in the suite would notice if that
    // scoping were dropped.
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.inputFormat = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
    cfg.colourPrimaries         = kCpBt709;
    cfg.transferCharacteristics = kTcBt709;
    cfg.matrixCoefficients      = kMcSmpte240M;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "a 3-plane session accepts matrix 7",
          "VkResult " + U32((uint32_t)r));
    Check(probe.matrixCoefficients == kMcSmpte240M,
          "and carries it through unaltered",
          "got " + U32(probe.matrixCoefficients));
}

void CaseChromaSitingIsSignalledOnlyWhereItIsKnown()
{
    g_currentCase = "chroma siting is signalled for the converting arm only";
    VkVideoEncoderConfig direct = BaseConfig();
    VkEncBoundConfigProbe dprobe{};
    Check(VkEncBuildAndProbeConfig(
              direct, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &dprobe) ==
              VK_SUCCESS,
          "NV12 session builds", "init failed");
    Check(dprobe.chromaLocInfoPresent == 0,
          "a DIRECT session signals no siting -- the content's siting is the "
          "caller's and this library never learns it",
          "got " + U32(dprobe.chromaLocInfoPresent));

    if (!kFilterCompiledIn) {
        return;
    }
    VkVideoEncoderConfig rgba = BaseConfig();
    rgba.inputFormat = VK_FORMAT_R8G8B8A8_UNORM;
    rgba.colourPrimaries         = kCpBt709;
    rgba.transferCharacteristics = kTcBt709;
    rgba.matrixCoefficients      = kMcBt709;
    VkEncBoundConfigProbe rprobe{};
    Check(VkEncBuildAndProbeConfig(
              rgba, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &rprobe) ==
              VK_SUCCESS,
          "RGBA session builds", "init failed");
    Check(rprobe.chromaLocInfoPresent == 1,
          "a converting session signals its siting",
          "got " + U32(rprobe.chromaLocInfoPresent));
    Check(rprobe.chromaSampleLocType == 1,
          "and the type is 1 (centre) -- the 2x2 box average the filter runs, "
          "NOT the 0 (left) a raised flag used to advertise by default",
          "got " + U32(rprobe.chromaSampleLocType));

    // AND THE SAME ASSERTION ON THE VUI THE ARM BUILDS, on BOTH H.26x
    // codecs. Reading the config alone is what let a real defect through:
    // EncoderConfigH265::InitVuiParameters wrote chroma_sample_loc_type from
    // the config and then re-zeroed it unconditionally two hundred lines
    // later, so an H.265 session raised chroma_loc_info_present_flag and
    // advertised type 0 (left) for centre-sited samples -- and no encode row
    // could see it either, because every H.265 row in the matrix takes a path
    // that signals no siting at all.
    for (int arm = 0; arm < 2; arm++) {
        const VkVideoCodecOperationFlagBitsKHR codec =
            (arm == 0) ? VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR
                       : VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
        const char* codecName = (arm == 0) ? "H.264" : "H.265";

        VkVideoEncoderConfig direct2 = BaseConfig();
        direct2.codec = codec;
        VkEncBoundConfigProbe dv{};
        Check(VkEncBuildAndProbeConfig(direct2, codec, &dv) == VK_SUCCESS,
              "DIRECT session builds", codecName);
        const std::string wNoSiting =
            std::string(codecName) + " DIRECT VUI signals no siting";
        Check(dv.vuiChromaLocInfoPresent == 0, wNoSiting.c_str(),
              "got " + U32(dv.vuiChromaLocInfoPresent));

        VkVideoEncoderConfig rgba2 = BaseConfig();
        rgba2.codec = codec;
        rgba2.inputFormat = VK_FORMAT_R8G8B8A8_UNORM;
        rgba2.colourPrimaries         = kCpBt709;
        rgba2.transferCharacteristics = kTcBt709;
        rgba2.matrixCoefficients      = kMcBt709;
        VkEncBoundConfigProbe rv{};
        Check(VkEncBuildAndProbeConfig(rgba2, codec, &rv) == VK_SUCCESS,
              "converting session builds", codecName);
        const std::string wRaised = std::string(codecName) +
            " converting VUI raises chroma_loc_info_present_flag";
        Check(rv.vuiChromaLocInfoPresent == 1, wRaised.c_str(),
              "got " + U32(rv.vuiChromaLocInfoPresent));
        const std::string wType = std::string(codecName) +
            " converting VUI carries type 1 (centre) in BOTH field positions"
            " -- not the 0 a raised flag used to advertise";
        Check(rv.vuiChromaSampleLocTypeTop == 1 &&
                  rv.vuiChromaSampleLocTypeBottom == 1, wType.c_str(),
              U32(rv.vuiChromaSampleLocTypeTop) + " / " +
                  U32(rv.vuiChromaSampleLocTypeBottom));
    }

    // AV1 cannot express centre siting: chroma_sample_position offers only
    // UNKNOWN, VERTICAL and COLOCATED. UNKNOWN is therefore the correct
    // answer and not a gap -- signalling VERTICAL to look decisive would
    // assert a position half a chroma sample away from the one written.
    rgba.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    VkEncBoundConfigProbe aprobe{};
    Check(VkEncBuildAndProbeConfig(
              rgba, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, &aprobe) ==
              VK_SUCCESS,
          "AV1 RGBA session builds", "init failed");
    Check(aprobe.av1ChromaSamplePosition ==
              (uint32_t)STD_VIDEO_AV1_CHROMA_SAMPLE_POSITION_UNKNOWN,
          "AV1 signals chroma_sample_position UNKNOWN",
          "got " + U32(aprobe.av1ChromaSamplePosition));
}


//=============================================================================
// 4. HDR10 static metadata: the config surface, and the BYTES.
//
// THE GOLDEN VECTORS ARE A MEASUREMENT, not a transcription of a spec. Both
// payloads below were built by these same functions, spliced into real
// streams and read back by ffprobe 6.1.1 -- the H.265 SEI inside an encode on
// an RTX A4000, the AV1 OBUs inside a working AV1 stream because no device
// available to this work has AV1 encode. Every field of both, and the AV1
// fixed-point scales and primary permutation in particular, is what that
// read-back reported. A byte array with no such provenance would only pin the
// author's opinion.
//=============================================================================

// BT.2020 mastering display at 1000 cd/m^2 peak, 0.0001 cd/m^2 floor, in
// SMPTE ST 2086 units (chromaticity x 50000, luminance x 10000) and ST 2086
// order (green, blue, red).
//   green (0.170, 0.797)  blue (0.131, 0.046)  red (0.708, 0.292)
//   white D65 (0.3127, 0.3290)
VkVideoEncoderHdrMetadataInfo Hdr10Bt2020()
{
    VkVideoEncoderHdrMetadataInfo hdr{};
    hdr.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_HDR_METADATA_INFO;
    hdr.masteringDisplayPresent = VK_TRUE;
    hdr.displayPrimaryX[0] = 8500;  hdr.displayPrimaryY[0] = 39850;  // G
    hdr.displayPrimaryX[1] = 6550;  hdr.displayPrimaryY[1] = 2300;   // B
    hdr.displayPrimaryX[2] = 35400; hdr.displayPrimaryY[2] = 14600;  // R
    hdr.whitePointX = 15635;
    hdr.whitePointY = 16450;
    hdr.maxDisplayMasteringLuminance = 10000000;   // 1000.0000 cd/m^2
    hdr.minDisplayMasteringLuminance = 1;          //    0.0001 cd/m^2
    hdr.contentLightLevelPresent = VK_TRUE;
    hdr.maxContentLightLevel      = 1000;
    hdr.maxFrameAverageLightLevel = 400;
    return hdr;
}

std::string Hex(const uint8_t* p, size_t n)
{
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) {
        std::snprintf(b, sizeof(b), "%02x", p[i]);
        if (i != 0) s += " ";
        s += b;
    }
    return s;
}

void CheckBytes(const char* what, const uint8_t* got, size_t gotLen,
                const uint8_t* want, size_t wantLen)
{
    if ((gotLen == wantLen) && (std::memcmp(got, want, wantLen) == 0)) {
        Check(true, what, "");
        return;
    }
    Check(false, what,
          "got [" + Hex(got, gotLen) + "] want [" + Hex(want, wantLen) + "]");
}

void CaseH265HdrSeiBytes()
{
    g_currentCase = "the H.265 HDR10 prefix SEI is byte-exact";
    const VkVideoEncoderHdrMetadataInfo hdr = Hdr10Bt2020();
    uint8_t got[128];
    const uint32_t n = VkEncBuildHdrMetadataPayload(
        &hdr, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, got, sizeof(got));

    // 00 00 00 01                 4-byte start code
    // 4e 01                       nal_unit_type 39 (PREFIX_SEI_NUT), tid 0
    // 89 18                       payloadType 137, payloadSize 24
    //   2134 9baa                 G x=8500  y=39850
    //   1996 08fc                 B x=6550  y=2300
    //   8a48 3908                 R x=35400 y=14600
    //   3d13 4042                 white point 15635 / 16450
    //   0098 9680                 max luminance 10000000 (1000 cd/m^2)
    //   0000 0003 01              min luminance 1 -- WITH THE EMULATION
    //                             PREVENTION BYTE. Three zero bytes precede a
    //                             0x01, so 00 00 00 01 would open a NAL in
    //                             the middle of this one. This is the live
    //                             case, not a formality: a floor of 0.0001
    //                             cd/m^2 is the commonest HDR10 value there
    //                             is.
    // 90 04                       payloadType 144, payloadSize 4
    //   03e8 0190                 MaxCLL 1000, MaxFALL 400
    // 80                          rbsp_trailing_bits
    static const uint8_t kWant[] = {
        0x00, 0x00, 0x00, 0x01,
        0x4e, 0x01,
        0x89, 0x18,
        0x21, 0x34, 0x9b, 0xaa,
        0x19, 0x96, 0x08, 0xfc,
        0x8a, 0x48, 0x39, 0x08,
        0x3d, 0x13, 0x40, 0x42,
        0x00, 0x98, 0x96, 0x80,
        0x00, 0x00, 0x03, 0x00, 0x01,
        0x90, 0x04,
        0x03, 0xe8, 0x01, 0x90,
        0x80,
    };
    CheckBytes("mastering display + content light SEI", got, n,
               kWant, sizeof(kWant));
}

void CaseAv1HdrMetadataObuBytes()
{
    g_currentCase = "the AV1 HDR10 metadata OBUs are byte-exact";
    const VkVideoEncoderHdrMetadataInfo hdr = Hdr10Bt2020();
    uint8_t got[128];
    const uint32_t n = VkEncBuildHdrMetadataPayload(
        &hdr, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, got, sizeof(got));

    // 2a 1a                       OBU_METADATA, has_size_field, size 26
    //   02                        metadata_type 2 = HDR_MDCV
    //   b53f 4ac1                 RED   46399 / 19137  (0.708, 0.292)
    //   2b85 cc08                 GREEN 11141 / 52232  (0.170, 0.797)
    //   2189 0bc7                 BLUE   8585 /  3015  (0.131, 0.046)
    //   500d 5439                 white 20493 / 21561
    //   0003 e800                 luminance_max 256000, 24.8  -> 1000.0
    //   0000 0002                 luminance_min      2, 18.14 -> 0.000122
    //   80                        trailing_bits
    // 2a 06                       OBU_METADATA, size 6
    //   01 03e8 0190 80           metadata_type 1 = HDR_CLL, 1000, 400
    //
    // THE PRIMARY ORDER IS THE POINT OF THIS VECTOR. It is R,G,B here and
    // G,B,R in the H.265 vector above, from the SAME input array. ffprobe
    // reported BT.2020 for this spelling and reported BT.2020's GREEN as its
    // red for the other one.
    //
    // The luminance_min round trip is lossy and that is a property of the
    // format, not a bug: 0.0001 cd/m^2 is 1.6384 in 18.14, which rounds to 2,
    // i.e. 0.000122. H.265 carries the same value exactly.
    static const uint8_t kWant[] = {
        0x2a, 0x1a,
        0x02,
        0xb5, 0x3f, 0x4a, 0xc1,
        0x2b, 0x85, 0xcc, 0x08,
        0x21, 0x89, 0x0b, 0xc7,
        0x50, 0x0d, 0x54, 0x39,
        0x00, 0x03, 0xe8, 0x00,
        0x00, 0x00, 0x00, 0x02,
        0x80,
        0x2a, 0x06,
        0x01, 0x03, 0xe8, 0x01, 0x90, 0x80,
    };
    CheckBytes("MDCV + CLL metadata OBUs", got, n, kWant, sizeof(kWant));
}

void CaseEachHdrPayloadIsIndependent()
{
    g_currentCase = "each HDR payload is emitted on its own";
    // A mastering display of all zeros is a CLAIM -- it says the display is
    // black -- so a caller that knows only MaxCLL must not get one.
    VkVideoEncoderHdrMetadataInfo hdr{};
    hdr.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_HDR_METADATA_INFO;
    hdr.contentLightLevelPresent = VK_TRUE;
    hdr.maxContentLightLevel      = 600;
    hdr.maxFrameAverageLightLevel = 120;
    uint8_t got[128];
    const uint32_t n = VkEncBuildHdrMetadataPayload(
        &hdr, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, got, sizeof(got));
    static const uint8_t kWant[] = {
        0x00, 0x00, 0x00, 0x01, 0x4e, 0x01,
        0x90, 0x04, 0x02, 0x58, 0x00, 0x78,
        0x80,
    };
    CheckBytes("content light only -- no mastering display message", got, n,
               kWant, sizeof(kWant));

    // And nothing at all when nothing was declared.
    VkVideoEncoderHdrMetadataInfo none{};
    none.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_HDR_METADATA_INFO;
    const uint32_t z = VkEncBuildHdrMetadataPayload(
        &none, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, got, sizeof(got));
    Check(z == 0, "an empty declaration emits no SEI at all",
          "got " + U32(z) + " bytes");
}

void CaseHdrPayloadRefusesToTruncate()
{
    g_currentCase = "a payload that does not fit is refused, not clipped";
    // The caller cannot see a dropped SEI: byte counts, completion edges and
    // every counter are identical with and without it. So a short buffer must
    // produce 0 rather than a prefix.
    const VkVideoEncoderHdrMetadataInfo hdr = Hdr10Bt2020();
    uint8_t got[16];
    std::memset(got, 0xAA, sizeof(got));
    const uint32_t n = VkEncBuildHdrMetadataPayload(
        &hdr, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, got, sizeof(got));
    Check(n == 0, "a 16-byte buffer yields 0, not a truncated NAL",
          "got " + U32(n));
    Check(got[0] == 0xAA, "and nothing was written into it",
          "first byte " + U32(got[0]));
}

void CaseHdrMetadataBindsThroughThePnextChain()
{
    g_currentCase = "the chained HDR struct reaches the encoder config";
    VkVideoEncoderHdrMetadataInfo hdr = Hdr10Bt2020();
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    cfg.pNext = &hdr;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "binder accepted the chained HDR struct",
          "VkResult " + U32((uint32_t)r));
    Check(probe.hdrMasteringPresent == 1 && probe.hdrContentLightPresent == 1,
          "both payloads bound",
          U32(probe.hdrMasteringPresent) + " / " +
              U32(probe.hdrContentLightPresent));
    Check(probe.hdrMaxDisplayMasteringLuminance == 10000000 &&
              probe.hdrMinDisplayMasteringLuminance == 1,
          "luminance bounds bound",
          U32(probe.hdrMaxDisplayMasteringLuminance) + " / " +
              U32(probe.hdrMinDisplayMasteringLuminance));
    Check(probe.hdrMaxContentLightLevel == 1000 &&
              probe.hdrMaxFrameAverageLightLevel == 400,
          "MaxCLL / MaxFALL bound",
          U32(probe.hdrMaxContentLightLevel) + " / " +
              U32(probe.hdrMaxFrameAverageLightLevel));
    Check(probe.hdrGreenPrimaryX == 8500 && probe.hdrGreenPrimaryY == 39850,
          "the ST 2086 green primary bound unpermuted",
          U32(probe.hdrGreenPrimaryX) + " / " + U32(probe.hdrGreenPrimaryY));

    // A config with no chain must land NOTHING -- otherwise the assertions
    // above could be satisfied by a default rather than by the binder.
    VkVideoEncoderConfig plain = BaseConfig();
    plain.codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    VkEncBoundConfigProbe pprobe{};
    Check(VkEncBuildAndProbeConfig(
              plain, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &pprobe) ==
              VK_SUCCESS,
          "unchained config builds", "init failed");
    Check(pprobe.hdrMasteringPresent == 0 && pprobe.hdrContentLightPresent == 0,
          "an unchained config declares no HDR metadata",
          U32(pprobe.hdrMasteringPresent) + " / " +
              U32(pprobe.hdrContentLightPresent));
}

void CaseH264RefusesHdrMetadata()
{
    g_currentCase = "H.264 refuses HDR metadata it cannot carry";
    // There is no standard H.264 mastering-display or content-light SEI, so
    // accepting the chain would be accepted-and-ignored -- the defect class
    // this API refuses everywhere else.
    VkVideoEncoderHdrMetadataInfo hdr = Hdr10Bt2020();
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.pNext = &hdr;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    Check(r == VK_ERROR_INITIALIZATION_FAILED,
          "an H.264 session is refused rather than silently dropping it",
          "VkResult " + U32((uint32_t)r));
}

//=============================================================================
// 6. The colour-model declaration at the registration gate
//=============================================================================
//
// Section 1 asks the taxonomy what a (format, colorModel) pair MEANS. This
// group asks the registration gate what it DOES about a pair that means
// nothing -- a different question with a different answer, and the only one a
// producer can act on: ValidateImageDescriptor is the negotiation point,
// reached identically from RegisterImageResource and from QueryImageSupport,
// and a declaration refused there is one the producer can still correct
// before it commits an allocation.
//
// The session these run against has a null backend: no device, no worker
// threads, no negotiated encode format. That is not a limitation here, it is
// the point. Whether a declaration can be read against its format is a
// property of the DESCRIPTOR ALONE, so the gate that judges it must answer
// the same on a session that has negotiated nothing, and this group is what
// holds it to that.

class NullSession {
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

private:
    VkSharedBaseObj<VulkanVideoEncoderExt> m_encoder;
    VkEncNullBackendState                  m_backend{};
};

// The shape a producer actually hands in: a block-linear image it intends the
// encoder to read directly. Zero-initialised except for the fields named, so
// colorModel is FROM_FORMAT unless a case says otherwise -- which is exactly
// the descriptor an ordinary caller builds.
VkVideoEncoderExternalImageDescriptor DirectDescriptor(VkFormat format)
{
    VkVideoEncoderExternalImageDescriptor desc = {};
    desc.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
    desc.handleType = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
    desc.format     = format;
    desc.width      = 640;
    desc.height     = 360;
    desc.tiling     = VK_IMAGE_TILING_OPTIMAL;
    desc.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
    desc.residency  = VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
    desc.existingImage = (VkImage)(uintptr_t)0xA110C8ED;
    return desc;
}

VkVideoEncoderStatusCode Register(
    NullSession& s, const VkVideoEncoderExternalImageDescriptor& desc)
{
    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    const VkVideoEncoderStatusCode code =
        s.Get()->RegisterImageResource(desc, 0, &resource, nullptr);
    if (code == VK_VIDEO_ENCODER_STATUS_SUCCESS) {
        s.Get()->UnregisterImageResource(resource);
    }
    return code;
}

VkVideoEncoderStatusCode Query(
    NullSession& s, const VkVideoEncoderExternalImageDescriptor& desc)
{
    VkVideoEncoderImageSupport support{};
    support.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMAGE_SUPPORT;
    s.Get()->QueryImageSupport(desc, &support);
    return support.status;
}

void CaseContradictoryColorModelIsRefusedAtRegistration(NullSession& s)
{
    g_currentCase = "a contradictory colorModel is refused at registration";
    // THE CONTRACT. A declaration the format cannot carry is answered
    // COLOR_MODEL_UNSUPPORTED at the gate, which names the field that is
    // wrong: NV12 below is directly encodable and it is the declaration over
    // it that is refused. The alternative -- registering it and
    // letting the route fall out of an unresolvable colour model -- is the
    // accepted-and-silently-degraded shape: the registration succeeds, the
    // direct route is declined for a reason the caller is never told, and the
    // frames take a staging copy nobody asked for.
    struct Row {
        VkFormat                 format;
        VkVideoEncoderColorModel declared;
        const char*              why;
    };
    static const Row rows[] = {
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_RGB,
          "RGB declared over NV12, the directly-encodable case" },
        { VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_RGB,
          "RGB declared over 3-plane I420" },
        { VK_FORMAT_B8G8R8A8_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR,
          "YCbCr declared over BGRA8, which carries no packed reading" },
    };
    for (const Row& row : rows) {
        VkVideoEncoderExternalImageDescriptor desc =
            DirectDescriptor(row.format);
        desc.colorModel = row.declared;
        const VkVideoEncoderStatusCode reg = Register(s, desc);
        Check(reg == VK_VIDEO_ENCODER_STATUS_ERROR_COLOR_MODEL_UNSUPPORTED,
              (std::string("registration refuses it: ") + row.why).c_str(),
              "status " + U32((uint32_t)reg));
        // The query is not a second opinion. It runs the same predicate, so a
        // producer that negotiates hears the same word it would have heard
        // from the registration it was about to attempt.
        const VkVideoEncoderStatusCode q = Query(s, desc);
        Check(q == VK_VIDEO_ENCODER_STATUS_ERROR_COLOR_MODEL_UNSUPPORTED,
              (std::string("and the query agrees: ") + row.why).c_str(),
              "status " + U32((uint32_t)q));
    }
}

void CaseZeroInitialisedDescriptorStillRegisters(NullSession& s)
{
    g_currentCase = "FROM_FORMAT, the zero-initialised case, still registers";
    // VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT is 0, so this is what every
    // caller that never heard of the field declares. It reads the model off
    // the format and must pass wherever the format itself does; a gate that
    // caught it would refuse the entire installed base.
    VkVideoEncoderExternalImageDescriptor desc =
        DirectDescriptor(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
    Check(desc.colorModel == VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT,
          "the descriptor under test really is the zero-init one",
          "colorModel " + U32((uint32_t)desc.colorModel));
    const VkVideoEncoderStatusCode reg = Register(s, desc);
    Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "an NV12 descriptor declaring no colour model registers",
          "status " + U32((uint32_t)reg));
    const VkVideoEncoderStatusCode q = Query(s, desc);
    Check(q == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "and the query says so too", "status " + U32((uint32_t)q));
}

void CaseAgreeingDeclarationStillRegisters(NullSession& s)
{
    g_currentCase = "a declaration that agrees with the format still registers";
    // Naming the model the format already implies is legal and changes
    // nothing. This is the other half of the zero-init case: the gate refuses
    // a CONTRADICTION, not a declaration.
    VkVideoEncoderExternalImageDescriptor desc =
        DirectDescriptor(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
    desc.colorModel = VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR;
    const VkVideoEncoderStatusCode reg = Register(s, desc);
    Check(reg == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "NV12 declared YCbCr registers", "status " + U32((uint32_t)reg));
    const VkVideoEncoderStatusCode q = Query(s, desc);
    Check(q == VK_VIDEO_ENCODER_STATUS_SUCCESS,
          "and the query says so too", "status " + U32((uint32_t)q));
}

void CasePackedYcbcrDeclarationIsNotAContradiction(NullSession& s)
{
    g_currentCase = "the packed 4:4:4 declaration is not a contradiction";
    // AYUV has no Vulkan enumerant of its own and rides R8G8B8A8_UNORM, so
    // YCbCr declared over that format is a statement of fact -- the one
    // disagreement the library resolves rather than refuses. The gate must
    // let it past, and this is what proves the gate reads the packed table
    // instead of comparing two enums.
    //
    // Whether this session can then ROUTE the input is a separate question
    // with a separate answer: a null-backend session built no compute filter,
    // so the format needs a conversion this session does not have and the
    // honest verdict is CONVERSION_REQUIRED. That it is NOT
    // COLOR_MODEL_UNSUPPORTED is the assertion -- the colour-model gate did
    // not fire.
    VkVideoEncoderExternalImageDescriptor desc =
        DirectDescriptor(VK_FORMAT_R8G8B8A8_UNORM);
    desc.colorModel = VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR;
    const VkVideoEncoderStatusCode reg = Register(s, desc);
    Check(reg == VK_VIDEO_ENCODER_STATUS_ERROR_CONVERSION_REQUIRED,
          "AYUV declared YCbCr passes the colour-model gate and is answered "
          "on its routing, not on its declaration",
          "status " + U32((uint32_t)reg));
}

void CaseUnknownColorModelValueIsRefused(NullSession& s)
{
    g_currentCase = "a colorModel outside the enumeration is refused";
    // A value the enumeration does not define cannot agree with any format,
    // so it resolves to nothing and is refused by the same gate. Reading it as
    // FROM_FORMAT would give a caller that miscomputed the field an encode it
    // never asked for and no diagnostic anywhere.
    VkVideoEncoderExternalImageDescriptor desc =
        DirectDescriptor(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
    desc.colorModel = (VkVideoEncoderColorModel)7;
    const VkVideoEncoderStatusCode reg = Register(s, desc);
    Check(reg == VK_VIDEO_ENCODER_STATUS_ERROR_COLOR_MODEL_UNSUPPORTED,
          "an undefined colorModel is refused rather than read as 0",
          "status " + U32((uint32_t)reg));
}

}  // namespace

// The registration gate stands up an encoder session; every other group in
// this binary calls functions that read only their arguments. Selecting it
// keeps the two apart, so a session that cannot be created is reported as
// itself rather than as a taxonomy failure.
bool WantsRegistrationGroup(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--registration") == 0) {
            return true;
        }
    }
    return false;
}

int RunRegistrationGroup()
{
    std::printf("Encoder-ext colour-model declaration at the "
                "registration gate\n");
    std::printf("-------------------------------------------------------\n");

    NullSession session;
    if (!session.Open()) {
        return 2;
    }
    CaseContradictoryColorModelIsRefusedAtRegistration(session);
    CaseZeroInitialisedDescriptorStillRegisters(session);
    CaseAgreeingDeclarationStillRegisters(session);
    CasePackedYcbcrDeclarationIsNotAContradiction(session);
    CaseUnknownColorModelValueIsRefused(session);

    std::printf("-------------------------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (WantsRegistrationGroup(argc, argv)) {
        return RunRegistrationGroup();
    }
    std::printf("Encoder-ext input-format taxonomy, preprocess decision,\n");
    std::printf("transfer-function declaration and colour description\n");
    std::printf("-------------------------------------------------------\n");
    std::printf("compute filter compiled in: %s\n",
                kFilterCompiledIn ? "yes" : "no");

    CaseSemiPlanarIsDirect();
    CaseTwelveBitSemiPlanarIsViaFilter();
    CaseThreePlaneIsViaFilter();
    CaseRgbaIsViaFilterAndSinglePlane();
    CaseSrgbAndJunkAreUnsupported();
    CaseDeclaredColorModelDecidesTheClass();
    CaseAdvertisedListDropsWhatTheLibraryRefuses();
    CaseAdvertisedListPassesWhatTheLibraryRoutes();
    CaseAdvertisedListReportsOneFormatOnce();
    CaseAdvertisedListStopsAtCapacity();
    CaseYcbcrIsNeverClaimedAsRgba();
    CaseRgbaSessionSurvivesTheSinglePlaneGate();

    CaseContradictoryColorModelIsRefusedByTheBinder();
    CaseAgreeingColorModelStillBinds();
    CaseSemiPlanarBindsTwoPlanes();
    CaseTenBitBindsBitDepthAndPlanes();
    CaseFourFourFourBindsItsOwnSubsampling();
    CaseFourTwoZeroStillBindsFourTwoZero();
    CaseDirectFormatGetsNoFilter();
    CaseThreePlaneGetsAFilterWithoutAsking();
    CaseRgbaGetsAFilterWithoutAsking();
    CaseUnsupportedFormatStillRefused();
    CaseFilterArmReadsTheDeclaredColourModel();

    CaseUndeclaredInputOtfAssertsNothing();
    CaseAgreeingOtfDeclarationsAreAccepted();
    CaseMismatchedOtfDeclarationIsRefused();

    CasePartialColourSupplyDoesNotFabricateTheRest();
    CaseFullRangeOnlyDeclaresRangeAndNoColour();
    CaseAv1SignalsRangeWithoutAColourDescription();
    CaseHdr10CodePointsReachBothArms();
    CaseUnexpressibleMatrixIsRefusedOnAnRgbaSession();
    CaseUnspecifiedMatrixIsDerivedFromPrimaries();
    CaseMatrixDispositionTable();
    CaseYcbcrSessionKeepsAMatrixTheFilterCannotProduce();
    CaseChromaSitingIsSignalledOnlyWhereItIsKnown();

    CaseH265HdrSeiBytes();
    CaseAv1HdrMetadataObuBytes();
    CaseEachHdrPayloadIsIndependent();
    CaseHdrPayloadRefusesToTruncate();
    CaseHdrMetadataBindsThroughThePnextChain();
    CaseH264RefusesHdrMetadata();

    CaseProfileNumbersReachTheCodecConfigUnchanged();
    CaseProfileDefaultIsDerivedPerCodec();
    CaseProfileNumbersAreReadAgainstTheCodec();
    CaseProfileMustAdmitTheInputDepth();

    CaseFieldTableClassifiesEveryField();

    std::printf("-------------------------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
